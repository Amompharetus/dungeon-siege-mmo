// portraitfix.cpp - 0.7.6, players' PCs only: other players' portraits framed right at every resolution.
//
// The party panel draws another player's portrait by rendering a local clone of their hero with an
// orthographic camera (0x0051A5D0) and copying a 64x64 square out of the back buffer. The ortho view
// keeps the character the same size in PIXELS at every resolution, centred on the screen centre, but
// the square is placed proportionally to an 800x600 layout:
//     0051AB2F  x0 = ftol(width  * (1/800) * 380)     ; 0051AB3E call __ftol
//     0051AB57  y0 = ftol(height * (1/600) * 277)     ; 0051AB66 call __ftol
// with hand-made corrections only for 1024x768 and 640x480. Both corrections land on the same place
// relative to the centre (x0 = W/2-18, y0 = H/2-24 at 1024x768; W/2-18, H/2-22 at 640x480), and every
// other resolution drifts away from it: at 1920x1080 the square is 30 px left and 18 px up of the face,
// at 2560x1440 46/32 px, so the slot shows a sliver of the character or only darkness.
//
// Fix: the two __ftol calls are redirected here. 1024x768 and 640x480 keep the stock value (and their
// stock corrections that follow); every other resolution gets x0 = W/2-18, y0 = H/2-24. Size stays 64.
#include "siegehost.h"
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include "engine.h"
#include "areagroups.h"

namespace {
const DWORD kFtolVa = 0x0063DFD0;
const DWORD kSiteX = 0x0051AB3E, kSiteY = 0x0051AB66;
bool g_on = false;
int g_dx = -18, g_dy = -24;
int g_lastW = -1, g_lastH = -1, g_lastX = 0;
}

extern "C" int __cdecl SiegeHost_PortraitCoord(int axis, double v) {
    int* app = *(int**)0x007A99A0;
    int w = app ? app[0xC8 / 4] - app[0xC0 / 4] : 0, h = app ? app[0xCC / 4] - app[0xC4 / 4] : 0;
    bool stock = !g_on || w <= 0 || h <= 0 || (w == 1024 && h == 768) || (w == 640 && h == 480);
    int r = stock ? (int)v : (axis == 0 ? w / 2 + g_dx : h / 2 + g_dy);
    if (axis == 0) g_lastX = r;
    if (axis == 1 && (w != g_lastW || h != g_lastH)) {
        g_lastW = w; g_lastH = h;
        Log("PortraitFix: %dx%d - portrait square from x=%d y=%d (%s)", w, h, g_lastX, r, stock ? "stock" : "centred");
    }
    return r;
}

// Replaces `call __ftol` (value in ST0, result in eax): keeps esi/edi/ebx as __ftol does.
extern "C" void SiegeHost_PortraitX();
extern "C" void SiegeHost_PortraitY();
__asm__(
    ".globl _SiegeHost_PortraitX\n"
    "_SiegeHost_PortraitX:\n"
    "  sub $8, %esp\n"
    "  fstpl (%esp)\n"
    "  push $0\n"
    "  call _SiegeHost_PortraitCoord\n"
    "  add $12, %esp\n"
    "  xor %edx, %edx\n"
    "  ret\n"
    ".globl _SiegeHost_PortraitY\n"
    "_SiegeHost_PortraitY:\n"
    "  sub $8, %esp\n"
    "  fstpl (%esp)\n"
    "  push $1\n"
    "  call _SiegeHost_PortraitCoord\n"
    "  add $12, %esp\n"
    "  xor %edx, %edx\n"
    "  ret\n");

static bool RedirectCall(DWORD site, void* to) {
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != kFtolVa) return false;
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) return false;
    *(int32_t*)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

void PortraitFixInstall() {
    if (!Cfg().portraitFix) { Log("PortraitFix: off ([UI] PortraitFix=0) - portraits framed as the game does."); return; }
    char e[16];
    if (GetEnvironmentVariableA("SIEGEHOST_PORTRAIT_DX", e, sizeof e)) g_dx = atoi(e);   // lab only
    if (GetEnvironmentVariableA("SIEGEHOST_PORTRAIT_DY", e, sizeof e)) g_dy = atoi(e);   // lab only
    // Both sites must be the expected `call __ftol` before either is touched.
    BYTE* x = (BYTE*)kSiteX; BYTE* y = (BYTE*)kSiteY;
    if (x[0] != 0xE8 || kSiteX + 5 + *(int32_t*)(x + 1) != kFtolVa || y[0] != 0xE8 ||
        kSiteY + 5 + *(int32_t*)(y + 1) != kFtolVa) {
        Log("PortraitFix: VERIFY FAILED at 0x%08lX/0x%08lX - portraits framed as the game does.", kSiteX, kSiteY);
        return;
    }
    if (!RedirectCall(kSiteX, (void*)&SiegeHost_PortraitX) || !RedirectCall(kSiteY, (void*)&SiegeHost_PortraitY)) {
        Log("PortraitFix: patch failed (error %lu).", GetLastError());
        return;
    }
    g_on = true;
    Log("PortraitFix: armed - other players' portraits framed from the screen centre (%+d,%+d) at every "
        "resolution except 1024x768/640x480, which keep the stock framing (patched 0x%08lX, 0x%08lX).",
        g_dx, g_dy, kSiteX, kSiteY);
}

// ---- LAB ONLY (SIEGEHOST_CHATDIAG=1): why the party panel rebuild's gate fails (RE-11 §6). ----
namespace {
const auto Gate = (bool(__thiscall*)(void*))0x0051A4C9;
const auto RenderPortrait = (bool(__thiscall*)(void*, void*))0x0051A5D0;
DWORD g_gateCalls = 0, g_gateFails = 0, g_renderCalls = 0, g_renderFails = 0, g_lastGateLog = 0;
}
extern "C" bool __fastcall SiegeHost_GateDiag(void* self, void*) {
    int ws = -1, a78 = -1; float rf = -1;
    if (*(int*)0x007ACF44) ws = *(int*)(*(int*)0x007ACF44 + 0x20);
    if (*(int*)0x007A99A0) a78 = *(int*)(*(int*)0x007A99A0 + 0x78);
    if (*(int*)0x007ACFC8 && *(int*)(*(int*)0x007ACFC8 + 0x1E0)) rf = *(float*)(*(int*)(*(int*)0x007ACFC8 + 0x1E0) + 0x6F4);
    DWORD rc0 = g_renderCalls, rf0 = g_renderFails;
    bool r = Gate(self);
    ++g_gateCalls; if (!r) ++g_gateFails;
    DWORD now = GetTickCount();
    if (now - g_lastGateLog > 2000 || r) {
        g_lastGateLog = now;
        Log("GATEDIAG: gate=%d ws=0x%X app78=%d render6f4=%.3f renders %lu (failed %lu) | totals gate %lu/%lu failed, render %lu/%lu failed",
            r, ws, a78, rf, g_renderCalls - rc0, g_renderFails - rf0, g_gateFails, g_gateCalls, g_renderFails, g_renderCalls);
    }
    return r;
}
extern "C" bool __fastcall SiegeHost_RenderDiag(void* self, void*, void* go) {
    bool r = RenderPortrait(self, go);
    ++g_renderCalls; if (!r) ++g_renderFails;
    return r;
}
static bool RedirectAny(DWORD site, DWORD expectTarget, void* to) {
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != expectTarget) return false;
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) return false;
    *(int32_t*)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}
void PortraitDiagInstall() {
    char e[4];
    if (!GetEnvironmentVariableA("SIEGEHOST_CHATDIAG", e, sizeof e)) return;
    bool a = RedirectAny(0x00519F37, 0x0051A4C9, (void*)&SiegeHost_GateDiag);
    bool b = RedirectAny(0x0051A5A6, 0x0051A5D0, (void*)&SiegeHost_RenderDiag);
    Log("GATEDIAG: lab hooks gate=%d render=%d", a, b);
}

// ---- The party panel's player filter: nearby players only, ServerHost left out, never more than 7. ----
// The panel's three player loops (count 0x00519935, rebuild 0x00519F8C, gate 0x0051A548) each skip
// players for which the cdecl validity check 0x00569054 returns false. Those three calls come here:
//  1. the same check;
//  2. not the server's host character (hero name at Player+0x58, a wide GPG string, equal to
//     [Host] HostCharacter, "ServerHost" by default) unless [UI] ShowServerHost=1 - it is not a person;
//  3. [UI] NearbyOnly=1 (default): only players in the same area group as you. Every PC knows each
//     player's last named place at Player+0xF4 (Player::RCSetWorldLocation 0x0056BA42, the call that
//     prints "X has entered Y"); ids are the map's world_locations.gas ids, e.g. 505 = the town of
//     Elddim. The group each id belongs to is the table in src/areagroups.h, shared with the game
//     server's monster scaling so the two can never disagree. 0 = not known yet (the first seconds
//     after joining) counts as nearby;
//  4. at most 7 other same-team players, in the server's own player order. The panel has 7 remote
//     slots (character_2..8_mp) and the rebuild fills a 7-entry array at this+0 with NO bound check
//     (its slot pointer just steps forward per player), so an 8th would write past it. Stock DS1
//     never has more than 8 players; our servers take 30.
// All three loops get the same answer, so the cached count, the slots and the gate stay consistent.
// The engine only rebuilds when the COUNT changes, so PanelFilterTick also forces a rebuild (cache at
// panel+0x1C = -1, panel singleton at [0x007AD568], cleared by its destructor 0x005197FD) whenever the
// set of shown players changes with the count unchanged (one walks in as another walks out).
namespace {
const DWORD kValidVa = 0x00569054;
const DWORD kValidSites[] = {0x00519935, 0x00519F8C, 0x0051A548};
const auto FindTeam = (void*(__thiscall*)(void*, DWORD))0x0056FC53;
int kMaxOthers = 7;  // SIEGEHOST_PANEL_MAX (lab only) can lower it to test the limit
wchar_t g_hostHero[64] = L"ServerHost";
bool g_hideHost = false, g_nearby = false, g_filterOn = false;

bool IsHostHero(void* pl) {
    const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
    return w && wcscmp(w, g_hostHero) == 0;
}
unsigned AreaGroup(void* pl) { return areagroups::ForLocation(*(unsigned*)((char*)pl + 0xF4)); }
// Would the stock loops show q at all, and does our filter keep it (before the limit of 7)?
bool Shown(void* srv, void* q, void* me, DWORD myId, void* myTeam) {
    if (!eng::Player_IsValid(q) || (g_hideHost && IsHostHero(q))) return false;
    const wchar_t* hero = *(const wchar_t**)((char*)q + 0x58);
    if (!hero || !*hero) return false;  // no hero yet (still on the character screen): nothing to show
    DWORD qid = *(DWORD*)((char*)q + 0xC);
    if (qid == myId || FindTeam(srv, qid) != myTeam) return false;
    if (g_nearby) {
        unsigned a = AreaGroup(q), b = AreaGroup(me);
        if (a && b && a != b) return false;
    }
    return true;
}
// The players the panel should show now, in order (at most kMaxOthers).
int ShownList(void* out[], int cap) {
    void* srv = *(void**)0x007ACF70;
    if (!srv) return 0;
    void* me = eng::Server_GetLocalHumanPlayer(srv);
    if (!me) return 0;
    DWORD myId = *(DWORD*)((char*)me + 0xC);
    void* myTeam = FindTeam(srv, myId);
    void** it = *(void***)((char*)srv + 4);
    void** end = *(void***)((char*)srv + 8);
    int n = 0;
    for (; it && it != end && n < cap; ++it)
        if (Shown(srv, *it, me, myId, myTeam)) out[n++] = *it;
    return n;
}
}
extern "C" bool __cdecl SiegeHost_PanelPlayerValid(void* pl) {
    if (!eng::Player_IsValid(pl)) return false;
    if (g_hideHost && IsHostHero(pl)) return false;
    void* srv = *(void**)0x007ACF70;
    if (!srv) return true;
    void* me = eng::Server_GetLocalHumanPlayer(srv);
    if (!me || pl == me) return true;  // the loops exclude the local player themselves
    DWORD myId = *(DWORD*)((char*)me + 0xC);
    void* myTeam = FindTeam(srv, myId);
    if (FindTeam(srv, *(DWORD*)((char*)pl + 0xC)) != myTeam) return true;  // other team: the loops skip it
    void* list[8];
    int n = ShownList(list, kMaxOthers);
    for (int i = 0; i < n; ++i) if (list[i] == pl) return true;
    return false;
}
// Main thread, every frame (cheap: at most 31 players).
void PanelFilterTick() {
    if (!g_filterOn) return;
    static void* last[8]; static int lastN = -1; static unsigned lastGroup = 0;
    void* panel = *(void**)0x007AD568;
    void* list[8];
    int n = panel ? ShownList(list, kMaxOthers) : 0;
    bool changed = n != lastN;
    for (int i = 0; !changed && i < n; ++i) changed = list[i] != last[i];
    if (!changed) return;
    void* srv = *(void**)0x007ACF70;
    void* me = srv ? eng::Server_GetLocalHumanPlayer(srv) : nullptr;
    unsigned g = me ? AreaGroup(me) : 0;
    if (panel) {
        *(int*)((char*)panel + 0x1C) = -1;  // the engine's own per-frame check rebuilds the panel
        if (lastN >= 0 || n > 0)
            Log("PortraitFix: party panel now %d other player(s)%s (your area group %u%s; player %08lX).", n,
                g_nearby ? " nearby" : "", g, g != lastGroup ? ", changed" : "", me ? *(DWORD*)((char*)me + 0xC) : 0);
    }
    memcpy(last, list, sizeof(void*) * n); lastN = n; lastGroup = g;
}
void PanelHostFilterInstall() {
    for (DWORD s : kValidSites) {
        BYTE* p = (BYTE*)s;
        if (p[0] != 0xE8 || s + 5 + *(int32_t*)(p + 1) != kValidVa) {
            Log("PortraitFix: VERIFY FAILED at 0x%08lX - party panel player filter not installed.", s);
            return;
        }
    }
    g_hideHost = !Cfg().showServerHost;
    g_nearby = Cfg().nearbyOnly != 0;
    char e[8];
    if (GetEnvironmentVariableA("SIEGEHOST_PANEL_MAX", e, sizeof e)) { int n = atoi(e); if (n >= 1 && n <= 7) kMaxOthers = n; }
    MultiByteToWideChar(CP_ACP, 0, Cfg().hostCharacter.c_str(), -1, g_hostHero, 63);
    for (DWORD s : kValidSites) RedirectAny(s, kValidVa, (void*)&SiegeHost_PanelPlayerValid);
    g_filterOn = true;
    Log("PortraitFix: party panel lists at most %d other players%s%s (patched 0x%08lX, 0x%08lX, 0x%08lX).", kMaxOthers,
        g_nearby ? ", only those in your area (the table in src/areagroups.h, new in 0.8.1)" : "",
        g_hideHost ? (std::string(", without '") + Cfg().hostCharacter + "' (the server's host character)").c_str() : "",
        kValidSites[0], kValidSites[1], kValidSites[2]);
}

// ---- LAB ONLY, game server (SIEGEHOST_LAB_MOVELOC="1001@40,501@100"): once 3 players are in (ServerHost +
// two clients), set the LAST player's world location through the engine's own server->clients call
// Player::RCSetWorldLocation (exactly what a location trigger does when someone walks in), at the
// given seconds. Lets the lab test players moving between areas without walking them there.
void LabMoveLocTick() {
    static int on = -1; static char spec[128];
    if (on < 0) on = GetEnvironmentVariableA("SIEGEHOST_LAB_MOVELOC", spec, sizeof spec) ? 1 : 0;
    if (!on) return;
    static DWORD t0 = 0; static int done = 0;
    void* srv = *(void**)0x007ACF70;
    if (!srv) return;
    void** it = *(void***)((char*)srv + 4); void** end = *(void***)((char*)srv + 8);
    void* lastPl = nullptr; int n = 0;
    for (; it && it != end; ++it) {
        if (!eng::Player_IsValid(*it)) continue;
        const wchar_t* w = *(const wchar_t**)((char*)*it + 0x58);
        if (!w || !*w) continue;           // only players with a hero
        ++n;
        char want[16];
        if (GetEnvironmentVariableA("SIEGEHOST_LAB_MOVELOC_PLAYER", want, sizeof want)) {   // hex player id
            if (*(DWORD*)((char*)*it + 0xC) == strtoul(want, 0, 16)) lastPl = *it;
        } else if (wcscmp(w, g_hostHero) != 0) lastPl = *it;  // never move the host
    }
    if (n < 3) { t0 = 0; return; }
    if (!t0) { t0 = GetTickCount(); Log("LABMOVELOC: 3 players in, schedule '%s'", spec); }
    // entries id@sec, comma separated
    int idx = 0; const char* p = spec;
    while (*p) {
        unsigned id = (unsigned)atoi(p); const char* at = strchr(p, '@'); if (!at) break;
        int sec = atoi(at + 1);
        if (idx == done && GetTickCount() - t0 >= (DWORD)sec * 1000) {
            const auto RCSetWorldLocation = (void*(__thiscall*)(void*, unsigned))0x0056BA42;
            RCSetWorldLocation(lastPl, id);
            Log("LABMOVELOC: set player %08lX world location to %u (now %u)", *(DWORD*)((char*)lastPl + 0xC), id, *(unsigned*)((char*)lastPl + 0xF4));
            ++done;
        }
        ++idx;
        const char* c = strchr(p, ','); if (!c) break; p = c + 1;
    }
}
