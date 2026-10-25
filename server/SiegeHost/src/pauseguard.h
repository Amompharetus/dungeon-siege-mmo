#pragma once
// Pause guard state machine (SiegeHost 0.7.2) — pure logic, no engine calls, so it can be built
// and tested on any host compiler. See tests/pauseguard_test.cpp and the notes in autohost.cpp.
//
// Why it exists: pausing is off on our servers, but the host flag alone does not stop a client's
// pause request (lab ), so a paused world has to be resumed from the host. The care
// is needed around joins — resume at the wrong moment and we could interfere with one.
//
// What changed in 0.7.2, and why it is safe. Measured on live NA:
//   * (server): the world pauses on CREATE_PLAYER and the engine never lets go. It
//     stayed paused until this guard resumed it, at the first tick after the old 30 s stand-down
//     expired, to within 0.4 s. The 30 s WAS the freeze players complained about.
//   * (client): the joining client reaches ws_mp_staging_area_client about 0.2 s
//     after CONNECT_COMPLETE — 172 ms in one run, 250 ms in another. There is no slow world
//     transfer at connect.
//   * The map load happens after the player ticks Ready (client enters ws_load_map), and the
//     server was not paused during it — paused=0 across the whole Ready→in-world transition.
//
// So the stand-down only has to outlast the connect handshake, not a world load. 5 s is ~25x the
// measured 0.2 s and still leaves the connect window untouched.

#ifndef SIEGEHOST_PAUSEGUARD_NO_WINDOWS
#include <windows.h>
#endif

struct PauseGuardState {
    DWORD lastJoinMs = 0;     // when the remote player count last went up (0 = nobody has joined)
    DWORD pausedSinceMs = 0;  // when the world was first seen paused (0 = not paused)
    DWORD lastUnpauseMs = 0;  // when we last asked the engine to resume (0 = never)
};

struct PauseGuardTuning {
    DWORD joinGraceMs = 5000;  // don't touch a pause this soon after someone connected
    DWORD pausedForMs = 250;  // let a pause stand this long before overriding it
    DWORD cooldownMs = 1000;   // minimum gap between two resume attempts
};

// True when a paused world should be resumed.
//
// All arithmetic is unsigned on purpose: GetTickCount wraps every 49.7 days and (now - then) stays
// correct across the wrap, which a signed comparison against an absolute deadline would not.
inline bool ShouldResume(const PauseGuardState& s, DWORD now, const PauseGuardTuning& t) {
    if (!s.pausedSinceMs) return false;                                         // not paused
    if (s.lastJoinMs && now - s.lastJoinMs < t.joinGraceMs) return false;       // still connecting
    if (now - s.pausedSinceMs < t.pausedForMs) return false;                    // let it stand a moment
    if (s.lastUnpauseMs && now - s.lastUnpauseMs < t.cooldownMs) return false;  // don't hammer
    return true;
}

// Advance the state for one tick. Returns true when the caller should ask the engine to resume.
// Split from ShouldResume so a test can drive whole sequences one tick at a time.
inline bool PauseGuardTick(PauseGuardState& s, DWORD now, bool paused, bool playerCountRose,
                           const PauseGuardTuning& t) {
    if (playerCountRose) s.lastJoinMs = now;
    if (!paused) {
        s.pausedSinceMs = 0;
        return false;
    }
    if (!s.pausedSinceMs) s.pausedSinceMs = now;
    if (!ShouldResume(s, now, t)) return false;
    s.lastUnpauseMs = now;
    return true;
}
