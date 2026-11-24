// hosthide.cpp - hides the dedicated ServerHost character everywhere players can see or interact with it.
//
// Under DS1's listen-server model, ServerHost (slot 0 on the server machine) must stay alive in
// Server::m_Players and keep a spawned hero in Elddim so AutoHost, world frustums, ShopStock, and
// QuestMode work. Rather than deleting it, this module removes it from every player-facing surface:
//
//   Client-side (Role::Client, when [UI] ShowServerHost=0):
//     - Staging screen lobby (staging_area_players): layout 0x0048959E, status tick 0x004872DD,
//       ping updater 0x0048A3F1, start-location count 0x0048EEFD. (Client-only so server AutoHost
//       still sees and ticks Ready on its own row.)
//     - `\` / `"` Multiplayer Scoreboard (multiplayer_ranks): row count 0x00515785 (shrinks box
//       cleanly and clamps 1..8), ranking loop 0x0051594A.
//     - Overhead MP nameplate & team crest (L / always-on labels): 0x004EDC70.
//     - Loading screen progress bars: 0x004E5A46, 0x004E5AD6, 0x0049063F.
//     - In-game Players panel (0x00490D53), chat recipient dropdown (0x004EEDFB),
//       kick listbox (0x004EE3BF), end-game summary (0x0049138C, 0x0049181F).
//     - Area-entry chat announcements ("ServerHost has entered Elddim"): 0x0056BB53.
//     - Local fallback 3D cloaking in HostHide_ClientTick() (GoAspect::SetIsVisible(false),
//       SetIsSelectable(false), SetIsCollidable(false), SetIsInvincible(true), clear shadow /
//       selection-ring / path-block bits, GoMind::SetMayBeAttacked(false)).
//       Note: 0x004EB14C (minimap / megamap) checks GoAspect::GetIsVisible() AND bit 0x08, so
//       cloaking the hero hides its minimap dot automatically too.
//
//   Server-side (Role::Server, when [UI] ShowServerHost=0):
//     - HostHide_ServerTick() cloaks ServerHost's spawned hero via GoAspect::SSetIsVisible(false)
//       (0x00532098), GoAspect::SSetIsSelectable(false) (0x00532278), SetIsCollidable(false),
//       SetIsInvincible(true), GoMind::SetMayBeAttacked(false), and clears shadow/path-block/
//       selection-indicator flags. GoAspect::Xfer serializes these flags to JIP clients, and
//       server-side AI perception (0x005DD5A5 / GoMind::GetVisible) + collision ignore ServerHost.
#include "hosthide.h"
#include "hosthide_logic.h"
#include "siegehost.h"
#include "engine.h"
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

// Verified DSLOA.exe v1.50 addresses
constexpr DWORD kPlayerIsValidVa     = 0x00569054;
constexpr DWORD kPlayerGetTeamVa     = 0x0056D509;
constexpr DWORD kPlayerGetPartyVa    = 0x0056C31A;
constexpr DWORD kRanksCountMovVa     = 0x00515785;  // A1 30 FA 7A 00: mov eax, [0x007AFA30]
constexpr DWORD kHumanCountGlobalVa  = 0x007AFA30;
constexpr DWORD kLocationGetTeamCall = 0x0056BB53;  // call 0x0056D509 inside Player::RCSetWorldLocation

constexpr DWORD kGoAspectSSetIsVisibleVa    = 0x00532098;
constexpr DWORD kGoAspectSetIsVisibleVa     = 0x0053218F;
constexpr DWORD kGoAspectSSetIsSelectableVa = 0x00532278;
constexpr DWORD kGoAspectSetIsSelectableVa  = 0x0053236F;
constexpr DWORD kGoAspectSetIsCollidableVa  = 0x004711A2;
constexpr DWORD kGoAspectSetIsInvincibleVa  = 0x004711BA;
constexpr DWORD kGoMindSetMayBeAttackedVa   = 0x0049915B;

using PlayerGetTeamFn     = int   (__thiscall*)(void*);
using PlayerGetPartyFn    = void* (__thiscall*)(void*);
using GoAspectBoolFn      = void  (__thiscall*)(void*, bool);
using GoMindBoolFn        = void  (__thiscall*)(void*, bool);

const auto Player_GetTeam          = (PlayerGetTeamFn)kPlayerGetTeamVa;
const auto Player_GetParty         = (PlayerGetPartyFn)kPlayerGetPartyVa;
const auto GoAspect_SSetIsVisible  = (GoAspectBoolFn)kGoAspectSSetIsVisibleVa;
const auto GoAspect_SetIsVisible   = (GoAspectBoolFn)kGoAspectSetIsVisibleVa;
const auto GoAspect_SSetSelectable = (GoAspectBoolFn)kGoAspectSSetIsSelectableVa;
const auto GoAspect_SetSelectable  = (GoAspectBoolFn)kGoAspectSetIsSelectableVa;
const auto GoAspect_SetCollidable  = (GoAspectBoolFn)kGoAspectSetIsCollidableVa;
const auto GoAspect_SetInvincible  = (GoAspectBoolFn)kGoAspectSetIsInvincibleVa;
const auto GoMind_SetMayBeAttacked = (GoMindBoolFn)kGoMindSetMayBeAttackedVa;

struct CallSite {
    DWORD va;
    const char* label;
};

// Every client-side UI loop that calls Player::IsValid (0x00569054) to build player-facing lists.
// Verified directly in DSLOA.exe v1.50 disassembly.
constexpr CallSite kClientUiValidSites[] = {
    { 0x0048959E, "staging_layout"       },  // FUN_0048952b: staging_area_players rows + local row index
    { 0x004872DD, "staging_status"       },  // FUN_00486cd6: per-frame staging ready/in-game status
    { 0x0048A3F1, "staging_ping"         },  // FUN_0048a3d5: staging ping indicators
    { 0x0048EEFD, "staging_startloc"     },  // FUN_0048ee7d: staging start-location row count
    { 0x0051594A, "scoreboard_ranks"     },  // FUN_00515770: `\` / `"` multiplayer_ranks rows
    { 0x004EDC70, "overhead_labels"      },  // FUN_004edc07: MP overhead nameplate & team crest
    { 0x004E5A46, "loadbar_max"          },  // FUN_004e5889: loading bar leader check
    { 0x004E5AD6, "loadbar_rows"         },  // FUN_004e5889: loading bar player names
    { 0x0049063F, "loading_progress"     },  // FUN_0049060f: MP loading progress list
    { 0x00490D53, "ingame_player_panel"  },  // FUN_00490cfb: in_game_player_panel
    { 0x004EEDFB, "chat_recipients"      },  // FUN_004eecad: in-game chat combo_box_players
    { 0x004EE3BF, "kick_listbox"         },  // FUN_004ee2b3: in_game_host_options_kick listbox
    { 0x0049138C, "endgame_stats"        },  // FUN_0049111b: end_game_summary_stats
    { 0x0049181F, "endgame_summary"      },  // FUN_0049166e: end_game_summary rows
};

bool g_hideHost = false;
wchar_t g_hostName[64] = L"ServerHost";
bool g_hostNameReady = false;

void EnsureHostNameInit() {
    if (g_hostNameReady) return;
    g_hostNameReady = true;
    g_hideHost = !Cfg().showServerHost;
    g_hostName[0] = L'\0';
    MultiByteToWideChar(CP_ACP, 0, Cfg().hostCharacter.c_str(), -1, g_hostName, 63);
    g_hostName[63] = L'\0';
    if (!g_hostName[0]) wcscpy(g_hostName, L"ServerHost");
}

bool ReadablePtr(const void* p, size_t n) {
    return p && (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, n);
}

const wchar_t* ReadWideGp(void* player, size_t off) {
    if (!ReadablePtr((const char*)player + off, sizeof(void*))) return nullptr;
    const wchar_t* s = *(const wchar_t**)((const char*)player + off);
    if (!ReadablePtr(s, sizeof(wchar_t))) return nullptr;
    return s;
}

bool IsHostPlayer(void* player) {
    if (!ReadablePtr(player, 0x60)) return false;
    EnsureHostNameInit();
    const wchar_t* profileName = ReadWideGp(player, 0x04);
    const wchar_t* heroName    = ReadWideGp(player, 0x58);
    return hosthide::MatchesHostName(profileName, heroName, g_hostName);
}

bool IsLocalHuman(void* player) {
    void* srv = *(void**)0x007ACF70;
    if (!srv) return false;
    void* me = eng::Server_GetLocalHumanPlayer(srv);
    return me && me == player;
}

bool RedirectCall5(DWORD site, DWORD expectTarget, void* to) {
    BYTE* p = (BYTE*)site;
    if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != expectTarget) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    *(int32_t*)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    VirtualProtect(p, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool PatchBytes5ToCall(DWORD site, const BYTE expect[5], void* to) {
    BYTE* p = (BYTE*)site;
    if (memcmp(p, expect, 5) != 0) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt)) return false;
    p[0] = 0xE8;
    *(int32_t*)(p + 1) = (int32_t)((DWORD)to - (site + 5));
    VirtualProtect(p, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

bool IsWorldInGame() {
    void* wsObj = *(void**)0x007ACF44;
    if (!ReadablePtr(wsObj, 0x24)) return false;
    int ws = *(int*)((const char*)wsObj + 0x20);
    return ws == eng::WS_MP_INGAME || ws == eng::WS_MP_INGAME_JIP;
}

// Applies full 3D/collision/selection/shadow/minimap/AI cloaking to one Go*.
// On the server (useRpc=true), broadcasts SSetIsVisible(false) and SSetIsSelectable(false)
// so connected & JIP clients also receive the flags cleanly; on clients (useRpc=false),
// applies the local-only setters without generating network traffic.
bool CloakGo(void* go, bool useRpc, const char* roleTag) {
    if (!ReadablePtr(go, 0x50)) return false;
    void* aspect = *(void**)((char*)go + 0x04);
    void* mind   = *(void**)((char*)go + 0x34);
    if (!ReadablePtr(aspect, 0x40)) return false;

    uint32_t* flagsPtr = (uint32_t*)((char*)aspect + 0x3C);
    uint32_t beforeFlags = *flagsPtr;
    bool needAspect = hosthide::NeedsAspectCloak(beforeFlags);

    bool needMind = false;
    if (ReadablePtr(mind, 0x198)) {
        uint8_t mindByte = *((const uint8_t*)mind + 0x194);
        needMind = ((mindByte >> 2) & 1u) != 0;
    }

    if (!needAspect && !needMind) return false;

    // 1. Visibility: call the engine setter so nema::Aspect::Hide + path-block cleanup run cleanly.
    if ((beforeFlags & hosthide::kAspectVisible) != 0) {
        if (useRpc) GoAspect_SSetIsVisible(aspect, false);
        else        GoAspect_SetIsVisible(aspect, false);
    }

    // 2. Selectability: call the engine setter so any existing selection/rollover clears cleanly.
    if ((beforeFlags & hosthide::kAspectSelectable) != 0) {
        if (useRpc) GoAspect_SSetSelectable(aspect, false);
        else        GoAspect_SetSelectable(aspect, false);
    }

    // 3. Collision & invincibility via engine accessors.
    if ((beforeFlags & hosthide::kAspectCollidable) != 0) {
        GoAspect_SetCollidable(aspect, false);
    }
    if ((beforeFlags & hosthide::kAspectInvincible) == 0) {
        GoAspect_SetInvincible(aspect, true);
    }

    // 4. Clear remaining visual/minimap/path bits (draw_shadow 0x02, draw_selection_indicator 0x08,
    //    does_block_path 0x40) and enforce target bitmask (never touches 0x10).
    uint32_t afterFlags = hosthide::ComputeCloakedAspectFlags(*flagsPtr);
    *flagsPtr = afterFlags;

    // 5. Prevent monsters/AI from considering ServerHost attackable.
    if (needMind && ReadablePtr(mind, 0x198)) {
        GoMind_SetMayBeAttacked(mind, false);
    }

    DWORD goid = *(DWORD*)((const char*)go + 0x4C);
    Log("HostHide[%s]: cloaked host hero goid=0x%08lX (flags 0x%08lX -> 0x%08lX, mindCleared=%d)",
        roleTag, goid, (unsigned long)beforeFlags, (unsigned long)afterFlags, needMind ? 1 : 0);
    return true;
}

void CloakPlayerParty(void* player, bool useRpc, const char* roleTag) {
    if (!ReadablePtr(player, 0x70)) return;
    void* party = Player_GetParty(player);
    if (!ReadablePtr(party, 0x90)) return;
    void** begin = *(void***)((char*)party + 0x88);
    void** end   = *(void***)((char*)party + 0x8C);
    if (!begin || !end || begin >= end) return;
    size_t count = (size_t)(end - begin);
    if (count > 16) return;  // sanity guard
    if (!ReadablePtr(begin, count * sizeof(void*))) return;
    for (size_t i = 0; i < count; ++i) {
        if (begin[i]) CloakGo(begin[i], useRpc, roleTag);
    }
}

}  // namespace

// Replacement for `call 0x00569054` (bool __cdecl Player::IsValid(Player*)) at client UI sites.
extern "C" bool __cdecl SiegeHost_HideHostPlayerValid(void* pl) {
    bool valid = eng::Player_IsValid(pl);
    if (!valid) return false;
    if (!g_hideHost) return true;
    return hosthide::ShouldShowInUi(true, g_hideHost, IsLocalHuman(pl), IsHostPlayer(pl));
}

// Replacement for `mov eax, [0x007AFA30]` at 0x00515785 inside multiplayer_ranks (`\` scoreboard).
// Returns the number of visible non-AI, non-host human players, clamped to [1, 8].
extern "C" int __cdecl SiegeHost_RanksPlayerCount() {
    int rawCount = *(int*)kHumanCountGlobalVa;
    if (!g_hideHost) {
        return hosthide::ComputeRanksRowCount(rawCount, rawCount, false, 8);
    }
    void* srv = *(void**)0x007ACF70;
    if (!ReadablePtr(srv, 0x10)) {
        return hosthide::ComputeRanksRowCount(rawCount, rawCount, false, 8);
    }
    void** it  = *(void***)((char*)srv + 4);
    void** end = *(void***)((char*)srv + 8);
    int visibleHumans = 0;
    if (it && end && it < end) {
        size_t slots = (size_t)(end - it);
        if (slots <= 64 && ReadablePtr(it, slots * sizeof(void*))) {
            for (size_t i = 0; i < slots; ++i) {
                void* pl = it[i];
                if (!eng::Player_IsValid(pl)) continue;
                if (!ReadablePtr(pl, 0x20)) continue;
                if (*(int*)((const char*)pl + 0x14) == 2) continue;  // PC_COMPUTER
                if (!hosthide::ShouldShowInUi(true, true, IsLocalHuman(pl), IsHostPlayer(pl))) continue;
                ++visibleHumans;
            }
        }
    }
    return hosthide::ComputeRanksRowCount(visibleHumans, rawCount, true, 8);
}

// Replacement for `call Player::GetTeam` at 0x0056BB53 inside Player::RCSetWorldLocation.
// Only affects the team-match gate that prints "%S has entered %S" (Player+0xF4 is already updated).
extern "C" int __fastcall SiegeHost_LocationCheckTeam(void* self, void*) {
    int team = Player_GetTeam(self);
    if (!g_hideHost) return team;
    if (hosthide::ShouldSuppressLocationChat(true, IsLocalHuman(self), IsHostPlayer(self))) {
        void* srv = *(void**)0x007ACF70;
        void* me = srv ? eng::Server_GetLocalHumanPlayer(srv) : nullptr;
        int myTeam = me ? Player_GetTeam(me) : team;
        return myTeam ^ 1;  // guaranteed != myTeam -> skips chat print cleanly
    }
    return team;
}

void HostHide_ClientInstall() {
    EnsureHostNameInit();
    if (!g_hideHost) {
        Log("HostHide: off ([UI] ShowServerHost=1) - '%s' remains visible everywhere.", Cfg().hostCharacter.c_str());
        return;
    }

    int uiOk = 0, uiFail = 0;
    for (const auto& s : kClientUiValidSites) {
        if (RedirectCall5(s.va, kPlayerIsValidVa, (void*)&SiegeHost_HideHostPlayerValid)) {
            ++uiOk;
        } else {
            ++uiFail;
            Log("HostHide: VERIFY FAILED at 0x%08lX (%s) - site skipped.", s.va, s.label);
        }
    }

    const BYTE kRanksCountExpect[5] = { 0xA1, 0x30, 0xFA, 0x7A, 0x00 };
    bool ranksCountOk = PatchBytes5ToCall(kRanksCountMovVa, kRanksCountExpect, (void*)&SiegeHost_RanksPlayerCount);
    if (!ranksCountOk) {
        Log("HostHide: VERIFY FAILED at 0x%08lX (scoreboard_count) - site skipped.", kRanksCountMovVa);
    }

    bool locChatOk = RedirectCall5(kLocationGetTeamCall, kPlayerGetTeamVa, (void*)&SiegeHost_LocationCheckTeam);
    if (!locChatOk) {
        Log("HostHide: VERIFY FAILED at 0x%08lX (location_chat) - site skipped.", kLocationGetTeamCall);
    }

    Log("HostHide[client]: armed for '%s' (ui_loops=%d/%d, scoreboard_count=%d, location_chat=%d, local_3d_cloak=on).",
        Cfg().hostCharacter.c_str(), uiOk, uiOk + uiFail, ranksCountOk ? 1 : 0, locChatOk ? 1 : 0);
}

static void ReadUiTextAscii(void* w, char* out, size_t cap) {
    out[0] = '\0';
    if (!ReadablePtr(w, 0x1A8)) return;
    const wchar_t* ws = *(const wchar_t**)((const char*)w + 0x1A0);
    if (!ReadablePtr(ws, sizeof(wchar_t))) return;
    WideCharToMultiByte(CP_ACP, 0, ws, -1, out, (int)cap, nullptr, nullptr);
    out[cap - 1] = '\0';
}

// Lab-only self-check (SIEGEHOST_HOSTHIDE_DIAG=1): inspects live staging_area_players and
// multiplayer_ranks (\ scoreboard) UI widgets directly in UIShell so headless lab runs can
// assert ServerHost is absent from every row without needing a desktop screenshot.
static void HostHideLabDiagTick() {
    static int enabled = -1;
    if (enabled < 0) {
        char e[8];
        enabled = GetEnvironmentVariableA("SIEGEHOST_HOSTHIDE_DIAG", e, sizeof e) ? 1 : 0;
    }
    if (!enabled) return;

    static bool loggedStaging = false;
    static DWORD inGameAt = 0;
    static bool openedRanks = false;
    static bool loggedRanks = false;

    if (!loggedStaging && eng::InterfaceVisible("staging_area_players")) {
        if (void* w1 = eng::FindWindow("player1_text", "staging_area_players")) {
            char t1[64];
            ReadUiTextAscii(w1, t1, sizeof t1);
            if (t1[0]) {
                loggedStaging = true;
                for (int i = 1; i <= 8; ++i) {
                    char nTxt[32], nBg[32], txt[64];
                    sprintf(nTxt, "player%d_text", i);
                    sprintf(nBg, "player%d_bg", i);
                    void* wt = eng::FindWindow(nTxt, "staging_area_players");
                    bool vis = eng::WindowVisible(nBg, "staging_area_players");
                    ReadUiTextAscii(wt, txt, sizeof txt);
                    if (vis || txt[0]) {
                        Log("HOSTHIDE_DIAG[staging] row=%d bg_vis=%d text='%s'", i, vis ? 1 : 0, txt);
                    }
                }
            }
        }
    }

    if (IsWorldInGame() && !loggedRanks) {
        if (!inGameAt) inGameAt = GetTickCount();
        if (GetTickCount() - inGameAt < 3000) return;
        const auto UIGame_Singleton = (void* (__cdecl*)())0x004C89CD;
        const auto ShowRanks = (void (__thiscall*)(void*, bool))0x00515664;
        void* uigame = UIGame_Singleton();
        void* ranks = ReadablePtr(uigame, 0x40) ? *(void**)((char*)uigame + 0x3C) : nullptr;
        if (ranks && !openedRanks) {
            openedRanks = true;
            ShowRanks(ranks, true);
            Log("HOSTHIDE_DIAG[ranks] toggled multiplayer_ranks open (computedRows=%d, rawHumanCount=%d)",
                SiegeHost_RanksPlayerCount(), *(int*)kHumanCountGlobalVa);
            return;  // let one game tick run FUN_00515770 to populate rows
        }
        if (openedRanks && eng::InterfaceVisible("multiplayer_ranks")) {
            loggedRanks = true;
            for (int i = 1; i <= 8; ++i) {
                char nName[32], txt[64];
                sprintf(nName, "player%dname", i);
                void* wn = eng::FindWindow(nName, "multiplayer_ranks");
                bool vis = wn && eng::UIWindow_GetVisible(wn);
                ReadUiTextAscii(wn, txt, sizeof txt);
                if (vis || txt[0]) {
                    Log("HOSTHIDE_DIAG[ranks] row=%d vis=%d name='%s'", i, vis ? 1 : 0, txt);
                }
            }
            if (ranks) ShowRanks(ranks, false);
        }
    }
}

void HostHide_ClientTick() {
    EnsureHostNameInit();
    if (!g_hideHost) return;

    static DWORD s_lastTick = 0;
    DWORD now = GetTickCount();
    if (now - s_lastTick < 250) return;
    s_lastTick = now;

    HostHideLabDiagTick();

    if (!IsWorldInGame()) return;
    void* srv = *(void**)0x007ACF70;
    if (!ReadablePtr(srv, 0x10)) return;
    void* me = eng::Server_GetLocalHumanPlayer(srv);
    void** it  = *(void***)((char*)srv + 4);
    void** end = *(void***)((char*)srv + 8);
    if (!it || !end || it >= end) return;
    size_t slots = (size_t)(end - it);
    if (slots > 64 || !ReadablePtr(it, slots * sizeof(void*))) return;

    for (size_t i = 0; i < slots; ++i) {
        void* pl = it[i];
        if (!pl || pl == me) continue;
        if (!eng::Player_IsValid(pl)) continue;
        if (!IsHostPlayer(pl)) continue;
        CloakPlayerParty(pl, /*useRpc=*/false, "client");
    }
}

void HostHide_ServerTick() {
    EnsureHostNameInit();
    if (!g_hideHost) return;

    static DWORD s_lastTick = 0;
    DWORD now = GetTickCount();
    if (now - s_lastTick < 250) return;
    s_lastTick = now;

    if (!IsWorldInGame()) return;
    void* srv = *(void**)0x007ACF70;
    if (!ReadablePtr(srv, 0x10)) return;
    void* hostPl = eng::Server_GetLocalHumanPlayer(srv);
    if (!hostPl || !eng::Player_IsValid(hostPl)) return;
    if (!IsHostPlayer(hostPl)) return;
    CloakPlayerParty(hostPl, /*useRpc=*/true, "server");
}
