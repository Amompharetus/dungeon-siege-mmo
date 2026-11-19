// portraitfilter.h — pure, engine-free decision helpers for the multiplayer party panel (team_portraits).
#pragma once
#include "jipguard.h"

// Should a remote same-team player appear in the top-left party panel?
//
// Rejects:
//   - invalid players or the server's own HostCharacter (when hideHost is enabled);
//   - players without a hero name, the local player, or other-team players;
//   - players who are still joining (Player+0x55 IsJIP != 0) or have no spawned hero Go yet
//     (Player::GetParty() == nullptr or party members vector empty). Staging-lobby players often
//     have a non-empty hero name (Player+0x58) and Player+0xF4 == 0 (AreaGroup 0, which bypasses
//     the area filter); letting them pass made Panel::Update's count loop (0x00519935) count N+1
//     while Rebuild (0x00519FD7) skipped them and left panel+0x1C == N, causing per-frame rebuild
//     thrashing, slot theft, and portrait loss whenever Gate failed;
//   - players in a different area group (when nearbyOnly is enabled and both groups are known).
inline bool PortraitFilterShouldShowPlayer(
    bool valid,
    bool isHostHero,
    bool hideHost,
    bool hasHeroName,
    bool isLocalPlayer,
    bool sameTeam,
    bool isJip,
    bool hasSpawnedHero,
    bool nearbyOnly,
    unsigned playerAreaGroup,
    unsigned localAreaGroup) {
    if (!valid) return false;
    if (hideHost && isHostHero) return false;
    if (!hasHeroName) return false;
    if (isLocalPlayer || !sameTeam) return false;
    if (isJip || !hasSpawnedHero) return false;
    if (nearbyOnly && playerAreaGroup && localAreaGroup && playerAreaGroup != localAreaGroup)
        return false;
    return true;
}

// Should TeamPortraits::Gate (0x0051A4C9) allow the party panel to build and show portraits?
//
// Stock DSLOA.exe checks `worldState == ws_mp_ingame (0x28)` strictly at 0x0051A4D4 and rejects
// `ws_mp_ingame_jip (0x29)` AFTER Rebuild (0x00519E9E) has already hidden all 7 remote slots.
// With ready-first joining (jipqueue.cpp + jipguard.cpp), clients can run unpaused in 0x29 while
// another player sits in the staging lobby. Allow both 0x28 and unpaused 0x29 (realJoinHolding
// false), while still holding off during an actual map-load freeze (realJoinHolding true).
inline bool PortraitGateAllowsWorldState(int worldState, bool realJoinHolding) {
    if (worldState == JG_WS_MP_INGAME) return true;
    if (worldState == JG_WS_MP_INGAME_JIP) return !realJoinHolding;
    return false;
}
