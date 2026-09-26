// autohost.cpp — drives the DS1 front-end menus so a dedicated host starts a multiplayer game unattended.
// Runs on the game's main thread (called from the PeekMessageA hook). See 03-analysis/RE-02-ui-automation.md.
#include "siegehost.h"
#include "engine.h"
#include "pauseguard.h"
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

enum class Phase { Frontend, Staging, InGame };

DWORD g_start = 0, g_lastTick = 0, g_lastAction = 0, g_lastIdleLog = 0, g_lastHeartbeat = 0;
DWORD g_ingameSince = 0;
std::string g_lastObservation;
int g_selectOkAttempts = 0;
int g_startAttempts = 0;
bool g_stagingConfigured = false;
bool g_readyChecked = false;
int g_heroLogged = -1;           // last logged LocalHeroChosen() state
int g_createdCharacters = 0;
bool g_inGameAnnounced = false;
DWORD g_snapPending = 0;

bool Contains(const char* s, const char* sub) { return s && strstr(s, sub) != nullptr; }

bool g_mapLogged = false;
DWORD g_lastStartEnforce = 0;

// Map + start location (0.5.3). The game keeps these in WorldMap; the staging map list calls WorldMap::SSet.
void ConfigureMap(const Config& c) {
    void* wm = eng::WorldMap_Singleton();
    if (!wm) return;
    const char* curMap = eng::GpC(wm, 4);
    const char* curWorld = eng::GpC(wm, 0xC);
    if (!g_mapLogged) {
        g_mapLogged = true;
        Log("AutoHost: map before: map='%s' world='%s' (wanted map='%s' world='%s' start='%s')", curMap, curWorld, c.map.c_str(),
            c.mpWorld.c_str(), c.startLocation.c_str());
    }
    if (!c.map.empty()) {
        const char* world = c.mpWorld.empty() ? curWorld : c.mpWorld.c_str();
        if (_stricmp(curMap, c.map.c_str()) != 0 || (!c.mpWorld.empty() && _stricmp(curWorld, world) != 0)) {
            std::string w = world;  // curWorld may be freed by SSet
            bool ok = eng::WorldMap_SSet(wm, c.map.c_str(), w.c_str(), 0);
            Log("AutoHost: WorldMap::SSet('%s','%s') -> %d; now map='%s' world='%s'", c.map.c_str(), w.c_str(), ok,
                eng::GpC(wm, 4), eng::GpC(wm, 0xC));
            void** b = *(void***)((char*)wm + 0x1C);
            void** e = *(void***)((char*)wm + 0x20);
            std::string groups;
            for (char* g = (char*)b; g && g < (char*)e; g += 0x3C) {
                if (!groups.empty()) groups += ",";
                groups += eng::GpC(g, 0x14);
                if (g[0x10]) groups += "(default)";
            }
            Log("AutoHost: start groups on this map: %s", groups.c_str());
        }
    }
    if (void* srv = eng::Server_Singleton()) {
        if (g_lastStartEnforce == 0) {
            // 0.7.0: with AllowStartLocationSelection every player picks their own start town in the
            // staging screen (the engine still gates towns by its own required level per group, and
            // sends anyone below it back to the map default). Otherwise everyone is forced to
            // StartLocation, which is what 0.5.3-0.6.1 did.
            bool allow = c.allowStartSelect != 0;
            if (allow || !c.startLocation.empty()) {  // a config that sets neither is left alone
                eng::Server_RCSetAllowStartLocationSelection(srv, allow);
                Log("AutoHost: Server::RCSetAllowStartLocationSelection(%s)%s", allow ? "true" : "false",
                    allow && !c.startLocation.empty() ? " - players choose; StartLocation is only the default" : "");
                g_lastStartEnforce = GetTickCount();
            }
        }
    }
}

// Every player (host and joiners) spawns at StartLocation. Called in staging and in game.
void EnforceStartLocation(const Config& c) {
    if (c.allowStartSelect) return;  // the player's own choice wins
    if (c.startLocation.empty()) return;
    DWORD now = GetTickCount();
    if (g_lastStartEnforce && now - g_lastStartEnforce < 1000) return;
    g_lastStartEnforce = now;
    void* srv = eng::Server_Singleton();
    if (!srv) return;
    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    for (void** it = b; it && it < e; ++it) {
        void* pl = *it;
        if (!eng::Player_IsValid(pl)) continue;
        if (*(int*)((char*)pl + 0x14) == 2) continue;  // same filter the game uses when resetting start groups
        std::string cur = eng::GpC(pl, 0x4C);
        if (_stricmp(cur.c_str(), c.startLocation.c_str()) == 0) continue;
        eng::GpStr g(c.startLocation.c_str());
        eng::Player_RSSetStartingGroup(pl, &g.s);
        Log("AutoHost: start group of player %p: '%s' -> '%s' (now '%s')", pl, cur.empty() ? "(empty)" : cur.c_str(), c.startLocation.c_str(),
            eng::GpC(pl, 0x4C));
    }
}

void Act(const char* what, bool ok) {
    Log("AutoHost: %s -> %s", what, ok ? "ok" : "FAILED (window not found?)");
    g_lastAction = GetTickCount();
}

std::string VisibleInterfaces() {
    static const char* kWatch[] = {"main_menu", "multiplayer_provider", "lan_game_menu", "internet_game_menu", "host_game",
                                   "staging_area_players",
                                   "staging_area", "staging_area_select_character", "staging_area_create_character",
                                   "staging_area_import_character", "staging_area_game_settings", "staging_area_map_settings",
                                   "mp_dialog", "loading_screen", "in_game_menu", "zonematch"};
    std::string out;
    for (const char* n : kWatch)
        if (eng::InterfaceVisible(n)) { if (!out.empty()) out += ","; out += n; }
    return out.empty() ? "(none of watched)" : out;
}

void WriteHeartbeat(const char* ws, const char* phase) {
    DWORD now = GetTickCount();
    if (now - g_lastHeartbeat < 5000) return;
    g_lastHeartbeat = now;
    std::string path = LogDir() + "heartbeat.txt";
    if (FILE* f = fopen(path.c_str(), "w")) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "time=%04d-%02d-%02d %02d:%02d:%02d\npid=%lu\nphase=%s\nworldstate=%s\nplayers=%ld\nuptime_s=%lu\n", st.wYear,
                st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), phase, ws, RemotePlayerCount(),
                (now - g_start) / 1000);
        if (void* wm = eng::WorldMap_Singleton()) fprintf(f, "map=%s\nworld=%s\n", eng::GpC(wm, 4), eng::GpC(wm, 0xC));
        if (void* pl = eng::LocalPlayer()) fprintf(f, "start_location=%s\n", eng::GpC(pl, 0x4C));
        fprintf(f, "paused=%d\nworldtime=%.3f\n", eng::GamePaused() ? 1 : 0, eng::WorldTime());
        fputs(NR_HeartbeatLines().c_str(), f);
        fputs(JG_HeartbeatLines().c_str(), f);
        fputs(JG_JipHeartbeatLines().c_str(), f);
        fputs(ShopStockHeartbeatLines().c_str(), f);
        fputs(InvFixHeartbeatLines().c_str(), f);
        fputs(QuestModeHeartbeatLines().c_str(), f);
        fputs(JipQueueHeartbeatLines().c_str(), f);
        fclose(f);
    }
}

// LAB ONLY (SIEGEHOST_JIPDIAG=1). Dumps the world state and, for every valid Player, the JIP flag
// (Player+0x55, set by Player::RCSetJIP) and the player's own world state (Player+0xCC, set by
// Player::RCSetWorldState). Both are replicated to every machine, so the same dump works on a client.
// Used to prove what the world is actually waiting for while a joiner sits in the staging screen.
void JipDiagTick() {
    static int on = -1;
    if (on < 0) { char e[4]; on = GetEnvironmentVariableA("SIEGEHOST_JIPDIAG", e, sizeof e) ? 1 : 0; }
    if (!on) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    void* wsObj = eng::WorldState_Singleton();
    void* srv = eng::Server_Singleton();
    int ws = wsObj ? *(int*)((char*)wsObj + 0x20) : -1;
    int pend = wsObj ? *(int*)((char*)wsObj + 0x28) : -1;
    std::string line;
    if (srv) {
        void** b = *(void***)((char*)srv + 4);
        void** e = *(void***)((char*)srv + 8);
        int idx = 0;
        for (void** p = b; p && e && p != e; ++p, ++idx) {
            void* pl = *p;
            if (!pl || !eng::Player_IsValid(pl)) continue;
            int pws = *(int*)((char*)pl + 0xCC);
            int pnd = *(int*)((char*)pl + 0xD0);
            const char* pwsName = eng::WorldState_ToString(pws);
            char buf[192];
            _snprintf(buf, sizeof buf, " [#%d jip=%d ws=%s(%d) d0=%d frontend=%d]", idx, *((unsigned char*)pl + 0x55),
                      pwsName ? pwsName : "?", pws, pnd, eng::IsInFrontend(pws) ? 1 : 0);
            buf[sizeof buf - 1] = 0;
            line += buf;
        }
    }
    static std::string prev;
    std::string cur = std::string(eng::WorldState_ToString(ws) ? eng::WorldState_ToString(ws) : "?") + " pending=" +
                      std::to_string(pend) + line;
    // The world time is deliberately outside the change test: it is the thing being watched, and it
    // must be logged on a fixed cadence for "did the world advance between these two lines?" to mean
    // anything. Everything else only logs on change.
    static DWORD lastTime = 0;
    bool timeDue = now - lastTime >= 2000;
    if (cur == prev && !timeDue) return;
    if (timeDue) lastTime = now;
    prev = cur;
    Log("JIPDIAG: t=%.3f sim=%u pause=%d flags=0x%X [%s] world=%s", eng::WorldTime(), eng::WorldSimCount(),
        eng::GamePauseCount(), eng::GameFlags(), JG_JipSelfCheck().c_str(), cur.c_str());
}

}  // namespace

// Pause guard: the state machine and the measurements behind its timings live in pauseguard.h.
// 0.7.2 replaced a blanket 30-second stand-down after a join with a 5-second one, because the
// 30 seconds WAS the freeze players reported: the engine holds the pause from CREATE_PLAYER
// until something resumes it, and the joining client is already sitting in staging ~0.2 s in.
// (server trace) and (client log).

void AutoHostTick() {
    const Config& c = Cfg();
    DWORD now = GetTickCount();
    if (!g_start) { g_start = now; Log("AutoHost: armed (GameName='%s', HostCharacter='%s', MaxPlayers=%d)", c.gameName.c_str(), c.hostCharacter.c_str(), c.maxPlayers); }
    if (now - g_lastTick < 250) return;
    g_lastTick = now;

    NR_Tick();
    JipDiagTick();

    const char* ws = eng::CurrentWorldState();
    bool inGame = Contains(ws, "_ingame");
    WriteHeartbeat(ws, inGame ? "ingame" : "starting");
    JoinGuardServerTick();

    if (inGame) {
        EnforceStartLocation(c);
        // See the pause guard notes above AutoHostTick.
        static int guard = -1;
        if (guard < 0) { char e[4]; guard = GetEnvironmentVariableA("SIEGEHOST_NO_UNPAUSE_GUARD", e, sizeof e) ? 0 : 1; }
        static PauseGuardTuning tune;
        static bool tuneRead = false;
        if (!tuneRead) {  // lab override, e.g. SIEGEHOST_JOIN_GRACE_MS=30000 to get the old behaviour back
            tuneRead = true;
            char e[16];
            if (GetEnvironmentVariableA("SIEGEHOST_JOIN_GRACE_MS", e, sizeof e)) tune.joinGraceMs = (DWORD)atoi(e);
            Log("AutoHost: pause guard armed (join grace %lu ms, paused-for %lu ms, cooldown %lu ms, guard=%d)",
                tune.joinGraceMs, tune.pausedForMs, tune.cooldownMs, guard);
        }
        static PauseGuardState pg;
        static long lastCount = 0;
        long cnt = RemotePlayerCount();
        bool rose = cnt > lastCount;
        lastCount = cnt;

        bool paused = eng::GamePaused();
        DWORD heldBefore = pg.pausedSinceMs ? now - pg.pausedSinceMs : 0;
        DWORD sinceJoin = pg.lastJoinMs ? now - pg.lastJoinMs : 0;
        if (PauseGuardTick(pg, now, paused, rose, tune) && guard && !c.allowPausing) {
            if (void* tg = eng::TattooGame_Singleton()) eng::TattooGame_RequestPause(tg, false);
            // Both numbers are logged so that the engine re-asserting a pause shows up as a repeat in
            // the log, instead of having to be inferred from a heartbeat trace afterwards.
            Log("AutoHost: game paused for %lu ms while pausing is off -> resumed (paused now=%d, %lu ms after the last join)",
                heldBefore, eng::GamePaused(), sinceJoin);
        }
        if (!g_inGameAnnounced) {
            g_inGameAnnounced = true;
            g_ingameSince = now;
            Snapshot("in_game");
            Log("AutoHost: *** SERVER IS IN GAME (worldstate=%s) after %lu s — accepting players ***", ws, (now - g_start) / 1000);
            if (void* srv = eng::Server_Singleton()) {
                eng::Server_RCSetAllowJIP(srv, true);
                eng::Server_RCSetAllowPausing(srv, c.allowPausing != 0);
                Log("AutoHost: Server::RCSetAllowJIP(true), RCSetAllowPausing(%d) -> Server+0x28=%d", c.allowPausing,
                    *((unsigned char*)srv + 0x28));
            }
        }
        return;
    }
    if (g_inGameAnnounced) {
        Log("AutoHost: left the game (worldstate=%s). Exiting so the watchdog restarts a clean server.", ws);
        FlushAndExit(4);
    }

    // Watchdog for a stuck start sequence.
    if (c.stuckTimeoutSec > 0 && now - g_start > (DWORD)c.stuckTimeoutSec * 1000) {
        Snapshot("stuck");
        Log("AutoHost: STUCK — not in game after %d s (worldstate=%s, visible=%s). Exiting for restart.", c.stuckTimeoutSec, ws,
            VisibleInterfaces().c_str());
        FlushAndExit(3);
    }

    std::string vis = VisibleInterfaces();
    std::string obs = std::string(ws) + " | " + vis;
    if (obs != g_lastObservation) {
        Log("AutoHost: observe worldstate=%s visible=%s", ws, vis.c_str());
        g_lastObservation = obs;
        g_snapPending = now + 700;  // screenshot shortly after each screen change (UI has drawn)
    }

    if (g_snapPending && now >= g_snapPending) { g_snapPending = 0; Snapshot(obs.c_str()); }
    if (now - g_lastAction < 1500) return;  // let the UI settle after each action

    auto vis_has = [&](const char* n) {  // exact token match in comma list
        std::string key = "," + vis + ",";
        return key.find("," + std::string(n) + ",") != std::string::npos;
    };

    if (vis_has("mp_dialog")) {
        Act("dismiss mp_dialog (OK)", eng::Press("mp_dialog_button_ok", "mp_dialog"));
    } else if (vis_has("staging_area_create_character")) {
        bool a = eng::SetEditText("staging_char_name_edit_box", "staging_area_create_character", c.hostCharacter.c_str());
        Act("create host character: set name", a);
        Act("create host character: OK", eng::Press("staging_create_character_ok", "staging_area_create_character"));
        ++g_createdCharacters;
    } else if (vis_has("staging_area_select_character")) {
        const char* iface = "staging_area_select_character";
        if (g_selectOkAttempts < 3) {
            ++g_selectOkAttempts;
            // Select the first saved character (row 1) like a mouse click, then press OK.
            Act("select character: click row 1", eng::Send(eng::MSG_ONLBUTTONDOWN, "character1_bg", iface));
            Act("select character: OK", eng::Press("staging_character_ok", iface));
        } else if (g_createdCharacters < 2) {
            g_selectOkAttempts = 0;
            Act("select character: no usable character -> Create new", eng::Press("staging_character_create", iface));
        } else {
            Log("AutoHost: could not select or create a character (see screenshots/logs). Waiting for watchdog restart.");
            g_lastAction = now + 30000;
        }
    } else if (vis_has("host_game")) {
        Act("host_game: set game name", eng::SetEditText("game_name_edit_box", "host_game", c.gameName.c_str()));
        Act("host_game: OK", eng::Press("button_ok", "host_game"));
    } else if (vis_has("internet_game_menu")) {
        // Host through the Internet menu. The LAN menu (world state 0x21) turns DirectPlay compression off
        // (NetPipe+0x182), Internet (0x22) turns it on; clients compare that flag and refuse to join a mismatched host.
        Act("internet_game_menu: set player name",
            eng::SetEditText("internet_player_name_edit_box", "internet_game_menu", c.hostCharacter.c_str()));
        Act("internet_game_menu: Host Game", eng::Press("button_host", "internet_game_menu"));
    } else if (vis_has("lan_game_menu")) {
        Act("lan_game_menu: set player name", eng::SetEditText("lan_player_name_edit_box", "lan_game_menu", c.hostCharacter.c_str()));
        Act("lan_game_menu: Host Game", eng::Press("button_host", "lan_game_menu"));
    } else if (vis_has("staging_area")) {
        ConfigureMap(c);
        EnforceStartLocation(c);
        // Decide from the game's own Player data, not from which screens we saw.
        bool heroChosen = eng::LocalHeroChosen();
        if ((int)heroChosen != g_heroLogged) {
            g_heroLogged = heroChosen;
            void* pl = eng::LocalPlayer();
            Log("AutoHost: host hero chosen=%d (player=%p nameLen=%d templateLen=%d)", heroChosen, pl,
                pl ? eng::GpLen(pl, 0x58) : -1, pl ? eng::GpLen(pl, 0x64) : -1);
            if (heroChosen) { g_selectOkAttempts = 0; g_readyChecked = false; }
        }
        if (!g_stagingConfigured) {
            g_stagingConfigured = true;
            if (void* srv = eng::Server_Singleton()) {
                eng::Server_RCSetAllowJIP(srv, true);
                eng::Server_RCSetAllowPausing(srv, c.allowPausing != 0);
                if (c.maxPlayers > 0) eng::Server_RCSetMaxPlayers(srv, (unsigned)c.maxPlayers);
                Log("AutoHost: staging configured (AllowJIP=true, AllowPausing=%d, MaxPlayers=%d)", c.allowPausing, c.maxPlayers);
            }
        }
        if (!heroChosen) {
            // Open "Select character..." on the host's own row (the visible button), row 1 as fallback.
            const char* btn = "button_select_char1";
            static char name[32];
            for (int i = 1; i <= 8; ++i) {
                sprintf(name, "button_select_char%d", i);
                if (eng::WindowVisible(name, "staging_area_players")) { btn = name; break; }
            }
            char what[80];
            sprintf(what, "staging: open character select (%s)", btn);
            Act(what, eng::Press(btn, "staging_area_players"));
        } else if (!g_readyChecked) {
            g_readyChecked = true;
            Act("staging: check Ready", eng::Check("staging_checkbox_ready", "staging_area"));
        } else {
            ++g_startAttempts;
            Act("staging: Start Game", eng::Press("staging_area_button_start_game", "staging_area"));
            if (g_startAttempts % 5 == 0) g_readyChecked = false;  // re-assert ready occasionally
            g_lastAction = now + 3000;  // wait longer between start attempts
        }
    } else if (vis_has("multiplayer_provider")) {
        Act("multiplayer_provider: Internet", eng::Press("button_internet", "multiplayer_provider"));
    } else if (vis_has("main_menu")) {
        Act("main_menu: Multiplayer", eng::Press("button_multi_player", "main_menu"));
    } else if (now - g_lastIdleLog > 10000) {
        g_lastIdleLog = now;
        Log("AutoHost: waiting (worldstate=%s, visible=%s)", ws, vis.c_str());
    }
}
