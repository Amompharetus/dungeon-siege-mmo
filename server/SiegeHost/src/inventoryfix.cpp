// inventoryfix.cpp
//
// Two things, both about backpacks, and after 0.8.7/0.8.8/0.8.9 the second one is deliberately as
// small as a change can be.
//
// ---------------------------------------------------------------------------------------------------
// 1. AN ITEM THAT DOES NOT FIT MUST NOT VANISH  (0.8.4, unchanged since, patch at 0x005758CD)
// ---------------------------------------------------------------------------------------------------
// If a player tries to pick up an item from the ground and their default inventory is full
// and they have a backpack in their inventory, pressing Z picked it up but the item vanished.
//
// The engine already has the backpack fallback. The tail of the inventory add helper (FUN_005756C2,
// which GoInventory::RSAdd and RSCreateAddItem both go through) reads:
//
//   005758CA  mov  ecx,[esi+4]                 ; the owner Go
//   005758CD  call 0x00539711                  ; Go::IsScreenPartyMember
//   005758D4  je   0x005759EC                  ; not a screen party member -> return, and that is all
//   ...        try each backpack in the owner's main grid, then GoMind::RSDrop at their feet
//
// Both the backpack fallback AND the drop-at-your-feet safety net sit behind
// Go::IsScreenPartyMember. On a machine running the game that is fine: your own characters are the
// screen party. On a dedicated game server they are not. Measured in the lab:
//
//   INVDIAG: goid 2C0000AA 'dsx_mp_farmboy' screenParty=0   <- the player who joined
//   INVDIAG: goid 15000093 'dsx_mp_farmboy' screenParty=1   <- the server's own ServerHost
//
// So the server resolves a pickup for a character it does not consider its own party, the add returns
// having done nothing, and the item is already out of the world. One call site is redirected, on the
// game server only, to answer yes for any character owned by a human player as well. Every other
// container - shopkeepers, chests, pack mules, monsters - keeps the engine's own answer.
//
// ---------------------------------------------------------------------------------------------------
// 2. THE ONE-BACKPACK CAP IS REMOVED  (0.9.0, patch at 0x00575A6D)
// ---------------------------------------------------------------------------------------------------
// Background: examining the engine's one-backpack cap.
//
// FUN_005759F6 is the engine's "may this item go into this inventory" test, and it is the ONLY place
// the cap is decided. GoInventory::SAutoGive, which is the Z pickup, calls it at 0x00578F13; the
// drag-and-click placement calls it through FUN_005779A5. One predicate, both paths:
//
//   00575A40  mov  eax,[esi+0x04]        ; the owning Go
//   00575A43  mov  ecx,[eax+0x48]        ; GoStore - a shopkeeper is exempt, which is how a shop
//   00575A48  je   00575A56              ;           holds four, and also why buying one never
//   00575A4A  call 005E5E80              ;           consulted the cap in the first place
//   00575A51  jne  00575A73
//   00575A56  mov  ecx,edi
//   00575A58  call 005397AD              ; Go::IsBackpack(item)
//   00575A5F  je   00575A73              ; not a backpack -> carry on
//   00575A61  mov  eax,[esi+0x88]        ; m_backpackCount, a running total each machine keeps
//   00575A67  sub  eax,[ebp+0x14]        ;                  separately, and which drifts upward
//   00575A6A  cmp  eax,0x1
//   00575A6D  jge  00575B41              ; <- refuse.  SIX BYTES, replaced with NOPs.
//   00575A73  mov  ecx,[ebp+0x08]        ; the fall-through the "not a backpack" case already takes
//
// Six bytes of 0x90 at 0x00575A6D. Nothing is called, nothing is written, no list is walked, and the
// only behaviour that can change is that one refusal no longer happens - falling through to exactly
// where the engine already goes for every item that is not a backpack. Armed on the game server AND
// on a player's own machine, because each of them evaluates this predicate for its own half of the
// game.
//
// Because nothing reads m_backpackCount for a decision any more, the fact that it drifts stops
// mattering. That drift is what made left click fail after a shop visit while Z still worked.
//
// WHAT WAS REMOVED HERE AND WHY, so nobody puts it back:
//   * 0.8.7's one-per-character rule at 0x005756E8. It duplicated a rule the engine already had and
//     read the same drifting counter, so it made Z start failing too.
//   * 0.8.8's three-second resync timer. It walked the Server player vector from DLL load, which is
//     minutes before that vector is a vector, and hung NA hard enough to trip the watchdog.
//   * 0.8.9's redirect of the Go::IsBackpack call at 0x00575A58. That call is made on the placement
//     test for EVERY item, not only backpacks, so the code ran on rings and swords - paths it was
//     never meant for and never tested on.
//
// The rule that survives untouched, because it is the engine's own and nobody asked for it to go: a
// backpack still cannot be put inside a backpack (0x00575A25), and a pack animal still cannot carry
// one (0x00575A7F).
#include "siegehost.h"
#include "engine.h"
#include <stdint.h>
#include <stdio.h>

namespace {

const DWORD kSite = 0x005758CD;                 // "call Go::IsScreenPartyMember" inside the add helper
const DWORD kCallee = 0x00539711;               // Go::IsScreenPartyMember
const auto IsScreenPartyMember = (bool(__thiscall*)(void* go))0x00539711;

const int kGoid = 0x4C, kGoOwner = 0x78;

// GoInventory+0x86: this container IS a backpack. +0x88: m_backpackCount, the engine's running
// total. Go+0x2C: an item's own GoInventory, which is how the engine itself tells a backpack from
// anything else (0x00575A34 and again at 0x00575B34).
const int kInvIsBackpack = 0x86, kInvBackpackCount = 0x88, kGoInv = 0x2C;
// GoInventory+0x68 is the head of the engine's own list of what this character carries, read off
// FUN_0057BE76: node+0x0C is the Go*, node+0x10 the inventory location. 0x0057E120 advances the
// iterator and must be called rather than reimplemented, because it steps over tombstoned nodes
// against the sentinel at 0x007AFC84. Checked instruction by instruction: it reads [ecx] and writes
// [ecx] and touches nothing else, so a single-word iterator is right.
const int kInvItemList = 0x68, kNodeGo = 0x0C;
const auto ListAdvance = (void(__fastcall*)(void** it))0x0057E120;

// THE THREE ONE-BACKPACK CAPS
//
// 0.9.0 removed one and assumed it was the only one, because it was the only one the placement
// predicate consulted. It was not. Searching the whole exe for functions that mention both
// Go::IsBackpack and GoInventory+0x88 turns up three real gates. The other two live in the user
// interface, on a player's own machine, which is why the server looked perfectly healthy and why
// picking a backpack off the ground worked while putting one into a grid slot did not.
//
// [1] 0x00575A6D, in FUN_005759F6, the engine's "may this item go in here" predicate. Both the Z
//     pickup (GoInventory::SAutoGive calls it at 0x00578F13) and the ground left-click reach it.
//         00575A61  mov eax,[esi+0x88]      ; m_backpackCount
//         00575A67  sub eax,[ebp+0x14]
//         00575A6A  cmp eax,1
//         00575A6D  jge 00575B41            ; refuse   -> six NOPs, falls to 00575A73
//
// [2] 0x005F3CE6, in the item-shop and drag handler. This is the one that refuses a backpack you
//     have just bought.
//         005F3CBB  call 005397AD           ; Go::IsBackpack(item)
//         005F3CC2  je   005F3CEC           ; not a backpack -> carry on
//         005F3CCD  cmp byte [eax+0x1C],0   ; pack animal - LEFT ALONE
//         005F3CD1  jne  005F3F0F
//         005F3CDA  mov ecx,[ecx+0x34]      ; backpacks in this drag
//         005F3CDD  add ecx,[eax+0x88]      ; + m_backpackCount
//         005F3CE3  cmp ecx,1
//         005F3CE6  jge  005F3F0F           ; refuse   -> six NOPs, falls to 005F3CEC, exactly
//                                           ;            where a non-backpack already goes
//
// [3] FUN_004FEF07, the inventory drag-and-drop handler, which has TWO gates. Here the refusal is
//     the fall-through and the jump is the allow, so these become unconditional jumps rather than
//     NOPs. This is the pair that let a character shuffle their single backpack around but refused
//     the moment they had two.
//         004FF1EB  call 00573731           ; Contains(inv, item) - is it already in here
//         004FF1FA  cmp dword [eax+0x88],1
//         004FF201  jle 004FF346            ; <=1 -> allowed     -> make unconditional
//         004FF271  cmp dword [eax+0x88],1
//         004FF278  jl  004FF346            ; <1  -> allowed     -> make unconditional
//
// Every one is a six-byte edit verified byte for byte first, writing nothing but the instruction
// itself, and every one lands on a target the engine already uses for the allowed case.
// ONE BACKPACK PER CHARACTER, DONE BY FIXING THE NUMBER RATHER THAN THE RULE  (1.0.2)
//
// Legends of Aranna restricts backpacks to 1 per person.
//
// The engine's rule is correct and there is no reason to touch it. What was broken is the number the
// rule reads. GoInventory+0x88 (m_backpackCount) is a running total kept by whichever machine did
// the add or the removal: buying puts it up, throwing one on the ground does not reliably bring it
// down, and a player's machine and the server keep separate copies that drift apart. Once a copy is
// above zero, that machine refuses every backpack placement it is asked about. That is the whole
// bug, and every earlier attempt aimed at the wrong half of it.
//
// So the three caps stay exactly as the engine wrote them, and instead the count is recomputed from
// what the character is actually carrying at the moment each cap is about to read it:
//
//   [1] 00575A58  call Go::IsBackpack    inside FUN_005759F6, the placement test
//                 00575A61  mov eax,[esi+0x88]        <- esi is the inventory
//   [2] 005F3CBB  call Go::IsBackpack    inside the item shop and drag handler
//                 005F3CDD  add ecx,[eax+0x88]        <- eax from [[ebp-4]+0xC]+0x2C
//   [3] 004FF1EB  call GoInventory::Contains  inside FUN_004FEF07, the inventory drop handler
//                 004FF1FA  cmp [eax+0x88],1          <- both gates, eax = ecx at the call
//                 004FF271  cmp [eax+0x88],1
//
// Each stub asks Go::IsBackpack first and only recomputes when the item really is a backpack, then
// tail-calls the engine's own function with its arguments untouched. That is the difference from
// 0.8.9, which hooked the same call site but did its work unconditionally, so it ran on rings and
// swords too.
//
// The walk uses the engine's own list and its own iterator. 0057E120 was checked instruction by
// instruction this time: it reads [ecx] and writes [ecx] and nothing else, so a single-word iterator
// is correct. It is bounded, reads only, and gives up returning -1 on anything that does not look
// like an inventory, in which case the engine's own number is left alone.
struct CountSite {
    DWORD addr;
    DWORD callee;
    const char* what;
    void (*stub)();
};

const DWORD kIsBackpackFn = 0x005397AD;
const DWORD kContainsFn   = 0x00573731;

const auto TemplateName = (const char*(__thiscall*)(void* go))0x005390A3;

bool g_on = false, g_diag = false, g_countFix = false;
DWORD g_fixed = 0, g_fixedTotal = 0;
DWORD g_engineYes = 0, g_ourYes = 0, g_no = 0, g_lastSummary = 0, g_ourTotal = 0;

}  // namespace

// Called in place of Go::IsScreenPartyMember, thiscall with the Go in ECX and no stack arguments,
// which is __fastcall with one argument. Reads three fields and calls the original. Nothing here can
// allocate, block or re-enter the engine.
// 1.0.3: WorldIsInPlay and memory validation guards.
//
// While a character is being restored the engine puts each saved item back through the very
// placement predicate we hook, so walking that character's item list while the engine was
// still building it would encounter a half-linked list.
//
// Two guards prevent this:
//   1. Only recount while the world is actually being played (WorldIsInPlay). During a load the
//      engine is rebuilding the count itself and does not need external recounting.
//   2. Never dereference a node without checking the memory is really committed and readable first.
inline bool WorldIsInPlay() {
    void* ws = eng::WorldState_Singleton();
    if (!ws) return false;
    int st = eng::WorldState_GetCurrentState(ws);
    return st == 0x28 || st == 0x29;          // ws_mp_ingame, ws_mp_ingame_jip
}
static bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}
DWORD g_skippedLoading = 0;
bool g_bpTrace = false; DWORD g_bpTraceLines = 0;
bool g_bpNoop = false;   // lab only: stubs installed, body does absolutely nothing

int LiveBackpackCount(void* inv) {
    if (!Readable(inv, kInvItemList + 4)) return -1;
    void* head = *(void**)((char*)inv + kInvItemList);
    if (!Readable(head, 4)) return -1;
    void* it = *(void**)head;
    if (!Readable(it, kNodeGo + 4)) return -1;
    int n = 0;
    for (int guard = 0; it != head; ++guard) {
        if (guard > 2048) return -1;
        if (!Readable(it, kNodeGo + 4)) return -1;
        void* item = *(void**)((char*)it + kNodeGo);
        if (Readable(item, kGoInv + 4)) {
            void* gui = *(void**)((char*)item + 0x28);
            bool zeroSize = (Readable(gui, 0x20) && *(int*)((char*)gui + 0x18) == 0 && *(int*)((char*)gui + 0x1C) == 0);
            void* iinv = *(void**)((char*)item + kGoInv);
            if (!zeroSize && Readable(iinv, kInvIsBackpack + 1) && *(char*)((char*)iinv + kInvIsBackpack)) ++n;
        }
        ListAdvance(&it);
    }
    return n;
}

// Called from the stubs below, only ever for a backpack, only ever just before a cap reads the count.
int InvFixLiveBackpacks(void* inv) { return LiveBackpackCount(inv); }
// Lab only. Lets a probe build the situation a 0.9.3 character is in - already carrying more than
// one backpack - by getting the repair out of the way for two add calls.
bool InvFixSetCountFix(bool on) { bool was = g_countFix; g_countFix = on; return was; }

extern "C" void __cdecl SiegeHost_FixBackpackCount(void* inv) {
    if (g_bpNoop) return;            // lab only: proves whether the stub itself is safe
    if (!g_countFix || !inv) return;
    if (g_bpTrace && g_bpTraceLines < 300) {
        ++g_bpTraceLines;
        void* ws = eng::WorldState_Singleton();
        Log("BPTRACE %lu: enter inv=%p worldstate=0x%02X", g_bpTraceLines, inv,
            ws ? eng::WorldState_GetCurrentState(ws) : -1);
    }
    // Not while the world is loading. That is when a saved character's items are put back, one at a
    // time, through this very predicate, and the list we would walk is half built.
    if (!WorldIsInPlay()) { ++g_skippedLoading; return; }
    if (g_bpTrace && g_bpTraceLines < 300)
        Log("BPTRACE %lu: in play, recounting", g_bpTraceLines);
    int* counter = (int*)((char*)inv + kInvBackpackCount);
    int real = LiveBackpackCount(inv);
    if (real < 0 || *counter == real) return;
    if (g_diag)
        Log("BACKPACKCOUNT: this inventory said %d, actually carrying %d - corrected before the "
            "one-backpack rule read it.", *counter, real);
    *counter = real;
    ++g_fixed; ++g_fixedTotal;
}

extern "C" void* g_isBackpackOrig = (void*)kIsBackpackFn;
extern "C" void* g_containsOrig   = (void*)kContainsFn;
extern "C" void SiegeHost_BpCountPlacement();
extern "C" void SiegeHost_BpCountShop();
extern "C" void SiegeHost_BpCountDrop();

// [1] placement test: ECX is the item, ESI is the inventory.
__asm__(
    ".globl _SiegeHost_BpCountPlacement\n"
    "_SiegeHost_BpCountPlacement:\n"
    "  pushl %edx\n"
    "  pushl %esi\n"
    "  pushl %ecx\n"
    "  call *_g_isBackpackOrig\n"
    "  movzbl %al, %eax\n"
    "  popl %ecx\n"
    "  popl %esi\n"
    "  testl %eax, %eax\n"
    "  jz 1f\n"
    "  pushl %eax\n"
    "  pushal\n"
    "  pushl %esi\n"
    "  call _SiegeHost_FixBackpackCount\n"
    "  addl $4, %esp\n"
    "  popal\n"
    "  popl %eax\n"
    "1:\n"
    "  popl %edx\n"
    "  ret\n");

// [2] item shop and drag: ECX is the item; the inventory is [[ebp-4]+0xC]+0x2C, as the engine itself
// computes it three instructions later.
__asm__(
    ".globl _SiegeHost_BpCountShop\n"
    "_SiegeHost_BpCountShop:\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  call *_g_isBackpackOrig\n"
    "  movzbl %al, %eax\n"
    "  popl %ecx\n"
    "  testl %eax, %eax\n"
    "  jz 1f\n"
    "  pushl %eax\n"
    "  movl -4(%ebp), %edx\n"
    "  movl 12(%edx), %edx\n"
    "  movl 44(%edx), %edx\n"
    "  testl %edx, %edx\n"
    "  jz 2f\n"
    "  pushal\n"
    "  pushl %edx\n"
    "  call _SiegeHost_FixBackpackCount\n"
    "  addl $4, %esp\n"
    "  popal\n"
    "2:\n"
    "  popl %eax\n"
    "1:\n"
    "  popl %edx\n"
    "  ret\n");

// [3] inventory drop: ECX is the inventory and the item is the pushed argument, which the real
// Contains still needs, so it is left on the stack and this tail-calls through.
__asm__(
    ".globl _SiegeHost_BpCountDrop\n"
    "_SiegeHost_BpCountDrop:\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  movl 12(%esp), %eax\n"
    "  movl %eax, %ecx\n"
    "  testl %ecx, %ecx\n"
    "  jz 1f\n"
    "  call *_g_isBackpackOrig\n"
    "  movzbl %al, %eax\n"
    "  jmp 2f\n"
    "1:\n"
    "  xorl %eax, %eax\n"
    "2:\n"
    "  popl %ecx\n"
    "  testl %eax, %eax\n"
    "  jz 3f\n"
    "  pushal\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_FixBackpackCount\n"
    "  addl $4, %esp\n"
    "  popal\n"
    "3:\n"
    "  popl %edx\n"
    "  jmp *_g_containsOrig\n");

const CountSite kSites[] = {
    { 0x00575A58, kIsBackpackFn, "pickup and placement", &SiegeHost_BpCountPlacement },
    { 0x005F3CBB, kIsBackpackFn, "item shop and drag",   &SiegeHost_BpCountShop },
    { 0x004FF1EB, kContainsFn,   "inventory drop",       &SiegeHost_BpCountDrop },
};
const int kSiteCount = 3;

int InstallCountFix() {
    int done = 0;
    for (int i = 0; i < kSiteCount; ++i) {
        BYTE* p = (BYTE*)kSites[i].addr;
        if (p[0] != 0xE8 || kSites[i].addr + 5 + *(int32_t*)(p + 1) != kSites[i].callee) {
            Log("InvFix: VERIFY FAILED at 0x%08lX (backpack count: %s) - the one-backpack rule there "
                "will read the engine's own running total, which can drift.",
                kSites[i].addr, kSites[i].what);
            continue;
        }
        DWORD o;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) continue;
        *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)kSites[i].stub - (kSites[i].addr + 5));
        VirtualProtect(p, 5, o, &o);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        ++done;
    }
    g_countFix = (done == kSiteCount);
    return done;
}

extern "C" bool __fastcall SiegeHost_InvOwnerMayUseBackpacks(void* go) {
    if (!go) return false;
    if (IsScreenPartyMember(go)) { ++g_engineYes; return true; }
    void* owner = *(void**)((char*)go + kGoOwner);       // Go+0x78 is the owning Player
    if (!owner) { ++g_no; return false; }
    if (*(int*)((char*)owner + 0x14) != 1) { ++g_no; return false; }   // 1 = human controller
    if (!*(void**)go) { ++g_no; return false; }                        // must be a character
    ++g_ourYes; ++g_ourTotal;
    if (g_diag)
        Log("INVFIX: goid %08lX '%s' is a player's character; letting the item fall through to a "
            "backpack instead of vanishing.", *(DWORD*)((char*)go + kGoid), TemplateName(go));
    return true;
}

// 4. GROUND ITEM AND GOLD PERSISTENCE ACROSS LOGOUT / DISCONNECT
// ---------------------------------------------------------------------------------------------------
// When a player disconnects, Player::~Player (0x005692E8) calls GoDb::PurgePlayerObjects (0x0054C749),
// which iterates dynamic objects and calls GoDb::SMarkForDeletion (0x0054C1C9) on anything where
// go->m_Player == disconnectingPlayer.
//
// Because dropped gold (GoMind::RSDropGold) and dropped items retained the player as owner,
// PurgePlayerObjects destroyed them upon logout - so alts or joining friends could not pick them up.
//
// We hook GoDb::PurgePlayerObjects to set a purge flag, and hook GoDb::SMarkForDeletion to intercept
// items and gold lying on the ground (no actor, no parent, has placement, has aspect, is gold or item).
// Instead of deleting them, we reassign their owner to Server::GetComputerPlayer, preserving them
// in the world as long as the server remains running.

extern "C" void __fastcall Orig_GoDb_PurgePlayerObjects(void* godb, void* unused_edx, void* playerId, int flags);
__asm__(
    ".text\n"
    ".globl _Orig_GoDb_PurgePlayerObjects\n"
    ".globl @Orig_GoDb_PurgePlayerObjects@16\n"
    "_Orig_GoDb_PurgePlayerObjects:\n"
    "@Orig_GoDb_PurgePlayerObjects@16:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x98, %esp\n"
    "  movl $0x0054C752, %eax\n"
    "  jmp *%eax\n"
);

extern "C" void __fastcall Orig_GoDb_SMarkForDeletion(void* godb, void* unused_edx, void* go, int p3, int p4, int p5);
__asm__(
    ".text\n"
    ".globl _Orig_GoDb_SMarkForDeletion\n"
    ".globl @Orig_GoDb_SMarkForDeletion@24\n"
    "_Orig_GoDb_SMarkForDeletion:\n"
    "@Orig_GoDb_SMarkForDeletion@24:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x0c, %esp\n"
    "  movl $0x0054C1CF, %eax\n"
    "  jmp *%eax\n"
);

const DWORD kPurgePlayerObjectsVa = 0x0054C749;
const DWORD kSMarkForDeletionVa   = 0x0054C1C9;

volatile int g_inPurgePlayerObjects = 0;
DWORD g_groundRescued = 0;
DWORD g_groundRescuedTotal = 0;

static void* GetComputerPlayer() {
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x20)) return nullptr;
    typedef void* (__thiscall* GetCompPlayerFn)(void* server);
    return ((GetCompPlayerFn)0x0047272F)(srv);
}

static void SetGoPlayer(void* go, void* player) {
    if (!go || !Readable(go, 0x80)) return;
    if (player) {
        typedef void (__thiscall* SetPlayerFn)(void* go, void* player);
        ((SetPlayerFn)0x0053904E)(go, player);
    } else {
        *(void**)((char*)go + 0x78) = nullptr;
    }
}

static bool IsGroundItemOrGold(void* go) {
    if (!go || !Readable(go, 0x98)) return false;

    // Must NOT have an Actor (characters, monsters, summons have Actor at +0x00)
    if (*(void**)go != nullptr) return false;

    // Must NOT have a Parent (items inside inventory/chest have Parent at +0x6C)
    if (*(void**)((char*)go + 0x6C) != nullptr) return false;

    // Must NOT be marked as inside an inventory (Go::IsInsideInventory bit at +0x97 bit 6)
    if (((*(unsigned char*)((char*)go + 0x97) >> 6) & 1) != 0) return false;

    // Must have Placement (placed in world at +0x40)
    if (*(void**)((char*)go + 0x40) == nullptr) return false;

    // Must have Aspect (at +0x04)
    if (*(void**)((char*)go + 0x04) == nullptr) return false;

    // Must be either gold (+0x24) or item (+0x28 Gui or +0x10 Common, with no party +0x38)
    bool isGold = (*(void**)((char*)go + 0x24) != nullptr);
    bool isItem = (*(void**)((char*)go + 0x38) == nullptr) &&
                  ((*(void**)((char*)go + 0x28) != nullptr) || (*(void**)((char*)go + 0x10) != nullptr));

    return isGold || isItem;
}

extern "C" void __fastcall Hook_GoDb_PurgePlayerObjects(void* godb, void* unused_edx, void* playerId, int flags) {
    ++g_inPurgePlayerObjects;
    Orig_GoDb_PurgePlayerObjects(godb, unused_edx, playerId, flags);
    --g_inPurgePlayerObjects;
}

extern "C" void __fastcall Hook_GoDb_SMarkForDeletion(void* godb, void* unused_edx, void* go, int p3, int p4, int p5) {
    if (g_inPurgePlayerObjects > 0 && IsGroundItemOrGold(go)) {
        void* compPlayer = GetComputerPlayer();
        SetGoPlayer(go, compPlayer);
        ++g_groundRescued;
        ++g_groundRescuedTotal;
        ++g_ourTotal;
        const char* name = TemplateName ? TemplateName(go) : "unknown";
        DWORD goid = *(DWORD*)((char*)go + 0x4C);
        Log("InvFix: preserved ground %s '%s' (%08lX) from deletion on player logout (reassigned to ComputerPlayer)",
            (*(void**)((char*)go + 0x24) != nullptr) ? "gold" : "item", name, goid);
        return;
    }
    Orig_GoDb_SMarkForDeletion(godb, unused_edx, go, p3, p4, p5);
}


void InvFixTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (!g_lastSummary) g_lastSummary = now;
    if (now - g_lastSummary < 60000) return;
    g_lastSummary = now;
    if (g_ourYes) {
        Log("InvFix: last minute %lu time(s) a player's character was about to lose an item that did not "
            "fit, and was sent to their backpack or their feet instead (%lu total since this server "
            "started).", g_ourYes, g_ourTotal);
        g_engineYes = g_ourYes = g_no = 0;
    }
    if (g_groundRescued) {
        Log("InvFix: last minute %lu dropped item(s)/gold rescued from deletion on player logout (%lu total).",
            g_groundRescued, g_groundRescuedTotal);
        g_groundRescued = 0;
    }
}

std::string InvFixHeartbeatLines() {
    if (!g_on) return std::string("invfix=off\nbackpackcap=on\nbackpackcount_fixed=0\n");
    char b[128];
    snprintf(b, sizeof b, "invfix=on\ninvfix_rescued=%lu\nground_rescued=%lu\nbackpackcap=on\n"
             "backpackcount_sites=%d\nbackpackcount_fixed=%lu\n",
             g_ourTotal, g_groundRescuedTotal, g_countFix ? kSiteCount : 0, g_fixedTotal);
    return b;
}

void InvFixInstall() {
    char e[8];
    g_diag = GetEnvironmentVariableA("SIEGEHOST_INVDIAG", e, sizeof e) != 0;
    g_bpTrace = GetEnvironmentVariableA("SIEGEHOST_BPTRACE", e, sizeof e) != 0;  // lab only
    g_bpNoop  = GetEnvironmentVariableA("SIEGEHOST_BPNOOP", e, sizeof e) != 0;   // lab only
    bool capOff = !GetEnvironmentVariableA("SIEGEHOST_NO_BPCOUNTFIX", e, sizeof e);

    // A player's own machine decides left-click placement; the server decides Z. Both run the same
    // predicate, so the cap has to come off in both. Nothing else in this file applies to a client.
    if (Cfg().role != Role::Server) {
        if (capOff) {
            int n = InstallCountFix();
            Log("InvFix: one backpack per character - the game's own rule is left exactly as it is, "
                "and the number it reads is recomputed from what you are really carrying at %d of %d "
                "places on this machine, so it cannot drift after a shop visit.", n, kSiteCount);
        }
        return;
    }

    if (capOff) {
        int n = InstallCountFix();
        Log("InvFix: one backpack per character - the game's own rule is untouched; the number it "
            "reads is recomputed from what the character really carries at %d of %d places.",
            n, kSiteCount);
    }

    // Ground item and gold persistence across player logout / disconnect (server only)
    if (!GetEnvironmentVariableA("SIEGEHOST_NO_GROUND_PERSIST", e, sizeof e)) {
        // 1. Hook GoDb::PurgePlayerObjects at 0x0054C749 (9 bytes: 55 8B EC 81 EC 98 00 00 00)
        const BYTE kPurgeExpect[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00 };
        BYTE* pPurge = (BYTE*)kPurgePlayerObjectsVa;
        if (memcmp(pPurge, kPurgeExpect, 9) == 0) {
            DWORD o;
            if (VirtualProtect(pPurge, 9, PAGE_EXECUTE_READWRITE, &o)) {
                pPurge[0] = 0xE9;
                *(int32_t*)(pPurge + 1) = (int32_t)((DWORD)(void*)&Hook_GoDb_PurgePlayerObjects - (kPurgePlayerObjectsVa + 5));
                for (int i = 5; i < 9; ++i) pPurge[i] = 0x90;
                VirtualProtect(pPurge, 9, o, &o);
                FlushInstructionCache(GetCurrentProcess(), pPurge, 9);
                g_on = true;
                Log("InvFix: GoDb::PurgePlayerObjects hooked at 0x%08lX (tracking player purge)", kPurgePlayerObjectsVa);
            }
        } else {
            Log("InvFix: VERIFY FAILED at 0x%08lX for GoDb::PurgePlayerObjects", kPurgePlayerObjectsVa);
        }

        // 2. Hook GoDb::SMarkForDeletion at 0x0054C1C9 (6 bytes: 55 8B EC 83 EC 0C)
        const BYTE kSMarkExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };
        BYTE* pSMark = (BYTE*)kSMarkForDeletionVa;
        if (memcmp(pSMark, kSMarkExpect, 6) == 0) {
            DWORD o;
            if (VirtualProtect(pSMark, 6, PAGE_EXECUTE_READWRITE, &o)) {
                pSMark[0] = 0xE9;
                *(int32_t*)(pSMark + 1) = (int32_t)((DWORD)(void*)&Hook_GoDb_SMarkForDeletion - (kSMarkForDeletionVa + 5));
                pSMark[5] = 0x90;
                VirtualProtect(pSMark, 6, o, &o);
                FlushInstructionCache(GetCurrentProcess(), pSMark, 6);
                g_on = true;
                Log("InvFix: GoDb::SMarkForDeletion hooked at 0x%08lX (ground items/gold persist across logout)", kSMarkForDeletionVa);
            }
        } else {
            Log("InvFix: VERIFY FAILED at 0x%08lX for GoDb::SMarkForDeletion", kSMarkForDeletionVa);
        }
    }

    if (GetEnvironmentVariableA("SIEGEHOST_NO_INVFIX", e, sizeof e)) {
        Log("InvFix: off (SIEGEHOST_NO_INVFIX) - an item that does not fit is lost, as in 0.8.3.");
        return;
    }
    if (!Cfg().invFix) {
        Log("InvFix: off ([Store] BackpackPickup=0) - an item that does not fit is lost, as in 0.8.3.");
        return;
    }
    BYTE* p = (BYTE*)kSite;
    if (p[0] != 0xE8 || kSite + 5 + *(int32_t*)(p + 1) != kCallee) {
        Log("InvFix: VERIFY FAILED at 0x%08lX - feature off, nothing patched.", kSite);
        return;
    }
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
        Log("InvFix: VirtualProtect failed (%lu) - feature off.", GetLastError());
        return;
    }
    *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&SiegeHost_InvOwnerMayUseBackpacks - (kSite + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    g_on = true;
    Log("InvFix: armed - an item that will not fit in a player's pack now goes into their backpack, "
        "and if that is full too it drops at their feet, instead of disappearing (patched 0x%08lX, "
        "one call site). On a dedicated server the engine skipped both because a joined player's "
        "character is not the server's own screen party.", kSite);
}
