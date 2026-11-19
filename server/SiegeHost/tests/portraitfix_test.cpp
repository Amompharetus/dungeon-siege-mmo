#include "../src/portraitfilter.h"
#include <cassert>
#include <cstdio>

static void TestStagingPlayerExcluded() {
    // A player sitting in the staging lobby after selecting/auto-loading a hero:
    // Player+0x58 is non-empty, Player+0xF4 == 0 (AreaGroup 0), IsJIP == 1, GetParty() == nullptr.
    // Must be excluded so Panel::Update's count loop never disagrees with Rebuild.
    assert(!PortraitFilterShouldShowPlayer(
        /*valid=*/true, /*isHostHero=*/false, /*hideHost=*/true,
        /*hasHeroName=*/true, /*isLocalPlayer=*/false, /*sameTeam=*/true,
        /*isJip=*/true, /*hasSpawnedHero=*/false,
        /*nearbyOnly=*/true, /*playerAreaGroup=*/0, /*localAreaGroup=*/505));

    // Even if their party has just begun spawning while IsJIP is still 1, keep them out until admitted.
    assert(!PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        /*isJip=*/true, /*hasSpawnedHero=*/true,
        true, 0, 505));

    // Even with IsJIP == 0, if the player has no spawned hero Go yet, exclude them so
    // CountLoop (0x00519935) and Rebuild (0x00519FD7) stay strictly 1-to-1.
    assert(!PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        /*isJip=*/false, /*hasSpawnedHero=*/false,
        true, 505, 505));

    // Staging player with nearbyOnly=false is still excluded.
    assert(!PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        /*isJip=*/true, /*hasSpawnedHero=*/false,
        /*nearbyOnly=*/false, 0, 505));
}

static void TestSameAreaAndNearbyPlayers() {
    // Live same-area remote player -> shown.
    assert(PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        /*isJip=*/false, /*hasSpawnedHero=*/true,
        /*nearbyOnly=*/true, /*playerAreaGroup=*/505, /*localAreaGroup=*/505));

    // Live newly-spawned remote player whose world location (0xF4) is still 0 -> shown.
    assert(PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        /*isJip=*/false, /*hasSpawnedHero=*/true,
        /*nearbyOnly=*/true, /*playerAreaGroup=*/0, /*localAreaGroup=*/505));

    // Live remote player in a different area group -> hidden when nearbyOnly=true, shown when false.
    assert(!PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        false, true, /*nearbyOnly=*/true, /*playerAreaGroup=*/1001, /*localAreaGroup=*/505));
    assert(PortraitFilterShouldShowPlayer(
        true, false, true, true, false, true,
        false, true, /*nearbyOnly=*/false, /*playerAreaGroup=*/1001, /*localAreaGroup=*/505));

    // ServerHost hidden when hideHost=true, shown when hideHost=false.
    assert(!PortraitFilterShouldShowPlayer(
        true, /*isHostHero=*/true, /*hideHost=*/true, true, false, true,
        false, true, true, 505, 505));
    assert(PortraitFilterShouldShowPlayer(
        true, /*isHostHero=*/true, /*hideHost=*/false, true, false, true,
        false, true, true, 505, 505));

    // Local player, other-team player, invalid player, empty hero name -> always excluded.
    assert(!PortraitFilterShouldShowPlayer(false, false, true, true, false, true, false, true, true, 505, 505));
    assert(!PortraitFilterShouldShowPlayer(true, false, true, false, false, true, false, true, true, 505, 505));
    assert(!PortraitFilterShouldShowPlayer(true, false, true, true, true, true, false, true, true, 505, 505));
    assert(!PortraitFilterShouldShowPlayer(true, false, true, true, false, false, false, true, true, 505, 505));
}

static void TestGateWorldStatePolicy() {
    // Normal gameplay (ws_mp_ingame = 0x28): always allowed.
    assert(PortraitGateAllowsWorldState(JG_WS_MP_INGAME, false));
    assert(PortraitGateAllowsWorldState(JG_WS_MP_INGAME, true));

    // ws_mp_ingame_jip (0x29) while running unpaused (someone in staging lobby, no real map load):
    // MUST be allowed so Rebuild never wipes out same-area portraits.
    assert(PortraitGateAllowsWorldState(JG_WS_MP_INGAME_JIP, /*realJoinHolding=*/false));

    // ws_mp_ingame_jip (0x29) during an actual map-load freeze: hold off safely.
    assert(!PortraitGateAllowsWorldState(JG_WS_MP_INGAME_JIP, /*realJoinHolding=*/true));

    // Non-ingame states (invalid, mega map, staging, character select, session lost): rejected.
    assert(!PortraitGateAllowsWorldState(JG_WS_INVALID, false));
    assert(!PortraitGateAllowsWorldState(0x06, false));  // ws_mega_map
    assert(!PortraitGateAllowsWorldState(JG_WS_LOAD_MAP, false));
    assert(!PortraitGateAllowsWorldState(0x24, false));  // ws_mp_staging_area_client
    assert(!PortraitGateAllowsWorldState(0x25, false));  // ws_mp_character_select
    assert(!PortraitGateAllowsWorldState(0x2A, false));  // ws_mp_session_lost
}

int main() {
    TestStagingPlayerExcluded();
    TestSameAreaAndNearbyPlayers();
    TestGateWorldStatePolicy();
    std::printf("portraitfix_test: ALL PASSED\n");
    return 0;
}
