// Unit tests for staggered Character Abilities cooldowns (30s personal, 15s other unlocked,
// never touching an ability whose original 30s cooldown is still active).
// Build & run: g++ -std=c++17 -Wall -Wextra -Werror tests/ability_cooldown_test.cpp -o test && ./test

#include "../src/ability_cooldown.h"
#include <cassert>
#include <cstdio>

using namespace abilitycd;

static void TestExactUserSequence() {
    CooldownTracker cd;
    const bool allUnlocked[4] = { true, true, true, true };
    const uint32_t t0 = 100000u;

    // Initially all ready, no flash
    for (int i = 0; i < 4; ++i) {
        assert(cd.CanUse(i, t0));
        assert(cd.RemainingMs(i, t0) == 0);
        assert(!cd.JustCameReady(i, t0));
    }

    // t = 0s: Use Q -> Q = 30s, W/E/R = 15s
    cd.OnUse(0, t0, allUnlocked);
    assert(cd.RemainingMs(0, t0) == 30000u);
    assert(cd.RemainingMs(1, t0) == 15000u);
    assert(cd.RemainingMs(2, t0) == 15000u);
    assert(cd.RemainingMs(3, t0) == 15000u);
    assert(!cd.CanUse(0, t0 + 14999u));
    assert(!cd.CanUse(1, t0 + 14999u));

    // t = 15s: W/E/R ready, Q still has 15s remaining
    const uint32_t t15 = t0 + 15000u;
    assert(!cd.CanUse(0, t15));
    assert(cd.RemainingMs(0, t15) == 15000u);
    assert(cd.CanUse(1, t15));
    assert(cd.CanUse(2, t15));
    assert(cd.CanUse(3, t15));
    assert(cd.JustCameReady(1, t15 + 100u));
    assert(!cd.JustCameReady(1, t15 + 400u));

    // t = 15s: Use W -> W = 30s, E/R = 15s, Q untouched (still 15s remaining, ready at t0 + 30s)
    cd.OnUse(1, t15, allUnlocked);
    assert(cd.RemainingMs(0, t15) == 15000u);
    assert(cd.RemainingMs(1, t15) == 30000u);
    assert(cd.RemainingMs(2, t15) == 15000u);
    assert(cd.RemainingMs(3, t15) == 15000u);

    // t = 30s: Q, E, R are ready; W has 15s left
    const uint32_t t30 = t0 + 30000u;
    assert(cd.CanUse(0, t30));
    assert(!cd.CanUse(1, t30));
    assert(cd.RemainingMs(1, t30) == 15000u);
    assert(cd.CanUse(2, t30));
    assert(cd.CanUse(3, t30));

    // t = 30s: Use E -> E = 30s, Q/R = 15s, W untouched (still 15s left)
    cd.OnUse(2, t30, allUnlocked);
    assert(cd.RemainingMs(0, t30) == 15000u);
    assert(cd.RemainingMs(1, t30) == 15000u);
    assert(cd.RemainingMs(2, t30) == 30000u);
    assert(cd.RemainingMs(3, t30) == 15000u);
}

static void TestDelayedSecondCastLeavesOriginalUntouched() {
    CooldownTracker cd;
    const bool allUnlocked[4] = { true, true, true, true };
    const uint32_t t0 = 500000u;

    // t = 0s: Use Q -> Q ready at t0+30s, W/E/R ready at t0+15s
    cd.OnUse(0, t0, allUnlocked);

    // t = 20s: Q has ONLY 10s remaining of its original 30s CD; W/E/R are ready.
    // Using W must NOT extend Q back up to 15s!
    const uint32_t t20 = t0 + 20000u;
    assert(cd.RemainingMs(0, t20) == 10000u);
    assert(cd.CanUse(1, t20));

    cd.OnUse(1, t20, allUnlocked);
    assert(cd.RemainingMs(0, t20) == 10000u); // Q completely untouched since t0!
    assert(cd.RemainingMs(1, t20) == 30000u);
    assert(cd.RemainingMs(2, t20) == 15000u);
    assert(cd.RemainingMs(3, t20) == 15000u);

    // t = 30s: Q finishes its original 30s cooldown on schedule; W has 20s left; E/R have 5s left (shared)
    const uint32_t t30 = t0 + 30000u;
    assert(cd.CanUse(0, t30));
    assert(cd.RemainingMs(1, t30) == 20000u);
    assert(cd.RemainingMs(2, t30) == 5000u);

    // Using Q at t = 30s sets Q=30s, leaves W untouched at 20s, and puts E/R on a fresh 15s lockout
    cd.OnUse(0, t30, allUnlocked);
    assert(cd.RemainingMs(0, t30) == 30000u);
    assert(cd.RemainingMs(1, t30) == 20000u); // W untouched!
    assert(cd.RemainingMs(2, t30) == 15000u); // E refreshed to 15s shared lockout
    assert(cd.RemainingMs(3, t30) == 15000u); // R refreshed to 15s shared lockout
}

static void TestLockedAbilitiesUntouched() {
    CooldownTracker cd;
    const bool qeOnly[4] = { true, false, true, false };
    const uint32_t t0 = 200000u;

    cd.OnUse(0, t0, qeOnly);
    assert(cd.RemainingMs(0, t0) == 30000u);
    assert(cd.RemainingMs(1, t0) == 0u);     // W locked -> never placed on CD
    assert(cd.RemainingMs(2, t0) == 15000u); // E unlocked -> 15s CD
    assert(cd.RemainingMs(3, t0) == 0u);     // R locked -> never placed on CD

    // If player levels Ranged to 30 at t0+5s, W is immediately usable
    assert(cd.CanUse(1, t0 + 5000u));
}

static void TestServerJitterToleranceAndWraparound() {
    CooldownTracker cd;
    const bool allUnlocked[4] = { true, true, true, true };
    // Near 32-bit GetTickCount() wraparound
    const uint32_t t0 = 0xFFFFE000u;

    cd.OnUse(0, t0, allUnlocked);
    // Shared CD (15s) with 1000ms tolerance -> rejected at 13.9s, accepted at 14.0s
    assert(!cd.CanUse(1, t0 + 13999u, 1500u, 1000u));
    assert(cd.CanUse(1, t0 + 14000u, 1500u, 1000u));

    // Own CD (30s) with 1500ms tolerance -> rejected at 28.4s, accepted at 28.5s
    assert(!cd.CanUse(0, t0 + 28499u, 1500u, 1000u));
    assert(cd.CanUse(0, t0 + 28500u, 1500u, 1000u));

    // Use W slightly early due to jitter (t0 + 14500ms): Q must remain untouched (15500ms left)
    cd.OnUse(1, t0 + 14500u, allUnlocked);
    assert(cd.RemainingMs(0, t0 + 14500u) == 15500u);
    assert(cd.RemainingMs(1, t0 + 14500u) == 30000u);
    assert(cd.RemainingMs(2, t0 + 14500u) == 15000u);
}

int main() {
    TestExactUserSequence();
    TestDelayedSecondCastLeavesOriginalUntouched();
    TestLockedAbilitiesUntouched();
    TestServerJitterToleranceAndWraparound();
    std::printf("ability_cooldown_test: all assertions passed!\n");
    return 0;
}
