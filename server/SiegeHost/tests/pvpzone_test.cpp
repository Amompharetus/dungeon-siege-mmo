#include "../src/pvpzone_logic.h"
#include <cassert>
#include <cstdio>
#include <set>

using namespace pvpzone;

static void TestTableIntegrity() {
    assert(kPvpPenTileCount == 91);

    int counts[6] = {0};
    std::set<uint32_t> seen;
    for (size_t i = 0; i < kPvpPenTileCount; ++i) {
        const auto& t = kPvpPenTiles[i];
        assert(t.memNode != 0);
        assert(seen.insert(t.memNode).second);  // no duplicate terrain node GUIDs
        assert(t.penId >= 1 && t.penId <= 5);
        assert(t.rule <= PR_CCAV_FLIP);
        ++counts[t.penId];
    }

    assert(counts[1] == 12);  // Elddim Animal Pen
    assert(counts[2] == 12);  // Meren Animal Pen
    assert(counts[3] == 23);  // Hiroth Mule Pen (flipped)
    assert(counts[4] == 24);  // Hiroth Pasture Pen
    assert(counts[5] == 20);  // Hiroth Pig Pen

    // Byte-swap sanity
    assert(GasToMemNode(0x664CAE3Cu) == 0x3CAE4C66u);
    assert(GasToMemNode(GasToMemNode(0x79EEEC70u)) == 0x79EEEC70u);
}

static void TestPenRulesGeometry() {
    // PR_INTERIOR: always inside
    assert(CheckPenRule(PR_INTERIOR,  0.0f,  0.0f));
    assert(CheckPenRule(PR_INTERIOR,  3.5f,  3.5f));
    assert(CheckPenRule(PR_INTERIOR, -3.5f, -3.5f));

    // PR_Z_LT (standard straight/entry fence at z = +1.50, interior on -Z side)
    assert(CheckPenRule(PR_Z_LT, 0.0f,  0.0f));
    assert(CheckPenRule(PR_Z_LT, 0.0f,  1.49f));
    assert(!CheckPenRule(PR_Z_LT, 0.0f, 1.50f));
    assert(!CheckPenRule(PR_Z_LT, 0.0f, 1.75f));

    // PR_Z_GT (flipped Hiroth Mule Pen straight/entry fence, interior on +Z side)
    assert(CheckPenRule(PR_Z_GT, 0.0f,  1.80f));
    assert(CheckPenRule(PR_Z_GT, 0.0f,  1.51f));
    assert(!CheckPenRule(PR_Z_GT, 0.0f, 1.50f));
    assert(!CheckPenRule(PR_Z_GT, 0.0f, 0.50f));

    // PR_CNVX_IN (standard outer corner: inside iff x < +1.50 && z < +1.50)
    assert(CheckPenRule(PR_CNVX_IN,  0.0f,  0.0f));
    assert(CheckPenRule(PR_CNVX_IN,  1.40f, 1.40f));
    assert(!CheckPenRule(PR_CNVX_IN, 1.60f, 0.0f));
    assert(!CheckPenRule(PR_CNVX_IN, 0.0f,  1.60f));
    assert(!CheckPenRule(PR_CNVX_IN, 1.60f, 1.60f));

    // PR_CNVX_FLIP (flipped Hiroth Mule Pen inner corner: inside iff x > +1.50 || z > +1.50)
    assert(CheckPenRule(PR_CNVX_FLIP,  1.80f, 0.0f));
    assert(CheckPenRule(PR_CNVX_FLIP,  0.0f,  1.80f));
    assert(CheckPenRule(PR_CNVX_FLIP,  1.80f, 1.80f));
    assert(!CheckPenRule(PR_CNVX_FLIP, 1.40f, 1.40f));
    assert(!CheckPenRule(PR_CNVX_FLIP, 0.0f,  0.0f));

    // PR_CCAV_IN (standard concave inner corner: outside only in NW notch x < -1.50 && z > +1.50)
    assert(CheckPenRule(PR_CCAV_IN,   0.0f,  0.0f));
    assert(CheckPenRule(PR_CCAV_IN,   1.80f, 1.80f));
    assert(CheckPenRule(PR_CCAV_IN,  -1.80f, 0.0f));
    assert(CheckPenRule(PR_CCAV_IN,   0.0f,  1.80f));
    assert(!CheckPenRule(PR_CCAV_IN, -1.80f, 1.80f));

    // PR_CCAV_FLIP (flipped Hiroth Mule Pen outer corner: inside only in NW notch x < -1.50 && z > +1.50)
    assert(CheckPenRule(PR_CCAV_FLIP, -1.80f,  1.80f));
    assert(!CheckPenRule(PR_CCAV_FLIP, 0.0f,   0.0f));
    assert(!CheckPenRule(PR_CCAV_FLIP, -1.00f, 1.80f));
    assert(!CheckPenRule(PR_CCAV_FLIP, -1.80f, 1.00f));
    assert(!CheckPenRule(PR_CCAV_FLIP,  1.80f, 1.80f));
}

static void TestRealWorldPenLookups() {
    // Pen 1: Elddim
    assert(LookupPenAtNodePos(GasToMemNode(0x664CAE3Cu), 0.0f,  0.0f) == 1);
    assert(LookupPenAtNodePos(GasToMemNode(0x880F50BAu), 1.0f, -1.0f) == 1);
    assert(LookupPenAtNodePos(GasToMemNode(0x24BAE204u), 0.0f,  0.5f) == 1);  // inside gate
    assert(LookupPenAtNodePos(GasToMemNode(0x24BAE204u), 0.0f,  1.75f) == 0); // just outside gate
    assert(LookupPenAtNodePos(GasToMemNode(0xDB2E2DA2u), 0.0f,  0.0f) == 0);  // Elddim town square (safe)

    // Pen 2: Meren
    assert(LookupPenAtNodePos(GasToMemNode(0x1B0F5415u), 0.0f, 0.0f) == 2);
    assert(LookupPenAtNodePos(GasToMemNode(0x387A8AD2u), 0.0f, 0.2f) == 2);
    assert(LookupPenAtNodePos(GasToMemNode(0x387A8AD2u), 0.0f, 1.8f) == 0);

    // Pen 3: Hiroth Mule Pen (FLIPPED)
    assert(LookupPenAtNodePos(GasToMemNode(0x1844420Cu), 0.0f,  0.0f) == 3);
    assert(LookupPenAtNodePos(GasToMemNode(0x79EEEC70u), 0.0f,  1.8f) == 3);  // inside flipped gate (+Z)
    assert(LookupPenAtNodePos(GasToMemNode(0x79EEEC70u), 0.0f,  0.5f) == 0);  // outside flipped gate (-Z)
    assert(LookupPenAtNodePos(GasToMemNode(0x2EE5730Du), 1.8f,  0.0f) == 3);  // flipped cnvx inner corner inside
    assert(LookupPenAtNodePos(GasToMemNode(0x2EE5730Du), 0.0f,  0.0f) == 0);  // flipped cnvx inner corner outside
    assert(LookupPenAtNodePos(GasToMemNode(0x2BD7F3F5u), -1.8f, 1.8f) == 3);  // flipped ccav outer corner inside
    assert(LookupPenAtNodePos(GasToMemNode(0x2BD7F3F5u),  0.0f, 0.0f) == 0);  // flipped ccav outer corner outside

    // Pen 4: Hiroth Pasture Pen
    assert(LookupPenAtNodePos(GasToMemNode(0x112E021Cu),  0.0f, 0.0f) == 4);
    assert(LookupPenAtNodePos(GasToMemNode(0x5ED0A027u),  0.0f, 0.0f) == 4);
    assert(LookupPenAtNodePos(GasToMemNode(0x5ED0A027u), -1.8f, 1.8f) == 0);

    // Pen 5: Hiroth Pig Pen
    assert(LookupPenAtNodePos(GasToMemNode(0x4A94A63Eu),  0.0f, 0.0f) == 5);
    assert(LookupPenAtNodePos(GasToMemNode(0x3F500476u),  0.0f, 1.0f) == 5);
    assert(LookupPenAtNodePos(GasToMemNode(0x3F500476u),  0.0f, 1.8f) == 0);

    // Invalid node
    assert(LookupPenAtNodePos(0, 0.0f, 0.0f) == 0);
}

static void TestEnemyAndPotionRules() {
    // Same pen -> PvP enemies
    for (int p = 1; p <= 5; ++p) {
        assert(ArePvpEnemiesState(false, p, false, p));
    }
    // Different pens -> NOT enemies (prevents cross-pen fighting in Hiroth!)
    assert(!ArePvpEnemiesState(false, 3, false, 4));
    assert(!ArePvpEnemiesState(false, 4, false, 5));
    assert(!ArePvpEnemiesState(false, 1, false, 2));
    // Pen vs town bystander -> NOT enemies
    assert(!ArePvpEnemiesState(false, 1, false, 0));
    assert(!ArePvpEnemiesState(false, 0, false, 1));
    // Pen vs outdoor PvP -> NOT enemies
    assert(!ArePvpEnemiesState(false, 1, true, 0));
    assert(!ArePvpEnemiesState(true, 0, false, 1));
    // Both outdoor PvP -> enemies
    assert(ArePvpEnemiesState(true, 0, true, 0));
    // One outdoor PvP, one PvE -> NOT enemies
    assert(!ArePvpEnemiesState(true, 0, false, 0));
    assert(!ArePvpEnemiesState(false, 0, true, 0));
    // Both PvE -> NOT enemies
    assert(!ArePvpEnemiesState(false, 0, false, 0));

    // Health/Rejuv potion blocking rules
    // 1. Always blocked inside any town animal pen (even alone, no combat)
    for (int p = 1; p <= 5; ++p) {
        assert(ShouldBlockHealthPotionsState(p, false, false, false));
    }
    // 2. Outdoor PvP solo (no enemy within 40m, no recent combat) -> ALLOWED (PvE intact)
    assert(!ShouldBlockHealthPotionsState(0, true, false, false));
    // 3. Outdoor PvP with alive enemy within 40m -> BLOCKED
    assert(ShouldBlockHealthPotionsState(0, true, true, false));
    // 4. Outdoor PvP within 15s of PvP hit (even if enemy ran >40m away) -> BLOCKED
    assert(ShouldBlockHealthPotionsState(0, true, false, true));
    // 5. Outdoor PvP with both -> BLOCKED
    assert(ShouldBlockHealthPotionsState(0, true, true, true));
    // 6. PvE town/wilderness -> ALWAYS ALLOWED
    assert(!ShouldBlockHealthPotionsState(0, false, false, false));
    assert(!ShouldBlockHealthPotionsState(0, false, true, true));
}

int main() {
    TestTableIntegrity();
    TestPenRulesGeometry();
    TestRealWorldPenLookups();
    TestEnemyAndPotionRules();
    std::printf("pvpzone_test: ALL PASS (%u nodes, 5 pens, all rules verified)\n",
                (unsigned)kPvpPenTileCount);
    return 0;
}
