#pragma once
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace gamepad {

constexpr float kPi = 3.14159265358979323846f;

struct Stick2D {
    float x = 0.0f;         // [-1, 1], +X = right
    float y = 0.0f;         // [-1, 1], +Y = up
    float magnitude = 0.0f; // [0, 1] after deadzone & response curve
};

inline Stick2D ProcessStick(int16_t rawX, int16_t rawY, float deadzone = 0.18f, float responseExp = 1.35f) {
    float nx = (rawX < 0) ? (static_cast<float>(rawX) / 32768.0f) : (static_cast<float>(rawX) / 32767.0f);
    float ny = (rawY < 0) ? (static_cast<float>(rawY) / 32768.0f) : (static_cast<float>(rawY) / 32767.0f);
    if (nx < -1.0f) nx = -1.0f;
    if (nx >  1.0f) nx =  1.0f;
    if (ny < -1.0f) ny = -1.0f;
    if (ny >  1.0f) ny =  1.0f;

    float dz = deadzone;
    if (dz < 0.02f) dz = 0.02f;
    if (dz > 0.85f) dz = 0.85f;

    float len = std::sqrt(nx * nx + ny * ny);
    if (len <= dz) {
        return {0.0f, 0.0f, 0.0f};
    }

    float clampedLen = (len > 1.0f) ? 1.0f : len;
    float linear = (clampedLen - dz) / (1.0f - dz);
    float curved = (responseExp > 0.0f) ? std::pow(linear, responseExp) : linear;
    if (curved > 1.0f) curved = 1.0f;

    float inv = 1.0f / len;
    return {nx * inv * curved, ny * inv * curved, curved};
}

struct TriggerHysteresis {
    bool pressed = false;
    bool Update(uint8_t rawValue, float pressThresh = 0.35f, float releaseThresh = 0.20f) {
        float v = static_cast<float>(rawValue) / 255.0f;
        if (!pressed && v >= pressThresh) {
            pressed = true;
        } else if (pressed && v <= releaseThresh) {
            pressed = false;
        }
        return pressed;
    }
};

struct RepeatTimer {
    bool held = false;
    uint32_t nextFireMs = 0;

    // Returns true on initial press and at repeat intervals while held.
    bool Update(bool isDown, uint32_t nowMs, uint32_t initialDelayMs = 340, uint32_t repeatMs = 95) {
        if (!isDown) {
            held = false;
            nextFireMs = 0;
            return false;
        }
        if (!held) {
            held = true;
            nextFireMs = nowMs + initialDelayMs;
            return true;
        }
        if (static_cast<int32_t>(nowMs - nextFireMs) >= 0) {
            nextFireMs = nowMs + repeatMs;
            return true;
        }
        return false;
    }
};

// Combines horizontal screen-forward & screen-right XZ basis vectors with stick input.
inline bool ComputeCameraRelativeMoveDir(
    float fwdX, float fwdZ,
    float rightX, float rightZ,
    float stickX, float stickY,
    float& outX, float& outZ)
{
    float fl = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
    float rl = std::sqrt(rightX * rightX + rightZ * rightZ);
    if (fl < 1e-5f || rl < 1e-5f) {
        outX = 0.0f;
        outZ = 0.0f;
        return false;
    }
    fwdX /= fl; fwdZ /= fl;
    rightX /= rl; rightZ /= rl;

    float dx = rightX * stickX + fwdX * stickY;
    float dz = rightZ * stickX + fwdZ * stickY;
    float dl = std::sqrt(dx * dx + dz * dz);
    if (dl < 1e-4f) {
        outX = 0.0f;
        outZ = 0.0f;
        return false;
    }
    outX = dx / dl;
    outZ = dz / dl;
    return true;
}

struct ProbeOffset {
    float dx;
    float dz;
};

// Generates wall-sliding candidate offsets around the desired movement direction.
inline std::vector<ProbeOffset> GenerateWallSlideOffsets(float dirX, float dirZ, float baseDist = 2.6f) {
    struct Spec { float deg; float distScale; };
    static constexpr Spec kSpecs[] = {
        {  0.0f, 1.00f },
        { 28.0f, 0.95f },
        {-28.0f, 0.95f },
        { 55.0f, 0.85f },
        {-55.0f, 0.85f },
        {  0.0f, 0.55f },
        { 75.0f, 0.65f },
        {-75.0f, 0.65f },
    };
    std::vector<ProbeOffset> out;
    out.reserve(sizeof(kSpecs) / sizeof(kSpecs[0]));
    for (const auto& s : kSpecs) {
        float rad = s.deg * (kPi / 180.0f);
        float c = std::cos(rad);
        float sn = std::sin(rad);
        float rx = dirX * c - dirZ * sn;
        float rz = dirX * sn + dirZ * c;
        float d = baseDist * s.distScale;
        out.push_back({rx * d, rz * d});
    }
    return out;
}

// ----------------------------------------------------------------------------------------------
// Smart World Target / Interact Scoring
// ----------------------------------------------------------------------------------------------
enum class TargetKind : uint8_t {
    None = 0,
    Enemy = 1,
    Interactable = 2, // Chest, door, lever, shrine, displacer, NPC/merchant, tombstone
    Breakable = 3,    // Barrel, crate, pot
    GroundItem = 4,   // Loot item on ground
    AllyPlayer = 5,   // Other player (for trade)
};

struct TargetCandidate {
    uint32_t goid = 0;
    TargetKind kind = TargetKind::None;
    float dist = 0.0f;      // World distance in meters
    float dirX = 0.0f;      // Unit direction from hero to target (XZ)
    float dirZ = 0.0f;
    float screenX = 0.0f;
    float screenY = 0.0f;
    bool onScreen = false;
};

inline float ScoreTargetCandidate(
    const TargetCandidate& c,
    float aimDirX, float aimDirZ, bool hasAimDir,
    uint32_t currentLockedGoid, bool manualLock)
{
    if (!c.goid || c.kind == TargetKind::None) return -1e9f;

    // Base priority by kind and range ceiling
    float base = 0.0f;
    float maxRange = 16.0f;
    switch (c.kind) {
        case TargetKind::Enemy:
            base = 100.0f;
            maxRange = 18.0f;
            break;
        case TargetKind::Interactable:
            // Nearby interactables (chests, doors, NPCs, shrines) win when standing close and no enemy is point-blank
            base = (c.dist <= 4.5f) ? 98.0f : 72.0f;
            maxRange = 10.0f;
            break;
        case TargetKind::Breakable:
            base = 65.0f;
            maxRange = 8.0f;
            break;
        case TargetKind::GroundItem:
            base = 55.0f;
            maxRange = 7.0f;
            break;
        case TargetKind::AllyPlayer:
            base = 40.0f;
            maxRange = 6.5f;
            break;
        default:
            return -1e9f;
    }

    if (c.dist > maxRange) return -1e9f;

    float score = base - c.dist * 4.5f;

    if (hasAimDir) {
        float dot = c.dirX * aimDirX + c.dirZ * aimDirZ;
        score += dot * 26.0f;
    }

    if (c.onScreen) {
        score += 28.0f;
    }

    // Sticky lock hysteresis prevents target jitter
    if (c.goid == currentLockedGoid) {
        score += manualLock ? 180.0f : 18.0f;
    }

    return score;
}

// Deterministically cycles active AWP slots (0=Melee, 1=Ranged, 2=Spell1, 3=Spell2)
// in HUD visual order: dir > 0 moves left-to-right (RB: 0->1->2->3->0),
// dir < 0 moves right-to-left (LB: 3->2->1->0->3), skipping unavailable slots.
// Returns the next slot index in 0..3, or -1 if no other slot is available.
inline int SelectNextWeaponSlot(int currentSlot, int dir, const bool available[4]) {
    if (!available) return -1;
    int step = (dir >= 0) ? 1 : -1;
    int cur = (currentSlot >= 0 && currentSlot < 4) ? currentSlot : 0;
    for (int i = 1; i <= 4; ++i) {
        int cand = (cur + step * i + 16) % 4;
        if (cand == cur) return -1;
        if (available[cand]) return cand;
    }
    return -1;
}

inline int SelectBestTargetIndex(
    const std::vector<TargetCandidate>& candidates,
    float aimDirX, float aimDirZ, bool hasAimDir,
    uint32_t currentLockedGoid, bool manualLock)
{
    int bestIdx = -1;
    float bestScore = -1e8f;
    for (size_t i = 0; i < candidates.size(); ++i) {
        float s = ScoreTargetCandidate(candidates[i], aimDirX, aimDirZ, hasAimDir, currentLockedGoid, manualLock);
        if (s > bestScore) {
            bestScore = s;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

// Cycles deterministically through enemies first, then interactables/breakables, sorted by distance.
// When no combat/interactable targets exist nearby (e.g. standing over a post-combat loot pile),
// falls back to cycling GroundItems so players can inspect each dropped item's Ground Tooltip with R3.
inline uint32_t CycleNextTargetGoid(const std::vector<TargetCandidate>& candidates, uint32_t currentGoid, int delta = 1) {
    std::vector<const TargetCandidate*> pool;
    for (const auto& c : candidates) {
        if (c.goid != 0 && (c.kind == TargetKind::Enemy || c.kind == TargetKind::Interactable || c.kind == TargetKind::Breakable)) {
            pool.push_back(&c);
        }
    }
    if (pool.empty()) {
        for (const auto& c : candidates) {
            if (c.goid != 0 && c.kind == TargetKind::GroundItem) {
                pool.push_back(&c);
            }
        }
    }
    if (pool.empty()) return 0;

    std::sort(pool.begin(), pool.end(), [](const TargetCandidate* a, const TargetCandidate* b) {
        if (a->kind != b->kind) return static_cast<int>(a->kind) < static_cast<int>(b->kind);
        if (std::fabs(a->dist - b->dist) > 0.25f) return a->dist < b->dist;
        return a->goid < b->goid;
    });

    int curIdx = -1;
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i]->goid == currentGoid) {
            curIdx = static_cast<int>(i);
            break;
        }
    }
    if (curIdx < 0) return pool[0]->goid;
    int n = static_cast<int>(pool.size());
    int nextIdx = (curIdx + (delta >= 0 ? 1 : -1) + n) % n;
    return pool[nextIdx]->goid;
}

// ----------------------------------------------------------------------------------------------
// Idle Ground-Item Hover Sync (triggers GroundTooltip natively without combat clutter)
// ----------------------------------------------------------------------------------------------
struct GroundTooltipHoverState {
    bool     hoverActive = false;
    uint32_t hoveredGoid = 0;
    float    hoveredX    = 0.0f;
    float    hoveredY    = 0.0f;
    uint32_t idleSinceMs = 0;
    bool     wasIdle     = false;
};

enum class GroundHoverAction : uint8_t {
    None        = 0, // No cursor change needed (waiting for dwell, stationary hover, or already parked)
    MoveToItem  = 1, // Sync hidden virtual cursor onto (x, y) over the ground item
    ParkNeutral = 2, // Return hidden virtual cursor to safe neutral park (50%, 22%)
};

struct GroundHoverUpdate {
    GroundHoverAction action = GroundHoverAction::None;
    float x = 0.0f;
    float y = 0.0f;
};

inline GroundHoverUpdate UpdateGroundTooltipHover(
    GroundTooltipHoverState& st,
    uint32_t nowMs,
    bool isIdle,
    bool hasGroundItem,
    uint32_t itemGoid,
    float itemX,
    float itemY,
    int screenW,
    int screenH,
    uint32_t hoverDelayMs = 150,
    float repollDeltaPx = 3.0f)
{
    const float parkX = (screenW > 0) ? (static_cast<float>(screenW) * 0.5f) : 512.0f;
    const float parkY = (screenH > 0) ? (static_cast<float>(screenH) * 0.22f) : 168.0f;

    // Keep synced cursor strictly inside the 3D playfield (never over top portraits or bottom HUD bars)
    bool inSafePlayfield = hasGroundItem && itemGoid != 0 && screenW > 64 && screenH > 64 &&
                           itemX >= 16.0f && itemX <= static_cast<float>(screenW - 16) &&
                           itemY >= static_cast<float>(screenH) * 0.07f &&
                           itemY <= static_cast<float>(screenH) * 0.84f;

    if (!isIdle || !inSafePlayfield) {
        st.wasIdle = false;
        st.idleSinceMs = 0;
        if (st.hoverActive) {
            st.hoverActive = false;
            st.hoveredGoid = 0;
            return {GroundHoverAction::ParkNeutral, parkX, parkY};
        }
        return {GroundHoverAction::None, 0.0f, 0.0f};
    }

    if (!st.wasIdle) {
        st.wasIdle = true;
        st.idleSinceMs = nowMs;
    }

    uint32_t elapsed = nowMs - st.idleSinceMs;
    if (elapsed < hoverDelayMs) {
        return {GroundHoverAction::None, 0.0f, 0.0f};
    }

    float dx = itemX - st.hoveredX;
    float dy = itemY - st.hoveredY;
    bool movedEnough = (dx * dx + dy * dy) >= (repollDeltaPx * repollDeltaPx);

    if (!st.hoverActive || st.hoveredGoid != itemGoid || movedEnough) {
        st.hoverActive = true;
        st.hoveredGoid = itemGoid;
        st.hoveredX = itemX;
        st.hoveredY = itemY;
        return {GroundHoverAction::MoveToItem, itemX, itemY};
    }

    return {GroundHoverAction::None, 0.0f, 0.0f};
}

// ----------------------------------------------------------------------------------------------
// Universal UI Focus Navigation & UIGridbox Stepping
// ----------------------------------------------------------------------------------------------
struct UiRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    int CenterX() const { return (left + right) / 2; }
    int CenterY() const { return (top + bottom) / 2; }
    bool Contains(int x, int y) const { return x >= left && x <= right && y >= top && y <= bottom; }
    bool Valid() const { return right > left && bottom > top; }
};

struct UiFocusWidget {
    uintptr_t id = 0;       // Unique pointer/id of UIWindow*
    UiRect rect{};
    int panelId = 0;        // Grouping key (interface + panel role)
    int uiType = 0;         // DSLOA eUIWindowType
    bool isGridbox = false;
    bool isSlider = false;
    bool isListbox = false;
    bool isEditbox = false;
};

// Finds the best directional neighbor for D-Pad navigation.
// dx, dy should be (-1,0), (+1,0), (0,-1), or (0,+1).
inline int FindDirectionalNeighbor(
    const std::vector<UiFocusWidget>& widgets,
    int curX, int curY,
    uintptr_t currentId,
    int currentPanelId,
    int dx, int dy)
{
    if (widgets.empty() || (dx == 0 && dy == 0)) return -1;

    auto scorePass = [&](bool samePanelOnly) -> int {
        int bestIdx = -1;
        float bestCost = 1e9f;
        for (size_t i = 0; i < widgets.size(); ++i) {
            const auto& w = widgets[i];
            if (!w.rect.Valid()) continue;
            if (currentId != 0 && w.id == currentId) continue;
            if (samePanelOnly && w.panelId != currentPanelId) continue;

            float vx = static_cast<float>(w.rect.CenterX() - curX);
            float vy = static_cast<float>(w.rect.CenterY() - curY);

            float primary = vx * static_cast<float>(dx) + vy * static_cast<float>(dy);
            if (primary <= 4.0f) continue; // Must lie strictly in the requested direction

            float perp = std::fabs(vx * static_cast<float>(dy) - vy * static_cast<float>(dx));

            // Allow a wide cone (perp <= primary * 2.5) with strong preference for aligned elements
            if (perp > primary * 2.5f + 48.0f) continue;

            float cost = primary + perp * 2.6f;
            if (w.panelId != currentPanelId) {
                cost += 60.0f; // Mild penalty for crossing to another panel
            }
            if (cost < bestCost) {
                bestCost = cost;
                bestIdx = static_cast<int>(i);
            }
        }
        return bestIdx;
    };

    // Pass 1: Prefer staying within the current panel if a nearby neighbor exists
    if (currentId != 0) {
        int sameIdx = scorePass(true);
        if (sameIdx >= 0) return sameIdx;
    }
    // Pass 2: Search across all visible panels
    return scorePass(false);
}

// Cycles between distinct panels sorted left-to-right by their horizontal center.
// Returns the widget index to focus in the target panel, or -1 if < 2 panels exist.
inline int CycleUiPanel(
    const std::vector<UiFocusWidget>& widgets,
    int currentPanelId,
    int direction /* +1 = RB/next, -1 = LB/prev */)
{
    struct PanelSummary {
        int panelId = 0;
        int sumX = 0;
        int count = 0;
        int bestWidgetIdx = -1;
        int bestWidgetArea = -1;
    };

    std::vector<PanelSummary> panels;
    for (size_t i = 0; i < widgets.size(); ++i) {
        const auto& w = widgets[i];
        if (!w.rect.Valid()) continue;
        PanelSummary* ps = nullptr;
        for (auto& p : panels) {
            if (p.panelId == w.panelId) { ps = &p; break; }
        }
        if (!ps) {
            panels.push_back({w.panelId, 0, 0, static_cast<int>(i), -1});
            ps = &panels.back();
        }
        ps->sumX += w.rect.CenterX();
        ps->count += 1;
        // Prefer focusing the gridbox inside a panel if present, otherwise the topmost/first widget
        int score = w.isGridbox ? 1000000 : (10000 - w.rect.top);
        if (score > ps->bestWidgetArea) {
            ps->bestWidgetArea = score;
            ps->bestWidgetIdx = static_cast<int>(i);
        }
    }

    if (panels.size() < 2) return -1;

    std::sort(panels.begin(), panels.end(), [](const PanelSummary& a, const PanelSummary& b) {
        int ax = a.sumX / (a.count > 0 ? a.count : 1);
        int bx = b.sumX / (b.count > 0 ? b.count : 1);
        if (ax != bx) return ax < bx;
        return a.panelId < b.panelId;
    });

    int curIdx = -1;
    for (size_t i = 0; i < panels.size(); ++i) {
        if (panels[i].panelId == currentPanelId) {
            curIdx = static_cast<int>(i);
            break;
        }
    }
    int n = static_cast<int>(panels.size());
    int targetPanelIdx = (curIdx < 0) ? 0 : ((curIdx + (direction >= 0 ? 1 : -1) + n) % n);
    return panels[targetPanelIdx].bestWidgetIdx;
}

// ----------------------------------------------------------------------------------------------
// UIGridbox Cell Stepping
// ----------------------------------------------------------------------------------------------
struct GridStepResult {
    bool stayedInGrid = false;
    int col = 0;
    int row = 0;
    UiRect cellRect{};
};

inline UiRect ComputeGridCellRect(const UiRect& gridRect, int cols, int rows, int col, int row) {
    if (cols <= 0 || rows <= 0 || !gridRect.Valid()) return {};
    if (col < 0) col = 0;
    if (col >= cols) col = cols - 1;
    if (row < 0) row = 0;
    if (row >= rows) row = rows - 1;

    int w = gridRect.Width();
    int h = gridRect.Height();
    int l = gridRect.left + (col * w) / cols;
    int r = gridRect.left + ((col + 1) * w) / cols;
    int t = gridRect.top  + (row * h) / rows;
    int b = gridRect.top  + ((row + 1) * h) / rows;
    return {l, t, r, b};
}

inline void PixelToGridCell(const UiRect& gridRect, int cols, int rows, int px, int py, int& outCol, int& outRow) {
    outCol = 0;
    outRow = 0;
    if (cols <= 0 || rows <= 0 || !gridRect.Valid()) return;
    int w = gridRect.Width();
    int h = gridRect.Height();
    int c = ((px - gridRect.left) * cols) / w;
    int r = ((py - gridRect.top) * rows) / h;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    outCol = c;
    outRow = r;
}

// Steps one cell inside a gridbox. If stepping past the grid boundary, stayedInGrid = false
// so the caller can seamlessly snap out to neighboring UI widgets (Paperdoll, Close, Sort, etc.).
inline GridStepResult StepGridCell(const UiRect& gridRect, int cols, int rows, int curCol, int curRow, int dx, int dy) {
    int nc = curCol + dx;
    int nr = curRow + dy;
    if (nc < 0 || nc >= cols || nr < 0 || nr >= rows) {
        return {false, curCol, curRow, ComputeGridCellRect(gridRect, cols, rows, curCol, curRow)};
    }
    return {true, nc, nr, ComputeGridCellRect(gridRect, cols, rows, nc, nr)};
}

// ----------------------------------------------------------------------------------------------
// Virtual Cursor Integration (Resolution-Scaled + Magnetic Hover Slowdown)
// ----------------------------------------------------------------------------------------------
inline void IntegrateVirtualCursor(
    float& curX, float& curY,
    const Stick2D& stick,
    float dtSec,
    int screenW, int screenH,
    float speedMultiplier,
    bool fastBoost,
    bool hoveringInteractive)
{
    if (screenW <= 0 || screenH <= 0 || dtSec <= 0.0f) return;
    if (dtSec > 0.1f) dtSec = 0.1f;

    if (stick.magnitude <= 0.0f) return;

    // Scale base speed to screen height so 720p, 1080p, and 4K feel identical (~0.85 screen-heights/sec at full stick)
    float basePxPerSec = static_cast<float>(screenH) * 0.85f * speedMultiplier;
    if (fastBoost) {
        basePxPerSec *= 1.65f;
    } else if (hoveringInteractive) {
        basePxPerSec *= 0.55f; // Gentle magnetic precision over clickable controls/items
    }

    // XInput stick +Y is up, screen +Y is down
    curX += stick.x * basePxPerSec * dtSec;
    curY -= stick.y * basePxPerSec * dtSec;

    if (curX < 1.0f) curX = 1.0f;
    if (curY < 1.0f) curY = 1.0f;
    if (curX > static_cast<float>(screenW - 2)) curX = static_cast<float>(screenW - 2);
    if (curY > static_cast<float>(screenH - 2)) curY = static_cast<float>(screenH - 2);
}

} // namespace gamepad
