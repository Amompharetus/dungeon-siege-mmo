#pragma once

// Pure math/decision helpers for the MCP melee intercept & head-to-head fixes.
// Guarded with SIEGEHOST_MCPFIX_NO_WINDOWS so host unit tests can build and run cleanly.

#include <cmath>
#include <cstdint>

namespace mcpfix {

enum : uint32_t {
    REQ_OK              = 0,
    REQ_OK_IN_RANGE     = 1,
    REQ_OK_INSIDE_RANGE = 2,
    REQ_OK_BEYOND_RANGE = 3,
    REQ_OK_CROWDED      = 4,
    REQ_FAILED          = 5,
    REQ_OUT_OF_RANGE_12 = 12,
};

struct Vec3 {
    float x, y, z;
};

struct PosSample {
    double t;
    float  x, y, z;
};

inline bool IsRequestOk(int r) {
    return r >= 0 && r <= 4;
}

inline bool IsRequestFailed(int r) {
    return r < 0 || r > 4;
}

// Head-to-Head (0x00621417) waypoint list validator:
// Vanilla returned REQUEST_OK_IN_RANGE (1) whenever waypoint count < 3, even after
// direct distance already exceeded minRange + 0.1. Only reject < 2 or zero-length pairs.
inline bool ShouldRunHeadToHead(uint32_t listSize, float twoNodeDistSq) {
    if (listSize >= 3) return true;
    if (listSize < 2)  return false;
    return twoNodeDistSq >= 1e-6f;
}

inline float Dist3D(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = bx - ax;
    float dy = by - ay;
    float dz = bz - az;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Given candidate target samples in chronological order over [tStart, tHorizon],
// pick the earliest target position where attackerSpeed * (t - tStart) + range >= dist(attStart, target(t)).
// Never returns a sample already <= range when dist0 > range (returns index 0 as safe pursuit target).
template <typename PosFn, typename DistFn>
double SelectMeleeInterceptTime(
    double tStart,
    double tTargetEnd,
    float  attackerSpeed,
    float  range,
    float  lookAhead,
    float  maxTimeParam,
    float  dist0,
    const PosFn& sampleTargetPos,
    const DistFn& distFromAttStart
) {
    if (dist0 <= range) return tStart;
    if (tTargetEnd >= 1e100 || tTargetEnd <= tStart + 1e-4) return tStart;
    if (attackerSpeed <= 0.05f) return tStart;

    double horizonSpan = static_cast<double>(lookAhead > maxTimeParam ? lookAhead : maxTimeParam) + 2.0;
    if (horizonSpan < 1.5) horizonSpan = 1.5;
    double tHorizon = tStart + horizonSpan;
    if (tTargetEnd < tHorizon) tHorizon = tTargetEnd;
    if (tHorizon <= tStart + 1e-4) return tStart;

    constexpr int kSteps = 16;
    for (int i = 1; i <= kSteps; ++i) {
        double t = tStart + (tHorizon - tStart) * (static_cast<double>(i) / static_cast<double>(kSteps));
        float d = distFromAttStart(sampleTargetPos(t));
        float reach = attackerSpeed * static_cast<float>(t - tStart) + range;
        if (reach >= d) {
            // Only aim at future point if it stays outside melee range from attacker's start;
            // otherwise pursue current position so MakeRequest actually advances.
            return (d > range) ? t : tStart;
        }
    }
    double dEnd = static_cast<double>(distFromAttStart(sampleTargetPos(tHorizon)));
    return (dEnd > static_cast<double>(range)) ? tHorizon : tStart;
}

// Classify the final request return code after attacker's plan end is known.
// Guarantees we never return 0/1/2 ("attack now") while attackerEnd is still beyond melee range.
inline uint32_t ClassifyFinalMeleeResult(int rawReqRet, float realDistAtAttEnd, float range) {
    if (IsRequestFailed(rawReqRet)) {
        return REQ_OUT_OF_RANGE_12;
    }
    if (realDistAtAttEnd > range + 0.05f) {
        return REQ_OK_BEYOND_RANGE;
    }
    return (realDistAtAttEnd <= 0.8f * range) ? REQ_OK_INSIDE_RANGE : REQ_OK_IN_RANGE;
}

} // namespace mcpfix
