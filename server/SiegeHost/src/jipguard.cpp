// jipguard.cpp — the staging-screen freeze fix (0.7.3). See jipguard.h for the decision it uses.
//
// THE FREEZE, EXACTLY (RE-10)
// The world enters ws_mp_ingame_jip (0x29) the moment a connection is created, and stays there until
// the joiner is fully in the world. Two independent things then stop the world on EVERY machine:
//
//  1. The world-state change handler (0x004C64C5, case 0x29) calls AppModule::SystemPause(true)
//     (0x00414411), which bumps the system pause count at AppModule+0x80. The per-frame update
//     (0x00477118) sees that count and takes its paused branch, which calls WorldTime::Update(0.0)
//     instead of WorldTime::Update(dt) — so the world clock literally stops. This is NOT the pause
//     button: that is AppModule+0x7C (IsUserPaused), which stays 0 throughout. It is why the 0.7.2
//     pause guard, which only ever un-set the user pause, changed nothing.
//  2. The per-frame sim step (0x00581459) skips the world update outright while the state is 0x29.
//
// THE FIX
//  1. The two SystemPause calls the state handler makes for JIP - the one entering 0x29 and the one
//     leaving it - are redirected to SiegeHost. Entering, the pause is DEFERRED while the joiner is
//     still choosing a character, and performed for real the moment they press Ready. Leaving, it is
//     undone only if it was ever performed. The pause count is therefore always balanced: it is the
//     engine's own call, made later or not at all, never an extra or a missing one.
//     (An earlier attempt used TattooGame+0x4C bit 3, which makes the frame update ignore the pause
//     count. It works, but the pause also stops the application clock, so the world crawled at about
//     an eighth speed instead of running. Deferring the pause itself is the fix.)
//  2. The sim-step test is patched so it consults the same decision instead of the bare state.
//
// The engine's join-in-progress panel is left exactly as it is: with the pause deferred it does not
// cover the world, and an earlier attempt to hide it by hand hung the game server
// outright.
//
// Nothing here changes the world state or the join protocol. The joiner's path through staging,
// load and into the world is byte-for-byte the stock sequence.
#include "siegehost.h"
#include "engine.h"
#include "jipguard.h"
#include <cstdio>
#include <cstring>

namespace {

// --- the sim-step patch -------------------------------------------------------------------------
//   00581459  a1 44 cf 7a 00   mov  eax,[0x7acf44]      ; WorldState*
//   0058146c  74 32            je   005814a0            ; not in game -> skip the sim
//   0058146e  a1 44 cf 7a 00   mov  eax,[0x7acf44]      <-- patched, 11 bytes
//   00581473  83 78 20 29      cmp  dword [eax+0x20],0x29
//   00581477  74 27            je   005814a0            ; in JIP -> skip the sim
//   00581479  d9 44 24 08      flds [esp+8]             ; dt, the sim runs from here
// becomes
//   0058146e  e8 <rel32>       call SiegeHost_JipHold   ; bool, cdecl, no args
//   00581473  84 c0            test al,al
//   00581475  75 29            jne  005814a0
//   00581477  90 90            nop; nop
// eax is dead after the compare, ecx is dead until 0x0058147E reloads it from esi, edx is
// caller-saved, and esi/edi/ebx are preserved by the compiler for SiegeHost_JipHold like any other
// C function. The stack is untouched, so the function's own `ret 8` is unaffected.
const DWORD kSimStepVa = 0x00581459;
const DWORD kPatchVa = 0x0058146E;
const BYTE kExpect[] = {0xA1, 0x44, 0xCF, 0x7A, 0x00, 0x83, 0x78, 0x20, 0x29, 0x74, 0x27};
const DWORD kSkipVa = 0x005814A0;

// AppModule::SystemPause(bool) - the system pause refcount at AppModule+0x80. thiscall, ret 4.
const auto SystemPause = (void(__thiscall*)(void*, int))0x00414411;
const DWORD kAppModuleGlobal = 0x007A99B0;   // the TattooGame/AppModule singleton the handler uses

// The two call sites in the world-state change handler (0x004C64C5). Both are
//   mov ecx,[0x7A99B0]; push <0|1>; call 0x00414411
// and only the `call` is replaced, so ecx and the pushed argument are left exactly as the engine set
// them and our replacement cleans the one stack argument itself (__stdcall, ret 4).
const DWORD kPauseEnterVa = 0x004C74E3;  // case 0x29:            SystemPause(true)
const DWORD kPauseLeaveVa = 0x004C7400;  // case 0x28 from 0x29:  SystemPause(false)
const BYTE kPauseEnterExpect[] = {0xE8, 0x29, 0xCF, 0xF4, 0xFF};
const BYTE kPauseLeaveExpect[] = {0xE8, 0x0C, 0xD0, 0xF4, 0xFF};

// The rest of `case 0x29` is the on-screen half: the "someone is joining" panel, the paused
// indicator, the closing of the player's inventory, and the grey glass over everything. Four more
// plain `call rel32` sites in the same block, deferred the same way and re-issued through the
// engine's own functions when the joiner presses Ready.
//
//   004C74D0  mov ecx,[0x7ACF94]
//   004C74D6  call 0x00490427        ; show + fill in "join_in_progress"        (ecx only, no args)
//   004C74E8  mov ecx,[0x7ACFA4]
//   004C74EE  push 1
//   004C74F0  call 0x00497C0E        ; the paused UI group / pause-play buttons (ret 4)
//   004C74F5  mov ecx,[0x7ACF90]
//   004C74FB  call 0x004A11D7        ; close character / spell / backpack       (ecx only, no args)
//   004C7505  push "jip_glass"       ; gpstring built on the stack
//   004C751E  call 0x006E8C12        ; UIShell::ShowInterface                   (ret 4)
//
// These are patched on CLIENTS ONLY. The dedicated server has no one looking at its screen, and
// AutoHost is already driving its shell — that combination is what hung the server.
const DWORD kShowPanelVa = 0x004C74D6;
const DWORD kPausedUiVa = 0x004C74F0;
const DWORD kClosePanelsVa = 0x004C74FB;
const DWORD kShowGlassVa = 0x004C751E;
const BYTE kShowPanelExpect[] = {0xE8, 0x4C, 0x8F, 0xFC, 0xFF};
const BYTE kPausedUiExpect[] = {0xE8, 0x19, 0x07, 0xFD, 0xFF};
const BYTE kClosePanelsExpect[] = {0xE8, 0xD7, 0x9C, 0xFD, 0xFF};
const BYTE kShowGlassExpect[] = {0xE8, 0xEF, 0x16, 0x22, 0x00};

// The engine's own versions, for re-issuing them later. Each takes its `this` from the same global
// the call site loads into ecx.
// 0.7.4 - the input lock. Right after the glass, `case 0x29` also does
//   004c752c  a1 d4 cf 7a 00   mov eax,[0x7ACFD4]          ; WorldOptions*
//   004c7531  6a 00            push 0                      ; (belongs to the next call)
//   004c7533  c6 40 46 01      mov byte [eax+0x46],1       ; <-- this
// WorldOptions+0x46 is read in exactly two places (full .text scan): 0x00549565, in the per-frame
// update, which skips the game's whole input step while it is set (no hotkeys: inventory, map,
// spells, ...), and 0x00535AFD, which stops world objects under the cursor from being clickable
// (shops, NPCs). Stock, it is harmless because the world is frozen anyway. With the freeze deferred
// it is the "no menus while someone is on the character screen" bug. The leave path
// clears it unconditionally (0x004C80B3 `and byte [eax+0x46],0`), which is left alone. The write is
// NOPed on clients and performed by SiegeHost at the same moment as the rest of the join screen.
const DWORD kInputLockVa = 0x004C7533;
const BYTE kInputLockExpect[] = {0xC6, 0x40, 0x46, 0x01};
const DWORD kWorldOptionsGlobal = 0x007ACFD4;
const int kWorldOptionsInputLock = 0x46;

// 0.7.4 - the menus fix, part 2: HOLD THE STATE CHANGE ITSELF on players' PCs.
// The input lock above is only one of ~25 places where the stock game refuses gameplay input while
// the world is in ws_mp_ingame_jip: opening the inventory/character panel returns early at
// 0x0049BB81, move/fight/target orders at 0x004EE82A..0x004EEA3A, the potion hotkeys at
// 0x004EEA82/0x004EEB21, attack/guard at 0x00512C4B.., spell rotation at 0x00503AAD, and the map key
// asks for a state change the 0x29 transition table does not allow. Stock that is
// all moot because everyone is frozen. So instead of patching every reader, a player's PC simply does
// not ENTER 0x29 while the joiner is still choosing a character.
// A client learns the state from WorldState::RCSetWorldStateIfInGame (0x00522B4E), broadcast by the
// server's OnConnectionCreated. Its local path is
//   if (IsInGame(cur) && cur != s && pending != s)  WorldState::Request(s)      ; 0x00522C6E
//   00522c67  e8 ec f8 f6 ff   call WorldState singleton
//   00522c6c  8b c8            mov ecx,eax
//   00522c6e  e8 9b fb ff ff   call 0x0052280E      <-- redirected (thiscall: state, double; ret 0xC)
// SiegeHost_JipStateRequest holds a request for 0x29 while no JIP player has left the character
// screen, and performs it itself (through WorldState::Request, 0x00470E02, exactly the same call)
// the moment one has - so from the Ready click on, every PC goes through the stock join: freeze,
// join screen, input lock, and back to 0x28 when the server says so. The server is not touched: its
// own world state still goes to 0x29 at once, which is what drives the join (0x004778DB).
// The function entry itself is NOT hooked: the bytes after its prologue are FuBi's RPC markers.
const DWORD kStateRequestVa = 0x00522C6E;
const BYTE kStateRequestExpect[] = {0xE8, 0x9B, 0xFB, 0xFF, 0xFF};
const DWORD kWorldStateRequestImpl = 0x0052280E;
const auto WorldState_Request = (int(__thiscall*)(void*, int))0x00470E02;   // Request(state), ret 4
const auto IsInGameFn = (bool(__cdecl*)(int))0x0052245E;

const auto ShowJoinPanel = (void(__thiscall*)(void*))0x00490427;
const auto HideJoinPanel = (void(__thiscall*)(void*))0x00490571;
const auto PausedUi = (void(__thiscall*)(void*, int))0x00497C0E;
const auto ClosePlayerPanels = (void(__thiscall*)(void*))0x004A11D7;
const auto UIShell_ShowInterface = (void(__thiscall*)(void*, const eng::gpstring*))0x006E8C12;
const auto UIShell_HideInterface = (void(__thiscall*)(void*, const eng::gpstring*))0x006E8C20;
const DWORD kJoinPanelGlobal = 0x007ACF94;
const DWORD kPausedUiGlobal = 0x007ACFA4;
const DWORD kPlayerPanelsGlobal = 0x007ACF90;

// Player layout (RE-10): +0x55 JIP flag (Player::RCSetJIP 0x00569C2E), +0xCC the player's own world
// state (Player::RCSetWorldState 0x0056B8E1). Both are replicated to every machine, so this reads
// the same on the game server and on a player's PC.
const int kPlayerJipFlag = 0x55;
const int kPlayerWorldState = 0xCC;
// Server: +4 and +8 are begin/end of a vector<Player*>.
const int kServerPlayersBegin = 4;
const int kServerPlayersEnd = 8;

bool g_disabled = false;   // SIEGEHOST_NO_JIP_GUARD=1 -> leave the stock behaviour completely alone
bool g_patched = false;
bool g_quiet = false;      // true while we are suppressing the freeze for a joiner in staging
bool g_owed = false;       // the engine asked for the JIP pause and we deferred it
bool g_paid = false;       // ...and we have since performed it
bool g_uiOwed = false;     // clients: the engine's join panel/glass were skipped and owe a show
bool g_uiPatched = false;  // clients: the four on-screen call sites were redirected
bool g_lockPatched = false; // clients: the engine's input-lock write at kInputLockVa is NOPed (0.7.4)
bool g_statePatched = false; // clients: the 0x29 state request is filtered (0.7.4)
bool g_stateHeld = false;    // clients: the server asked for 0x29 and this PC is holding it
bool g_joinerSeen = false;   // ...and a JIP-flagged player has been seen here since
DWORD g_stateHolds = 0;
int g_deferring = -1;      // this episode's decision, shared by every stub in the same block
DWORD g_holds = 0, g_releases = 0, g_calls = 0, g_episodes = 0;

void DoSystemPause(bool on) {
    void* app = *(void**)kAppModuleGlobal;
    if (app) SystemPause(app, on ? 1 : 0);
}

// Reads the replicated player table and answers the one question that matters: is a real join under
// way, or is the joiner still choosing a character? Shared by the sim-step patch and the tick.
bool ReadJipStates(int* states, int cap, int& n) {
    n = 0;
    bool readable = false;
    if (void* srv = eng::Server_Singleton()) {
        void** b = *(void***)((char*)srv + kServerPlayersBegin);
        void** e = *(void***)((char*)srv + kServerPlayersEnd);
        if (b && e && e >= b && (size_t)(e - b) <= 256) {
            readable = true;
            for (void** p = b; p != e && n < cap; ++p) {
                void* pl = *p;
                if (!pl || !eng::Player_IsValid(pl)) continue;
                if (!*((unsigned char*)pl + kPlayerJipFlag)) continue;
                states[n++] = *(int*)((char*)pl + kPlayerWorldState);
            }
        }
    }
    return readable;
}

bool HoldNow(int worldState) {
    int states[64];
    int n = 0;
    bool readable = false;
    if (void* srv = eng::Server_Singleton()) {
        void** b = *(void***)((char*)srv + kServerPlayersBegin);
        void** e = *(void***)((char*)srv + kServerPlayersEnd);
        if (b && e && e >= b && (size_t)(e - b) <= 256) {
            readable = true;
            for (void** p = b; p != e && n < (int)(sizeof states / sizeof states[0]); ++p) {
                void* pl = *p;
                if (!pl || !eng::Player_IsValid(pl)) continue;
                if (!*((unsigned char*)pl + kPlayerJipFlag)) continue;
                states[n++] = *(int*)((char*)pl + kPlayerWorldState);
            }
        }
    }
    return JipShouldHold(worldState, states, n, readable);
}

// Every stub in `case 0x29` must reach the same conclusion, and they run back to back inside one
// call of the engine's handler, so the answer is computed once and reused until the episode ends.
bool DeferThisEpisode() {
    if (g_deferring < 0) {
        void* wsObj = eng::WorldState_Singleton();
        int ws = wsObj ? *(int*)((char*)wsObj + 0x20) : JG_WS_MP_INGAME_JIP;
        g_deferring = HoldNow(ws) ? 0 : 1;
    }
    return g_deferring == 1;
}

// The write the engine would have made at kInputLockVa.
void SetInputLock() {
    if (!g_lockPatched) return;
    if (void* wo = *(void**)kWorldOptionsGlobal) *((unsigned char*)wo + kWorldOptionsInputLock) = 1;
}

// Put the engine's own join-in-progress screen up, exactly as `case 0x29` would have. Clients only:
// these are the calls that must not be made on the dedicated server.
void ShowJipScreen() {
    if (!g_uiOwed) return;
    g_uiOwed = false;
    if (void* p = *(void**)kJoinPanelGlobal) ShowJoinPanel(p);
    if (void* p = *(void**)kPausedUiGlobal) PausedUi(p, 1);
    if (void* p = *(void**)kPlayerPanelsGlobal) ClosePlayerPanels(p);
    if (void* ui = eng::UI()) {
        eng::GpStr g("jip_glass");
        UIShell_ShowInterface(ui, &g.s);
    }
    SetInputLock();
}

// Clears the input lock (WorldOptions+0x46).
void ClearInputLock() {
    if (void* wo = *(void**)kWorldOptionsGlobal) *((unsigned char*)wo + kWorldOptionsInputLock) = 0;
}

// Dismiss the join-in-progress screen, glass overlay, paused UI, and clear input lock.
void DismissJipScreen() {
    g_uiOwed = false;
    if (void* p = *(void**)kJoinPanelGlobal) HideJoinPanel(p);
    if (void* p = *(void**)kPausedUiGlobal) PausedUi(p, 0);
    if (void* ui = eng::UI()) {
        eng::GpStr g("jip_glass");
        UIShell_HideInterface(ui, &g.s);
        eng::GpStr j("join_in_progress");
        UIShell_HideInterface(ui, &j.s);
    }
    ClearInputLock();
}

}  // namespace

// The four on-screen calls in `case 0x29`, redirected on clients. Each either does what the engine
// asked, or records that it is owed and does nothing.
extern "C" void __cdecl SiegeHost_JipShowPanel() {
    if (!DeferThisEpisode()) { if (void* p = *(void**)kJoinPanelGlobal) ShowJoinPanel(p); return; }
    g_uiOwed = true;
}

extern "C" void __stdcall SiegeHost_JipPausedUi(int on) {
    if (!DeferThisEpisode()) { if (void* p = *(void**)kPausedUiGlobal) PausedUi(p, on); return; }
    g_uiOwed = true;
}

extern "C" void __cdecl SiegeHost_JipClosePanels() {
    if (!DeferThisEpisode()) { if (void* p = *(void**)kPlayerPanelsGlobal) ClosePlayerPanels(p); return; }
    g_uiOwed = true;
}

// The caller has already built the gpstring on its own stack and passed it; hand it straight back to
// UIShell::ShowInterface, or drop it. The caller destroys the string either way.
extern "C" void __stdcall SiegeHost_JipShowGlass(const eng::gpstring* name) {
    if (!DeferThisEpisode()) { if (void* ui = eng::UI()) UIShell_ShowInterface(ui, name); SetInputLock(); return; }
    g_uiOwed = true;
}

// 0.7.4: the redirected WorldState::Request inside RCSetWorldStateIfInGame, players' PCs only.
// Returns true to drop the request (this PC holds ws_mp_ingame_jip), false to perform it.
extern "C" bool __cdecl SiegeHost_JipFilterRequest(int requestedState) {
    if (g_disabled) return false;
    int states[64];
    int n = 0;
    bool readable = ReadJipStates(states, 64, n);
    if (!JipHoldStateChange(requestedState, states, n, readable)) {
        if (requestedState != JG_WS_MP_INGAME_JIP && g_stateHeld) {
            g_stateHeld = false;
            Log("JipGuard: the server moved on to state %d while this PC was holding the join state - released.",
                requestedState);
        }
        return false;
    }
    g_stateHeld = true;
    g_joinerSeen = n > 0;
    ++g_stateHolds;
    ++g_episodes;
    Log("JipGuard: someone is connecting - this PC stays in the normal game state (menus, orders and shops "
        "keep working) until they press Ready. Episode %lu.", g_episodes);
    return true;
}

// thiscall-compatible trampoline for the call site: [esp+4] = requested state, then a double; ecx =
// WorldState. Either tail-jumps into the real WorldState::Request with everything untouched, or
// returns 0 and pops the three argument dwords exactly as the real one (ret 0xC) would.
extern "C" void SiegeHost_JipStateRequest();
__asm__(
    ".globl _SiegeHost_JipStateRequest\n"
    "_SiegeHost_JipStateRequest:\n"
    "  push %ecx\n"
    "  pushl 8(%esp)\n"
    "  call _SiegeHost_JipFilterRequest\n"
    "  add $4, %esp\n"
    "  pop %ecx\n"
    "  test %al, %al\n"
    "  jnz 1f\n"
    "  mov $0x0052280E, %eax\n"
    "  jmp *%eax\n"
    "1:\n"
    "  xor %eax, %eax\n"
    "  ret $0xC\n");

// Called from the game's main loop by the patched sim step, once per simulation step.
extern "C" bool __cdecl SiegeHost_JipHold() {
    ++g_calls;
    void* wsObj = eng::WorldState_Singleton();
    if (!wsObj) return true;
    int worldState = *(int*)((char*)wsObj + 0x20);
    if (worldState != JG_WS_MP_INGAME_JIP) { ++g_releases; return false; }
    if (g_disabled) { ++g_holds; return true; }
    bool hold = HoldNow(worldState);
    if (hold) ++g_holds; else ++g_releases;
    return hold;
}

// Redirected from the world-state handler's own SystemPause calls (see kPauseEnterVa/kPauseLeaveVa).
// __stdcall so the one argument the engine pushed is cleaned exactly as AppModule::SystemPause would.
extern "C" void __stdcall SiegeHost_JipPauseEnter(int) {
    if (g_disabled) { DoSystemPause(true); g_paid = true; return; }
    void* wsObj = eng::WorldState_Singleton();
    int worldState = wsObj ? *(int*)((char*)wsObj + 0x20) : JG_WS_MP_INGAME_JIP;
    (void)worldState;
    if (!DeferThisEpisode()) {  // somebody is already past the character screen: pause now, as stock
        DoSystemPause(true);
        g_paid = true;
        return;
    }
    g_owed = true;
    g_quiet = true;
    ++g_episodes;
    Log("JipGuard: a joiner is on the character screen - the world keeps running for everyone else "
        "(join pause deferred). Episode %lu.", g_episodes);
}

extern "C" void __stdcall SiegeHost_JipPauseLeave(int) {
    g_owed = false;
    g_quiet = false;
    g_uiOwed = false;   // the engine's own hide calls follow this one and are no-ops either way
    g_deferring = -1;
    if (g_paid) { g_paid = false; DoSystemPause(false); }
    else Log("JipGuard: the joiner left before starting to load - nothing to un-pause.");
    if (Cfg().role == Role::Client) {
        DismissJipScreen();
    }
}

// Players' PCs: while holding, watch the replicated players. The first JIP player to leave the
// character screen means the real join has begun - make the request the server asked for, exactly as
// RCSetWorldStateIfInGame would have. If every JIP player disappears first, the joiner left (or
// finished) without pressing Ready here; the server has already gone back to 0x28, so drop the debt.
static void JipStateTick() {
    if (!g_stateHeld) return;
    int states[64];
    int n = 0;
    bool readable = ReadJipStates(states, 64, n);
    void* ws = eng::WorldState_Singleton();
    int cur = ws ? *(int*)((char*)ws + 0x20) : 0;
    int pending = ws ? *(int*)((char*)ws + 0x28) : 0;
    if (!ws || !IsInGameFn(cur)) { g_stateHeld = false; return; }   // we left the game ourselves
    if (n > 0) g_joinerSeen = true;
    if (readable && JipRealJoinUnderway(states, n)) {
        g_stateHeld = false;
        if (cur != JG_WS_MP_INGAME_JIP && pending != JG_WS_MP_INGAME_JIP) WorldState_Request(ws, JG_WS_MP_INGAME_JIP);
        Log("JipGuard: the joiner pressed Ready - entering the stock join state now (was %d).", cur);
        return;
    }
    if (readable && g_joinerSeen && n == 0) {
        g_stateHeld = false;
        Log("JipGuard: the joiner is gone and never pressed Ready - nothing to do.");
    }
}

// Called every frame on the game's main thread, for both roles.
void JG_JipTick() {
    if (g_statePatched) JipStateTick();
    if (g_disabled) return;
    void* wsObj = eng::WorldState_Singleton();
    if (!wsObj) return;
    int worldState = *(int*)((char*)wsObj + 0x20);

    // Client safety check: in normal gameplay (ws_mp_ingame), dismiss any stuck JIP UI or input lock
    if (Cfg().role == Role::Client && worldState == JG_WS_MP_INGAME) {
        void* wo = *(void**)kWorldOptionsGlobal;
        bool locked = wo && *((unsigned char*)wo + kWorldOptionsInputLock) != 0;
        bool glassVis = eng::InterfaceVisible("jip_glass");
        bool joinVis = eng::InterfaceVisible("join_in_progress");
        if (locked || glassVis || joinVis) {
            DismissJipScreen();
            Log("JipGuard: cleaned up stuck JIP UI/input-lock while in ws_mp_ingame (locked=%d, glass=%d, join=%d).",
                locked ? 1 : 0, glassVis ? 1 : 0, joinVis ? 1 : 0);
        }
    }

    // Client check: if in ws_mp_ingame_jip but no JIP player exists (crashed/disconnected mid-join)
    if (Cfg().role == Role::Client && worldState == JG_WS_MP_INGAME_JIP) {
        int states[64];
        int n = 0;
        bool readable = ReadJipStates(states, 64, n);
        if (readable && n == 0) {
            DismissJipScreen();
            if (g_paid) { g_paid = false; DoSystemPause(false); }
            WorldState_Request(wsObj, JG_WS_MP_INGAME);
            Log("JipGuard: joiner vanished/crashed mid-join - restored ws_mp_ingame, dismissed UI, cleared input lock.");
            return;
        }
    }

    // 0.8.6. With ready-first joining (src/jipqueue.cpp) the world deliberately stays in
    // ws_mp_ingame_jip after the joiner who was loading has been let in, because somebody else is
    // still on the character screen. The pause belongs to that map load, not to the episode, so it
    // has to be given back as soon as everyone who is left is still choosing - otherwise the world
    // stays frozen for the people already in it, which is the very thing this file exists to stop.
    if (g_paid && worldState == JG_WS_MP_INGAME_JIP && !HoldNow(worldState)) {
        g_paid = false; g_owed = true; g_deferring = -1;
        DoSystemPause(false);
        if (Cfg().role == Role::Client) {
            DismissJipScreen();
        }
        Log("JipGuard: the joiner who was loading is in or disconnected - the world runs again while the others are "
            "still choosing (pause count %d).", eng::GamePauseCount());
    }
    if (!g_owed && !g_uiOwed) return;
    if (worldState != JG_WS_MP_INGAME_JIP) return;   // the leave hook will clear the debt
    if (!HoldNow(worldState)) return;                // still choosing a character
    g_quiet = false;
    g_deferring = -1;
    if (g_owed) { g_owed = false; g_paid = true; DoSystemPause(true); }
    ShowJipScreen();
    Log("JipGuard: the joiner pressed Ready - stock join screen and freeze applied (pause count %d).",
        eng::GamePauseCount());
}

void JG_InstallJipGuard() {
    if (g_patched) return;
    char e[8];
    if (GetEnvironmentVariableA("SIEGEHOST_NO_JIP_GUARD", e, sizeof e)) {
        g_disabled = true;
        Log("JipGuard: DISABLED by SIEGEHOST_NO_JIP_GUARD - the stock join freeze is left in place.");
        return;
    }
    if (memcmp((void*)kPatchVa, kExpect, sizeof kExpect) != 0) {
        std::string got;
        char b[4];
        for (size_t i = 0; i < sizeof kExpect; ++i) { sprintf(b, "%02X", ((BYTE*)kPatchVa)[i]); got += b; }
        g_disabled = true;
        Log("JipGuard: VERIFY FAILED at 0x%08lX (found %s) - NOT patched, the stock join freeze stays.", kPatchVa,
            got.c_str());
        return;
    }
    BYTE code[sizeof kExpect];
    DWORD target = (DWORD)(void*)&SiegeHost_JipHold;
    code[0] = 0xE8;
    *(DWORD*)(code + 1) = target - (kPatchVa + 5);
    code[5] = 0x84; code[6] = 0xC0;                             // test al,al
    code[7] = 0x75; code[8] = (BYTE)(kSkipVa - (kPatchVa + 9)); // jne 0x005814A0
    code[9] = 0x90; code[10] = 0x90;

    DWORD old;
    if (!VirtualProtect((LPVOID)kPatchVa, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        g_disabled = true;
        Log("JipGuard: VirtualProtect failed at 0x%08lX (error %lu) - NOT patched.", kPatchVa, GetLastError());
        return;
    }
    memcpy((void*)kPatchVa, code, sizeof code);
    VirtualProtect((LPVOID)kPatchVa, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kPatchVa, sizeof code);
    struct Site { const char* name; DWORD va; const BYTE* expect; void* fn; };
    Site sites[7];
    int n = 0;
    sites[n++] = {"SystemPause(true) entering ws_mp_ingame_jip", kPauseEnterVa, kPauseEnterExpect,
                  (void*)&SiegeHost_JipPauseEnter};
    sites[n++] = {"SystemPause(false) leaving ws_mp_ingame_jip", kPauseLeaveVa, kPauseLeaveExpect,
                  (void*)&SiegeHost_JipPauseLeave};
    // The on-screen half is for players only. The dedicated server keeps the stock join screen: no
    // one is looking at it, and AutoHost already owns its UI shell.
    bool wantUi = Cfg().role == Role::Client;
    if (wantUi) {
        sites[n++] = {"show the join-in-progress panel", kShowPanelVa, kShowPanelExpect,
                      (void*)&SiegeHost_JipShowPanel};
        sites[n++] = {"paused UI group", kPausedUiVa, kPausedUiExpect, (void*)&SiegeHost_JipPausedUi};
        sites[n++] = {"close the player's open panels", kClosePanelsVa, kClosePanelsExpect,
                      (void*)&SiegeHost_JipClosePanels};
        sites[n++] = {"show the jip_glass overlay", kShowGlassVa, kShowGlassExpect,
                      (void*)&SiegeHost_JipShowGlass};
        sites[n++] = {"WorldState::Request in RCSetWorldStateIfInGame", kStateRequestVa, kStateRequestExpect,
                      (void*)&SiegeHost_JipStateRequest};
    }
    for (int i = 0; i < n; ++i) {
        const Site& st = sites[i];
        if (memcmp((void*)st.va, st.expect, 5) != 0) {
            std::string got;
            char bb[4];
            for (int k = 0; k < 5; ++k) { sprintf(bb, "%02X", ((BYTE*)st.va)[k]); got += bb; }
            g_disabled = true;
            Log("JipGuard: VERIFY FAILED at 0x%08lX (%s, found %s) - nothing is deferred; behaviour is stock.",
                st.va, st.name, got.c_str());
            return;
        }
    }
    if (wantUi && memcmp((void*)kInputLockVa, kInputLockExpect, sizeof kInputLockExpect) != 0) {
        std::string got;
        char bb[4];
        for (size_t k = 0; k < sizeof kInputLockExpect; ++k) { sprintf(bb, "%02X", ((BYTE*)kInputLockVa)[k]); got += bb; }
        g_disabled = true;
        Log("JipGuard: VERIFY FAILED at 0x%08lX (input lock, found %s) - nothing is deferred; behaviour is stock.",
            kInputLockVa, got.c_str());
        return;
    }
    for (int i = 0; i < n; ++i) {
        const Site& st = sites[i];
        BYTE call[5];
        call[0] = 0xE8;
        *(DWORD*)(call + 1) = (DWORD)st.fn - (st.va + 5);
        DWORD o;
        if (!VirtualProtect((LPVOID)st.va, 5, PAGE_EXECUTE_READWRITE, &o)) {
            g_disabled = true;
            Log("JipGuard: VirtualProtect failed at 0x%08lX (error %lu) - pause NOT deferred.", st.va, GetLastError());
            return;
        }
        memcpy((void*)st.va, call, 5);
        VirtualProtect((LPVOID)st.va, 5, o, &o);
        FlushInstructionCache(GetCurrentProcess(), (LPCVOID)st.va, 5);
        Log("JipGuard: redirected %s at 0x%08lX -> %p", st.name, st.va, st.fn);
    }
    g_uiPatched = wantUi;
    g_statePatched = wantUi;
    if (wantUi) {
        DWORD o;
        if (VirtualProtect((LPVOID)kInputLockVa, sizeof kInputLockExpect, PAGE_EXECUTE_READWRITE, &o)) {
            memset((void*)kInputLockVa, 0x90, sizeof kInputLockExpect);
            VirtualProtect((LPVOID)kInputLockVa, sizeof kInputLockExpect, o, &o);
            FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kInputLockVa, sizeof kInputLockExpect);
            g_lockPatched = true;
            Log("JipGuard: deferred the input lock at 0x%08lX (menus, hotkeys and shops stay usable while a "
                "joiner is on the character screen)", kInputLockVa);
        } else {
            // The redirected glass stub is then the only thing that changed; the engine keeps writing the
            // lock itself, exactly as 0.7.3 did.
            Log("JipGuard: VirtualProtect failed at 0x%08lX (error %lu) - input lock left to the engine.",
                kInputLockVa, GetLastError());
        }
    }

    g_patched = true;
    Log("JipGuard: armed (%d call sites, on-screen half %s). While a joiner is on the character screen the "
        "world keeps running and %s; both come back the moment they press Ready.",
        n, wantUi ? "ON (client)" : "off (server)",
        wantUi ? "the join-in-progress screen stays off" : "the join screen is left to the engine");
}

// Lab self-check: prints the patched bytes back and the live counters.
std::string JG_JipSelfCheck() {
    std::string bytes;
    char b[4];
    for (size_t i = 0; i < sizeof kExpect; ++i) { sprintf(b, "%02X", ((BYTE*)kPatchVa)[i]); bytes += b; }
    char buf[200];
    _snprintf(buf, sizeof buf, "patched=%d bytes=%s hold@%p calls=%lu holds=%lu", g_patched ? 1 : 0, bytes.c_str(),
              (void*)&SiegeHost_JipHold, g_calls, g_holds);
    buf[sizeof buf - 1] = 0;
    return buf;
}

std::string JG_JipHeartbeatLines() {
    char buf[256];
    _snprintf(buf, sizeof buf, "jipguard=%s\njip_ui=%s\njip_inputlock=%s\njip_stateheld=%d\njip_stateholds=%lu\njip_quiet=%d\njip_episodes=%lu\njip_calls=%lu\njip_holds=%lu\n",
              g_patched ? "on" : "off", g_uiPatched ? "on" : "off", g_lockPatched ? "deferred" : "stock", g_stateHeld ? 1 : 0, g_stateHolds, g_quiet ? 1 : 0, g_episodes, g_calls, g_holds);
    buf[sizeof buf - 1] = 0;
    return buf;
}
