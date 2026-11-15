// Tests for the SiegeHost pause guard (0.7.2). Builds and runs on the host compiler — the state
// machine in pauseguard.h touches no engine memory on purpose, so the part that could misbehave on
// a live server can be driven through whole sequences here.
//
//   g++ -std=c++17 -O1 -Wall -o /tmp/pgtest tests/pauseguard_test.cpp && /tmp/pgtest
//
// What is being protected:
//   * a joining player must not have the world resumed under them during the connect handshake;
//   * a player who parks on the character-select screen must NOT freeze everyone (the 0.7.2 fix);
//   * if the engine re-asserts a pause, the guard must not turn into a hammer loop;
//   * GetTickCount wraps every 49.7 days and the guard must survive it.

#include <cstdio>
#include <cstdlib>

// Stand in for <windows.h> so the header can be built natively.
using DWORD = unsigned long;
#define SIEGEHOST_PAUSEGUARD_NO_WINDOWS 1
#include "../src/pauseguard.h"

static int g_fail = 0;
static int g_ran = 0;

static void check(bool ok, const char* what) {
    ++g_ran;
    if (!ok) {
        ++g_fail;
        printf("  FAIL: %s\n", what);
    }
}

// Drive the guard for `seconds`, 4 ticks a second like AutoHostTick, and report when it first
// resumed. Returns the millisecond offset of the first resume, or -1 if it never did.
struct Sim {
    PauseGuardState s;
    PauseGuardTuning t;
    DWORD now = 0;
    int resumes = 0;
    long firstResumeMs = -1;
    long lastResumeMs = -1;
    long minGapMs = -1;

    void tick(bool paused, bool rose = false) {
        if (PauseGuardTick(s, now, paused, rose, t)) {
            if (firstResumeMs < 0) firstResumeMs = (long)now;
            if (lastResumeMs >= 0) {
                long gap = (long)now - lastResumeMs;
                if (minGapMs < 0 || gap < minGapMs) minGapMs = gap;
            }
            lastResumeMs = (long)now;
            ++resumes;
        }
        now += 250;
    }
    void run(int ms, bool paused, bool roseOnFirstTick = false) {
        for (int i = 0; i < ms / 250; ++i) tick(paused, roseOnFirstTick && i == 0);
    }
};

int main() {
    printf("pause guard tests\n");

    // 1. The reported bug: a player joins and sits in staging. The world must come back quickly,
    //    and must NOT stay frozen for the whole staging period.
    {
        Sim sim;
        sim.now = 100000;           // arbitrary uptime
        sim.run(1000, false);       // quiet, running
        sim.tick(false, true);      // CREATE_PLAYER: player count rises, world not paused yet
        DWORD joinAt = sim.s.lastJoinMs;
        sim.run(120000, true);      // the engine pauses and holds it for two minutes of staging
        check(sim.firstResumeMs >= 0, "staging pause is resumed at all");
        long delay = sim.firstResumeMs - (long)joinAt;
        check(delay >= 5000, "not resumed before the 5 s join grace");
        check(delay <= 5500, "resumed promptly once the grace expires");
        printf("  join -> resume: %ld ms (old behaviour was 30000+)\n", delay);
    }

    // 2. The connect window is still protected: nothing may resume inside the grace.
    {
        Sim sim;
        sim.now = 50000;
        sim.tick(false, true);
        sim.run(4750, true);  // paused for the whole grace, minus a tick
        check(sim.resumes == 0, "no resume during the join grace");
    }

    // 3. A pause with no join anywhere near it (a player pressing the pause button) is overridden
    //    after the paused-for window, without waiting on any join grace.
    {
        Sim sim;
        sim.now = 900000;
        sim.run(1000, false);
        sim.run(10000, true);
        check(sim.firstResumeMs >= 0, "a plain pause is resumed");
        check(sim.resumes >= 1, "plain pause resumed at least once");
    }

    // 4. A pause shorter than the paused-for window is left alone (the engine's own brief pauses).
    {
        Sim sim;
        sim.now = 7000;
        sim.run(2500, true);   // 2.5 s, under the 3 s threshold
        sim.run(1000, false);
        check(sim.resumes == 0, "a brief pause is not touched");
    }

    // 5. The engine re-asserting the pause must not produce a hammer loop: the cooldown has to
    //    space the attempts out.
    {
        Sim sim;
        sim.now = 200000;
        sim.run(1000, false);
        // paused, resumed, engine re-pauses immediately, repeatedly, for 30 s
        for (int i = 0; i < 120; ++i) sim.tick(true);
        check(sim.resumes > 1, "re-asserted pause is fought repeatedly");
        // The property that matters is the spacing, not the count: every attempt is an
        // RSUserPause broadcast, so they must stay at least a cooldown apart.
        check(sim.minGapMs >= (long)sim.t.cooldownMs, "attempts stay at least a cooldown apart");
        printf("  re-asserted pause over 30 s: %d attempts, closest %ld ms apart (cooldown %lu)\n",
               sim.resumes, sim.minGapMs, sim.t.cooldownMs);
    }

    // 6. A second player joining while the world is already paused restarts the grace, so the
    //    guard never resumes during someone else's connect.
    {
        Sim sim;
        sim.now = 400000;
        sim.tick(false, true);
        sim.run(4000, true);        // 4 s into the first player's grace
        sim.tick(true, true);       // second player connects
        DWORD secondJoin = sim.s.lastJoinMs;
        sim.run(4750, true);
        check(sim.resumes == 0, "second join restarts the grace");
        sim.run(2000, true);
        check(sim.firstResumeMs >= (long)secondJoin + 5000, "resume waits for the second player's grace");
    }

    // 7. Unpausing clears the state, so the next pause is judged fresh rather than inheriting the
    //    first one's age.
    {
        Sim sim;
        sim.now = 10000;
        sim.run(10000, true);       // resumed somewhere in here
        int after = sim.resumes;
        check(after > 0, "first pause resumed");
        sim.run(1000, false);       // world running -> state cleared
        check(sim.s.pausedSinceMs == 0, "pausedSince cleared when the world runs");
        sim.run(2500, true);        // new pause, under the threshold
        check(sim.resumes == after, "a fresh short pause is not resumed on the old timer");
    }

    // 8. GetTickCount wrap. Start just before the 49.7-day rollover and cross it mid-pause.
    {
        Sim sim;
        sim.now = 0xFFFFF000;       // ~16 s before the wrap
        sim.tick(false, true);
        sim.run(120000, true);      // wraps through zero while paused
        check(sim.firstResumeMs >= 0, "guard still resumes across the tick-count wrap");
        printf("  wrap: first resume at tick %ld\n", sim.firstResumeMs);
    }

    // 9. Tuning is honoured — the lab can put the old 30 s behaviour back.
    {
        Sim sim;
        sim.t.joinGraceMs = 30000;
        sim.now = 600000;
        sim.tick(false, true);
        DWORD joinAt = sim.s.lastJoinMs;
        sim.run(20000, true);
        check(sim.resumes == 0, "30 s tuning holds the pause for 20 s");
        sim.run(11000, true);
        check(sim.firstResumeMs >= (long)joinAt + 30000, "30 s tuning resumes only after 30 s");
    }

    // 10. No join has ever happened (lastJoinMs == 0): the grace must not apply and must not be
    //     read as "joined at tick 0", which would make a fresh server wait forever after a wrap.
    {
        Sim sim;
        sim.now = 1000;
        sim.run(5000, true);
        check(sim.firstResumeMs >= 0, "pause with no join at all is resumed");
        check(sim.firstResumeMs <= 4000, "resumed on the paused-for window, not a join grace");
    }

    printf("%d checks, %d failed\n", g_ran, g_fail);
    return g_fail ? 1 : 0;
}
