#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "../src/gamepad_math.h"

using namespace gamepad;

static bool Near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

static void TestStickProcessing() {
    // Inside deadzone -> exact zero
    auto s0 = ProcessStick(0, 0, 0.18f, 1.35f);
    assert(s0.x == 0.0f && s0.y == 0.0f && s0.magnitude == 0.0f);

    auto sDz = ProcessStick(4000, -3500, 0.18f, 1.35f); // ~0.16 magnitude
    assert(sDz.magnitude == 0.0f && sDz.x == 0.0f && sDz.y == 0.0f);

    // Full right deflection -> (1, 0, 1)
    auto sRight = ProcessStick(32767, 0, 0.18f, 1.35f);
    assert(Near(sRight.x, 1.0f) && Near(sRight.y, 0.0f) && Near(sRight.magnitude, 1.0f));

    // Full down-left diagonal clamps cleanly to unit circle
    auto sDiag = ProcessStick(-32768, -32768, 0.18f, 1.35f);
    assert(Near(sDiag.magnitude, 1.0f));
    assert(Near(sDiag.x, -0.7071f, 0.01f) && Near(sDiag.y, -0.7071f, 0.01f));

    // Mid-deflection scales monotonically and smoothly
    auto sMid = ProcessStick(19000, 0, 0.18f, 1.35f);
    assert(sMid.magnitude > 0.25f && sMid.magnitude < 0.75f);
    assert(Near(sMid.x, sMid.magnitude) && Near(sMid.y, 0.0f));
}

static void TestTriggerHysteresis() {
    TriggerHysteresis h{};
    assert(!h.Update(0));
    assert(!h.Update(80));  // ~0.31 < 0.35
    assert(h.Update(95));   // ~0.37 >= 0.35 -> pressed
    assert(h.Update(70));   // ~0.27 > 0.20 -> stays pressed (no chatter!)
    assert(!h.Update(45));  // ~0.17 <= 0.20 -> released
    assert(!h.Update(75));  // ~0.29 < 0.35 -> stays released
}

static void TestRepeatTimer() {
    RepeatTimer rt{};
    assert(!rt.Update(false, 1000));
    assert(rt.Update(true, 1000, 340, 95));  // Initial press fires immediately
    assert(!rt.Update(true, 1200, 340, 95)); // Still in initial delay
    assert(rt.Update(true, 1340, 340, 95));  // First repeat fires at +340ms
    assert(!rt.Update(true, 1400, 340, 95)); // Waiting for repeat cadence
    assert(rt.Update(true, 1435, 340, 95));  // Second repeat fires at +95ms
    assert(!rt.Update(false, 1450, 340, 95));
    assert(rt.Update(true, 1460, 340, 95));  // Fresh press fires immediately again
}

static void TestCameraRelativeMovementAndWallSlide() {
    float ox = 0.0f, oz = 0.0f;
    // Camera facing +Z, right = +X: stick up -> +Z, stick right -> +X
    assert(ComputeCameraRelativeMoveDir(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, ox, oz));
    assert(Near(ox, 0.0f) && Near(oz, 1.0f));

    assert(ComputeCameraRelativeMoveDir(0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, ox, oz));
    assert(Near(ox, 1.0f) && Near(oz, 0.0f));

    // Rotated camera 45 deg: stick up+right combines and normalizes cleanly
    assert(ComputeCameraRelativeMoveDir(0.7071f, 0.7071f, 0.7071f, -0.7071f, 1.0f, 1.0f, ox, oz));
    assert(Near(ox * ox + oz * oz, 1.0f));
    assert(Near(ox, 1.0f, 0.01f) && Near(oz, 0.0f, 0.01f));

    // Zero stick returns false
    assert(!ComputeCameraRelativeMoveDir(0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, ox, oz));

    // Wall-slide probe offsets start straight ahead and branch symmetrically
    auto probes = GenerateWallSlideOffsets(0.0f, 1.0f, 2.5f);
    assert(probes.size() == 8);
    assert(Near(probes[0].dx, 0.0f) && Near(probes[0].dz, 2.5f));
    assert(Near(probes[1].dx, -probes[2].dx) && Near(probes[1].dz, probes[2].dz));
    assert(Near(probes[3].dx, -probes[4].dx) && Near(probes[3].dz, probes[4].dz));
    assert(probes[5].dz < probes[0].dz && probes[5].dz > 0.0f);
}

static void TestTargetScoringAndCycling() {
    std::vector<TargetCandidate> cands = {
        {0x101, TargetKind::Enemy,        7.0f,  0.0f,  1.0f, 500, 300, true},
        {0x102, TargetKind::Enemy,        6.5f,  0.0f, -1.0f, 500, 600, true},
        {0x201, TargetKind::Interactable, 2.5f,  0.0f,  1.0f, 510, 360, true},
        {0x301, TargetKind::GroundItem,   2.0f,  1.0f,  0.0f, 550, 400, true},
    };

    // Facing +Z with close interactable (2.5m) vs distant enemy (7m): nearby chest/NPC wins!
    int idx = SelectBestTargetIndex(cands, 0.0f, 1.0f, true, 0, false);
    assert(idx == 2 && cands[idx].goid == 0x201);

    // Manual lock on enemy 0x102 stays locked even while facing away
    idx = SelectBestTargetIndex(cands, 0.0f, 1.0f, true, 0x102, true);
    assert(idx == 1 && cands[idx].goid == 0x102);

    // Target cycling steps deterministically through enemies then interactables
    uint32_t c1 = CycleNextTargetGoid(cands, 0, +1);
    assert(c1 == 0x102); // Closest enemy first
    uint32_t c2 = CycleNextTargetGoid(cands, c1, +1);
    assert(c2 == 0x101);
    uint32_t c3 = CycleNextTargetGoid(cands, c2, +1);
    assert(c3 == 0x201);
    uint32_t c4 = CycleNextTargetGoid(cands, c3, +1);
    assert(c4 == 0x102); // Wraps cleanly
}

static void TestUiFocusSnappingAndPanels() {
    // Simulate Paperdoll (panel 1, left), Inventory Grid (panel 2, mid), Shared Stash (panel 3, right)
    std::vector<UiFocusWidget> ws = {
        {1, { 40, 100, 100, 160}, 1, 13, false, false, false, false}, // Paperdoll helm
        {2, { 40, 180, 100, 260}, 1, 13, false, false, false, false}, // Paperdoll chest
        {3, {200, 100, 400, 420}, 2, 10, true,  false, false, false}, // Inventory grid
        {4, {200, 440, 280, 470}, 2,  1, false, false, false, false}, // Sort button
        {5, {460, 100, 660, 420}, 3, 10, true,  false, false, false}, // Shared Stash grid
    };

    // From helm (id=1), Down snaps to chest (id=2) within same panel
    int downIdx = FindDirectionalNeighbor(ws, 70, 130, 1, 1, 0, +1);
    assert(downIdx == 1 && ws[downIdx].id == 2);

    // From helm (id=1), Right crosses to Inventory Grid (id=3)
    int rightIdx = FindDirectionalNeighbor(ws, 70, 130, 1, 1, +1, 0);
    assert(rightIdx == 2 && ws[rightIdx].id == 3);

    // From Inventory Grid (id=3), Down snaps to Sort button (id=4)
    int sortIdx = FindDirectionalNeighbor(ws, 300, 260, 3, 2, 0, +1);
    assert(sortIdx == 3 && ws[sortIdx].id == 4);

    // Panel cycling (RB / LB): Paperdoll(1) -> Inventory(2) -> Shared Stash(3) -> Paperdoll(1)
    int pNext = CycleUiPanel(ws, 1, +1);
    assert(pNext == 2 && ws[pNext].panelId == 2); // Prefers gridbox inside panel 2!
    int pNext2 = CycleUiPanel(ws, 2, +1);
    assert(pNext2 == 4 && ws[pNext2].panelId == 3);
    int pWrap = CycleUiPanel(ws, 3, +1);
    assert(pWrap == 0 && ws[pWrap].panelId == 1);
    int pPrev = CycleUiPanel(ws, 1, -1);
    assert(pPrev == 4 && ws[pPrev].panelId == 3);
}

static void TestGridboxStepping() {
    UiRect grid{100, 200, 300, 500}; // 200x300 px -> 4 cols x 6 rows (50x50 cells)
    auto c00 = ComputeGridCellRect(grid, 4, 6, 0, 0);
    assert(c00.left == 100 && c00.top == 200 && c00.right == 150 && c00.bottom == 250);

    int col = -1, row = -1;
    PixelToGridCell(grid, 4, 6, 225, 375, col, row);
    assert(col == 2 && row == 3);

    auto stepIn = StepGridCell(grid, 4, 6, 2, 3, +1, 0);
    assert(stepIn.stayedInGrid && stepIn.col == 3 && stepIn.row == 3);

    // Stepping right from rightmost col spills out of grid cleanly
    auto stepOut = StepGridCell(grid, 4, 6, 3, 3, +1, 0);
    assert(!stepOut.stayedInGrid);
}

static void TestVirtualCursorIntegration() {
    float x = 500.0f, y = 400.0f;
    Stick2D right = {1.0f, 0.0f, 1.0f};

    // Normal motion vs magnetic hover slowdown vs fast boost
    float xNorm = x, yNorm = y;
    IntegrateVirtualCursor(xNorm, yNorm, right, 0.1f, 1920, 1080, 1.0f, false, false);

    float xMag = x, yMag = y;
    IntegrateVirtualCursor(xMag, yMag, right, 0.1f, 1920, 1080, 1.0f, false, true);

    float xFast = x, yFast = y;
    IntegrateVirtualCursor(xFast, yFast, right, 0.1f, 1920, 1080, 1.0f, true, false);

    assert(xMag - x < xNorm - x);
    assert(xFast - x > xNorm - x);

    // Clamping at screen borders
    float xEdge = 1915.0f, yEdge = 5.0f;
    Stick2D upRight = {0.7071f, 0.7071f, 1.0f};
    IntegrateVirtualCursor(xEdge, yEdge, upRight, 0.1f, 1920, 1080, 1.0f, true, false);
    assert(xEdge <= 1918.0f && yEdge >= 1.0f);
}

int main() {
    TestStickProcessing();
    TestTriggerHysteresis();
    TestRepeatTimer();
    TestCameraRelativeMovementAndWallSlide();
    TestTargetScoringAndCycling();
    TestUiFocusSnappingAndPanels();
    TestGridboxStepping();
    TestVirtualCursorIntegration();
    std::printf("gamepad_test: ALL 8 TEST SUITES PASSED!\n");
    return 0;
}
