// jipguard.h — the decision behind the staging-screen freeze fix (0.7.3).
//
// WHAT THE ENGINE DOES (RE-10):
//   Server::OnConnectionCreated (0x0057160B) puts the world into ws_mp_ingame_jip (0x29) the moment a
//   connection is created, and Server::RSCreatePlayer (0x00570AF7) flags the new Player as JIP because
//   the world is in that state. The world only leaves 0x29 once no Player carries the JIP flag, and
//   the flag is cleared when the joiner reaches ws_wait_for_begin. So the world sits in 0x29 for the
//   WHOLE time the joiner is on the character-select screen - minutes, if they take minutes.
//
//   The freeze itself is one branch, in the per-frame sim step at 0x00581459:
//       if (IsInGame(worldState) && worldState != 0x29) { UpdateWorld(dt); if (local) ServerTick(); }
//   Every machine runs it, so everyone's world stops. Nothing is "paused" in the pause-button sense,
//   which is why the server heartbeat reported paused=0 throughout.
//
// WHAT THIS CHANGES:
//   Only the 0x29 half of that test, and only while no joiner has actually started joining. Measured
//   in the lab : a Player sitting in the staging screen has Player+0xCC == ws_invalid
//   (0) and flips to ws_load_map (7) the instant they press Ready, ~20 s before the map has loaded.
//   So "some JIP player has left the front end" is an accurate, early signal that the real join has
//   begun, and the engine's freeze is restored for the part it was written for.
//
//   The world state itself is NOT touched. Every other consumer of ws_mp_ingame_jip - the UI gates
//   that block the inventory and the exit button, and the join state machine at 0x004C81F0 that walks
//   the joiner staging -> load -> in game - sees exactly what it saw before.
#pragma once

// eWorldState values (string table at 0x0078E650; index == enum value, cross-checked against IsInGame
// at 0x0052245E). ws_invalid is what a Player reports before it has ever been given a world state.
enum {
    JG_WS_INVALID = 0x00,
    JG_WS_LOAD_MAP = 0x07,
    JG_WS_WAIT_FOR_BEGIN = 0x0B,
    JG_WS_MP_PROVIDER_SELECT = 0x1F,
    JG_WS_MP_MAP_SELECT = 0x26,
    JG_WS_MP_INGAME = 0x28,
    JG_WS_MP_INGAME_JIP = 0x29,
};

// Mirrors IsInFrontend(eWorldState) at 0x00522486 for the multiplayer range only: 0x1F..0x26 covers
// provider select, lan/internet browser, match maker, both staging screens, character select and map
// select. The lower frontend ranges cannot appear on a Player that is joining an in-progress game.
inline bool JipIsMpFrontend(int worldState) {
    return worldState >= JG_WS_MP_PROVIDER_SELECT && worldState <= JG_WS_MP_MAP_SELECT;
}

// A JIP-flagged player who is still choosing a character has not started joining yet.
inline bool JipPlayerStillPicking(int playerWorldState) {
    return playerWorldState == JG_WS_INVALID || JipIsMpFrontend(playerWorldState);
}

// True when some JIP-flagged player has left the character screen, i.e. a real join is under way.
inline bool JipRealJoinUnderway(const int* jipStates, int n) {
    for (int i = 0; i < n; ++i)
        if (!JipPlayerStillPicking(jipStates[i])) return true;
    return false;
}

// 0.7.4, players' PCs only. The server broadcasts "world state -> ws_mp_ingame_jip" the moment a
// connection is created. Returns true to HOLD that change on this PC (the joiner is still choosing a
// character, or has not even been replicated here yet), false to let it through (a real join is
// under way, or it is any other state). Only ws_mp_ingame_jip is ever held.
inline bool JipHoldStateChange(int requestedState, const int* jipStates, int n, bool playersReadable) {
    if (requestedState != JG_WS_MP_INGAME_JIP) return false;
    if (!playersReadable) return false;                   // cannot tell -> behave exactly like stock
    return !JipRealJoinUnderway(jipStates, n);
}

// The decision, kept free of engine calls so it can be tested natively.
//   worldState      - WorldState::GetCurrentState()
//   jipStates       - Player+0xCC for every valid Player whose JIP flag (Player+0x55) is set
//   n               - how many
//   playersReadable - false when the Server singleton or its player vector could not be read
// Returns true to keep the engine's freeze (the stock behaviour), false to let the world run.
inline bool JipShouldHold(int worldState, const int* jipStates, int n, bool playersReadable) {
    if (worldState != JG_WS_MP_INGAME_JIP) return false;  // engine would not freeze anyway
    if (!playersReadable) return true;                    // cannot tell -> behave exactly like stock
    return JipRealJoinUnderway(jipStates, n);             // a real join is under way
}
