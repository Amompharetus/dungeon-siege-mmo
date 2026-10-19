// autojoin.cpp — client side: walks Main menu -> Multiplayer -> Internet -> Connect(<server IP>) automatically.
// The player still picks/creates their character in the staging screen. See 03-analysis/RE-02-ui-automation.md.
#include "siegehost.h"
#include "engine.h"
#include <cstring>
#include <string>
#include <vector>
#include <winsock2.h>
#include <iphlpapi.h>

namespace {
DWORD g_lastTick = 0, g_lastAction = 0, g_nextConnect = 0, g_lastConnect = 0;
bool g_waitLogged = false;
int g_connectAttempts = 0;
bool g_done = false;
DWORD g_snap = 0;
std::string g_lastObs;

bool Has(const std::string& vis, const char* n) {
    std::string key = "," + vis + ",";
    return key.find("," + std::string(n) + ",") != std::string::npos;
}

// Addresses to try. For a server on this same PC (127.0.0.1) also try this PC's own IPv4 addresses (LAN, VPN), because
// DirectPlay may answer from / bind to those instead of loopback.
std::vector<std::string> g_addrs;
void BuildAddresses(const std::string& primary) {
    g_addrs.push_back(primary);
    if (primary != "127.0.0.1" && primary != "localhost") return;
    ULONG len = 0;
    GetAdaptersInfo(nullptr, &len);
    std::vector<BYTE> buf(len ? len : 1);
    if (len && GetAdaptersInfo((IP_ADAPTER_INFO*)buf.data(), &len) == NO_ERROR) {
        for (auto* a = (IP_ADAPTER_INFO*)buf.data(); a; a = a->Next)
            for (auto* ip = &a->IpAddressList; ip; ip = ip->Next) {
                std::string s = ip->IpAddress.String;
                if (s.empty() || s == "0.0.0.0") continue;
                bool dup = false;
                for (auto& x : g_addrs) dup |= (x == s);
                if (!dup) g_addrs.push_back(s);
            }
    }
    std::string all;
    for (auto& x : g_addrs) all += x + " ";
    Log("AutoJoin: addresses to try: %s", all.c_str());
}

// Session diagnostics: dump the game's NetPipe session list (what the "Looking for game" loop reads, 0x71E4D6).
void DumpSessions(const char* why) {
    void* np = *(void**)0x007ACF64;
    if (!np) { Log("AutoJoin[diag] %s: NetPipe=null", why); return; }
    char* b = (char*)np;
    void** it = *(void***)(b + 0x90), **end = *(void***)(b + 0x94);
    int n = (it && end >= it) ? (int)(end - it) : -1;
    Log("AutoJoin[diag] %s: sessions=%d cur9C=%p A0=%p flagA4=%d gameTime=%.3f tick=%lu", why, n, *(void**)(b + 0x9C),
        *(void**)(b + 0xA0), *(BYTE*)(b + 0xA4), *(double*)0x007AA250, GetTickCount());
    for (int i = 0; i < n && i < 8; ++i) {
        char* ss = (char*)it[i];
        if (!ss) continue;
        Log("AutoJoin[diag]   #%d %p t20=%.3f bC6=%d bBC=%d players=%lu", i, (void*)ss, *(double*)(ss + 0x20), *(BYTE*)(ss + 0xC6),
            *(BYTE*)(ss + 0xBC), *(DWORD*)(ss + 0xDC));
    }
}

void Act(const char* what, bool ok) {
    Log("AutoJoin: %s -> %s", what, ok ? "ok" : "FAILED");
    g_lastAction = GetTickCount();
}
}  // namespace


// Lab-only (SIEGEHOST_LABPLAY env): after joining, create/select a character and click Ready like a player would,
// then log where the hero spawned. Used to verify StartLocation. Never set on players' PCs.
static void LabPlayTick() {
    static DWORD last = 0, ingameAt = 0;
    static int created = 0, selectTries = 0;
    static bool readyDone = false, reported = false;
    DWORD now = GetTickCount();
    if (now - last < 1500) return;
    last = now;
    const char* ws = eng::CurrentWorldState();
    if (strstr(ws, "_ingame")) {
        if (!ingameAt) { ingameAt = now; Log("LabPlay: in game (%s)", ws); }
        if (!reported && now - ingameAt > 15000) {
            reported = true;
            void* pl = eng::LocalPlayer();
            Log("LabPlay: local player %p start group='%s'", pl, pl ? eng::GpC(pl, 0x4C) : "?");
            void* wm = eng::WorldMap_Singleton();
            if (wm) Log("LabPlay: client WorldMap map='%s' world='%s'", eng::GpC(wm, 4), eng::GpC(wm, 0xC));
            Snapshot("labplay_ingame");
        }
        static bool exitPressed = false;
        char envx[4];
        static bool escSent = false;
        if (!escSent && now - ingameAt > 90000 && GetEnvironmentVariableA("SIEGEHOST_LAB_SAVEEXIT", envx, sizeof envx)) {
            escSent = true;
            keybd_event(VK_ESCAPE, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            Log("LabPlay: ESC sent (in_game_menu visible=%d)", eng::InterfaceVisible("in_game_menu"));
        } else if (escSent && !exitPressed && now - ingameAt > 94000) {
            exitPressed = true;
            Log("LabPlay: in_game_menu visible=%d", eng::InterfaceVisible("in_game_menu"));
            Act("labplay: in-game menu SAVE AND EXIT", eng::Press("resume_button_client", "in_game_menu"));
        }
        static int pstep = 0;
        if (reported && pstep == 0 && now - ingameAt > 60000) {
            pstep = 1;
            Log("LabPlay: PAUSE requested by client (paused before=%d)", eng::GamePaused());
            if (void* tg = eng::TattooGame_Singleton()) eng::TattooGame_RequestPause(tg, true);
        } else if (pstep == 1 && now - ingameAt > 66000) {
            pstep = 2;
            Log("LabPlay: 6 s after pause request: client paused=%d", eng::GamePaused());
        } else if (pstep == 2 && now - ingameAt > 80000) {
            pstep = 3;
            Log("LabPlay: 20 s after pause request: client paused=%d; requesting unpause", eng::GamePaused());
            if (void* tg = eng::TattooGame_Singleton()) eng::TattooGame_RequestPause(tg, false);
        }
        return;
    }
    const char* sc = "staging_area_create_character";
    const char* ss = "staging_area_select_character";
    if (eng::InterfaceVisible(sc)) {
        eng::SetEditText("staging_char_name_edit_box", sc, "LabHero");
        Act("labplay: create character OK", eng::Press("staging_create_character_ok", sc));
        ++created;
    } else if (eng::InterfaceVisible(ss)) {
        if (selectTries++ < 3) {
            eng::Send(eng::MSG_ONLBUTTONDOWN, "character1_bg", ss);
            Act("labplay: select character OK", eng::Press("staging_character_ok", ss));
        } else if (created < 2) {
            selectTries = 0;
            Act("labplay: create new", eng::Press("staging_character_create", ss));
        }
    } else if (eng::InterfaceVisible("staging_area")) {
        if (!eng::LocalHeroChosen()) {
            static char name[32];
            const char* btn = "button_select_char1";
            for (int i = 1; i <= 8; ++i) {
                sprintf(name, "button_select_char%d", i);
                if (eng::WindowVisible(name, "staging_area_players")) { btn = name; break; }
            }
            Act("labplay: open character select", eng::Press(btn, "staging_area_players"));
        } else if (!readyDone) {
            readyDone = true;
            void* pl = eng::LocalPlayer();
            char sg[32];  // lab only: SIEGEHOST_LAB_STARTGROUP=crystwind asks the server for another start town
            if (pl && GetEnvironmentVariableA("SIEGEHOST_LAB_STARTGROUP", sg, sizeof sg)) {
                eng::GpStr g(sg);
                eng::Player_RSSetStartingGroup(pl, &g.s);
            }
            Log("LabPlay: staging start group='%s'", pl ? eng::GpC(pl, 0x4C) : "?");
            Snapshot("labplay_staging");
            Act("labplay: check Ready", eng::Check("staging_checkbox_ready", "staging_area"));
        }
    }
}

// 0.6.1: after the player leaves the MMO game (Save and exit -> game summary, kicked, server lost), close DSLOA.exe so
// the launcher uploads the character right away instead of the player being left in the multiplayer menus.
static void ExitAfterGameTick() {
    if (!Cfg().exitAfterGame) return;
    static bool wasInGame = false;
    static DWORD leftAt = 0, lastLog = 0;
    static std::string lastVis;
    DWORD now = GetTickCount();
    const char* ws = eng::CurrentWorldState();
    if (strstr(ws, "_ingame")) { wasInGame = true; leftAt = 0; return; }
    // 0.7.1: also arm in the staging area, so "Leave Game" from staging closes the game too. Safe
    // against the way in: AutoJoin sets g_done the moment staging is reached and stops retrying, so
    // the menus we pass through before staging can never trigger this.
    if (strstr(ws, "staging_area")) { wasInGame = true; leftAt = 0; return; }
    if (!wasInGame) return;
    static const char* kWatch[] = {"end_game_summary", "mp_dialog", "in_game_menu", "main_menu", "multiplayer_provider", "internet_game_menu", "message_box_large"};
    std::string vis;
    for (const char* n : kWatch)
        if (eng::InterfaceVisible(n)) { if (!vis.empty()) vis += ","; vis += n; }
    if (vis != lastVis || now - lastLog > 5000) { lastVis = vis; lastLog = now; Log("ExitAfterGame: worldstate=%s visible=%s", ws, vis.c_str()); }
    bool left = vis.find("end_game_summary") != std::string::npos || strstr(ws, "main_menu") || strstr(ws, "provider_select") ||
                strstr(ws, "internet_game") || strstr(ws, "session_lost") || strstr(ws, "game_ended");
    if (left && !leftAt) {
        leftAt = now;
        Log("ExitAfterGame: the player left the game (%s). Closing the game in 5 s so the launcher can back up the character.", ws);
        Snapshot("left_game");
    }
    if (leftAt && now - leftAt >= 5000) {
        Log("ExitAfterGame: closing Dungeon Siege now.");
        FlushAndExit(0);
    }
}


// LAB ONLY (SIEGEHOST_JIPDIAG=1): the client's own view of whether the world is moving. The world
// clock ([0x7ACF50]+0x10) advances only when the client actually simulates, so this is the direct
// answer to "is the game frozen on a player's PC while someone else sits in the staging screen?".
static void ClientJipDiag() {
    static int on = -1;
    if (on < 0) { char e[4]; on = GetEnvironmentVariableA("SIEGEHOST_JIPDIAG", e, sizeof e) ? 1 : 0; }
    if (!on) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 2000) return;
    last = now;
    // Ask the shell directly whether the join-in-progress panel and the grey jip_glass overlay are
    // up. Screenshots of a stationary character are byte-identical frame to frame and prove nothing;
    // UIShell::IsInterfaceVisible is the engine's own answer.
    Log("CLIENTDIAG: t=%.3f sim=%u pause=%d jip_panel=%d jip_glass=%d in_game_menu=%d worldstate=%s",
        eng::WorldTime(), eng::WorldSimCount(), eng::GamePauseCount(),
        eng::InterfaceVisible("join_in_progress") ? 1 : 0, eng::InterfaceVisible("jip_glass") ? 1 : 0,
        eng::InterfaceVisible("in_game_menu") ? 1 : 0, eng::CurrentWorldState());
    auto rb = [](DWORD globalVa, int off) -> int {
        void* o = *(void**)globalVa;
        return o ? (int)*(unsigned char*)((char*)o + off) : -1;
    };
    Log("CLIENTDIAG-UI: wo46=%d uigame6d=%d menu5=%d pui58=%d character=%d world_map=%d",
        rb(0x007ACFD4, 0x46), rb(0x007ACF8C, 0x6D), rb(0x007ACFA4, 5), rb(0x007ACFA4, 0x58),
        eng::InterfaceVisible("character") ? 1 : 0, eng::InterfaceVisible("world_map") ? 1 : 0);
}

void AutoJoinTick() {
    const Config& c = Cfg();
    NR_Tick();
    ClientJipDiag();
    if (g_done) ExitAfterGameTick();
    if (g_done) {
        char e[4];
        static int lab = -1;
        if (lab < 0) lab = GetEnvironmentVariableA("SIEGEHOST_LABPLAY", e, sizeof e) ? 1 : 0;
        if (lab) LabPlayTick();
        return;
    }
    if (c.joinAddress.empty()) return;
    DWORD now = GetTickCount();
    if (now - g_lastTick < 250) return;
    g_lastTick = now;

    const char* ws = eng::CurrentWorldState();
    // Once we pressed Connect, any world state that is not one of the front-end menus means the join is progressing
    // (staging, character select, JIP loading, in game) -> hand over to the player.
    static const char* kMenus[] = {"ws_init", "ws_intro", "ws_logo", "ws_main_menu", "ws_mp_provider_select",
                                   "ws_mp_internet_game", "ws_mp_lan_game", "ws_mp_zonematch", "(no WorldState)", "(null)"};
    bool inMenus = false;
    for (const char* m : kMenus) if (strcmp(ws, m) == 0) inMenus = true;
    if (strstr(ws, "staging_area_client") || strstr(ws, "_ingame") || strstr(ws, "character_select") || strstr(ws, "loading") ||
        (g_connectAttempts > 0 && !inMenus)) {
        Log("AutoJoin: reached %s — handing control to the player.", ws);
        g_done = true;
        return;
    }

    static const char* kWatch[] = {"main_menu", "multiplayer_provider", "internet_game_menu", "mp_dialog", "zonematch"};
    std::string vis;
    for (const char* n : kWatch)
        if (eng::InterfaceVisible(n)) { if (!vis.empty()) vis += ","; vis += n; }
    std::string obs = std::string(ws) + " | " + vis;
    if (obs != g_lastObs) { Log("AutoJoin: observe worldstate=%s visible=%s", ws, vis.c_str()); g_lastObs = obs; if (Cfg().snapshots) g_snap = now + 700; }
    if (g_snap && now >= g_snap) { g_snap = 0; Snapshot(obs.c_str()); }
    if (now - g_lastAction < 1200) return;

    if (Has(vis, "mp_dialog")) {
        // right after Connect the game shows "Looking for game... (N)" in mp_dialog, whose only button is
        // Cancel. Pressing it aborted every join. Leave the dialog alone for 45 s after Connect ("Connecting to game..." also uses it); only a dialog that is
        // still open after that (a real error) gets dismissed.
        if (g_lastConnect && now - g_lastConnect < 45000) {
            static DWORD lastDump = 0;
            if (now - lastDump >= 2000) { lastDump = now; DumpSessions("waiting"); }
            if (!g_waitLogged) { Log("AutoJoin: mp_dialog open (\"Looking for game...\") — waiting, not cancelling"); g_waitLogged = true; }
            return;
        }
        Act("dismiss mp_dialog (connection error?) — retry in 10 s", eng::Press("mp_dialog_button_ok", "mp_dialog"));
        g_nextConnect = now + 10000;
    } else if (Has(vis, "internet_game_menu")) {
        static bool selftest = false;
        char env[8];
        if (!selftest && GetEnvironmentVariableA("SIEGEHOST_SELFTEST", env, sizeof env)) {  // lab only
            selftest = true;
            static const char* k = "4B000000062F0000006D313130020100320000003800D3070A0010000100000000001000000000000100215036C701000000029FF852CD5DE15A64522D7FF1E68EA7792800000001000000";
            BYTE blob[75];
            for (int i = 0; i < 75; ++i) { unsigned v; sscanf(k + i * 2, "%2x", &v); blob[i] = (BYTE)v; }
            Log("AutoJoin: SELFTEST CompareSessionInfo with host blob");
            CompareSessionInfo(blob);
        }
        if (now < g_nextConnect) return;
        if (++g_connectAttempts > c.joinRetries) {
            Log("AutoJoin: giving up after %d attempts; the player can connect manually.", c.joinRetries);
            g_done = true;
            return;
        }
        Act("internet: set player name", eng::SetEditText("internet_player_name_edit_box", "internet_game_menu", c.playerName.c_str()));
        if (g_addrs.empty()) BuildAddresses(c.joinAddress);
        const std::string& addr = g_addrs[(g_connectAttempts - 1) % g_addrs.size()];
        Act("internet: set server IP", eng::SetEditText("ip_edit_box", "internet_game_menu", addr.c_str()));
        char what[128];
        sprintf(what, "internet: Connect to %s (attempt %d)", addr.c_str(), g_connectAttempts);
        Act(what, eng::Press("button_connect", "internet_game_menu"));
        g_lastConnect = now;
        DumpSessions("after Connect");
        g_waitLogged = false;
        g_nextConnect = now + 50000;
    } else if (Has(vis, "multiplayer_provider")) {
        Act("provider: Internet", eng::Press("button_internet", "multiplayer_provider"));
    } else if (Has(vis, "main_menu")) {
        Act("main_menu: Multiplayer", eng::Press("button_multi_player", "main_menu"));
    }
}
