#pragma once
// pvpzone_logic.h - Pure, unit-testable town animal pen geometry and PvP rule helpers.
// Shared between src/pvpzone.cpp and tests/pvpzone_test.cpp.

#include <cstddef>
#include <cstdint>

namespace pvpzone {

// In DSLOA.exe memory (GoPlacement+0x20), SiegePos.node stores the .gas node GUID byte-swapped.
constexpr uint32_t GasToMemNode(uint32_t g) {
    return ((g & 0x000000FFu) << 24) |
           ((g & 0x0000FF00u) << 8)  |
           ((g & 0x00FF0000u) >> 8)  |
           ((g & 0xFF000000u) >> 24);
}

// Verified fence rail position inside t_grs01_fence_generic_wood-*.sno is at +/-1.50m
// (rails span [1.40, 1.60] on straight/entry tiles, and [1.40, 1.60] x [1.40, 1.60] on corners).
constexpr float kFenceLine = 1.50f;

enum PenRule : uint8_t {
    PR_INTERIOR  = 0, // enclosed interior floor tile (always inside)
    PR_Z_LT      = 1, // standard straight/entry fence tile: interior on -Z side (z < +1.50)
    PR_Z_GT      = 2, // flipped straight/entry fence tile (Hiroth Pen #1): interior on +Z side (z > +1.50)
    PR_CNVX_IN   = 3, // standard convex outer corner: interior at x < +1.50 && z < +1.50
    PR_CNVX_FLIP = 4, // flipped convex inner corner (Hiroth Pen #1): interior at x > +1.50 || z > +1.50
    PR_CCAV_IN   = 5, // standard concave inner corner: interior everywhere except NW notch (x < -1.50 && z > +1.50)
    PR_CCAV_FLIP = 6, // flipped concave outer corner (Hiroth Pen #1): interior only in NW notch (x < -1.50 && z > +1.50)
};

struct PenTile {
    uint32_t memNode;
    uint8_t  penId;
    uint8_t  rule;
};

// All 91 verified terrain nodes across the 5 town animal pens in MpWorld.dsmap:
//   Pen 1: Elddim Animal Pen  (town_center, 12 nodes: 2 interior + 10 fence)
//   Pen 2: Meren Animal Pen   (ocean,       12 nodes: 2 interior + 10 fence)
//   Pen 3: Hiroth Mule Pen    (castle_town, 23 nodes: 9 interior + 14 fence, flipped orientation)
//   Pen 4: Hiroth Pasture Pen (castle_town, 24 nodes: 10 interior + 14 fence)
//   Pen 5: Hiroth Pig Pen     (castle_town, 20 nodes: 6 interior + 14 fence)
static constexpr PenTile kPvpPenTiles[] = {
    // Pen 1: Elddim Animal Pen (town_center)
    {GasToMemNode(0x664CAE3Cu), 1, PR_INTERIOR},
    {GasToMemNode(0x880F50BAu), 1, PR_INTERIOR},
    {GasToMemNode(0x0E93D506u), 1, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x24BAE204u), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x4074D575u), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x5E59AD28u), 1, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x6A66155Cu), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x72C1714Cu), 1, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x7C373C51u), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-b
    {GasToMemNode(0x90811425u), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xAD67D26Cu), 1, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xBB77138Au), 1, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a

    // Pen 2: Meren Animal Pen (ocean)
    {GasToMemNode(0x1B0F5415u), 2, PR_INTERIOR},
    {GasToMemNode(0x9DA226F3u), 2, PR_INTERIOR},
    {GasToMemNode(0x187A7495u), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0x387A8AD2u), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x79647623u), 2, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x8B69EB6Au), 2, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x91F9FD2Bu), 2, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x951E9074u), 2, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x972DB21Bu), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0xAFBDAA34u), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0xBDAD2B9Eu), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-b
    {GasToMemNode(0xE57F5EB2u), 2, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-a

    // Pen 3: Hiroth Mule Pen (castle_town — flipped fence orientation)
    {GasToMemNode(0x1844420Cu), 3, PR_INTERIOR},
    {GasToMemNode(0x27AC7D97u), 3, PR_INTERIOR},
    {GasToMemNode(0x4B8EC8FCu), 3, PR_INTERIOR},
    {GasToMemNode(0x581A3E8Fu), 3, PR_INTERIOR},
    {GasToMemNode(0x80CBAA3Fu), 3, PR_INTERIOR},
    {GasToMemNode(0x86892C11u), 3, PR_INTERIOR},
    {GasToMemNode(0x909151C4u), 3, PR_INTERIOR},
    {GasToMemNode(0xAA11A40Eu), 3, PR_INTERIOR},
    {GasToMemNode(0xEE5BB68Au), 3, PR_INTERIOR},
    {GasToMemNode(0x0337D4ECu), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x2BD7F3F5u), 3, PR_CCAV_FLIP}, // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0x2EE5730Du), 3, PR_CNVX_FLIP}, // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x41CD29D1u), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0x48576E6Bu), 3, PR_CCAV_FLIP}, // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0x4906BEC4u), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x537979DCu), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0x57B94F21u), 3, PR_CCAV_FLIP}, // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0x69FA0017u), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x79EEEC70u), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x91BA1191u), 3, PR_CCAV_FLIP}, // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0xD14FB006u), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xDD847BBCu), 3, PR_Z_GT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0xE703AC19u), 3, PR_CCAV_FLIP}, // t_grs01_fence_generic_wood-ccav-a

    // Pen 4: Hiroth Pasture Pen (castle_town)
    {GasToMemNode(0x112E021Cu), 4, PR_INTERIOR},
    {GasToMemNode(0x1DE67810u), 4, PR_INTERIOR},
    {GasToMemNode(0x2DB6CF57u), 4, PR_INTERIOR},
    {GasToMemNode(0x5D7CBF2Au), 4, PR_INTERIOR},
    {GasToMemNode(0x9590D24Bu), 4, PR_INTERIOR},
    {GasToMemNode(0x9E976670u), 4, PR_INTERIOR},
    {GasToMemNode(0xA53128ACu), 4, PR_INTERIOR},
    {GasToMemNode(0xB9E12767u), 4, PR_INTERIOR},
    {GasToMemNode(0xC7E8AF3Eu), 4, PR_INTERIOR},
    {GasToMemNode(0xCCF74180u), 4, PR_INTERIOR},
    {GasToMemNode(0x0F8B92E3u), 4, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x2079FCDEu), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x4E790BFDu), 4, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x4EDE20E2u), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x5ED0A027u), 4, PR_CCAV_IN},   // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0x731F5B1Du), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x82211CB4u), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x8AB94A52u), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xA4887167u), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xAC9F8E19u), 4, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xC53C2F8Eu), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xD2DCEFDDu), 4, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xD9151C05u), 4, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0xF30D1A28u), 4, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a

    // Pen 5: Hiroth Pig Pen (castle_town)
    {GasToMemNode(0x0640C0B9u), 5, PR_INTERIOR},
    {GasToMemNode(0x4A94A63Eu), 5, PR_INTERIOR},
    {GasToMemNode(0x68B521F6u), 5, PR_INTERIOR},
    {GasToMemNode(0x7968DDB1u), 5, PR_INTERIOR},
    {GasToMemNode(0xA69951B6u), 5, PR_INTERIOR},
    {GasToMemNode(0xCD8A9920u), 5, PR_INTERIOR},
    {GasToMemNode(0x17E12A19u), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0x1978AAD2u), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0x2AEF6882u), 5, PR_CCAV_IN},   // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0x3F500476u), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x45605F1Cu), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-entry-a
    {GasToMemNode(0x50CAF830u), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x8587C9F0u), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x08-a
    {GasToMemNode(0x8AC03A3Eu), 5, PR_CCAV_IN},   // t_grs01_fence_generic_wood-ccav-a
    {GasToMemNode(0xC33296C3u), 5, PR_Z_LT},      // t_grs01_fence_generic_wood-04x04-a
    {GasToMemNode(0xCB65C75Au), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xD31E1332u), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xEE5D9E27u), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xEEB8C728u), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
    {GasToMemNode(0xF25B7927u), 5, PR_CNVX_IN},   // t_grs01_fence_generic_wood-cnvx-a
};

constexpr size_t kPvpPenTileCount = sizeof(kPvpPenTiles) / sizeof(kPvpPenTiles[0]);

inline const char* PenName(int penId) {
    switch (penId) {
        case 1:  return "the Elddim Animal Pen";
        case 2:  return "the Meren Animal Pen";
        case 3:  return "the Hiroth Mule Pen";
        case 4:  return "the Hiroth Pasture Pen";
        case 5:  return "the Hiroth Pig Pen";
        default: return "PvP Pen";
    }
}

inline bool CheckPenRule(uint8_t rule, float x, float z) {
    switch (rule) {
        case PR_INTERIOR:  return true;
        case PR_Z_LT:      return z < kFenceLine;
        case PR_Z_GT:      return z > kFenceLine;
        case PR_CNVX_IN:   return x < kFenceLine && z < kFenceLine;
        case PR_CNVX_FLIP: return x > kFenceLine || z > kFenceLine;
        case PR_CCAV_IN:   return !(x < -kFenceLine && z > kFenceLine);
        case PR_CCAV_FLIP: return (x < -kFenceLine && z > kFenceLine);
        default:           return false;
    }
}

inline int LookupPenAtNodePos(uint32_t memNode, float x, float z) {
    if (memNode == 0) return 0;
    for (size_t i = 0; i < kPvpPenTileCount; ++i) {
        if (kPvpPenTiles[i].memNode == memNode) {
            return CheckPenRule(kPvpPenTiles[i].rule, x, z) ? (int)kPvpPenTiles[i].penId : 0;
        }
    }
    return 0;
}

// Two distinct human non-host players are PvP enemies iff:
//   - Both are in the SAME town animal pen (prevents cross-pen or inside-vs-outside fighting), OR
//   - Neither is in a town animal pen and both are in an outdoor PvP world location (402, 409, 901).
inline bool ArePvpEnemiesState(bool inOutdoorA, int penA, bool inOutdoorB, int penB) {
    if (penA != 0 || penB != 0) {
        return penA != 0 && penA == penB;
    }
    return inOutdoorA && inOutdoorB;
}

// Health & Rejuvenation potions are blocked iff:
//   - Always inside any of the 5 town animal pens (penId != 0), OR
//   - In an outdoor PvP location (402, 409, 901) while another alive PvP enemy is within 40m
//     OR within 15s of active PvP combat.
inline bool ShouldBlockHealthPotionsState(int penId, bool inOutdoorPvp, bool enemyWithinRange, bool inRecentCombat) {
    if (penId != 0) return true;
    if (inOutdoorPvp && (enemyWithinRange || inRecentCombat)) return true;
    return false;
}

}  // namespace pvpzone
