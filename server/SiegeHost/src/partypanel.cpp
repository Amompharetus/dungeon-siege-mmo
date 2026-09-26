// partypanel.cpp - 0.7.5, players' PCs only: the top-left panel shows only your own character.
//
// The multiplayer party panel ("team_portraits", slots awp_itemslot_portrait_2..8, groups
// character_%d_mp) lists the OTHER players on your team - including the server's own ServerHost.
// Every frame 0x005198F7.. counts the valid same-team remote players and, when the count differs
// from the cached one at this+0x1C, rebuilds the panel (0x00519E9E), which hides all seven remote
// slots and re-shows one per player (RE-11). The count is taken with
//     00519967  75 01   jne +1          ; different team -> skip
//     00519969  43      inc ebx         ; <-- this
// NOPing that one byte makes the count always 0: it equals the cache, the rebuild never runs, and
// the remote slots are never shown. Your own portraits are a different panel and are untouched.
// It also removes RE-11's stuck rebuild. [UI] ShowOtherPlayers=1 leaves the game as it is.
#include "siegehost.h"
#include "engine.h"
#include <cstring>

namespace {
const DWORD kCountIncVa = 0x00519969;
const BYTE kCountIncExpect[] = {0x75, 0x01, 0x43};  // from 0x00519967
bool g_on = false;
// UIShell::ShowGroup(const char* group, bool show, bool, const char* interface) - the same call the
// engine's own rebuild makes first (0x00519F1B..), used here to hide the seven remote slots once.
const auto ShowGroup = (void(__thiscall*)(void*, const char*, bool, bool, const char*))0x006E8D2A;
DWORD g_last = 0;
}

// Players' PCs: the rebuild that normally hides the remote slots never runs now, so the interface's
// default (visible, empty) would stay. Once a second in game, hide any remote slot group that is
// visible, exactly as the engine's rebuild does.
void PartyPanelTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (now - g_last < 1000) return;
    g_last = now;
    void* ui = eng::UI();
    if (!ui) return;
    char name[48], group[32];
    for (int i = 2; i <= 8; ++i) {
        sprintf(name, "awp_itemslot_portrait_%d", i);
        void* w = eng::FindWindow(name, "team_portraits");
        if (!w || !*((unsigned char*)w + 0x108)) continue;
        sprintf(group, "character_%d_mp", i);
        ShowGroup(ui, group, false, false, "team_portraits");
    }
}

void PartyPanelInstall() {
    if (Cfg().showOtherPlayers) { Log("PartyPanel: off ([UI] ShowOtherPlayers=1) - other players' portraits shown as usual."); return; }
    if (memcmp((void*)(kCountIncVa - 2), kCountIncExpect, sizeof kCountIncExpect) != 0) {
        Log("PartyPanel: VERIFY FAILED at 0x%08lX - panel left as the game has it.", kCountIncVa);
        return;
    }
    DWORD o;
    if (!VirtualProtect((void*)kCountIncVa, 1, PAGE_EXECUTE_READWRITE, &o)) {
        Log("PartyPanel: VirtualProtect failed (error %lu) - panel left as the game has it.", GetLastError());
        return;
    }
    *(BYTE*)kCountIncVa = 0x90;
    VirtualProtect((void*)kCountIncVa, 1, o, &o);
    FlushInstructionCache(GetCurrentProcess(), (void*)kCountIncVa, 1);
    g_on = true;
    Log("PartyPanel: armed - the top-left panel shows only your own character (patched 0x%08lX).", kCountIncVa);
}

// LAB ONLY (SIEGEHOST_CHATDIAG=1): which remote portrait slots are visible, from the engine.
void PartyPanelDiagTick() {
    static int on = -1;
    if (on < 0) { char e[4]; on = GetEnvironmentVariableA("SIEGEHOST_CHATDIAG", e, sizeof e) ? 1 : 0; }
    if (!on) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 3000) return;
    last = now;
    std::string s;
    char name[48], b[16];
    for (int i = 1; i <= 8; ++i) {
        sprintf(name, "awp_itemslot_portrait_%d", i);
        void* w = eng::FindWindow(name, "team_portraits");
        sprintf(b, " %d:%s", i, w ? (*((unsigned char*)w + 0x108) ? "vis" : "hid") : "-");
        s += b;
    }
    Log("PARTYDIAG: slots%s", s.c_str());
    // players and their world location (Player+0xF4, set by Player::RCSetWorldLocation)
    void* srv = *(void**)0x007ACF70;
    if (srv) {
        std::string pl;
        void** it = *(void***)((char*)srv + 4); void** end = *(void***)((char*)srv + 8);
        for (; it && it != end; ++it) {
            void* q = *it;
            if (!eng::Player_IsValid(q)) continue;
            const wchar_t* w = *(const wchar_t**)((char*)q + 0x58);
            char nm[40] = "?"; if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, nm, sizeof nm, 0, 0);
            char b2[80]; sprintf(b2, " %s%s=%u", q == eng::Server_GetLocalHumanPlayer(srv) ? "*" : "", nm, *(unsigned*)((char*)q + 0xF4)); pl += b2;
        }
        Log("PARTYDIAG: locations%s", pl.c_str());
    }
}
