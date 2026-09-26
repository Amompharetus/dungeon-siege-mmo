// areascale.cpp - 0.8.0, game server only: monsters scale with the players in THEIR area, not everyone online.
//
// How the game scales (LoA v1.50): when a computer-controlled actor is built FROM A TEMPLATE,
// the GoAspect template constructor 0x005BCF59 calls 0x005BE682, which multiplies the actor's max life and XP
// value by world/global/formula/formulas.gas [mp_scaling] (life .35 .5 .8 1 2.15 3.1 4.17 5, xp .35 .59 .8 1
// 1.25 1.5 1.75 2, by player count 1..8, 9+ = 8). The count is the Go's own Go+0x5C, read by 0x005390B5 at
// 0x005BE6BE; it was stamped from the global human count [0x007AFA30] when the Go was built, and the server
// sends each Go's count to clients with it, so every PC uses the server's number.
//
// Change 1 (0.7.6, placed monsters): the call at 0x005BE6BE comes here. For the new monster we find which
// players' world frustums contain its node (node+0x108 frustum mask, Player+0x18 its FrustumId, Go+0x68 as a
// fallback) - the players it is being loaded for. Their area groups (Player+0xF4 through the table in
// src/areagroups.h, shared with the portrait panel so the two can never disagree) are the monster's area;
// the count is the number of real
// players in those groups. The result is written to Go+0x5C, so clients get the same count. If no real player's
// frustum contains the monster (e.g. ServerHost's own area at world start) the stock count is kept.
// [AutoHost] AreaScaling=0 turns it off. 0.7.8: ServerHost is not counted; [AutoHost] CountServerHost=1
// restores the stock +1 without a rebuild.
//
// Change 2 (0.8.0, generated monsters - RE-15 revised). A monster from a generator, spawner or spell IS
// scaled at creation - generator_advanced_a2.skrit does MakeGoCloneReq(owner.Goid, child_template_name$) then
// GoDb.SCloneGo, and the clone's aspect goes through 0x005BE682 like any other. The trouble is WHEN: the Go is
// built before it is put on the ground, so at that moment it has no terrain node, no frustum mask and no
// Prevents spawned monsters from defaulting to full-server health scaling.
//
// 0.8.0 corrects it one frame later, once the monster is standing somewhere:
//   * the clone call site (0x0054A876, GoDb's server clone helper) goes through a stub that notes two things
//     and nothing else: the goid GoDb has already allocated for the new Go (its caller keeps it at [ebp+8],
//     written at 0x0054A82B before the constructor runs) and the Go pointer. No stack-lifetime object is kept,
//     which is the whole lesson of and why 0.7.9's version of this hook could freeze the game.
//   * AreaScaleTick, on the game's own main loop, looks the monster up by goid through GoDb::FindGo - a Go that
//     has gone away is simply dropped, and a dead pointer is never dereferenced - works out the area count C
//     for where it now stands, and rescales it from the count S it was made with (Go+0x5C, which is by
//     definition the count its life was multiplied by): life and XP x table[C] / table[S], using the game's own
//     tables at 0x005B6DD9 and 0x005B6E02 so there is no copy of [mp_scaling] to keep in step. The writes go
//     through GoAspect::SSetMaxLife / SSetNaturalMaxLife / SSetCurrentLife, the server's replicating setters,
//     so every client's health bar shows the corrected number.
//   * only creatures are touched (Go::IsActor plus 0x005BE682's own three tests), so shop stock, dropped gold
//     and spell effects are left exactly as the game made them.
// Go+0x5C is then set to C, which keeps the invariant the whole file relies on: a Go's +0x5C is always the
// count its life was scaled by, so cloning a clone stays correct.

#include "siegehost.h"
#include "engine.h"
#include "areagroups.h"
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>

// ---- The clone stub --------------------------------------------------------------------------------------
// Two call sites build a cloned Go (0x0053DA5B), both byte-checked, both with the same argument shape
// (arg0 = the Go being cloned from, arg1 = its owning Player, arg2 = a flag; ecx = the new Go):
//   GoDb clone helper  0x0054A7BA, call at 0x0054A876 - the SERVER clone path, and the one a generator,
//        spawner or spell reaches. Its caller frame also holds the new Go's goid at [ebp+8], put there by
//        GoDb 0x00552C50 before the constructor runs (0x0054A82B).
//   GoDb::CloneLocalGo 0x0054B29A, call at 0x0054B343 - the LOCAL clone path (character creation, gold
//        splitting, explosions, UI). It has NO goid at [ebp+8] - that slot is its GoCloneReq - so this site is
//        off by default and only the lab switch turns it on.
extern "C" void* g_ctorTail = (void*)0x0053DA5B;
extern "C" void __cdecl SiegeHost_CloneCtorPre(DWORD goid, void* srcGo, void* newGo);
extern "C" void SiegeHost_CloneCtor();
__asm__(
    ".globl _SiegeHost_CloneCtor\n"
    "_SiegeHost_CloneCtor:\n"
    "  pushal\n"
    "  movl 0x8(%ebp), %eax\n"      // the goid GoDb allocated for the new Go
    "  movl 0x24(%esp), %edx\n"     // constructor arg0: the Go this one is cloned from
    "  pushl %ecx\n"                // ecx: the new Go
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  call _SiegeHost_CloneCtorPre\n"
    "  addl $12, %esp\n"
    "  popal\n"
    "  jmp *_g_ctorTail\n");

namespace {
const DWORD kSite = 0x005BE6BE, kStockVa = 0x005390B5;
const DWORD kCtorSiteA = 0x0054B343, kCtorSiteB = 0x0054A876, kCtorVa = 0x0053DA5B;

const auto StockCount   = (int(__fastcall*)(void* go))0x005390B5;
const auto NodeLookup   = (void*(__thiscall*)(void* engine, const void* guid))0x006711C9;
const auto TemplateName = (const char*(__thiscall*)(void* go))0x005390A3;
// GoDb::FindGo: goid -> Go*, or null. Safe from the main loop; NOT safe from inside a constructor.
const auto FindGo       = (void*(__thiscall*)(void* godb, DWORD goid))0x00550A6B;
// The two [mp_scaling] tables the stock scaling itself uses, on the formulas singleton [0x007AD0F4].
// Both clamp the index into the table, so any count is safe.
const auto LifeMul      = (float(__thiscall*)(void* formulas, int count))0x005B6DD9;
const auto XpMul        = (float(__thiscall*)(void* formulas, int count))0x005B6E02;
// GoAspect life setters. The S- forms are the server's: they replicate the new value to every client.
const auto SSetMaxLife        = (void(__thiscall*)(void* aspect, float v))0x00534A0E;
const auto SSetNaturalMaxLife = (void(__thiscall*)(void* aspect, float v))0x00534B1A;
const auto SSetCurrentLife    = (void(__thiscall*)(void* aspect, float v))0x005348D7;

bool g_on = false, g_diag = false;
DWORD g_changed = 0, g_kept = 0, g_same = 0, g_clones = 0, g_cloneSkip = 0, g_lastSummary = 0;
bool g_countHost = false;
wchar_t g_host[64] = L"ServerHost";

bool IsRealPlayer(void* pl) {
    if (!eng::Player_IsValid(pl)) return false;
    if (*(int*)((char*)pl + 0x14) != 1) return false;  // human controller, same test as the global count
    const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
    if (!w || !*w) return false;                        // no hero (still on the character screen)
    return wcscmp(w, g_host) != 0;                      // not the server's own ServerHost
}
unsigned Group(void* pl) { return areagroups::ForLocation(*(unsigned*)((char*)pl + 0xF4)); }

// Which world frustums cover this Go: the terrain node it stands on, plus its own membership.
DWORD MaskOf(void* go, void** engine_out = nullptr) {
    if (!go) return 0;
    void* engine = *(void**)0x007ACF80;
    void* gp = *(void**)((char*)go + 0x40);
    DWORD mask = *(DWORD*)((char*)go + 0x68);
    if (engine && gp && *(DWORD*)((char*)gp + 0x20)) {
        void* node = NodeLookup(engine, (char*)gp + 0x20);
        if (node) mask |= *(DWORD*)((char*)node + 0x108);
    }
    if (engine_out) *engine_out = engine;
    return mask;
}

// How many real players are in the area covered by this frustum mask: the players the Go is being loaded for,
// plus everyone else in their map location groups. 0 means nobody covers it at all, and the caller keeps
// whatever count the Go already had. groupsOut, when given, is a 96-byte buffer for the diagnostic list.
int AreaCount(DWORD mask, int* coveringOut = nullptr, char* groupsOut = nullptr) {
    if (coveringOut) *coveringOut = 0;
    if (groupsOut) *groupsOut = 0;
    void* srv = *(void**)0x007ACF70;
    if (!srv) return 0;
    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    unsigned groups[32]; int ng = 0;
    void* cov[32]; int nc = 0; int covering = 0;
    for (void** it = b; it && it != e; ++it) {
        void* pl = *it;
        if (!IsRealPlayer(pl)) continue;
        DWORD fr = *(DWORD*)((char*)pl + 0x18);
        if (!fr || !(mask & fr)) continue;
        ++covering;
        if (nc < 32) cov[nc++] = pl;
        unsigned g = Group(pl);
        if (!g) continue;
        bool seen = false;
        for (int i = 0; i < ng; ++i) seen |= groups[i] == g;
        if (!seen && ng < 32) groups[ng++] = g;
    }
    if (coveringOut) *coveringOut = covering;
    if (!covering) return 0;
    int n = 0;
    for (void** it = b; it && it != e; ++it) {
        void* pl = *it;
        if (!IsRealPlayer(pl)) continue;
        bool in = false;
        unsigned g = Group(pl);
        for (int i = 0; i < ng && !in; ++i) in = g && groups[i] == g;
        for (int i = 0; i < nc && !in; ++i) in = cov[i] == pl;
        if (in) ++n;
    }
    if (g_countHost) ++n;   // [AutoHost] CountServerHost=1 restores the stock +1 for ServerHost
    if (n < 1) n = 1;
    if (groupsOut) for (int i = 0; i < ng && i < 8; ++i)
        sprintf(groupsOut + strlen(groupsOut), "%s%u", i ? "," : "", groups[i]);
    return n;
}

// A clone we are allowed to rescale. The last three tests are 0x005BE682's own: the Go belongs to the computer
// player (Go+0x78 is its Player, Player+0x14 == 2), its goid is not of the excluded class, and it has an aspect.
// The first is ours: Go::IsActor (0x00539442 is 'Go+0x00 != 0', Go::GetActor is 'mov eax,[ecx]; ret'), so we
// only ever touch creatures. Without it we would also be rescaling everything else a clone builds - shop stock,
// dropped gold, spell effects - which the stock game never scales after creation and we have no business
// touching.
void* ScalableAspect(void* go) {
    if (!go) return nullptr;
    if (!*(void**)go) return nullptr;                    // not an actor
    void* owner = *(void**)((char*)go + 0x78);
    if (!owner || *(int*)((char*)owner + 0x14) != 2) return nullptr;   // not computer-controlled
    if ((*(DWORD*)((char*)go + 0x4C) & 0xC0000000) == 0x80000000) return nullptr;
    return *(void**)((char*)go + 0x04);   // Go::GetAspect is 'mov eax,[ecx+4]; ret'
}

// ---- The pending clones ----------------------------------------------------------------------------------
// All the stub has to remember is which Go was just cloned. Everything else is read a frame later, from the
// Go itself, on the main loop - no stack-lifetime object is ever kept, which is the whole lesson of
// and the reason 0.7.9's version of this hook could freeze the game.
struct CloneRec {
    DWORD goid;        // the goid GoDb allocated for the new Go
    void* go;          // the Go pointer the constructor was given; only ever compared, never dereferenced
    volatile LONG ready;
};
const int kPend = 512;
CloneRec g_pend[kPend];
volatile LONG g_pendN = 0;
DWORD g_pendLost = 0;

// GoAspect fields, read off 0x005BE682: +0x50/+0x54 experience value, +0xA8 natural max life, +0xAC max life,
// +0xB0 current life (0x005349D6 writes it).
const int kAspXp = 0x50, kAspXp2 = 0x54, kAspNatural = 0xA8, kAspMax = 0xAC, kAspCur = 0xB0;

void ApplyCloneScale(const CloneRec& r) {
    void* godb = *(void**)0x007ACF4C;
    if (!godb || !r.goid || r.goid == *(DWORD*)0x007ADC88) { ++g_cloneSkip; return; }
    void* go = FindGo(godb, r.goid);
    if (!go || go != r.go) {                 // gone, or the goid is not the Go the stub was told about
        ++g_cloneSkip;
        if (g_diag) Log("AREASCALE-CLONEGONE: goid %08lX resolved to %p, expected %p.", r.goid, go, r.go);
        return;
    }
    void* asp = ScalableAspect(go);
    if (!asp) { ++g_cloneSkip; return; }
    DWORD mask = MaskOf(go);
    int covering = 0; char gs[96] = "";
    int c = AreaCount(mask, &covering, g_diag ? gs : nullptr);
    if (!c) {                                // still nobody near it: leave it exactly as the game made it
        ++g_kept;
        if (g_diag) Log("AREASCALE-CLONEKEPT: goid %08lX '%s' mask %08lX count %d - nobody covers it.",
                        r.goid, TemplateName(go), mask, *(int*)((char*)go + 0x5C));
        return;
    }
    // Go+0x5C is the count this monster's life and XP were multiplied by when it was made: the engine's own
    // scaling reads it through 0x005390B5, and SiegeHost_AreaScaleCount either wrote its own number there or
    // returned the one already in it. That invariant is what lets us correct the monster with a ratio instead
    // of having to know its template's base numbers.
    int s = *(int*)((char*)go + 0x5C); if (s < 1) s = 1;
    if (c == s) {
        ++g_same;
        if (g_diag) Log("AREASCALE-CLONE: goid %08lX '%s' area count %d already matches - nothing to do.",
                        r.goid, TemplateName(go), c);
        return;
    }
    void* form = *(void**)0x007AD0F4;
    if (!form) { ++g_cloneSkip; return; }
    float ls = LifeMul(form, s), lc = LifeMul(form, c);
    float xs = XpMul(form, s),   xc = XpMul(form, c);
    if (ls <= 0.0f || xs <= 0.0f) { ++g_cloneSkip; return; }
    float lr = lc / ls, xr = xc / xs;
    float mx  = *(float*)((char*)asp + kAspMax);
    float nat = *(float*)((char*)asp + kAspNatural);
    float cur = *(float*)((char*)asp + kAspCur);
    SSetNaturalMaxLife(asp, nat * lr);
    SSetMaxLife(asp, mx * lr);
    SSetCurrentLife(asp, cur * lr);
    *(float*)((char*)asp + kAspXp)  *= xr;
    *(float*)((char*)asp + kAspXp2) *= xr;
    *(int*)((char*)go + 0x5C) = c;           // keep the invariant: +0x5C is the count the life was scaled by
    ++g_changed; ++g_clones;
    if (g_diag)
        Log("AREASCALE-CLONE: goid %08lX '%s' mask %08lX covering %d groups [%s] count %d -> area count %d, "
            "life x%.3f (%.1f -> %.1f, current %.1f -> %.1f), xp x%.3f",
            r.goid, TemplateName(go), mask, covering, gs, s, c, lr, mx, mx * lr, cur, cur * lr, xr);
}

void CloneFixTick() {
    if (!g_on) return;
    LONG n = InterlockedExchange(&g_pendN, 0);
    if (n <= 0) return;
    if (n > kPend) { g_pendLost += n - kPend; n = kPend; }
    CloneRec batch[kPend];
    int m = 0;
    for (LONG i = 0; i < n; ++i)
        if (InterlockedExchange(&g_pend[i].ready, 0)) batch[m++] = g_pend[i];
    for (int i = 0; i < m; ++i) ApplyCloneScale(batch[i]);
}
}  // namespace

// The stub's C half. Reads only, plus one store - nothing that can block, allocate or re-enter the engine:
// that is the whole lesson of . The real work happens a frame later, from the main loop.
extern "C" void __cdecl SiegeHost_CloneCtorPre(DWORD goid, void* srcGo, void* newGo) {
    (void)srcGo;
    if (!g_on || !newGo) return;
    LONG i = InterlockedIncrement(&g_pendN) - 1;
    if (i < 0 || i >= kPend) return;                     // more clones in one frame than the queue holds
    CloneRec& r = g_pend[i];
    r.goid = goid; r.go = newGo;
    InterlockedExchange(&r.ready, 1);
}

extern "C" int __fastcall SiegeHost_AreaScaleCount(void* go) {
    int stock = StockCount(go);
    if (!g_on || !go) return stock;
    DWORD mask = MaskOf(go);
    int covering = 0; char gs[96] = "";
    int count = AreaCount(mask, &covering, g_diag ? gs : nullptr);
    if (!count) {
        ++g_kept;
        if (g_diag)
            Log("AREASCALE-KEPT: goid %08lX '%s' mask %08lX -> stock %d",
                *(DWORD*)((char*)go + 0x4C), TemplateName(go), mask, stock);
        return stock;
    }
    *(int*)((char*)go + 0x5C) = count;
    if (count == stock) ++g_same; else ++g_changed;
    if (g_diag)
        Log("AREASCALE: goid %08lX '%s' mask %08lX covering %d groups [%s] -> count %d (stock %d)",
            *(DWORD*)((char*)go + 0x4C), TemplateName(go), mask, covering, gs, count, stock);
    return count;
}

// Once a minute: how many monsters were scaled by area, kept, or unchanged (production log line).
void AreaScaleTick() {
    if (!g_on) return;
    CloneFixTick();
    DWORD now = GetTickCount();
    static DWORD lastDiag = 0;
    if (g_diag && now - lastDiag > 10000) {  // lab only: every player's frustum and area
        lastDiag = now;
        void* srv = *(void**)0x007ACF70;
        std::string s;
        if (srv) for (void** it = *(void***)((char*)srv + 4); it && it != *(void***)((char*)srv + 8); ++it) {
            void* pl = *it;
            if (!eng::Player_IsValid(pl)) continue;
            const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
            char nm[40] = "?"; if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, nm, sizeof nm, 0, 0);
            char b[120]; sprintf(b, " [%s id %08lX ctl %d frustum %08lX loc %u%s]", nm, *(DWORD*)((char*)pl + 0xC),
                                *(int*)((char*)pl + 0x14), *(DWORD*)((char*)pl + 0x18), *(unsigned*)((char*)pl + 0xF4),
                                IsRealPlayer(pl) ? " real" : "");
            s += b;
        }
        Log("AREASCALE: players%s | global count %d", s.c_str(), *(int*)0x007AFA30);
    }
    if (now - g_lastSummary < 60000) return;
    g_lastSummary = now;
    if (g_changed || g_same || g_kept)
        Log("AreaScale: last minute %lu monsters scaled for their area (%lu of them differ from the count they "
            "were made with), %lu of them rescaled after a generator, spawner or spell cloned them, %lu with no "
            "player near kept their count%s.",
            g_changed + g_same, g_changed, g_clones, g_kept,
            g_pendLost ? " (the clone queue overflowed)" : "");
    g_changed = g_same = g_kept = g_clones = g_cloneSkip = 0; g_pendLost = 0;
}

void AreaScaleInstall() {
    if (!Cfg().areaScaling) { Log("AreaScale: off ([Server] AreaScaling=0) - monsters scale with everyone online."); return; }
    char e[8];
    g_diag = GetEnvironmentVariableA("SIEGEHOST_SCALEDIAG", e, sizeof e) != 0;  // lab only
    BYTE* p = (BYTE*)kSite;
    if (p[0] != 0xE8 || kSite + 5 + *(int32_t*)(p + 1) != kStockVa) {
        Log("AreaScale: VERIFY FAILED at 0x%08lX - monsters scale with everyone online.", kSite);
        return;
    }
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) { Log("AreaScale: VirtualProtect failed (%lu).", GetLastError()); return; }
    *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&SiegeHost_AreaScaleCount - (kSite + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    // The clone path (RE-15). Only the server clone helper is armed by default: it is the one a generator,
    // spawner or spell goes through, and the only one of the two whose caller frame carries the new goid.
    // SIEGEHOST_CLONESITES=none|A|B|AB is a lab switch. If the byte check fails we keep the rest working and
    // generated monsters simply stay as they were in 0.7.9.
    const DWORD sites[2] = { kCtorSiteA, kCtorSiteB };
    char which[8] = "B";
    GetEnvironmentVariableA("SIEGEHOST_CLONESITES", which, sizeof which);
    bool wantA = strchr(which, 'A') != nullptr, wantB = strchr(which, 'B') != nullptr;
    int armed = 0;
    for (int i = 0; i < 2; ++i) {
        if (i == 0 && !wantA) continue;
        if (i == 1 && !wantB) continue;
        BYTE* c = (BYTE*)sites[i];
        if (c[0] != 0xE8 || sites[i] + 5 + *(int32_t*)(c + 1) != kCtorVa) continue;
        DWORD oc;
        if (!VirtualProtect(c, 5, PAGE_EXECUTE_READWRITE, &oc)) continue;
        *(int32_t*)(c + 1) = (int32_t)((DWORD)(void*)&SiegeHost_CloneCtor - (sites[i] + 5));
        VirtualProtect(c, 5, oc, &oc);
        FlushInstructionCache(GetCurrentProcess(), c, 5);
        ++armed;
    }
    Log("AreaScale: clone hook armed at %d of %d site(s) (wanted '%s').", armed, (int)wantA + (int)wantB, which);
    Log("AreaScale: areas come from the table in src/areagroups.h - %d places in %d groups, not the "
        "map's own hundreds (new in 0.8.1). 'groups [5]' in a log line below now means group 5 of "
        "that table, which is not what it meant before.", areagroups::kCount, areagroups::GroupCount());
    MultiByteToWideChar(CP_ACP, 0, Cfg().hostCharacter.c_str(), -1, g_host, 63);
    g_countHost = Cfg().countServerHost != 0;
    g_on = true;
    Log("AreaScale: armed - a new monster's life and XP scale with the players in its area (their map location "
        "group)%s, not everyone online, whether it was placed on the map or made by a generator (patched "
        "0x%08lX).", g_countHost ? " + ServerHost" : ", ServerHost not counted", kSite);
}

// ---- LAB ONLY on clients (SIEGEHOST_SCALEDIAG=1): log the count each monster arrives with from the server. ----
extern "C" int __fastcall SiegeHost_ScaleCountClientDiag(void* go) {
    int c = StockCount(go);
    Log("AREASCALE-CLIENT: goid %08lX '%s' count %d (Go+0x5C %d)", *(DWORD*)((char*)go + 0x4C), TemplateName(go), c,
        *(int*)((char*)go + 0x5C));
    return c;
}
void AreaScaleClientDiagInstall() {
    char e[4];
    if (!GetEnvironmentVariableA("SIEGEHOST_SCALEDIAG", e, sizeof e)) return;
    BYTE* p = (BYTE*)kSite;
    if (p[0] != 0xE8 || kSite + 5 + *(int32_t*)(p + 1) != kStockVa) return;
    DWORD o;
    VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o);
    *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&SiegeHost_ScaleCountClientDiag - (kSite + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("AREASCALE-CLIENT: lab log of monster counts on");
}
