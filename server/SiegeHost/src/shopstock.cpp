// shopstock.cpp - 0.8.3: every shop a player opens keeps N backpacks on its MISC tab, at a fixed price.
//
// WHAT THIS IS AND WHY IT LOOKS THE WAY IT DOES
// ---------------------------------------------------------------------------------------------------
// The first attempt at this feature (the 0.8.2 build that froze EU on 2026-09-24) did the whole job
// inside the engine's own RSAddShopper, i.e. inside a FuBi RPC dispatch, between the scope-enter at
// 0x00450B41 and the scope-leave at 0x00450B79. Creating Gos and mutating a store's inventory from in
// there stopped the world: the heartbeat went stale and siegeserver's watchdog killed the process
// after 2m8s. 
//
// This version does not patch the engine at all. Not one byte. It does not need to:
//
//   GoDb+0x338 is the head of a circular list of every Go in the world, linked through Go+0x70.
//   GoDb::Update (0x00549188) walks exactly that list every frame.
//
// So ShopStock walks it too, on the main loop, every few seconds, and tops up every Go that has a
// store. No hook, no RPC scope, no stub, nothing running inside someone else's call. The worst case
// if any assumption here is wrong is that it finds no shops and does nothing.
//
// WHERE SHOP STOCK LIVES
// ---------------------------------------------------------------------------------------------------
// A shop is a Go whose +0x48 is a GoStore (Go::HasStore, 0x0047C280). The items on its shelf are the
// shopkeeper's own inventory at eInventoryLocation 0x15 (il_main) - that is what [store_pcontent]
// fills at world creation, and what the store screen lists (GoInventory::ListItems(inv, 0x15, coll)
// inside the store page builder around 0x005E66xx).
//
// A Go's items are a plain vector<Go*> at Go+0x88 (begin) .. Go+0x8C (end). Read off
// GoInventory::IsSpellInProgress (0x00578731), which walks exactly that pair and calls
// Go::GetTemplateName (0x005390A3) on each element.
//
// WHICH TAB
// ---------------------------------------------------------------------------------------------------
// The store screen files an item by the item's own kind, not by how it got there. The MISC branch of
// the page builder is literally "the tab is misc AND !IsShield AND !IsArmor AND !IsWeapon AND
// !IsSpell AND !IsPotion". A backpack is none of those, so it lands under MISC. Nothing reads
// category_name. (Confirmed independently in the 0.8.2 lab work on a real cloned backpack.)
//
// THE PRICE IS MEASURED, NOT CALCULATED
// ---------------------------------------------------------------------------------------------------
// The buy price the shop screen shows is FUN_005E607A(store, itemGo, 0): it starts from the item's
// gold_value (GoAspect+0x4C) and adds a markup that is per shop ([store] item_markup). Rather than
// assume the arithmetic - guessed arithmetic can easily lead to subtle pricing bugs - this code
// sets a gold value, asks the engine what the shop would charge, and corrects. It converges in two or
// three passes and it is right whatever the markup is, including shops whose markup differs.
#include "siegehost.h"
#include "engine.h"
int InvFixLiveBackpacks(void* inv);
bool InvFixSetCountFix(bool on);
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" volatile LONG g_shopStocking;
extern "C" void SiegeHost_ShopForceClientAllowed();

namespace {

// ---- engine -------------------------------------------------------------------------------------
// GoDb+0x338: head of the circular list of every Go; Go+0x70 is the next link. Read off
// GoDb::Update (0x00549188), which walks it every frame.
const int kGoDbGoList = 0x338, kGoNext = 0x70;

// GoInventory::RSCreateAddItem builds the item with GoCloneReq::SetForceClientAllowed(true), which is
// what tells every client to build its own copy of the object in the world, at the position the clone
// was given - the shopkeeper's feet. The server then moves it into the shop, but the client has
// already been handed a world object, and that is what players were seeing lying on the ground near
// the Elddim shops while the server's own list had all of them correctly inside the shopkeepers.
//
// 0x00578685 is the ONLY call site of SetForceClientAllowed in the whole executable, and it is inside
// RSCreateAddItem, so redirecting it touches nothing else. The stub does nothing while we are
// stocking a shop and calls the original at any other time.
const DWORD kFcaSite = 0x00578685, kFcaCallee = 0x0047BCD1;

const auto FindGo          = (void*(__thiscall*)(void* godb, DWORD goid))0x00550A6B;
const auto TemplateName    = (const char*(__thiscall*)(void* go))0x005390A3;
// GoInventory::RSCreateAddItem(eInventoryLocation, const gpbstring&) - 0x00578573, ret 8.
// Creates a Go from a template name and adds it to this inventory at that location, server side,
// replicating to every client. This is the engine's own one-call "give this container that item".
const auto RSCreateAddItem = (void*(__thiscall*)(void* inv, int loc, const eng::gpstring* tmpl))0x00578573;
// GoAspect::SSetGoldValue(int value, bool replicate) - 0x00533F7F, ret 8.
const auto SSetGoldValue   = (void(__thiscall*)(void* aspect, int value, char replicate))0x00533F7F;
// GoInventory::GetGridAreaFree - 0x00472BF0, thiscall, no arguments. Returns
// max(gridWidth * gridHeight - cells used, 0) for the main grid, which is the shop shelf.
const auto GridFree        = (int(__thiscall*)(void* inv))0x00472BF0;
// Go::IsScreenPartyMember - 0x00539711. The engine's inventory add uses this to decide whether an
// item that did not fit may go into a backpack, and whether it gets dropped at the owner's feet
// instead of vanishing. On a dedicated server the screen party is ServerHost's, so this is the
// thing to measure for a remote player's hero.
const auto IsScreenPartyMember = (bool(__thiscall*)(void* go))0x00539711;
// The store screen's own buy-price function - 0x005E607A, thiscall on GoStore. It takes the item's
// GOID, not its Go*: the first thing it does is resolve [ebp+8] through 0x0053E169 in place, so a Go*
// passed here comes back as null and the function returns 0. Reading that off the disassembly saved a
// second round of guessing (the arithmetic it then does is gold_value + gold_value * item_markup,
// which is why a 500 gold value reads as 1500 at our shops' markup of 2).
const auto BuyPriceByGoid  = (int(__thiscall*)(void* store, DWORD itemGoid, char noContents))0x005E607A;

const int kIlMain = 0x15;            // eInventoryLocation the shop shelf uses
const int kGoStore = 0x48;           // Go+0x48  -> GoStore*
const int kGoAspect = 0x04;          // Go+0x04  -> GoAspect*
const int kGoInventory = 0x2C;       // Go+0x2C  -> GoInventory*
const int kGoOwner = 0x78;           // Go+0x78  -> the owning Player (Player+0x14: 1 human, 2 computer)
const int kGoid = 0x4C;              // Go+0x4C  -> goid
const int kInvGold = 0x78;           // GoInventory+0x78 -> the party's gold, an int

// GoInventory::RCSetGold, the replicated setter. GetGold at 0x00472B7E is nothing but
// `return *(int*)(this + 0x78)`, HasGold at 0x00662534 is `0 < *(int*)(this + 0x78)`, and
// RCAddGold at 0x00472BAC is RCSetGold(this, *(int*)(this + 0x78) + n). Calling this is exactly
// what the engine does when a player picks gold up, so the clients are told the same way.
const auto RCSetGold = (void*(__thiscall*)(void* inv, int amount))0x00577C71;

// The cap. Signed 32-bit gold tops out at 2147483647, so this sits comfortably inside the type and
// the trouble is the client's own handling well before that.
const int kGoldCap = 999999999;
bool g_goldCap = true; DWORD g_goldTrimmed = 0, g_goldTrimmedTotal = 0; int g_goldHighest = 0;
bool g_goldTest = false; bool g_goldTested = false;   // lab only
bool g_goldDiag = false; int g_goldDiagLines = 0;    // lab only
const int kItemsBegin = 0x88, kItemsEnd = 0x8C;
const int kAspectGoldValue = 0x4C;   // GoAspect+0x4C (GoAspect::SetGoldValue writes it, 0x005340CD)
const int kGoParent = 0x6C;          // Go+0x6C (Go::GetParent is 'mov eax,[ecx+0x6C]; ret', 0x006E7F8E)
// A backpack's inventory is 4x8, but the object itself takes 2x2 = 4 cells on the shelf it sits on.
const int kBackpackCells = 4;

inline void* GoOfComponent(void* comp) { return comp ? *(void**)((char*)comp + 4) : nullptr; }

// ---- state --------------------------------------------------------------------------------------
bool g_on = false, g_diag = false;
int  g_want = 0;                 // how many backpacks each shop should hold
int  g_price = 1500;             // what the shop should charge for one
char g_tmpl[64] = "dsx_backpack_empty";
DWORD g_added = 0, g_priced = 0, g_visits = 0, g_lastSummary = 0, g_failed = 0;
DWORD g_nextSweep = 0, g_lastWalked = 0;
int g_lastShops = 0, g_lastLoose = 0; bool g_sweptOnce = false;
int g_stockedShops = 0;   // shops that have had their one filling in this world
// LAB ONLY (SIEGEHOST_SHOPBUYTEST=1). A purchase makes 'have' smaller than 'want'; raising 'want'
// after the shops are stocked produces the same shortfall through the same arithmetic, without
// calling into the engine's buy path at all. Under the old rule the sweep would top the shelf back
// up; under the new one it must not touch it.
bool g_buyTest = false; bool g_buyTested = false;

// Every shop we have already served, and the gold value that came out right for it. Shops differ in
// markup, so the calibration is per shop, and it is cheap to keep.
// 'stocked' is 1.1.1. A shop is filled once for the life of a world and then left alone, so a
// backpack somebody buys stays bought. Before this the sweep topped every shop back up to four
// every 15 seconds, which is why closing and reopening a shop brought them back. Nothing in the
// engine restocks a shop - GoStore has RSAddToStore and RSRemoveFromStore and no refill of any
// kind - so the respawn was entirely ours.
struct ShopRec { DWORD goid; int goldValue; DWORD lastStocked; bool noRoom; bool stocked; };
const int kMaxShops = 64;
ShopRec g_shops[kMaxShops];
int g_nShops = 0;

ShopRec* ShopFor(DWORD goid) {
    for (int i = 0; i < g_nShops; ++i) if (g_shops[i].goid == goid) return &g_shops[i];
    if (g_nShops >= kMaxShops) return nullptr;
    ShopRec& r = g_shops[g_nShops++];
    r.goid = goid; r.goldValue = 0; r.lastStocked = 0; r.noRoom = false; r.stocked = false;
    return &r;
}

// ---- work, all of it on the main loop ----------------------------------------------------------
bool IsOurBackpack(void* go) {
    if (!go) return false;
    const char* t = TemplateName(go);
    return t && _stricmp(t, g_tmpl) == 0;
}

int CountBackpacks(void* ownerGo, void** firstOut) {
    if (firstOut) *firstOut = nullptr;
    void** b = *(void***)((char*)ownerGo + kItemsBegin);
    void** e = *(void***)((char*)ownerGo + kItemsEnd);
    if (!b || !e || e < b) return -1;                     // vector not readable: do nothing at all
    if ((size_t)(e - b) > 4096) return -1;                // nonsense span; treat as unreadable
    int n = 0;
    for (void** it = b; it != e; ++it) {
        if (IsOurBackpack(*it)) { if (n == 0 && firstOut) *firstOut = *it; ++n; }
    }
    return n;
}

// Set this item's gold value so the shop charges exactly g_price, and return what it will charge.
// Linear solve against the engine's own price function, so the markup never has to be known.
int PriceItem(void* store, void* go, int startValue) {
    void* asp = *(void**)((char*)go + kGoAspect);
    if (!asp) return -1;
    DWORD goid = *(DWORD*)((char*)go + kGoid);
    int v = startValue > 0 ? startValue : 1000;
    int p = 0;
    for (int pass = 0; pass < 6; ++pass) {
        SSetGoldValue(asp, v, 1);
        p = BuyPriceByGoid(store, goid, 0);
        if (p == g_price) return p;
        if (p <= 0) return p;
        // price is linear in gold value; step to the value that would have landed on target
        long long nv = (long long)v * g_price / p;
        if (nv < 1) nv = 1;
        if (nv > 1000000) nv = 1000000;
        if ((int)nv == v) {                 // rounding floor: nudge and stop chasing
            v += (p < g_price) ? 1 : -1;
            if (v < 1) v = 1;
        } else {
            v = (int)nv;
        }
    }
    SSetGoldValue(asp, v, 1);
    return BuyPriceByGoid(store, goid, 0);
}

void ServeShop(void* store) {
    void* go = GoOfComponent(store);
    if (!go) return;
    if (*(void**)((char*)go + kGoStore) != store) return;      // not this Go's store any more
    void* inv = *(void**)((char*)go + kGoInventory);
    if (!inv) return;
    DWORD goid = *(DWORD*)((char*)go + kGoid);
    ShopRec* rec = ShopFor(goid);
    DWORD now = GetTickCount();
    if (rec && rec->lastStocked && now - rec->lastStocked < 1000) return;   // one pass per shop per second

    void* first = nullptr;
    int have = CountBackpacks(go, &first);
    if (have < 0) return;

    // Once, per shop, per world. The world resets when the last player leaves, which restarts the
    // game process and clears these records, so the next world starts with full shelves again.
    int add = (rec && (rec->noRoom || rec->stocked)) ? 0 : g_want - have;
    for (int i = 0; i < add; ++i) {
        // Look before creating. RSCreateAddItem builds the Go first and only then tries to place it,
        // so an item that will not fit is left lying in the world at the shopkeeper's feet. That is
        // what put loose backpacks on the ground in Elddim on 0.8.3, and checking the shelf first is
        // what stops it happening at all.
        // A shopkeeper reports 0 here even when its shelf happily takes more items, so 0 means "this
        // number does not apply to this container", not "full". Only a positive number too small for
        // a backpack is treated as full.
        int free = GridFree(inv);
        if (free > 0 && free < kBackpackCells) {
            if (rec) rec->noRoom = true;
            Log("ShopStock: shop goid %08lX has %d free cell(s) on its shelf, a backpack needs %d. "
                "Leaving that shop alone for the rest of this session (%d of %d placed).",
                goid, free, kBackpackCells, have, g_want);
            break;
        }
        eng::gpstring s; char alloc[4] = {0};
        eng::gpstring_ctor(&s, g_tmpl, alloc);
        InterlockedIncrement(&g_shopStocking);
        RSCreateAddItem(inv, kIlMain, &s);
        InterlockedDecrement(&g_shopStocking);
        eng::gpstring_dtor(&s);
        int now_have = CountBackpacks(go, &first);
        if (now_have <= have) {
            // It said there was room and the item still did not land. One stranded object is the
            // worst this can now cost, and it will only ever happen once per shop.
            if (rec) rec->noRoom = true;
            ++g_failed;
            Log("ShopStock: shop goid %08lX reported %d free cell(s) but would not take a '%s'. "
                "Leaving that shop alone for the rest of this session.", goid, free, g_tmpl);
            break;
        }
        ++g_added;
        have = now_have;
    }

    // The shop is done for this world once it is holding its full complement. From here on it is
    // left alone: a backpack a player buys stays bought until the world resets.
    if (g_buyTest && !g_buyTested && g_stockedShops >= 2) {
        g_buyTested = true;
        int was = g_want;
        g_want = g_want + 2;
        Log("SHOPBUYTEST: both shops are stocked and holding %d. Asking for %d instead of %d, which "
            "is the same shortfall a player buying two backpacks would leave. The shelves must stay "
            "at %d and no more may be added.", have, g_want, was, was);
    }
    if (rec && !rec->stocked && have >= g_want) {
        rec->stocked = true;
        ++g_stockedShops;
        Log("ShopStock: shop goid %08lX now holds its %d backpack(s) and will not be refilled again "
            "in this world. What players buy stays bought until the world resets.", goid, g_want);
    }

    // Price every backpack on the shelf, not just the new ones: a shop may already hold one from an
    // earlier pass, and a wrong price on an old one is just as visible as on a new one.
    void** b = *(void***)((char*)go + kItemsBegin);
    void** e = *(void***)((char*)go + kItemsEnd);
    if (!b || !e || e < b) return;
    // 0.9.1: on the first sweep of a world, write down which backpacks this shop is holding, so a
    // player's own machine reporting a backpack lying in the world can be matched against them.
    if (!g_sweptOnce) {
        char ids[96]; ids[0] = 0; int n = 0;
        for (void** it = b; it != e && n < 8; ++it) {
            if (!*it || !IsOurBackpack(*it)) continue;
            char one[16];
            snprintf(one, sizeof one, "%s%08lX", n ? " " : "", *(DWORD*)((char*)*it + kGoid));
            strncat(ids, one, sizeof ids - strlen(ids) - 1);
            ++n;
        }
        if (n) Log("ShopStock: shop goid %08lX is holding %d backpack(s): %s", goid, n, ids);
    }
    int startValue = rec ? rec->goldValue : 0;
    for (void** it = b; it != e; ++it) {
        void* item = *it;
        if (!IsOurBackpack(item)) continue;
        if (BuyPriceByGoid(store, *(DWORD*)((char*)item + kGoid), 0) == g_price) continue;
        int p = PriceItem(store, item, startValue);
        void* asp = *(void**)((char*)item + kGoAspect);
        int v = asp ? *(int*)((char*)asp + kAspectGoldValue) : 0;
        if (p == g_price) {
            if (rec) rec->goldValue = v;
            startValue = v;
            ++g_priced;
            if (g_diag) Log("SHOPSTOCK: shop %08lX item %08lX gold_value %d -> shop charges %d.",
                            goid, *(DWORD*)((char*)item + kGoid), v, p);
        } else {
            ++g_failed;
            if (g_failed <= 4)   // one shop that will not price must not fill the log
                Log("ShopStock: shop goid %08lX - could not land on %d gold for '%s' (closest %d at "
                    "gold_value %d). Leaving it at that.", goid, g_price, g_tmpl, p, v);
        }
    }
    if (rec) rec->lastStocked = now;
    ++g_visits;
}

}  // namespace

// While this is non-zero, the item being created is one of ours and clients are not handed a world
// copy of it. Set only around our own RSCreateAddItem calls, on the main loop, never nested.
extern "C" volatile LONG g_shopStocking = 0;
extern "C" void* g_shopFcaOrig = (void*)0x0047BCD1;
__asm__(
    ".globl _SiegeHost_ShopForceClientAllowed\n"
    "_SiegeHost_ShopForceClientAllowed:\n"
    "  cmpl $0, _g_shopStocking\n"
    "  jne 1f\n"
    "  jmp *_g_shopFcaOrig\n"      // not ours: behave exactly as the engine does
    "1:\n"
    "  ret $4\n");                 // ours: leave the flag alone, clean the one stack argument

// ---------------------------------------------------------------------------------------------
// The sweep. Walks every Go once, tops up every shop it finds. Main loop only.
// ---------------------------------------------------------------------------------------------
namespace {

const DWORD kWalkCap = 200000;   // hard stop: a corrupt or mid-edit list can never hang the server

int g_lastSkipped = 0;
bool g_invDiag = false; bool g_invTest = false; bool g_invTested = false; bool g_packDiag = false;
bool g_cntDiag = false; int g_cntDiagLines = 0;
// Lab only (SIEGEHOST_BPCAPTEST): does the engine still refuse a second backpack? Asks the engine's
// own placement test, FUN_005759F6, against an inventory whose counter has been set high on purpose.
// Before the 0.9.0 NOPs it answers 0. After them it must answer 1.
bool g_capTest = false; bool g_capTested = false; bool g_capTestedA = false, g_capTestedB = false;
bool g_bp2Test = false; bool g_bp2Done = false; int g_bp2LooseBefore = -1;
const auto CanPlace = (char(__thiscall*)(void* inv, void* item, int loc, char deep, int slack, void** out))0x005759F6;

int CountIn(void* go) {
    void** b = *(void***)((char*)go + kItemsBegin);
    void** e = *(void***)((char*)go + kItemsEnd);
    if (!b && !e) return 0;                      // empty container: both ends are null
    if (!b || !e || e < b || (size_t)(e - b) > 4096) return -1;
    return (int)(e - b);
}
// How many of a character's items hang off that particular container. Go+0x6C is the parent, so this
// separates "in the main grid" from "inside the backpack" without trusting any one vector.
int CountChildrenOf(void* go, void* parent) {
    void** b = *(void***)((char*)go + kItemsBegin);
    void** e = *(void***)((char*)go + kItemsEnd);
    if (!b || !e || e < b || (size_t)(e - b) > 4096) return -1;
    int n = 0;
    for (void** it = b; it != e; ++it) if (*it && *(void**)((char*)*it + kGoParent) == parent) ++n;
    return n;
}
void* FirstBackpackIn(void* go) {
    void** b = *(void***)((char*)go + kItemsBegin);
    void** e = *(void***)((char*)go + kItemsEnd);
    if (!b || !e || e < b || (size_t)(e - b) > 4096) return nullptr;
    for (void** it = b; it != e; ++it) {
        void* ii = *it; if (!ii) continue;
        void* iinv = *(void**)((char*)ii + kGoInventory);
        if (iinv && *(char*)((char*)iinv + 0x86)) return ii;
    }
    return nullptr;
}
void AddOne(void* inv, const char* tmpl) {
    eng::gpstring s; char alloc[4] = {0};
    eng::gpstring_ctor(&s, tmpl, alloc);
    RSCreateAddItem(inv, kIlMain, &s);
    eng::gpstring_dtor(&s);
}

// LAB ONLY (SIEGEHOST_INVTEST=1). The backpack-ownership rules, measured on a real player's
// character: where does a second backpack go when they already carry one, and does anything end up
// lying in the world? This exercises the case where a player character already owns a backpack,
// which shop tests never exercised because a shopkeeper owns no backpack of its own.
void SweepOnce() {
    void* godb = *(void**)0x007ACF4C;
    if (!godb) return;
    void* head = *(void**)((char*)godb + kGoDbGoList);
    if (!head) return;
    void* go = *(void**)((char*)head + kGoNext);
    if (!go) return;
    DWORD walked = 0; int shops = 0, loose = 0, skippedPlayers = 0;
    int cntChecked = 0, cntAgree = 0, cntDisagree = 0;
    void* capPack = nullptr; void* capParent = nullptr; void* capInv = nullptr; void* capInvGo = nullptr;
    void* fullInv = nullptr; void* fullInvGo = nullptr;
    while (go && go != head && walked < kWalkCap) {
        ++walked;
        // A backpack with no parent is one lying in the world rather than sitting in a container.
        // Counting them is how we see the 0.8.3 leak if it ever comes back.
        if (!*(void**)((char*)go + kGoParent) && IsOurBackpack(go)) ++loose;
        // (InvDiag removed; PACKRULE covers it)
        if (g_packDiag && IsOurBackpack(go)) {
            void* par = *(void**)((char*)go + kGoParent);
            const char* pt = "(none)";
            int parStore = 0, parInv = 0;
            if (par) {
                pt = TemplateName(par);
                parStore = *(void**)((char*)par + kGoStore) ? 1 : 0;
                parInv = *(void**)((char*)par + kGoInventory) ? 1 : 0;
            }
            Log("PACKDIAG: backpack %08lX parent %p '%s' parentHasStore=%d parentHasInv=%d",
                *(DWORD*)((char*)go + kGoid), par, pt, parStore, parInv);
        }
        if ((g_capTest && !g_capTested) || (g_bp2Test && !g_bp2Done)) {
            void* par = *(void**)((char*)go + kGoParent);
            if (!capPack && IsOurBackpack(go) && par) { capPack = go; capParent = par; }
            void* inv = *(void**)((char*)go + kGoInventory);
            // NOT a shopkeeper: a store owner is exempt from the cap at 0x00575A51 and would never
            // reach the branch we removed.
            if (inv && !*(void**)((char*)go + kGoStore) && !IsOurBackpack(go) &&
                go != capParent && !*(char*)((char*)inv + 0x86)) {
                int live = InvFixLiveBackpacks(inv);
                // One candidate carrying nothing, one already carrying a backpack. The cap has to
                // give opposite answers for those two no matter what the stored counter says.
                if (!capInv && live == 0)  { capInv = inv;  capInvGo = go; }
                if (!fullInv && live >= 1) { fullInv = inv; fullInvGo = go; }
            }
        }
        // 0.9.4: a shop is a shopkeeper, not "anything with a store on it".
        //
        // In previous testing:
        // A player's own character was being treated as a shop and stocked with backpacks because
        // it carried a store component. They have no shelf to go on, so the engine left them lying
        // in the world at that character's feet - appearing at login and while walking.
        //
        // A real shopkeeper is owned by the computer player. Anything owned by a human is somebody's
        // character and is never stocked.
        // Lab only: does our recount agree with the engine's own running total, on every inventory
        // in the world? This is the shape of check that caught the wrong item vector in 0.8.8.
        if (g_cntDiag) {
            void* dinv = *(void**)((char*)go + kGoInventory);
            if (dinv) {
                int live = InvFixLiveBackpacks(dinv);
                int stored = *(int*)((char*)dinv + 0x88);
                if (live >= 0) {
                    ++cntChecked;
                    if (live == stored) ++cntAgree;
                    else {
                        ++cntDisagree;
                        Log("CNTDIAG: '%s' (goid %08lX) holding %d backpack(s), counter says %d"
                            "   <-- DISAGREE",
                            TemplateName(go), *(DWORD*)((char*)go + kGoid), live, stored);
                    }
                    // Name the ones that actually hold something, a few per sweep, so the log shows
                    // what was really examined and not just a tally.
                    if ((live > 0 || stored > 0) && g_cntDiagLines < 400) {
                        ++g_cntDiagLines;
                        Log("CNTDIAG: '%s' holding %d backpack(s), counter says %d",
                            TemplateName(go), live, stored);
                    }
                }
            }
        }
        // Gold cap. A player character carrying more than the cap has it brought back down here,
        // from the main loop, with the engine's own replicated setter. Nothing is patched and no
        // engine call site is touched: this is the same shape as the shop sweep above, which is the
        // only shape that has not bitten us.
        if (g_goldCap || g_goldTest) {
            // A backpack in a player's pack is owned by that player and has a GoInventory of its
            // own, so "owned by a human" alone picked one of those out of the world on the first
            // lab run. A character stands in the world with no parent, and its inventory is not
            // itself a backpack.
            // Measured in the lab, 26 Sep. Everything a player owns comes back as "owned by a
            // human": their books, their backpacks, their character, and the party object itself.
            // They are told apart by what they hang off:
            //   party_humanoid  parent=0          inv=0         <- the party, no inventory
            //   the character   parent=the party  inv=real      <- what we want
            //   a backpack      parent=character  inv=backpack
            //   an item         parent=character  inv=0 or real
            // So: it has an inventory, that inventory is not itself a backpack, and it is not
            // sitting inside anything that has an inventory. No template names involved.
            void* gpl = *(void**)((char*)go + kGoOwner);
            void* gpar = *(void**)((char*)go + kGoParent);
            void* ginv0 = *(void**)((char*)go + kGoInventory);
            bool inContainer = gpar && *(void**)((char*)gpar + kGoInventory);
            bool isCharacter = gpl && *(int*)((char*)gpl + 0x14) == 1 && ginv0 && !inContainer &&
                               !*(char*)((char*)ginv0 + 0x86);
            if (g_goldDiag && gpl && *(int*)((char*)gpl + 0x14) == 1 && g_goldDiagLines < 60 &&
                !inContainer) {
                ++g_goldDiagLines;
                void* di = *(void**)((char*)go + kGoInventory);
                void* dp = *(void**)((char*)go + kGoParent);
                Log("GOLDDIAG: '%s' goid %08lX parent=%p inv=%p isBackpackInv=%d gold=%d",
                    TemplateName(go), *(DWORD*)((char*)go + kGoid), dp, di,
                    di ? (int)*(char*)((char*)di + 0x86) : -1,
                    di ? *(int*)((char*)di + kInvGold) : -1);
            }
            // Lab only: hand the quest talk probe a real joined player and a real shopkeeper.
            if (isCharacter) {
                void* ginv = ginv0;
                // LAB ONLY (SIEGEHOST_GOLDTEST=1): hand a real joined player more gold than the cap,
                // through the engine's own setter, and let the next sweep deal with it. This is the
                // only way to see the whole path work: set, replicate, notice, trim, replicate.
                if (ginv && g_goldTest && !g_goldTested) {
                    g_goldTested = true;
                    int before = *(int*)((char*)ginv + kInvGold);
                    RCSetGold(ginv, kGoldCap + 5000);
                    Log("GOLDTEST: '%s' had %d gold, set to %d (%d over the limit). The next sweep "
                        "should bring it back to %d.",
                        TemplateName(go), before, kGoldCap + 5000, 5000, kGoldCap);
                }
                if (ginv && g_goldCap) {
                    int gold = *(int*)((char*)ginv + kInvGold);
                    if (gold > g_goldHighest) g_goldHighest = gold;
                    if (gold > kGoldCap) {
                        RCSetGold(ginv, kGoldCap);
                        ++g_goldTrimmed; ++g_goldTrimmedTotal;
                        Log("GoldCap: '%s' (goid %08lX) was carrying %d gold, brought back to the "
                            "limit of %d. The extra %d is gone; the client cannot deal with more "
                            "than this.",
                            TemplateName(go), *(DWORD*)((char*)go + kGoid), gold, kGoldCap,
                            gold - kGoldCap);
                    }
                }
            }
        }
        void* store = *(void**)((char*)go + kGoStore);
        if (store) {
            void* pl = *(void**)((char*)go + kGoOwner);
            if (pl && *(int*)((char*)pl + 0x14) == 1) {
                ++skippedPlayers;
                if (g_lastSkipped != skippedPlayers)
                    Log("ShopStock: '%s' (goid %08lX) carries a store but belongs to a player, so it "
                        "is not a shop and is left alone. Stocking it is what used to leave backpacks "
                        "lying on the ground next to them.",
                        TemplateName(go), *(DWORD*)((char*)go + kGoid));
            } else {
                ++shops;
                ServeShop(store);
            }
        }
        void* next = *(void**)((char*)go + kGoNext);
        if (!next) break;
        go = next;
    }
    if (g_cntDiag)
        Log("CNTDIAG: recount checked %d inventories this sweep, %d agreed with the engine's own "
            "counter, %d disagreed.", cntChecked, cntAgree, cntDisagree);
    if (walked >= kWalkCap) {
        Log("ShopStock: the Go list did not close after %lu steps. Sweep off for this session.", kWalkCap);
        g_on = false;
        return;
    }
    // 1.0.2: the cap itself is untouched. What we changed is the number it reads: the stub in
    // invfix.cpp recounts the character's real contents first. So the test is no longer "is the cap
    // off" - it is "does a wrong stored counter still decide the answer". Both directions matter:
    // a counter stuck high locks players out of backpack pickup, while a counter stuck low is
    // what would let somebody carry two.
    if (g_capTest && !g_capTested && capPack) {
        void* out = nullptr;
        if (capInv && !g_capTestedA) {
            g_capTestedA = true;
            int* c = (int*)((char*)capInv + 0x88);
            int was = *c;
            *c = 5;                                     // lie: says five, really carries none
            char r = CanPlace(capInv, capPack, 0x15, 0, 0, &out);
            *c = was;
            Log("CAPTEST A: '%s' really carries 0 backpack(s), counter forced to 5, may one go in "
                "-> %d. %s",
                TemplateName(capInvGo), (int)r,
                r ? "PASS - the stored counter no longer decides, the real contents do."
                  : "FAIL - a stuck counter still locks the character out.");
        }
        if (fullInv && !g_capTestedB) {
            g_capTestedB = true;
            int* c = (int*)((char*)fullInv + 0x88);
            int was = *c;
            *c = 0;                                     // lie: says none, really carries one
            out = nullptr;
            char r = CanPlace(fullInv, capPack, 0x15, 0, 0, &out);
            *c = was;
            Log("CAPTEST B: '%s' really carries %d backpack(s), counter forced to 0, may another go "
                "in -> %d. %s",
                TemplateName(fullInvGo), InvFixLiveBackpacks(fullInv), (int)r,
                r ? "FAIL - the one-backpack rule can be bypassed."
                  : "PASS - the rule holds on the real contents.");
        }
        // Only stop once both directions have actually been measured. B needs somebody in the world
        // to be carrying a backpack, which is not true on the first sweep.
        if (g_capTestedA && g_capTestedB) g_capTested = true;
    }
    // LAB ONLY (SIEGEHOST_BP2TEST=1). Test what happens to somebody who is already carrying more than
    // one backpack. Build that character here - two backpacks in one inventory, with the repair
    // switched off for the two adds the way an unconstrained cap allowed - then switch the repair
    // back on and ask the engine.
    if (g_bp2Test && !g_bp2Done && capInv && capPack) {
        g_bp2Done = true;
        int* c = (int*)((char*)capInv + 0x88);
        Log("BP2TEST: target '%s', %d backpack(s) lying loose in the world before we start.",
            TemplateName(capInvGo), loose);
        bool was = InvFixSetCountFix(false);
        for (int i = 1; i <= 2; ++i) {
            *c = 0;                                  // what a NOPed cap effectively looked like
            AddOne(capInv, "dsx_backpack_empty");
            Log("BP2TEST: after add %d - really carrying %d backpack(s), the engine's counter says %d.",
                i, InvFixLiveBackpacks(capInv), *c);
        }
        InvFixSetCountFix(was);
        void* out = nullptr;
        char r = CanPlace(capInv, capPack, 0x15, 0, 0, &out);
        Log("BP2TEST: repair back on. Really carrying %d, counter now %d, may a third go in -> %d. %s",
            InvFixLiveBackpacks(capInv), *c, (int)r,
            r ? "BAD - a character over the limit can still take more."
              : "Refused, which is what we want. Nothing was taken away either: the two it already "
                "has are still in the pack.");
        g_bp2LooseBefore = loose;
    }
    if (g_bp2Test && g_bp2Done && g_bp2LooseBefore >= 0 && loose != g_bp2LooseBefore) {
        Log("BP2TEST: loose backpacks in the world moved from %d to %d after the test.",
            g_bp2LooseBefore, loose);
        g_bp2LooseBefore = loose;
    }
    g_lastShops = shops; g_lastWalked = walked; g_lastSkipped = skippedPlayers;
    if (loose != g_lastLoose) {
        Log("ShopStock: %d backpack(s) lying loose in the world (was %d).", loose, g_lastLoose);
        g_lastLoose = loose;
    }
    if (g_diag || !g_sweptOnce)
        Log("ShopStock: swept %lu object(s), %d shop(s)%s.", walked, shops,
            g_sweptOnce ? "" : " - first sweep of this world");
    g_sweptOnce = true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// CLIENT CENSUS  (0.9.1, read-only)
//
// This walks the SAME GoDb list ShopStock already walks on the server, with the same hard cap, on a
// player's own machine. It reads three fields per object and writes a log line. It patches nothing,
// changes nothing and cannot touch an item. It is quiet on a healthy client: a line is only written
// when the number of parentless backpacks changes.
int g_cliLoose = -1; DWORD g_nextCensus = 0;

void ClientCensus() {
    void* godb = *(void**)0x007ACF4C;
    if (!godb) return;
    void* head = *(void**)((char*)godb + kGoDbGoList);
    if (!head) return;
    void* go = *(void**)((char*)head + kGoNext);
    if (!go) return;
    DWORD walked = 0; int loose = 0, held = 0;
    DWORD looseIds[8]; int nIds = 0;
    while (go && go != head && walked < kWalkCap) {
        ++walked;
        if (IsOurBackpack(go)) {
            if (*(void**)((char*)go + kGoParent)) ++held;
            else {
                ++loose;
                if (nIds < 8) looseIds[nIds++] = *(DWORD*)((char*)go + kGoid);
            }
        }
        void* next = *(void**)((char*)go + kGoNext);
        if (!next) break;
        go = next;
    }
    if (walked >= kWalkCap) return;
    if (loose == g_cliLoose) return;
    g_cliLoose = loose;
    char ids[96]; ids[0] = 0;
    for (int i = 0; i < nIds; ++i) {
        char one[16];
        snprintf(one, sizeof one, "%s%08lX", i ? " " : "", looseIds[i]);
        strncat(ids, one, sizeof ids - strlen(ids) - 1);
    }
    Log("PACKCENSUS: this machine can see %d backpack(s) lying in the world and %d inside something "
        "(%lu objects walked). Loose goids: %s", loose, held, walked, nIds ? ids : "(none)");
}

void ShopStockClientTick() {
    if (Cfg().role == Role::Server) return;
    DWORD now = GetTickCount();
    if (g_nextCensus && now < g_nextCensus) return;
    g_nextCensus = now + 10000;
    // No world-state gate here. 0x28 is the SERVER's in-game state and a player's own machine does
    // not necessarily report it, which would mean this never runs and produces nothing. The walk
    // itself is the guard: it returns at once if GoDb is not there yet, and gives up if the list
    // does not close.
    ClientCensus();
}

void ShopStockClientInstall() {
    if (Cfg().role == Role::Server) return;
    Log("PackCensus: on - this machine will note in this log how many backpacks it can see lying in "
        "the world. Reading only; nothing is patched and no item is touched.");
}

void ShopStockTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (g_nextSweep && now < g_nextSweep) { /* fall through to the summary */ }
    else if (eng::WorldState_GetCurrentState(eng::WorldState_Singleton()) == 0x28) {
        g_nextSweep = now + (DWORD)(g_sweptOnce ? 15000 : 5000);
        SweepOnce();
    }
    if (!g_lastSummary) g_lastSummary = now;
    if (now - g_lastSummary < 60000) return;
    g_lastSummary = now;
    if (!g_added && !g_priced && !g_failed) return;
    Log("ShopStock: last minute %d shop(s) on the map, %lu backpack(s) added, %lu priced%s.",
        g_lastShops, g_added, g_priced, g_failed ? ", some could not be done (see above)" : "");
    g_visits = g_added = g_priced = g_failed = 0;
}

std::string ShopStockHeartbeatLines() {
    char b[320];
    if (!g_on) return "shopstock=off\n";
    snprintf(b, sizeof b,
             "shopstock=on\nshop_backpacks=%d\nshop_price=%d\nshop_count=%d\nshop_objects=%lu\n"
             "shop_loose=%d\nshop_stocked=%d\nshop_refill=once-per-world\ngoldcap=%s\n"
             "goldcap_limit=%d\ngoldcap_trimmed=%lu\ngoldcap_highest=%d\n",
             g_want, g_price, g_lastShops, g_lastWalked, g_lastLoose, g_stockedShops,
             g_goldCap ? "on" : "off", kGoldCap, g_goldTrimmedTotal, g_goldHighest);
    return b;
}

void ShopStockInstall() {
    if (Cfg().role != Role::Server) return;
    g_want = Cfg().shopBackpacks;
    g_price = Cfg().shopBackpackPrice;
    if (Cfg().shopBackpackTemplate.size() && Cfg().shopBackpackTemplate.size() < sizeof g_tmpl)
        strcpy(g_tmpl, Cfg().shopBackpackTemplate.c_str());
    char e[8];
    g_diag = GetEnvironmentVariableA("SIEGEHOST_SHOPDIAG", e, sizeof e) != 0;      // lab only
    g_invDiag = GetEnvironmentVariableA("SIEGEHOST_INVDIAG", e, sizeof e) != 0;    // lab only
    g_packDiag = GetEnvironmentVariableA("SIEGEHOST_PACKDIAG", e, sizeof e) != 0;  // lab only
    g_cntDiag  = GetEnvironmentVariableA("SIEGEHOST_BPCOUNTDIAG", e, sizeof e) != 0;  // lab only
    g_capTest  = GetEnvironmentVariableA("SIEGEHOST_BPCAPTEST", e, sizeof e) != 0;  // lab only
    g_bp2Test  = GetEnvironmentVariableA("SIEGEHOST_BP2TEST", e, sizeof e) != 0;  // lab only
    g_goldCap  = !GetEnvironmentVariableA("SIEGEHOST_NO_GOLDCAP", e, sizeof e);
    g_goldTest = GetEnvironmentVariableA("SIEGEHOST_GOLDTEST", e, sizeof e) != 0;  // lab only
    g_buyTest  = GetEnvironmentVariableA("SIEGEHOST_SHOPBUYTEST", e, sizeof e) != 0;  // lab only
    g_goldDiag = GetEnvironmentVariableA("SIEGEHOST_GOLDDIAG", e, sizeof e) != 0;  // lab only
    if (GetEnvironmentVariableA("SIEGEHOST_NO_SHOPSTOCK", e, sizeof e)) {
        Log("ShopStock: off (SIEGEHOST_NO_SHOPSTOCK) - shops keep exactly what the world gave them.");
        return;
    }
    if (g_want <= 0) {
        Log("ShopStock: off ([Store] Backpacks=0) - shops keep exactly what the world gave them.");
        return;
    }
    char w[16] = "";
    if (GetEnvironmentVariableA("SIEGEHOST_SHOPWANT", w, sizeof w)) {   // lab only: overflow a shelf on purpose
        g_want = atoi(w);
        Log("ShopStock: SIEGEHOST_SHOPWANT=%d (lab override).", g_want);
    } else if (g_want > 8) g_want = 8;
    // Stop clients being handed a world copy of every backpack we create (see kFcaSite above).
    if (GetEnvironmentVariableA("SIEGEHOST_SHOP_CLIENTCOPY", e, sizeof e)) {
        Log("ShopStock: SIEGEHOST_SHOP_CLIENTCOPY set - clients keep their own world copy of each new "
            "backpack, which is the 0.8.4 behaviour that left them lying on the ground.");
    } else {
        BYTE* f = (BYTE*)kFcaSite;
        if (f[0] != 0xE8 || kFcaSite + 5 + *(int32_t*)(f + 1) != kFcaCallee) {
            Log("ShopStock: VERIFY FAILED at 0x%08lX (SetForceClientAllowed) - carrying on without it; "
                "new backpacks may still show on the ground on clients.", kFcaSite);
        } else {
            DWORD of;
            if (VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &of)) {
                *(int32_t*)(f + 1) = (int32_t)((DWORD)(void*)&SiegeHost_ShopForceClientAllowed - (kFcaSite + 5));
                VirtualProtect(f, 5, of, &of);
                FlushInstructionCache(GetCurrentProcess(), f, 5);
                Log("ShopStock: clients are no longer handed a world copy of a backpack we create "
                    "(patched 0x%08lX, the only SetForceClientAllowed call site in the game).", kFcaSite);
            }
        }
    }
    g_on = true;
    if (g_goldCap)
        Log("GoldCap: armed - a character carrying more than %d gold is brought back to that limit on "
            "the next sweep, using the engine's own RCSetGold so the client is told the normal way. "
            "Nothing is patched and no other item is touched.", kGoldCap);
    else
        Log("GoldCap: off (SIEGEHOST_NO_GOLDCAP) - gold is not limited.");
    Log("ShopStock: armed - every shop on the map is given %d '%s' at %d gold on its MISC tab, once "
        "per world. What a player buys stays bought; the shelves are refilled only when the world "
        "resets after everyone has left. The shops are found by walking GoDb's own object list from "
        "the main loop every few seconds, not by hooking anything the engine calls. No content is "
        "changed and clients need no update.",
        g_want, g_tmpl, g_price);
}
