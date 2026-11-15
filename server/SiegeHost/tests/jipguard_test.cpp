// Native tests for the staging-freeze decision. Built and run by tests/run.sh on Linux; no engine.
#include "../src/jipguard.h"
#include <cstdio>
#include <vector>

static int g_fail = 0, g_ok = 0;
static void check(bool cond, const char* what) {
    if (cond) { ++g_ok; return; }
    ++g_fail;
    printf("FAIL: %s\n", what);
}

int main() {
    // --- the enum values the patch depends on -------------------------------------------------
    check(JG_WS_MP_INGAME == 0x28, "ws_mp_ingame is 0x28");
    check(JG_WS_MP_INGAME_JIP == 0x29, "ws_mp_ingame_jip is 0x29");

    // --- IsInFrontend mirror: 0x1F..0x26 inclusive, nothing else ------------------------------
    check(!JipIsMpFrontend(0x1E), "0x1E (ws_sp_outro) is not an mp frontend state");
    check(JipIsMpFrontend(0x1F), "0x1F ws_mp_provider_select is a frontend state");
    check(JipIsMpFrontend(0x23), "0x23 ws_mp_staging_area_server is a frontend state");
    check(JipIsMpFrontend(0x24), "0x24 ws_mp_staging_area_client is a frontend state");
    check(JipIsMpFrontend(0x25), "0x25 ws_mp_character_select is a frontend state");
    check(JipIsMpFrontend(0x26), "0x26 ws_mp_map_select is a frontend state");
    check(!JipIsMpFrontend(0x27), "0x27 is not a frontend state");
    check(!JipIsMpFrontend(JG_WS_MP_INGAME), "ws_mp_ingame is not a frontend state");
    check(!JipIsMpFrontend(JG_WS_LOAD_MAP), "ws_load_map is not a frontend state");

    // --- "still picking a character" ----------------------------------------------------------
    // Measured: the joiner's Player reports ws_invalid the whole time it is in the
    // staging screen on the SERVER, and its own machine reports a staging state.
    check(JipPlayerStillPicking(JG_WS_INVALID), "ws_invalid means not joining yet");
    check(JipPlayerStillPicking(0x24), "staging_area_client means not joining yet");
    check(!JipPlayerStillPicking(JG_WS_LOAD_MAP), "ws_load_map means the join has started");
    check(!JipPlayerStillPicking(0x09), "ws_loaded_map means the join has started");
    check(!JipPlayerStillPicking(JG_WS_WAIT_FOR_BEGIN), "ws_wait_for_begin means the join has started");
    check(!JipPlayerStillPicking(JG_WS_MP_INGAME), "ws_mp_ingame is not 'still picking'");

    // --- the decision -------------------------------------------------------------------------
    // Not in JIP at all: never our business.
    check(!JipShouldHold(JG_WS_MP_INGAME, nullptr, 0, true), "normal play: no hold");
    check(!JipShouldHold(0x24, nullptr, 0, true), "staging (own machine): no hold");
    {
        int loading[] = {JG_WS_LOAD_MAP};
        check(!JipShouldHold(JG_WS_MP_INGAME, loading, 1, true),
              "a loading player does not make us hold when the world is not in JIP");
    }

    // In JIP, joiner still in the staging screen -> let the world run. This is the whole fix.
    {
        int picking[] = {JG_WS_INVALID};
        check(!JipShouldHold(JG_WS_MP_INGAME_JIP, picking, 1, true), "JIP + joiner picking: world runs");
    }
    {
        int picking[] = {0x24};
        check(!JipShouldHold(JG_WS_MP_INGAME_JIP, picking, 1, true), "JIP + joiner in staging: world runs");
    }
    // In JIP with nothing flagged (the brief window right after the state is set, before the Player
    // exists): let the world run rather than freeze on a state nobody is waiting for.
    check(!JipShouldHold(JG_WS_MP_INGAME_JIP, nullptr, 0, true), "JIP with no JIP players: world runs");

    // In JIP and the joiner has pressed Ready -> restore the engine's freeze.
    {
        int loading[] = {JG_WS_LOAD_MAP};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, loading, 1, true), "JIP + joiner loading: hold");
    }
    {
        int loading[] = {0x09};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, loading, 1, true), "JIP + joiner loaded map: hold");
    }
    {
        int loading[] = {JG_WS_WAIT_FOR_BEGIN};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, loading, 1, true), "JIP + joiner waiting for begin: hold");
    }

    // Several joiners: one loading is enough to hold, whatever the others are doing.
    {
        int mixed[] = {JG_WS_INVALID, 0x24, JG_WS_LOAD_MAP};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, mixed, 3, true), "one of three loading: hold");
    }
    {
        int mixed[] = {JG_WS_LOAD_MAP, JG_WS_INVALID};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, mixed, 2, true), "first of two loading: hold");
    }
    {
        int allPicking[] = {JG_WS_INVALID, 0x24, 0x25};
        check(!JipShouldHold(JG_WS_MP_INGAME_JIP, allPicking, 3, true), "three joiners all picking: world runs");
    }

    // Safety: if the player list could not be read, do exactly what the stock engine does.
    check(JipShouldHold(JG_WS_MP_INGAME_JIP, nullptr, 0, false), "unreadable player list: hold (stock)");
    {
        int picking[] = {JG_WS_INVALID};
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, picking, 1, false), "unreadable overrides 'still picking'");
    }
    check(!JipShouldHold(JG_WS_MP_INGAME, nullptr, 0, false), "unreadable but not in JIP: still no hold");

    // A big list must not be scanned past n.
    {
        std::vector<int> v(64, JG_WS_INVALID);
        v[63] = JG_WS_LOAD_MAP;
        check(!JipShouldHold(JG_WS_MP_INGAME_JIP, v.data(), 63, true), "n is respected (loader beyond n ignored)");
        check(JipShouldHold(JG_WS_MP_INGAME_JIP, v.data(), 64, true), "n is respected (loader inside n found)");
    }

    // --- 0.7.4: players' PCs hold the switch to ws_mp_ingame_jip while the joiner picks ---------
    {
        const int none[1] = {0};
        check(JipHoldStateChange(0x29, none, 0, true), "0x29 with no JIP player replicated yet is held");
        const int picking[2] = {JG_WS_INVALID, 0x24};
        check(JipHoldStateChange(0x29, picking, 2, true), "0x29 while every joiner is still picking is held");
        const int ready[2] = {JG_WS_INVALID, JG_WS_LOAD_MAP};
        check(!JipHoldStateChange(0x29, ready, 2, true), "0x29 once any joiner has pressed Ready goes through");
        const int waiting[1] = {JG_WS_WAIT_FOR_BEGIN};
        check(!JipHoldStateChange(0x29, waiting, 1, true), "0x29 with a joiner waiting to begin goes through");
        check(!JipHoldStateChange(0x29, picking, 2, false), "unreadable player table -> stock (not held)");
        check(!JipHoldStateChange(0x28, picking, 2, true), "0x28 is never held");
        check(!JipHoldStateChange(0x06, picking, 2, true), "the mega map state is never held");
        check(!JipRealJoinUnderway(picking, 2), "picking joiners are not a real join");
        check(JipRealJoinUnderway(ready, 2), "a loading joiner is a real join");
        check(!JipRealJoinUnderway(none, 0), "no joiners, no join");
    }
    printf("%s: %d checks passed, %d failed\n", g_fail ? "FAILED" : "ok", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
