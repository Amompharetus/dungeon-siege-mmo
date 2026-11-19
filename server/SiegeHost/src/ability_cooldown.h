#pragma once

// Pure staggered cooldown state machine for the 4 Character Abilities (Q/W/E/R).
// Header-only and Windows-free so native host tests can exercise it directly.
//
// Rules:
//   - Using an unlocked ability puts that ability on a 30s personal cooldown (isOwn = true).
//   - Every other unlocked ability goes on a 15s shared cooldown (isOwn = false), EXCEPT any
//     ability that is still actively counting down its own 30s personal cooldown, which is
//     never touched once activated.
//   - Locked abilities (below required discipline level) are never placed on cooldown.

#include <cstdint>

namespace abilitycd {

static constexpr int      kSlotCount        = 4;
static constexpr uint32_t kOwnCooldownMs    = 30000u;
static constexpr uint32_t kSharedCooldownMs = 15000u;
static constexpr uint32_t kReadyFlashMs     = 400u;

struct SlotState {
    bool     active     = false;
    bool     isOwn      = false;
    bool     everCooled = false;
    uint32_t readyAtMs  = 0;
};

struct CooldownTracker {
    SlotState slots[kSlotCount] = {};

    void Reset() {
        for (int i = 0; i < kSlotCount; ++i) {
            slots[i] = SlotState{};
        }
    }

    void Expire(uint32_t now) {
        for (int i = 0; i < kSlotCount; ++i) {
            if (slots[i].active && (int32_t)(slots[i].readyAtMs - now) <= 0) {
                slots[i].active = false;
                slots[i].isOwn  = false;
            }
        }
    }

    uint32_t RemainingMs(int slot, uint32_t now, uint32_t toleranceMs = 0) const {
        if (slot < 0 || slot >= kSlotCount) return 0;
        const SlotState& s = slots[slot];
        if (!s.active) return 0;
        int32_t rem = (int32_t)(s.readyAtMs - now);
        if (rem <= (int32_t)toleranceMs) return 0;
        return (uint32_t)rem;
    }

    bool CanUse(int slot, uint32_t now, uint32_t ownToleranceMs = 0, uint32_t sharedToleranceMs = 0) const {
        if (slot < 0 || slot >= kSlotCount) return false;
        uint32_t tol = slots[slot].isOwn ? ownToleranceMs : sharedToleranceMs;
        return RemainingMs(slot, now, tol) == 0;
    }

    void OnUse(int slot, uint32_t now, const bool unlocked[kSlotCount]) {
        if (slot < 0 || slot >= kSlotCount) return;
        Expire(now);

        slots[slot].active     = true;
        slots[slot].isOwn      = true;
        slots[slot].everCooled = true;
        slots[slot].readyAtMs  = now + kOwnCooldownMs;

        for (int i = 0; i < kSlotCount; ++i) {
            if (i == slot) continue;
            if (!unlocked || !unlocked[i]) continue;
            // Never touch an ability that is still counting down its own 30s cooldown.
            if (slots[i].active && slots[i].isOwn) continue;

            slots[i].active     = true;
            slots[i].isOwn      = false;
            slots[i].everCooled = true;
            slots[i].readyAtMs  = now + kSharedCooldownMs;
        }
    }

    bool JustCameReady(int slot, uint32_t now) const {
        if (slot < 0 || slot >= kSlotCount) return false;
        const SlotState& s = slots[slot];
        if (!s.everCooled) return false;
        if (RemainingMs(slot, now) > 0) return false;
        uint32_t sinceReady = now - s.readyAtMs;
        return sinceReady < kReadyFlashMs;
    }
};

} // namespace abilitycd
