#include "../src/doorfix.h"
#include <cassert>
#include <cstdio>

static void TestTemplateMatching() {
    using doorfix::IsDoorTemplateName;
    using doorfix::IsGearDoorTemplateName;

    assert(IsDoorTemplateName("dsx_gear_door"));
    assert(IsDoorTemplateName("DSX_GEAR_DOOR"));
    assert(IsDoorTemplateName("3W_dsx_gear_door"));
    assert(IsDoorTemplateName("mmo_loa_dsx_gear_door"));
    assert(IsDoorTemplateName("base_door"));
    assert(IsDoorTemplateName("door_cav_01"));
    assert(IsDoorTemplateName("door_csl_dungeon_use_toggle_right_04"));
    assert(IsDoorTemplateName("door_glb_gate"));
    assert(IsDoorTemplateName("dsx_ice_gate_left"));
    assert(IsDoorTemplateName("dsx_ice_gate_right"));
    assert(IsDoorTemplateName("dsx_nassirom_door"));
    assert(IsDoorTemplateName("dsx_nassirom_door_r"));

    assert(!IsDoorTemplateName(nullptr));
    assert(!IsDoorTemplateName(""));
    assert(!IsDoorTemplateName("outdoor_lamp"));
    assert(!IsDoorTemplateName("indoor_torch"));
    assert(!IsDoorTemplateName("golem_clockwork"));
    assert(!IsDoorTemplateName("elevator_2s_1c_2n"));

    assert(IsGearDoorTemplateName("dsx_gear_door"));
    assert(IsGearDoorTemplateName("2W_dsx_gear_door"));
    assert(!IsGearDoorTemplateName("door_cav_01"));
}

static void TestPacketValidation() {
    using namespace doorfix;

    DoorSyncReport src{};
    src.magic = kDoorSyncMagic;
    src.version = kDoorSyncVersion;
    src.count = 3;
    src.flags = 0;
    src.heroNode = 0x20EC9A46u;
    src.doorGoids[0] = 0x100000CDu;
    src.doorGoids[1] = 0x070000C4u;
    src.doorGoids[2] = 0x220000A1u;

    size_t sz = SerializedReportSize(src.count);
    assert(sz == 12u + 12u);

    DoorSyncReport dst{};
    assert(ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), sz, &dst));
    assert(dst.count == 3);
    assert(dst.heroNode == 0x20EC9A46u);
    assert(ReportContainsGoid(dst, 0x100000CDu));
    assert(ReportContainsGoid(dst, 0x070000C4u));
    assert(!ReportContainsGoid(dst, 0xDEADBEEFu));

    // Truncated packet must be rejected
    assert(!ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), sz - 1, &dst));
    assert(!ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), 4, &dst));

    // Wrong magic / version / excessive count rejected
    src.magic = 0x11223344u;
    assert(!ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), sz, &dst));
    src.magic = kDoorSyncMagic;
    src.version = 2;
    assert(!ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), sz, &dst));
    src.version = kDoorSyncVersion;
    src.count = kMaxReportedDoors + 1;
    assert(!ValidateReportPacket(reinterpret_cast<const uint8_t*>(&src), sizeof(src), &dst));
}

static void TestMissingTrackerGraceCooldownAndCap() {
    using namespace doorfix;
    DoorMissingTracker tr;
    const uint32_t dpnid = 0xAABB0001u;
    const uint32_t goid  = 0x100000CDu;

    // 1st missing report -> never immediately resync (in-flight packet grace)
    assert(!tr.RecordMissing(dpnid, goid, 1000));
    // 2nd report too soon (< 1200ms) -> still wait
    assert(!tr.RecordMissing(dpnid, goid, 1900));
    // 3rd report after >= 1200ms -> RESYNC #1!
    assert(tr.RecordMissing(dpnid, goid, 2250));
    // Within 5000ms cooldown -> suppress
    assert(!tr.RecordMissing(dpnid, goid, 5000));
    // After cooldown -> RESYNC #2!
    assert(tr.RecordMissing(dpnid, goid, 7300));
    // Resyncs #3, #4, #5
    assert(tr.RecordMissing(dpnid, goid, 12400));
    assert(tr.RecordMissing(dpnid, goid, 17500));
    assert(tr.RecordMissing(dpnid, goid, 22600));
    // Cap reached (5) -> stop retrying
    assert(!tr.RecordMissing(dpnid, goid, 30000));

    // When door appears on client, tracker clears entry cleanly
    tr.MarkPresent(dpnid, goid);
    assert(tr.Size() == 0);

    // After re-missing, fresh grace window applies
    assert(!tr.RecordMissing(dpnid, goid, 40000));
    assert(tr.RecordMissing(dpnid, goid, 41300));

    // Disconnect clears player's entries
    tr.ClearPlayer(dpnid);
    assert(tr.Size() == 0);

    // Stale pruning
    assert(!tr.RecordMissing(dpnid, goid, 50000));
    tr.PruneStale(50000 + kStaleEntryMs - 1);
    assert(tr.Size() == 1);
    tr.PruneStale(50000 + kStaleEntryMs);
    assert(tr.Size() == 0);
}

static void TestHeroNodeChangeResetsRetryCap() {
    using namespace doorfix;
    DoorMissingTracker tr;
    const uint32_t dpnid = 0xAABB0002u;
    const uint32_t goid  = 0x090004F4u;
    const uint32_t nodeA = 0x21F0C6FBu;  // a9_r2a_clockdungeon
    const uint32_t nodeB = 0x12246AD7u;  // a9_r3_clockroom right before door 0x0B8001E7

    // Exhaust all 5 attempts while player is far away on nodeA
    assert(!tr.RecordMissing(dpnid, goid, nodeA, 1000));
    assert(tr.RecordMissing(dpnid, goid, nodeA, 2300));   // #1
    assert(tr.RecordMissing(dpnid, goid, nodeA, 7400));   // #2
    assert(tr.RecordMissing(dpnid, goid, nodeA, 12500));  // #3
    assert(tr.RecordMissing(dpnid, goid, nodeA, 17600));  // #4
    assert(tr.RecordMissing(dpnid, goid, nodeA, 22700));  // #5
    assert(!tr.RecordMissing(dpnid, goid, nodeA, 30000)); // capped on same node

    // Moving to nodeB within cooldown still respects cooldown
    assert(!tr.RecordMissing(dpnid, goid, nodeB, 25000));
    // Moving to nodeB after cooldown resets attempt cap and resyncs immediately
    assert(tr.RecordMissing(dpnid, goid, nodeB, 28000));
    // Subsequent cooldown still applies on nodeB
    assert(!tr.RecordMissing(dpnid, goid, nodeB, 30000));
    assert(tr.RecordMissing(dpnid, goid, nodeB, 33100));
}

static void TestPoisonGoCreateReqPrev() {
    using namespace doorfix;
    uint8_t req[0x50]  = {};
    uint8_t prev[0x50] = {};

    auto u32 = [](uint8_t* p, size_t off) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(p + off);
    };

    // Case 1: Packet #1 of session (prev.m_PlayerId == 0) MUST preserve 0 for XferPlayerId framing
    u32(req, 0x04)  = 0xB8u;
    u32(req, 0x08)  = 0x01u;
    u32(req, 0x10)  = 0x3F800000u;
    u32(req, 0x20)  = 0x00000000u;
    u32(prev, 0x04) = 0xB8u;
    u32(prev, 0x08) = 0x00u;
    u32(prev, 0x10) = 0x3F800000u;
    u32(prev, 0x20) = 0x00000000u;

    PoisonGoCreateReqPrev(req, prev);
    assert(u32(prev, 0x04) != u32(req, 0x04));
    assert(u32(prev, 0x08) == 0u);  // preserved!
    assert(u32(prev, 0x10) != u32(req, 0x10));
    assert(u32(prev, 0x20) != u32(req, 0x20));

    // Case 2: Packet #2+ (prev.m_PlayerId != 0) MUST poison to non-zero != req.m_PlayerId
    u32(prev, 0x08) = 0x01u;
    u32(req, 0x04)  = 0xFFFFFFFFu;  // edge value
    u32(prev, 0x04) = 0xFFFFFFFFu;
    PoisonGoCreateReqPrev(req, prev);
    assert(u32(prev, 0x04) != u32(req, 0x04));
    assert(u32(prev, 0x08) != 0u && u32(prev, 0x08) != u32(req, 0x08));

    // Edge value: req.m_PlayerId == 0x80000000u
    u32(req, 0x08)  = 0x80000000u;
    u32(prev, 0x08) = 0x80000000u;
    PoisonGoCreateReqPrev(req, prev);
    assert(u32(prev, 0x08) != 0u && u32(prev, 0x08) != u32(req, 0x08));
}

static void TestPoisonGoCloneReqPrev() {
    using namespace doorfix;
    uint8_t req[0x5C]  = {};
    uint8_t prev[0x5C] = {};

    auto u32 = [](uint8_t* p, size_t off) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(p + off);
    };

    const char* tpl = "golem_clockwork";
    u32(req, 0x00)  = 0x12000034u;
    u32(prev, 0x00) = 0x12000034u;
    u32(req, 0x08)  = 0x10u;
    u32(prev, 0x08) = 0x10u;
    u32(req, 0x10)  = 0x41200000u;
    u32(prev, 0x10) = 0x41200000u;
    u32(req, 0x20)  = 0x3F800000u;
    u32(prev, 0x20) = 0x3F800000u;
    std::memcpy(prev + 0x3C, &tpl, sizeof(tpl));

    PoisonGoCloneReqPrev(req, prev);
    assert(u32(prev, 0x00) != u32(req, 0x00));
    assert(u32(prev, 0x08) != 0u && u32(prev, 0x08) != u32(req, 0x08));
    assert(u32(prev, 0x10) != u32(req, 0x10));
    assert(u32(prev, 0x20) != u32(req, 0x20));
    const char* afterTpl = tpl;
    std::memcpy(&afterTpl, prev + 0x3C, sizeof(afterTpl));
    assert(afterTpl == nullptr);

    // Packet #1 invariant for clone req too
    u32(prev, 0x08) = 0u;
    PoisonGoCloneReqPrev(req, prev);
    assert(u32(prev, 0x08) == 0u);
}

int main() {
    TestTemplateMatching();
    TestPacketValidation();
    TestMissingTrackerGraceCooldownAndCap();
    TestHeroNodeChangeResetsRetryCap();
    TestPoisonGoCreateReqPrev();
    TestPoisonGoCloneReqPrev();
    std::printf("doorfix_test: ALL PASSED\n");
    return 0;
}
