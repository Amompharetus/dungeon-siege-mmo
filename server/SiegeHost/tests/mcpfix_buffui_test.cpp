// Host unit tests for MCP melee intercept/head-to-head helpers (mcpfix.h)
// and BuffUI state-icon cache & >99m timer formatting helpers (buffui_helpers.h).

#include "../src/mcpfix.h"
#include "../src/buffui_helpers.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_ran = 0;
static int g_fail = 0;

static void check(bool ok, const char* msg) {
    ++g_ran;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", msg);
    }
}

int main() {
    std::printf("mcpfix + buffui unit tests\n");

    // 1. Head-to-Head waypoint count guard:
    // Vanilla returned REQUEST_OK_IN_RANGE whenever size < 3, even when far apart on adjacent nav leaf.
    check(!mcpfix::ShouldRunHeadToHead(0, 100.0f), "size 0 never runs head-to-head");
    check(!mcpfix::ShouldRunHeadToHead(1, 100.0f), "size 1 never runs head-to-head");
    check(!mcpfix::ShouldRunHeadToHead(2, 0.0f),   "size 2 with zero segment is rejected safely");
    check(mcpfix::ShouldRunHeadToHead(2, 4.0f),    "size 2 with 2.0m segment runs head-to-head math");
    check(mcpfix::ShouldRunHeadToHead(3, 0.0f),    "size >= 3 runs vanilla head-to-head path");

    // 2. Moving-target melee intercept selection: slow krug_shaman approaching attacker at x=6.0m.
    // Attacker at x=0 (speed 4.5 m/s, melee range 1.7 m); target at x=6.0 walking -2.0 m/s toward 0.
    {
        double tStart = 100.0;
        double tTargetEnd = 103.0;
        auto samplePos = [&](double t) -> mcpfix::Vec3 {
            float dt = static_cast<float>(t - tStart);
            return { 6.0f - 2.0f * dt, 0.0f, 0.0f };
        };
        auto distFromOrigin = [&](const mcpfix::Vec3& p) -> float {
            return mcpfix::Dist3D(0.0f, 0.0f, 0.0f, p.x, p.y, p.z);
        };
        double tAim = mcpfix::SelectMeleeInterceptTime(
            tStart, tTargetEnd, 4.5f, 1.7f, 1.0f, 0.5f, 6.0f, samplePos, distFromOrigin
        );
        // Earliest meet is around dt = (6.0 - 1.7) / (4.5 + 2.0) = 0.66s (sample at ~0.75s, target at x ~ 4.5m > range 1.7m)
        check(tAim > tStart && tAim <= tStart + 1.0, "slow approaching enemy selects earliest intercept < 1s");
        mcpfix::Vec3 aimPos = samplePos(tAim);
        check(distFromOrigin(aimPos) > 1.7f, "aimPos stays outside range from attacker start so MakeRequest moves");
    }

    // 3. Moving-target intercept when predicted future target position enters <= melee range from att0:
    // Should clamp to tStart (targetPos0) so MakeRequest actually advances toward live target.
    {
        double tStart = 50.0;
        double tTargetEnd = 52.0;
        auto samplePos = [&](double t) -> mcpfix::Vec3 {
            float dt = static_cast<float>(t - tStart);
            return { 2.5f - 10.0f * dt, 0.0f, 0.0f };
        };
        auto distFromOrigin = [&](const mcpfix::Vec3& p) -> float {
            return mcpfix::Dist3D(0.0f, 0.0f, 0.0f, p.x, p.y, p.z);
        };
        double tAim = mcpfix::SelectMeleeInterceptTime(
            tStart, tTargetEnd, 4.5f, 1.7f, 1.0f, 0.5f, 2.5f, samplePos, distFromOrigin
        );
        check(std::fabs(tAim - tStart) < 1e-6, "fast closing enemy clamps aim to current target pos when future point <= range");
    }

    // 4. Final melee readiness classification prevents early swings when still out of range!
    check(mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_OK_IN_RANGE, 3.2f, 1.7f) == mcpfix::REQ_OK_BEYOND_RANGE,
          "realDist > range forces REQ_OK_BEYOND_RANGE even if MakeRequest returned 1");
    check(mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_OK_INSIDE_RANGE, 2.0f, 1.7f) == mcpfix::REQ_OK_BEYOND_RANGE,
          "realDist > range forces REQ_OK_BEYOND_RANGE even if MakeRequest returned 2");
    check(mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_OK_BEYOND_RANGE, 1.72f, 1.7f) == mcpfix::REQ_OK_IN_RANGE,
          "realDist within 1.7 + 0.05 tolerance becomes REQ_OK_IN_RANGE");
    check(mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_OK_BEYOND_RANGE, 1.1f, 1.7f) == mcpfix::REQ_OK_INSIDE_RANGE,
          "realDist <= 0.8*range classifies as REQ_OK_INSIDE_RANGE");
    check(mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_FAILED, 1.0f, 1.7f) == mcpfix::REQ_OUT_OF_RANGE_12,
          "failed pathing request stays failure 12");

    // 5. BuffUI StateIconCache: exact spellGoid takes priority over shared stateName collisions.
    {
        buffui::StateIconCache cache;
        cache.Record(0x1001, "inspiration", "icon_inspiration");
        cache.Record(0x1002, "inspiration", "icon_focused_mind");
        check(std::strcmp(cache.Lookup(0x1001, "inspiration"), "icon_inspiration") == 0,
              "cached lookup matches spell 0x1001 icon accurately despite shared stateName");
        check(std::strcmp(cache.Lookup(0x1002, "inspiration"), "icon_focused_mind") == 0,
              "cached lookup matches spell 0x1002 icon accurately despite shared stateName");
        check(std::strcmp(cache.Lookup(0x9999, "INSPIRATION"), "icon_focused_mind") == 0,
              "unseen spellGoid falls back case-insensitively to latest stateName");

        // Verify capacity 128 wrap eviction works without losing new entries.
        for (uint32_t i = 0; i < 140; ++i) {
            cache.Record(0x2000 + i, "state_x", "icon_x");
        }
        check(cache.count == buffui::StateIconCache::kCapacity, "cache respects capacity 128");
        check(cache.Lookup(0x2000 + 139, "state_x") != nullptr, "most recently recorded entry present");
    }

    // 6. BuffUI >99m icon timer formatting fits 2-char 15px width cleanly.
    {
        char l0[16] = {}, l1[16] = {};
        buffui::FormatOverNinetyNineMinutes(6000, l0, l1); // 100m00s = 1h40m
        check(std::strcmp(l0, "1h") == 0 && std::strcmp(l1, "40") == 0, "6000s -> 1h / 40");
        buffui::FormatOverNinetyNineMinutes(7205, l0, l1); // 120m05s = 2h00m
        check(std::strcmp(l0, "2h") == 0 && std::strcmp(l1, "00") == 0, "7205s -> 2h / 00");
        buffui::FormatOverNinetyNineMinutes(35999, l0, l1); // 9h59m59s
        check(std::strcmp(l0, "9h") == 0 && std::strcmp(l1, "59") == 0, "35999s -> 9h / 59");
        buffui::FormatOverNinetyNineMinutes(36000, l0, l1); // 10h00m
        check(std::strcmp(l0, "99") == 0 && std::strcmp(l1, "++") == 0, "36000s -> 99 / ++");
    }

    std::printf("%d/%d checks passed\n", g_ran - g_fail, g_ran);
    return g_fail ? 1 : 0;
}
