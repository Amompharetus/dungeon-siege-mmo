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

int main() {
    TestTemplateMatching();
    TestPacketValidation();
    TestMissingTrackerGraceCooldownAndCap();
    std::printf("doorfix_test: ALL PASSED\n");
    return 0;
}
