// autojoin.cpp — client side: walks Main menu -> Multiplayer -> Internet -> Connect(<server IP>) automatically.
// The player still picks/creates their character in the staging screen.
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

// End-game summary disconnect fix:
// When a player clicks "Save and Exit" / "Leave Game", DSLOA enters end_game_summary and calls
// FUN_0049211d (0x0049211D), which shows chat_disabled_text ("The connection has been terminated.",
// centered at x=193..377 inside chat_text_background 24..546) and hides end_game_chat_editbox,
// combo_box_players, and end_game_send_to — but forgets to hide staging_game_chat_outline
// (353..545), whose left border at x=353 slices straight through the text.
typedef void (__cdecl* DisableEndGameChatFn)();
DisableEndGameChatFn g_origDisableEndGameChat = nullptr;
bool g_endGameFixHooked = false;

void FixEndGameChatDisabledLayout() {
    void* txt = eng::FindWindow("chat_disabled_text", "end_game_summary");
    if (!txt || IsBadReadPtr(txt, 0x1C0)) return;
    if (*(const char*)((const char*)txt + 0x108) == 0) return;  // only when chat_disabled_text is actually shown

    // 1. Hide the orphaned Send-To dropdown border box that cuts across the text at x=353
    void* outline = eng::FindWindow("staging_game_chat_outline", "end_game_summary");
    if (outline && !IsBadReadPtr(outline, 0x110) && *(const char*)((const char*)outline + 0x108) != 0) {
        typedef void (__thiscall* SetVisFn)(void*, bool);
        ((SetVisFn)0x0047B06E)(outline, false);
    }

    // 2. Ensure chat_disabled_text is centered cleanly inside chat_text_background
    void* bg = eng::FindWindow("chat_text_background", "end_game_summary");
    if (bg && !IsBadReadPtr(bg, 0x70)) {
        int wantL = *(const int*)((const char*)bg + 0x60) + 4;
        int wantT = *(const int*)((const char*)bg + 0x64) + 4;
        int wantR = *(const int*)((const char*)bg + 0x68) - 4;
        int wantB = *(const int*)((const char*)bg + 0x6C) - 4;
        int curL  = *(const int*)((const char*)txt + 0x60);
        int curT  = *(const int*)((const char*)txt + 0x64);
        int curR  = *(const int*)((const char*)txt + 0x68);
        int curB  = *(const int*)((const char*)txt + 0x6C);
        if (wantR > wantL && wantB > wantT &&
            (curL != wantL || curT != wantT || curR != wantR || curB != wantB))
        {
            // UIWindow::SetRect(int left, int right, int top, int bottom, bool bAnimation)
            typedef void (__thiscall* SetRectFn)(void*, int, int, int, int, bool);
            ((SetRectFn)0x0047AFAB)(txt, wantL, wantR, wantT, wantB, false);
        }
    }
    if (*(const int*)((const char*)txt + 0x1BC) != 2) {
        typedef void (__thiscall* SetJustFn)(void*, int);
        ((SetJustFn)0x0047B4AA)(txt, 2 /*center*/);
    }
}

void __cdecl HookDisableEndGameChat() {
    if (g_origDisableEndGameChat) {
        g_origDisableEndGameChat();
    }
    FixEndGameChatDisabledLayout();
}
}  // namespace

void EndGameSummaryFix_Install() {
    if (g_endGameFixHooked) return;
    const DWORD kTarget = 0x0049211D;
    // Verified prologue: push ebp; mov ebp, esp; push ecx; push ecx; push edi
    static const BYTE kExpect[6] = {0x55, 0x8B, 0xEC, 0x51, 0x51, 0x57};
    if (memcmp((const void*)kTarget, kExpect, sizeof(kExpect)) != 0) {
        Log("EndGameFix: prologue mismatch at 0x%08lX; skipping hook.", kTarget);
        return;
    }

    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return;
    memcpy(tramp, kExpect, sizeof(kExpect));
    tramp[6] = 0xE9;
    *(DWORD*)(tramp + 7) = (DWORD)((kTarget + sizeof(kExpect)) - ((DWORD)(tramp + 6) + 5));
    g_origDisableEndGameChat = (DisableEndGameChatFn)tramp;

    BYTE patch[6] = {0xE9, 0, 0, 0, 0, 0x90};
    *(DWORD*)(patch + 1) = (DWORD)((BYTE*)&HookDisableEndGameChat - (BYTE*)(kTarget + 5));

    DWORD oldProt = 0;
    if (!VirtualProtect((LPVOID)kTarget, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProt)) {
        g_origDisableEndGameChat = nullptr;
        return;
    }
    memcpy((void*)kTarget, patch, sizeof(patch));
    VirtualProtect((LPVOID)kTarget, sizeof(patch), oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)kTarget, sizeof(patch));
    g_endGameFixHooked = true;
    Log("EndGameFix: hooked DisableEndGameChat at 0x%08lX (hides staging_game_chat_outline & aligns chat_disabled_text).", kTarget);
}


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
        static bool yesPressed = false;
        if (!escSent && now - ingameAt > 4000 && GetEnvironmentVariableA("SIEGEHOST_LAB_ENDGAME", envx, sizeof envx)) {
            escSent = true;
            void* uiGame = *(void**)0x007ACF8C;
            if (uiGame) {
                typedef uint8_t (__thiscall* OnEscFn)(void*);
                ((OnEscFn)0x004EF436)(uiGame);
            }
            Log("LabPlay: ENDGAME Esc triggered (in_game_menu visible=%d)", eng::InterfaceVisible("in_game_menu"));
        } else if (escSent && !exitPressed && eng::InterfaceVisible("in_game_menu") && GetEnvironmentVariableA("SIEGEHOST_LAB_ENDGAME", envx, sizeof envx)) {
            exitPressed = true;
            Act("labplay: in-game menu SAVE AND EXIT", eng::Press("exit_game_button_client", "in_game_menu"));
        } else if (exitPressed && !yesPressed && eng::InterfaceVisible("backend_dialog") && GetEnvironmentVariableA("SIEGEHOST_LAB_ENDGAME", envx, sizeof envx)) {
            yesPressed = true;
            Act("labplay: backend_dialog YES", eng::Press("backend_button_yes", "backend_dialog"));
        } else if (!escSent && now - ingameAt > 90000 && GetEnvironmentVariableA("SIEGEHOST_LAB_SAVEEXIT", envx, sizeof envx)) {
            escSent = true;
            keybd_event(VK_ESCAPE, 0, 0, 0);
            keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            Log("LabPlay: ESC sent (in_game_menu visible=%d)", eng::InterfaceVisible("in_game_menu"));
        } else if (escSent && !exitPressed && now - ingameAt > 94000 && GetEnvironmentVariableA("SIEGEHOST_LAB_SAVEEXIT", envx, sizeof envx)) {
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
    if (eng::InterfaceVisible("end_game_summary")) {
        FixEndGameChatDisabledLayout();
    }
    if (left && !leftAt) {
        leftAt = now;
        Log("ExitAfterGame: the player left the game (%s). Closing the game in 5 s so the launcher can back up the character.", ws);
        Snapshot("left_game");
    }
    static int endDiagCount = 0;
    static DWORD endDiagLast = 0;
    if (eng::InterfaceVisible("end_game_summary") && endDiagCount < 2 && (endDiagCount == 0 || now - endDiagLast >= 500)) {
        char envEnd[4];
        if (GetEnvironmentVariableA("SIEGEHOST_LAB_ENDGAME", envEnd, sizeof envEnd)) {
            ++endDiagCount;
            endDiagLast = now;
            void* uiShell = eng::UIShell_Singleton();
            int sw = uiShell ? *(const int*)((const char*)uiShell + 0x60) : 0;
            int sh = uiShell ? *(const int*)((const char*)uiShell + 0x64) : 0;
            Log("ENDGAME_DIAG[#%d]: screen=%dx%d", endDiagCount, sw, sh);
            static const char* kWins[] = {
                "background", "chat_title", "chat_background", "chat_text_background",
                "staging_game_chat_outline", "end_game_send_to", "combo_box_players",
                "button_combo_show_players", "end_game_chat_editbox", "chat_disabled_text",
                "end_game_close", "help_background"
            };
            for (const char* wn : kWins) {
                void* w = eng::FindWindow(wn, "end_game_summary");
                if (!w) { Log("ENDGAME_DIAG:   %-26s = NULL", wn); continue; }
                int l = *(const int*)((const char*)w + 0x60);
                int t = *(const int*)((const char*)w + 0x64);
                int r = *(const int*)((const char*)w + 0x68);
                int b = *(const int*)((const char*)w + 0x6C);
                int vis = (*(const char*)((const char*)w + 0x108) != 0) ? 1 : 0;
                int z = *(const int*)((const char*)w + 0x70);
                void* par = *(void**)((const char*)w + 0x88);
                int typ = *(const int*)((const char*)w + 0x04);
                if (strcmp(wn, "chat_disabled_text") == 0) {
                    int just = *(const int*)((const char*)w + 0x1BC);
                    void* font = *(void**)((const char*)w + 0x1B0);
                    int strW = 0, strH = 0;
                    if (font) {
                        typedef void (__thiscall* CalcSzFn)(void*, const char*, int*, int*);
                        ((CalcSzFn)0x00668B77)(font, "The connection has been terminated.", &strW, &strH);
                    }
                    int drawY = ((b - t) / 2 - strH / 2) + t;
                    int drawX = (just == 2) ? (((r - l) / 2 - strW / 2) + l) : (just == 1 ? (r - strW) : l);
                    if (drawX < l) drawX = l;
                    Log("ENDGAME_DIAG:   %-26s type=%d vis=%d z=%d par=%p rect=(%d,%d,%d,%d) just=%d font=%p str=(%dx%d) draw=(%d,%d)..(%d,%d)",
                        wn, typ, vis, z, par, l, t, r, b, just, font, strW, strH, drawX, drawY, drawX + strW, drawY + strH);
                } else {
                    Log("ENDGAME_DIAG:   %-26s type=%d vis=%d z=%d par=%p rect=(%d,%d,%d,%d)",
                        wn, typ, vis, z, par, l, t, r, b);
                }
            }
        }
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
