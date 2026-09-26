// questmode.cpp - 1.3.0: quests are shared across the world again, and nothing is patched.
//
// SHARED QUESTS DESIGN
// ---------------------------------------------------------------------------------------------------
// Quests are shared with the world and all players, and they get reset when the world resets.
//
// WHY THIS USES SHARED QUESTS INSTEAD OF PER-CHARACTER
// ---------------------------------------------------------------------------------------------------
// Earlier versions experimented with enabling the engine's per-character quest journal
// (use_player_journal, WorldMap+0x34). On a dedicated server, this encounters a fundamental engine
// design limitation: WHICH character is a quest question about?
//
// Every quest question content asks comes through a FuBi overload that takes no character and asks
// Server::GetScreenHero instead. On a listen server that is a real player. On a dedicated server it
// is ServerHost, a character siegeserver recreates on every start. Redirecting calls to the current
// talker worked for dialogs, but world scripts (such as check_quest.skrit gating the Elddim crypt)
// run off world messages with no conversation open at all, falling back to ServerHost and keeping
// doors shut.
//
// With the flag off, the question disappears rather than being answered:
//
//   005C73CC  Victory quest "completed":
//       mov  eax,ds:0x7ACFB8            ; the WorldMap singleton
//       cmp  BYTE PTR [eax+0x34],bl
//       je   shared                     ; 0 -> read the flag off the quest record: ONE PER WORLD
//       ...                             ; 1 -> GoDb::GetQuestBool(goid, world, "quest_%s_completed")
//   005C743B  "active", 005C74AA  "order", and about twenty more sites across quest and journal code
//
// The shared branch never looks at a goid. It reads the quest record in WORLD state. So:
//   - GetScreenHero returning ServerHost is harmless, because nothing consults the character at all
//   - quest state lives in the world, not in any character file, so a world reset clears it
//   - all redirects and the talker hook become dead code and are gone from this build
//   - it is the engine's default: use_player_journal defaults to false and neither shipped map sets
//     it, so this is stock retail multiplayer behaviour
//
// This module now patches NOTHING. No call sites, no stubs, no registers. It reads one byte to report
// which mode the world is really in, and forces it back to shared in the case where something
// else has turned it on.
//
// PROPERTIES OF SHARED QUESTS
// ---------------------------------------------------------------------------------------------------
//   - quest progress does not persist across world resets. The world resets when the last player logs off.
//   - somebody joining a world mid-way finds quests already done and quest doors already open, and
//     cannot do those quests until the world resets.
//   - one player can push a quest forward while another is part-way through it.
//   - GameAuditor bools (the guard captain's book) were already world-wide and are unchanged. The
//     whole quest system is consistently shared.
//
// WHAT THIS DOES NOT TOUCH
// ---------------------------------------------------------------------------------------------------
// Nothing a player owns. Items, gold, experience and level are character data and are not read or
// written here. Per-character quest bits already written into saves are simply ignored; they are a
// separate store and nothing deletes them.
#include "siegehost.h"
#include "engine.h"
#include <stdint.h>
#include <stdio.h>

namespace {

// WorldMap singleton. 00473099 WorldMap::FUBI_GetClassSingleton is "mov eax,ds:0x7ACFB8; ret".
void** const kWorldMapPtr = (void**)0x007ACFB8;
const int kUsePlayerJournal = 0x34;

// Only look at the flag once the world is actually in play. Read at four seconds into a load the
// WorldMap pointer can be non-null while the object behind it is still being built, and a byte read
// or written there means nothing.
inline bool WorldIsInPlay() {
    void* ws = eng::WorldState_Singleton();
    if (!ws) return false;
    int st = eng::WorldState_GetCurrentState(ws);
    return st == 0x28 || st == 0x29;          // ws_mp_ingame, ws_mp_ingame_jip
}

bool g_on = false;              // the module is running (it is, unless explicitly disabled)
bool g_forcedShared = false;    // we found the per-character byte set and cleared it
DWORD g_forced = 0;             // how many times
DWORD g_nextCheck = 0;

}  // namespace

// Two seconds apart, one pointer read and one byte compare. It walks nothing and calls nothing.
void QuestModeTick() {
    if (!g_on) return;
    DWORD now = GetTickCount();
    if (g_nextCheck && now < g_nextCheck) return;
    g_nextCheck = now + 2000;
    if (!WorldIsInPlay()) return;          // the world is still loading; the byte is not meaningful yet
    void* wm = *kWorldMapPtr;
    if (!wm) return;                       // no map loaded yet
    char* flag = (char*)wm + kUsePlayerJournal;
    if (!*flag) return;                    // shared, which is what we want and the default
    // Nothing in our content sets this and we no longer set it, so reaching here means a map file or
    // a mod turned it on. Per-character quests cannot work on a dedicated server, so put it back
    // rather than let quest questions be answered about ServerHost.
    *flag = 0;
    ++g_forced;
    if (!g_forcedShared) {
        g_forcedShared = true;
        Log("QuestMode: something had turned the per-character quest journal on "
            "(use_player_journal, WorldMap+0x34). Put back to shared. Per-character quests do not "
            "work on a dedicated server: every quest question that names no character is answered "
            "about ServerHost, which is not a player.");
    }
}

std::string QuestModeHeartbeatLines() {
    if (!g_on) return "quests=unmanaged\n";
    void* wm = *kWorldMapPtr;
    int live = (wm && WorldIsInPlay()) ? (int)*((char*)wm + kUsePlayerJournal) : -1;
    char b[128];
    snprintf(b, sizeof b, "quests=shared\nquests_perchar_flag=%d\nquests_forced_shared=%lu\n",
             live, g_forced);
    return b;
}

void QuestModeInstall() {
    char e[8];
    if (GetEnvironmentVariableA("SIEGEHOST_NO_QUESTMODE", e, sizeof e)) {
        Log("QuestMode: not watching the quest mode byte (SIEGEHOST_NO_QUESTMODE). Quests are "
            "whatever the map says, which for our maps is shared.");
        return;
    }
    g_on = true;
    Log("QuestMode: quests are shared across the world and reset when the world resets, which is the "
        "engine's own default. Nothing is patched in this build: no quest call sites, no talker hook, "
        "no stubs. Per-character quests were removed because on a dedicated server every quest "
        "question that names no character is answered about ServerHost rather than a player. No "
        "item, gold, experience or level is read or written here.");
}
