#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "siegehost.h"
#include "engine.h"
#include "gamepad_internal.h"

using namespace gp_internal;

namespace {

// DSLOA.exe v1.50 verified UI_TYPE_* enum (table at 0x007A5078)
constexpr int kUiWindow           = 0;
constexpr int kUiButton           = 1;
constexpr int kUiCheckbox         = 2;
constexpr int kUiSlider           = 3;
constexpr int kUiListbox          = 4;
constexpr int kUiRadioButton      = 5;
constexpr int kUiButtonMultistage = 6;
constexpr int kUiGridbox          = 10;
constexpr int kUiItemSlot         = 13;
constexpr int kUiEditBox          = 16;
constexpr int kUiComboBox         = 17;
constexpr int kUiListReport       = 19;
constexpr int kUiTab              = 23;

typedef uint32_t (__thiscall* SortInventoryFn)(void*);
static const auto s_sortInventory = (SortInventoryFn)0x0049A594;

// Interfaces that represent blocking/interactive menus or panels during gameplay.
// IMPORTANT: Never include always-on gameplay HUD bars (field_commands, BuffUI,
// multi_inventory, character_awp, team_portraits, mini_map, compass_hotpoints) or
// the controller will be trapped permanently in MenuUI mode!
static const char* const kBlockingGamePanels[] = {
    "character",
    "store",
    "spell",
    "DSX_backpack_ui",
    "multiplayer_trade",
    "dialogue_box",
    "journal",
    "lore_book",
    "in_game_menu",
    "in_game_host_options",
    "in_game_host_options_kick",
    "options_audio",
    "options_video",
    "options_game",
    "options_input",
    "options_bindings",
    "gold_transfer",
    "multiplayer_respawn",
    "defeat_dialog",
    "defeat_menu",
    "backend_dialog",
    "mp_dialog",
    "message_box_large",
    "hire_stats",
    "loadsave_game",
    "end_game",
    "end_game_summary",
    "world_map",
};

// Known modal dialogs — when visible, restrict D-Pad focus snapping to the top-most modal
static const char* const kModalInterfaces[] = {
    "backend_dialog",
    "frontend_dialog",
    "mp_dialog",
    "message_box_large",
    "defeat_dialog",
    "save_delete_confirm",
    "save_delete_fail",
    "select_name_dialog",
    "continue_dialog",
    "about_dialog",
    "cdcheck_dialog",
    "enter_password",
    "gold_transfer",
    "in_game_menu",
    "in_game_host_options",
    "in_game_host_options_kick",
    "multiplayer_respawn",
    "options_audio",
    "options_video",
    "options_game",
    "options_input",
    "options_bindings",
    "staging_area_create_character",
    "staging_area_select_character",
    "staging_area_game_settings",
    "staging_area_map_settings",
    "staging_area_change_password",
    "staging_area_select_team",
};

// Passive / non-interactive overlays to ignore during focus discovery
static const char* const kIgnoredInterfaces[] = {
    "siegehost_gamepad_hud",
    "tool_tip",
    "world_tip",
    "rollover_help",
    "status_bars",
    "member_labels",
    "console_output",
    "compass_hotpoints",
    "fade_screen",
    "background",
    "load_bar",
    "loading_screen",
    "jip_glass",
    "nis_subtitle",
    "multiplayer_timer",
};

static bool NameInList(const char* name, const char* const* list, size_t count) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < count; ++i) {
        if (_stricmp(name, list[i]) == 0) return true;
    }
    return false;
}

static bool IsInteractiveType(int uiType) {
    switch (uiType) {
        case kUiButton:
        case kUiCheckbox:
        case kUiSlider:
        case kUiListbox:
        case kUiRadioButton:
        case kUiButtonMultistage:
        case kUiGridbox:
        case kUiItemSlot:
        case kUiEditBox:
        case kUiComboBox:
        case kUiListReport:
        case kUiTab:
            return true;
        default:
            return false;
    }
}

struct CollectedWidget {
    gamepad::UiFocusWidget base{};
    void* win = nullptr;
    std::string ifaceName;
    int gridRows = 0;
    int gridCols = 0;
};

static int HashPanelId(const char* ifaceName, int subGroup) {
    uint32_t h = 2166136261u;
    if (ifaceName) {
        for (const unsigned char* p = (const unsigned char*)ifaceName; *p; ++p) {
            unsigned char c = (*p >= 'A' && *p <= 'Z') ? (unsigned char)(*p + 32) : *p;
            h ^= c;
            h *= 16777619u;
        }
    }
    h ^= (uint32_t)(subGroup * 2654435761u);
    return (int)(h & 0x7FFFFFFFu);
}

static void CollectWindowsFromInterface(void* iface, const char* ifaceName, int screenW, int screenH,
                                        std::vector<CollectedWidget>& out)
{
    if (!iface || !Readable(iface, 0x80)) return;
    int count = *(const int*)((const char*)iface + 0x40);
    void* head = *(void**)((const char*)iface + 0x38);
    void* nilNode = *(void**)kAddrUiTreeNil;
    if (count <= 0 || count > 2048 || !head || !Readable(head, 0x18)) return;

    // Iterative DFS over the MSVC6 RB-tree at iface+0x38 (root = head->_Parent at +0x04)
    void* root = *(void**)((const char*)head + 0x04);
    if (!root || root == nilNode || root == head) return;

    std::vector<void*> stack;
    stack.reserve(64);
    stack.push_back(root);

    int visited = 0;
    while (!stack.empty() && visited < 512) {
        void* node = stack.back();
        stack.pop_back();
        if (!node || node == nilNode || node == head || !Readable(node, 0x18)) continue;
        ++visited;

        void* left  = *(void**)((const char*)node + 0x00);
        void* right = *(void**)((const char*)node + 0x08);
        if (right && right != nilNode && right != head) stack.push_back(right);
        if (left  && left  != nilNode && left  != head) stack.push_back(left);

        void* win = *(void**)((const char*)node + 0x10);
        if (!win || !Readable(win, 0x150)) continue;

        bool visible = (*(const char*)((const char*)win + 0x108) != 0);
        bool enabled = (*(const char*)((const char*)win + 0x139) != 0);
        bool passThrough = (*(const char*)((const char*)win + 0x149) != 0);
        if (!visible || !enabled || passThrough) continue;

        int uiType = *(const int*)((const char*)win + 0x04);
        if (!IsInteractiveType(uiType)) continue;

        // Skip disabled UIButtons (verified in UIButton::DisableButton at 0x007072CF: this+0x1C0 = 1)
        if (uiType == kUiButton && Readable(win, 0x1C8) && *(const char*)((const char*)win + 0x1C0) != 0) {
            continue;
        }

        gamepad::UiRect rc{};
        rc.left   = *(const int*)((const char*)win + 0x60);
        rc.top    = *(const int*)((const char*)win + 0x64);
        rc.right  = *(const int*)((const char*)win + 0x68);
        rc.bottom = *(const int*)((const char*)win + 0x6C);

        if (rc.Width() < 8 || rc.Height() < 8) continue;
        if (rc.right <= 0 || rc.bottom <= 0 || rc.left >= screenW || rc.top >= screenH) continue;
        // Skip full-screen backdrop windows accidentally tagged interactive
        if (uiType != kUiGridbox && rc.Width() >= screenW - 20 && rc.Height() >= screenH - 20) continue;

        // Group character paperdoll slots separately from character inventory grid for LB/RB panel jumping
        int subGroup = 0;
        if (ifaceName && _stricmp(ifaceName, "character") == 0) {
            if (uiType == kUiGridbox) subGroup = 2;
            else if (uiType == kUiItemSlot) subGroup = 1;
            else subGroup = 2;
        }

        CollectedWidget cw{};
        cw.win = win;
        cw.ifaceName = ifaceName ? ifaceName : "";
        cw.base.id = (uintptr_t)win;
        cw.base.rect = rc;
        cw.base.panelId = HashPanelId(ifaceName, subGroup);
        cw.base.uiType = uiType;
        cw.base.isGridbox = (uiType == kUiGridbox);
        cw.base.isSlider  = (uiType == kUiSlider);
        cw.base.isListbox = (uiType == kUiListbox || uiType == kUiListReport);
        cw.base.isEditbox = (uiType == kUiEditBox);

        if (uiType == kUiGridbox && Readable(win, 0x1C0)) {
            int rows = *(const int*)((const char*)win + 0x1B4);
            int cols = *(const int*)((const char*)win + 0x1B8);
            if (rows > 0 && rows <= 40 && cols > 0 && cols <= 40) {
                cw.gridRows = rows;
                cw.gridCols = cols;
            }
        }

        // Deduplicate by UIWindow*
        bool dup = false;
        for (const auto& ex : out) {
            if (ex.win == win) { dup = true; break; }
        }
        if (!dup) out.push_back(cw);
    }
}

static void CollectVisibleWidgets(std::vector<CollectedWidget>& out) {
    out.clear();
    void* uiShell = eng::UIShell_Singleton();
    if (!uiShell || !Readable(uiShell, 0x14)) return;

    void** begin = *(void***)((const char*)uiShell + 0x04);
    void** end   = *(void***)((const char*)uiShell + 0x08);
    if (!begin || !end || end <= begin || (size_t)(end - begin) > 256) return;
    if (!Readable(begin, (size_t)(end - begin) * sizeof(void*))) return;

    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);

    // Check top-most modal interface (iterate back-to-front in Z-order)
    void* topModal = nullptr;
    const char* topModalName = nullptr;
    for (void** it = end; it != begin; ) {
        --it;
        void* iface = *it;
        if (!iface || !Readable(iface, 0x80)) continue;
        bool vis = (*(const char*)((const char*)iface + 0x02) != 0);
        bool en  = (*(const char*)((const char*)iface + 0x7B) != 0);
        if (!vis || !en) continue;
        const char* nm = eng::GpC(iface, 4);
        if (!nm || !*nm) continue;
        if (NameInList(nm, kIgnoredInterfaces, sizeof(kIgnoredInterfaces) / sizeof(kIgnoredInterfaces[0]))) continue;

        bool isModal = (*(const char*)((const char*)iface + 0x00) != 0) ||
                       NameInList(nm, kModalInterfaces, sizeof(kModalInterfaces) / sizeof(kModalInterfaces[0]));
        if (isModal) {
            // Confirm it actually has at least one interactive widget before trapping focus inside it
            std::vector<CollectedWidget> probe;
            CollectWindowsFromInterface(iface, nm, sw, sh, probe);
            if (!probe.empty()) {
                topModal = iface;
                topModalName = nm;
                break;
            }
        }
    }

    if (topModal) {
        CollectWindowsFromInterface(topModal, topModalName, sw, sh, out);
        return;
    }

    // Otherwise collect from all visible enabled interfaces
    bool inGame = IsWorldStateInGame();
    bool blockingPanel = inGame && GamepadUI_IsAnyBlockingPanelOpen();

    for (void** it = begin; it != end; ++it) {
        void* iface = *it;
        if (!iface || !Readable(iface, 0x80)) continue;
        bool vis = (*(const char*)((const char*)iface + 0x02) != 0);
        bool en  = (*(const char*)((const char*)iface + 0x7B) != 0);
        if (!vis || !en) continue;
        const char* nm = eng::GpC(iface, 4);
        if (!nm || !*nm) continue;
        if (NameInList(nm, kIgnoredInterfaces, sizeof(kIgnoredInterfaces) / sizeof(kIgnoredInterfaces[0]))) continue;

        // When an in-game panel like Inventory/Store/Stash is open (and not explicit HUD Cursor mode),
        // focus stays cleanly on the open panels instead of accidentally snapping to tiny corner HUD bars.
        if (blockingPanel && !g_hudCursorToggle) {
            if (!NameInList(nm, kBlockingGamePanels, sizeof(kBlockingGamePanels) / sizeof(kBlockingGamePanels[0]))) {
                continue;
            }
        }

        CollectWindowsFromInterface(iface, nm, sw, sh, out);
    }
}

// ----------------------------------------------------------------------------------------------
// UIGridbox occupancy inspection (verified in DSLOA.named.c line 715900:
//   cell[row][col] at grid + (row * 100 + col) * 0x18 + 0x274 (1-byte occupied), +0x278 (itemId))
// ----------------------------------------------------------------------------------------------
static bool ReadGridCell(void* grid, int rows, int cols, int row, int col, bool& outOccupied, int& outItemId) {
    outOccupied = false;
    outItemId = -1;
    if (!grid || row < 0 || col < 0 || row >= rows || col >= cols || row >= 40 || col >= 40) return false;
    size_t off = (size_t)(row * 100 + col) * 0x18u + 0x274u;
    if (!Readable((const char*)grid + off, 8)) return false;
    outOccupied = (*((const char*)grid + off) != 0);
    outItemId = *(const int*)((const char*)grid + off + 4);
    return true;
}

static bool ComputeGridItemBounds(void* grid, int rows, int cols, int itemId,
                                  int& minC, int& minR, int& maxC, int& maxR)
{
    if (itemId < 0) return false;
    minC = cols; minR = rows; maxC = -1; maxR = -1;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            bool occ = false;
            int id = -1;
            if (ReadGridCell(grid, rows, cols, r, c, occ, id) && occ && id == itemId) {
                if (c < minC) minC = c;
                if (r < minR) minR = r;
                if (c > maxC) maxC = c;
                if (r > maxR) maxR = r;
            }
        }
    }
    return maxC >= minC && maxR >= minR;
}

static bool FindFreeRectInGrid(void* grid, int rows, int cols, int itemW, int itemH, int& outCol, int& outRow) {
    if (itemW <= 0 || itemH <= 0 || itemW > cols || itemH > rows) return false;
    for (int r = 0; r <= rows - itemH; ++r) {
        for (int c = 0; c <= cols - itemW; ++c) {
            bool fits = true;
            for (int dr = 0; dr < itemH && fits; ++dr) {
                for (int dc = 0; dc < itemW; ++dc) {
                    bool occ = false;
                    int id = -1;
                    if (!ReadGridCell(grid, rows, cols, r + dr, c + dc, occ, id) || occ) {
                        fits = false;
                        break;
                    }
                }
            }
            if (fits) {
                outCol = c;
                outRow = r;
                return true;
            }
        }
    }
    return false;
}

static int GetNumActiveCursorItems() {
    void* uiShell = eng::UIShell_Singleton();
    if (!uiShell || !Readable(uiShell, 0x44)) return 0;
    return *(const int*)((const char*)uiShell + 0x3C);
}

// ----------------------------------------------------------------------------------------------
// UI Navigation runtime state
// ----------------------------------------------------------------------------------------------
gamepad::RepeatTimer g_repUp{};
gamepad::RepeatTimer g_repDown{};
gamepad::RepeatTimer g_repLeft{};
gamepad::RepeatTimer g_repRight{};

uintptr_t g_focusedWidgetId = 0;
int       g_focusedPanelId  = 0;
int       g_gridCol         = 0;
int       g_gridRow         = 0;
bool      g_usingFocusSnap  = false;
bool      g_uiEntered       = false;

// Remember where an item was picked up so pressing B safely puts it back
bool  g_hasPickupOrigin = false;
float g_pickupOriginX   = 0.0f;
float g_pickupOriginY   = 0.0f;

// Right-stick wheel accumulator for smooth list/dialogue scrolling
float g_wheelAccum = 0.0f;

// Deferred quick-transfer state (2-frame native click sequence: pick up -> place on counterpart grid)
struct PendingTransfer {
    bool active = false;
    float destX = 0.0f;
    float destY = 0.0f;
    float restoreX = 0.0f;
    float restoreY = 0.0f;
    int stage = 0; // 1 = waiting 1 frame after pickup to drop at dest
} g_transfer{};

static void SnapCursorToWidget(const CollectedWidget& cw, bool enterGridAtOrigin) {
    g_focusedWidgetId = cw.base.id;
    g_focusedPanelId  = cw.base.panelId;
    g_usingFocusSnap  = true;

    if (cw.base.isGridbox && cw.gridCols > 0 && cw.gridRows > 0) {
        if (enterGridAtOrigin) {
            // Land on first occupied item if browsing empty-handed, otherwise (0,0)
            int bestC = 0, bestR = 0;
            if (GetNumActiveCursorItems() == 0) {
                bool foundOcc = false;
                for (int r = 0; r < cw.gridRows && !foundOcc; ++r) {
                    for (int c = 0; c < cw.gridCols; ++c) {
                        bool occ = false;
                        int id = -1;
                        if (ReadGridCell(cw.win, cw.gridRows, cw.gridCols, r, c, occ, id) && occ) {
                            bestC = c;
                            bestR = r;
                            foundOcc = true;
                            break;
                        }
                    }
                }
            }
            g_gridCol = bestC;
            g_gridRow = bestR;
        }
        auto cellRc = gamepad::ComputeGridCellRect(cw.base.rect, cw.gridCols, cw.gridRows, g_gridCol, g_gridRow);
        SetVirtualCursorClient((float)cellRc.CenterX(), (float)cellRc.CenterY(), true, true);
    } else {
        SetVirtualCursorClient((float)cw.base.rect.CenterX(), (float)cw.base.rect.CenterY(), true, true);
    }
}

static bool TryQuickTransfer(const std::vector<CollectedWidget>& widgets, const CollectedWidget& srcGrid) {
    if (!srcGrid.base.isGridbox || srcGrid.gridCols <= 0 || srcGrid.gridRows <= 0) return false;
    if (GetNumActiveCursorItems() > 0) return false;

    float cx = 0.0f, cy = 0.0f;
    GetVirtualCursorClientF(cx, cy);
    int col = 0, row = 0;
    gamepad::PixelToGridCell(srcGrid.base.rect, srcGrid.gridCols, srcGrid.gridRows,
                             (int)std::lround(cx), (int)std::lround(cy), col, row);

    bool occ = false;
    int itemId = -1;
    if (!ReadGridCell(srcGrid.win, srcGrid.gridRows, srcGrid.gridCols, row, col, occ, itemId) || !occ || itemId < 0) {
        return false;
    }

    int minC = col, minR = row, maxC = col, maxR = row;
    ComputeGridItemBounds(srcGrid.win, srcGrid.gridRows, srcGrid.gridCols, itemId, minC, minR, maxC, maxR);
    int itemW = (maxC - minC) + 1;
    int itemH = (maxR - minR) + 1;

    // Find another open UIGridbox (e.g. Inventory <-> Shared Stash / Backpack / Store / Trade)
    const CollectedWidget* dstGrid = nullptr;
    for (const auto& w : widgets) {
        if (w.base.isGridbox && w.win != srcGrid.win && w.gridCols > 0 && w.gridRows > 0) {
            dstGrid = &w;
            break;
        }
    }
    if (!dstGrid) return false;

    int dstC = 0, dstR = 0;
    bool isDstStore = (_stricmp(dstGrid->ifaceName.c_str(), "store") == 0);
    if (isDstStore) {
        // Selling to a store accepts drop anywhere inside the store gridbox
        dstC = dstGrid->gridCols / 2;
        dstR = dstGrid->gridRows / 2;
    } else {
        if (!FindFreeRectInGrid(dstGrid->win, dstGrid->gridRows, dstGrid->gridCols, itemW, itemH, dstC, dstR)) {
            return false; // Destination grid full
        }
    }

    // Compute exact destination drop pixel (center of the item's target footprint so multi-cell placement aligns!)
    auto tl = gamepad::ComputeGridCellRect(dstGrid->base.rect, dstGrid->gridCols, dstGrid->gridRows, dstC, dstR);
    auto br = gamepad::ComputeGridCellRect(dstGrid->base.rect, dstGrid->gridCols, dstGrid->gridRows,
                                           isDstStore ? dstC : (dstC + itemW - 1),
                                           isDstStore ? dstR : (dstR + itemH - 1));
    float destX = (float)(tl.left + br.right) * 0.5f;
    float destY = (float)(tl.top  + br.bottom) * 0.5f;

    // Stage 1: native Left-Click to pick up the item from source grid; next frame drops it at dest!
    InjectMouseTap(false, false);
    g_transfer.active = true;
    g_transfer.destX = destX;
    g_transfer.destY = destY;
    g_transfer.restoreX = cx;
    g_transfer.restoreY = cy;
    g_transfer.stage = 1;
    return true;
}

} // namespace

// ----------------------------------------------------------------------------------------------
// gp_internal UI Interface
// ----------------------------------------------------------------------------------------------
namespace gp_internal {

bool GamepadUI_IsAnyBlockingPanelOpen() {
    for (const char* name : kBlockingGamePanels) {
        if (eng::InterfaceVisible(name)) return true;
    }
    return false;
}

bool GamepadUI_IsInventoryOpen() {
    return eng::InterfaceVisible("character");
}

void GamepadUI_OnExitUI() {
    g_uiEntered = false;
    g_usingFocusSnap = false;
    g_focusedWidgetId = 0;
    g_hasPickupOrigin = false;
    g_transfer.active = false;
    g_wheelAccum = 0.0f;
}

void GamepadUI_Tick(const PadFrame& pad) {
    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);

    // Handle MegaMap (ws_mega_map): B / D-Pad Up / Start closes cleanly; RS-Y zooms.
    if (IsWorldStateMegaMap()) {
        if (pad.Pressed(xinput_abi::kB) || pad.Pressed(xinput_abi::kDpadUp) || pad.Pressed(xinput_abi::kStart)) {
            GamepadWorld_ToggleMegaMap();
            return;
        }
        float cx = 0.0f, cy = 0.0f;
        GetVirtualCursorClientF(cx, cy);
        if (pad.leftStick.magnitude > 0.0f) {
            gamepad::IntegrateVirtualCursor(cx, cy, pad.leftStick, pad.dtSec, sw, sh,
                                            Cfg().gamepadCursorSpeed, pad.rtDown, false);
            SetVirtualCursorClient(cx, cy, true, true);
        }
        if (std::fabs(pad.rightStick.y) > 0.15f) {
            g_wheelAccum += pad.rightStick.y * pad.dtSec * 14.0f;
            int notches = (int)g_wheelAccum;
            if (notches != 0) {
                g_wheelAccum -= (float)notches;
                InjectMouseWheel(notches);
            }
        } else {
            g_wheelAccum = 0.0f;
        }
        g_overlay.showHint = true;
        g_overlay.hintText = "[MegaMap]  [LS] Cursor  [RS-Y] Zoom  [B / D-Up] Close";
        return;
    }

    std::vector<CollectedWidget> widgets;
    CollectVisibleWidgets(widgets);

    // Complete 2-frame native Quick-Transfer if in flight
    if (g_transfer.active) {
        if (g_transfer.stage == 1) {
            if (GetNumActiveCursorItems() > 0) {
                SetVirtualCursorClient(g_transfer.destX, g_transfer.destY, true, true);
                InjectMouseTap(false, false);
            }
            g_transfer.stage = 2;
            return;
        } else {
            // Return cursor to source grid so player can keep rapid-transferring items!
            SetVirtualCursorClient(g_transfer.restoreX, g_transfer.restoreY, true, true);
            g_transfer.active = false;
            g_transfer.stage = 0;
        }
    }

    float cx = 0.0f, cy = 0.0f;
    GetVirtualCursorClientF(cx, cy);

    if (!g_uiEntered) {
        g_uiEntered = true;
        // On entering a menu/panel, snap cursor cleanly onto:
        //   1) First UIGridbox (Inventory / Stash / Store), else
        //   2) Top-most clickable UIButton (e.g. "Resume Game" on in_game_menu instead of read-only info listbox), else
        //   3) First interactive widget.
        if (!widgets.empty()) {
            const CollectedWidget* initial = nullptr;
            for (const auto& w : widgets) {
                if (w.base.isGridbox) { initial = &w; break; }
            }
            if (!initial) {
                // Prefer real dialog/menu buttons (e.g. "RESUME GAME" 140x32) over tiny 16x16 scrollbar arrows
                int bestTop = 999999;
                for (const auto& w : widgets) {
                    if (w.base.uiType == kUiButton && w.base.rect.Width() >= 48 && w.base.rect.Height() >= 20 &&
                        w.base.rect.top < bestTop) {
                        bestTop = w.base.rect.top;
                        initial = &w;
                    }
                }
            }
            if (!initial) {
                int bestTop = 999999;
                for (const auto& w : widgets) {
                    if (w.base.uiType == kUiButton && w.base.rect.top < bestTop) {
                        bestTop = w.base.rect.top;
                        initial = &w;
                    }
                }
            }
            if (!initial) initial = &widgets[0];
            SnapCursorToWidget(*initial, true);
            GetVirtualCursorClientF(cx, cy);
        } else {
            SetVirtualCursorClient(cx, cy, true, true);
        }
    }

    // Locate which widget currently contains the cursor
    int hoveredIdx = -1;
    for (size_t i = 0; i < widgets.size(); ++i) {
        if (widgets[i].base.rect.Contains((int)std::lround(cx), (int)std::lround(cy))) {
            hoveredIdx = (int)i;
            break;
        }
    }

    // 1. Left Stick — Smooth Free Virtual Cursor (100% universal fallback across every screen & mod)
    if (pad.leftStick.magnitude > 0.0f) {
        g_usingFocusSnap = false;
        gamepad::IntegrateVirtualCursor(cx, cy, pad.leftStick, pad.dtSec, sw, sh,
                                        Cfg().gamepadCursorSpeed, pad.rtDown, hoveredIdx >= 0);
        SetVirtualCursorClient(cx, cy, true, true);

        // Refresh hovered widget after stick movement
        hoveredIdx = -1;
        for (size_t i = 0; i < widgets.size(); ++i) {
            if (widgets[i].base.rect.Contains((int)std::lround(cx), (int)std::lround(cy))) {
                hoveredIdx = (int)i;
                break;
            }
        }
        if (hoveredIdx >= 0) {
            g_focusedWidgetId = widgets[hoveredIdx].base.id;
            g_focusedPanelId  = widgets[hoveredIdx].base.panelId;
            if (widgets[hoveredIdx].base.isGridbox) {
                gamepad::PixelToGridCell(widgets[hoveredIdx].base.rect,
                                         widgets[hoveredIdx].gridCols, widgets[hoveredIdx].gridRows,
                                         (int)std::lround(cx), (int)std::lround(cy),
                                         g_gridCol, g_gridRow);
            }
        }
    } else {
        // Keep virtual cursor active while in UI mode
        SetVirtualCursorClient(cx, cy, true, false);
    }

    // 2. Right Stick Y — Smooth scrolling via WM_MOUSEWHEEL (dialogue, lore, journal, listboxes)
    if (std::fabs(pad.rightStick.y) > 0.15f) {
        g_wheelAccum += pad.rightStick.y * pad.dtSec * 14.0f;
        int notches = (int)g_wheelAccum;
        if (notches != 0) {
            g_wheelAccum -= (float)notches;
            InjectMouseWheel(notches);
        }
    } else {
        g_wheelAccum = 0.0f;
    }

    // Build flat vector of UiFocusWidget for pure spatial navigation helpers
    std::vector<gamepad::UiFocusWidget> flatWidgets;
    flatWidgets.reserve(widgets.size());
    for (const auto& w : widgets) flatWidgets.push_back(w.base);

    // 3. D-Pad Smart Navigation (cell-by-cell in UIGridbox, row-by-row in listboxes, sliders, directional snap)
    int dpadX = 0, dpadY = 0;
    if (g_repLeft.Update(pad.Down(xinput_abi::kDpadLeft),   pad.nowMs)) dpadX = -1;
    if (g_repRight.Update(pad.Down(xinput_abi::kDpadRight), pad.nowMs)) dpadX = +1;
    if (g_repUp.Update(pad.Down(xinput_abi::kDpadUp),       pad.nowMs)) dpadY = -1;
    if (g_repDown.Update(pad.Down(xinput_abi::kDpadDown),   pad.nowMs)) dpadY = +1;

    if ((dpadX != 0 || dpadY != 0) && !widgets.empty()) {
        bool handled = false;

        // 3a. Slider left/right adjustment
        if (hoveredIdx >= 0 && widgets[hoveredIdx].base.isSlider && dpadX != 0) {
            const auto& r = widgets[hoveredIdx].base.rect;
            int step = (std::max)(8, r.Width() / 12);
            float nx = cx + (float)(dpadX * step);
            if (nx < (float)(r.left + 3)) nx = (float)(r.left + 3);
            if (nx > (float)(r.right - 3)) nx = (float)(r.right - 3);
            SetVirtualCursorClient(nx, (float)r.CenterY(), true, true);
            InjectMouseTap(false, false);
            g_usingFocusSnap = true;
            handled = true;
        }
        // 3b. Listbox up/down row stepping
        else if (hoveredIdx >= 0 && widgets[hoveredIdx].base.isListbox && dpadY != 0) {
            const auto& r = widgets[hoveredIdx].base.rect;
            float ny = cy + (float)(dpadY * 18);
            if (ny >= (float)(r.top + 6) && ny <= (float)(r.bottom - 6)) {
                SetVirtualCursorClient((float)r.CenterX(), ny, true, true);
                InjectMouseTap(false, false);
                g_usingFocusSnap = true;
                handled = true;
            } else {
                InjectMouseWheel(-dpadY);
                handled = true;
            }
        }
        // 3c. UIGridbox smart cell / multi-cell-item stepping
        else if (hoveredIdx >= 0 && widgets[hoveredIdx].base.isGridbox &&
                 widgets[hoveredIdx].gridCols > 0 && widgets[hoveredIdx].gridRows > 0)
        {
            const auto& gw = widgets[hoveredIdx];
            gamepad::PixelToGridCell(gw.base.rect, gw.gridCols, gw.gridRows,
                                     (int)std::lround(cx), (int)std::lround(cy),
                                     g_gridCol, g_gridRow);

            int curCol = g_gridCol;
            int curRow = g_gridRow;

            // When browsing empty-handed, skip past the remaining cells of the same multi-cell item!
            if (GetNumActiveCursorItems() == 0) {
                bool curOcc = false;
                int curId = -1;
                if (ReadGridCell(gw.win, gw.gridRows, gw.gridCols, curRow, curCol, curOcc, curId) && curOcc && curId >= 0) {
                    int minC, minR, maxC, maxR;
                    if (ComputeGridItemBounds(gw.win, gw.gridRows, gw.gridCols, curId, minC, minR, maxC, maxR)) {
                        if (dpadX > 0) curCol = maxC;
                        if (dpadX < 0) curCol = minC;
                        if (dpadY > 0) curRow = maxR;
                        if (dpadY < 0) curRow = minR;
                    }
                }
            }

            auto step = gamepad::StepGridCell(gw.base.rect, gw.gridCols, gw.gridRows, curCol, curRow, dpadX, dpadY);
            if (step.stayedInGrid) {
                g_gridCol = step.col;
                g_gridRow = step.row;
                g_focusedWidgetId = gw.base.id;
                g_focusedPanelId  = gw.base.panelId;
                g_usingFocusSnap  = true;
                SetVirtualCursorClient((float)step.cellRect.CenterX(), (float)step.cellRect.CenterY(), true, true);
                handled = true;
            }
            // If step left the grid boundary, fall through to directional snap to jump cleanly to Paperdoll / Close / Stash!
        }

        // 3d. Universal directional focus snapping across all visible widgets
        if (!handled) {
            int nextIdx = gamepad::FindDirectionalNeighbor(
                flatWidgets,
                (int)std::lround(cx), (int)std::lround(cy),
                (hoveredIdx >= 0) ? widgets[hoveredIdx].base.id : 0,
                (hoveredIdx >= 0) ? widgets[hoveredIdx].base.panelId : g_focusedPanelId,
                dpadX, dpadY);
            if (nextIdx >= 0) {
                SnapCursorToWidget(widgets[nextIdx], true);
                hoveredIdx = nextIdx;
                GetVirtualCursorClientF(cx, cy);
            }
        }
    }

    // 4. LB / RB — Instant panel switching (Paperdoll <-> Inventory <-> Shared Stash / Backpack / Store / Trade / Spellbook)
    if (pad.Pressed(xinput_abi::kRightShoulder) || pad.Pressed(xinput_abi::kLeftShoulder)) {
        int dir = pad.Pressed(xinput_abi::kRightShoulder) ? +1 : -1;
        int targetIdx = gamepad::CycleUiPanel(flatWidgets, g_focusedPanelId, dir);
        if (targetIdx >= 0) {
            SnapCursorToWidget(widgets[targetIdx], true);
            hoveredIdx = targetIdx;
            GetVirtualCursorClientF(cx, cy);
        }
    }

    // 5. Action Buttons
    int activeItems = GetNumActiveCursorItems();
    if (activeItems == 0) {
        g_hasPickupOrigin = false;
    }

    // A = Left Click (Pick up / Place / Click button / Focus editbox)
    if (pad.Pressed(xinput_abi::kA)) {
        if (activeItems == 0) {
            g_pickupOriginX = cx;
            g_pickupOriginY = cy;
            g_hasPickupOrigin = true;
        }
        InjectMouseTap(false, pad.rtDown);
    }

    // X = 1-tap Quick-Transfer when 2 grids open (unless LT held), else Right-Click (Drink / Equip / Use / Inspect)
    if (pad.Pressed(xinput_abi::kX)) {
        bool didQuickTransfer = false;
        if (!pad.ltDown && hoveredIdx >= 0 && widgets[hoveredIdx].base.isGridbox) {
            didQuickTransfer = TryQuickTransfer(widgets, widgets[hoveredIdx]);
        }
        if (!didQuickTransfer) {
            InjectMouseTap(true, false);
        }
    }

    // Y = Auto-Sort Inventory (K) when Inventory is open, otherwise Double-Click (quick join / select char)
    if (pad.Pressed(xinput_abi::kY)) {
        if (GamepadUI_IsInventoryOpen()) {
            void* partyMgr = *(void**)kAddrUIPartyMgr;
            if (partyMgr && Readable(partyMgr, 0x40)) {
                s_sortInventory(partyMgr);
            }
        } else {
            InjectMouseDoubleClick();
        }
    }

    // B = Return held item safely to pickup slot, or close current panel / Esc
    if (pad.Pressed(xinput_abi::kB)) {
        if (activeItems > 0 && g_hasPickupOrigin) {
            SetVirtualCursorClient(g_pickupOriginX, g_pickupOriginY, true, true);
            InjectMouseTap(false, false);
            g_hasPickupOrigin = false;
        } else if (g_hudCursorToggle && IsWorldStateInGame() && !GamepadUI_IsAnyBlockingPanelOpen()) {
            g_hudCursorToggle = false;
            DisableVirtualCursor();
        } else if (IsWorldStateInGame()) {
            GamepadWorld_TriggerEscape();
        } else {
            InjectKeyTap(VK_ESCAPE);
        }
    }

    // Start = Esc (close panels / pause menu)
    if (pad.Pressed(xinput_abi::kStart)) {
        if (IsWorldStateInGame()) {
            GamepadWorld_TriggerEscape();
        } else {
            InjectKeyTap(VK_ESCAPE);
        }
    }

    // 6. Render native gold focus highlight around active widget or UIGridbox cell/item
    if (hoveredIdx >= 0) {
        const auto& hw = widgets[hoveredIdx];
        if (hw.base.isGridbox && hw.gridCols > 0 && hw.gridRows > 0) {
            int c = 0, r = 0;
            gamepad::PixelToGridCell(hw.base.rect, hw.gridCols, hw.gridRows,
                                     (int)std::lround(cx), (int)std::lround(cy), c, r);
            gamepad::UiRect box = gamepad::ComputeGridCellRect(hw.base.rect, hw.gridCols, hw.gridRows, c, r);
            // Frame entire multi-cell item cleanly when hovering an occupied item empty-handed
            if (GetNumActiveCursorItems() == 0) {
                bool occ = false;
                int id = -1;
                if (ReadGridCell(hw.win, hw.gridRows, hw.gridCols, r, c, occ, id) && occ && id >= 0) {
                    int minC, minR, maxC, maxR;
                    if (ComputeGridItemBounds(hw.win, hw.gridRows, hw.gridCols, id, minC, minR, maxC, maxR)) {
                        auto tl = gamepad::ComputeGridCellRect(hw.base.rect, hw.gridCols, hw.gridRows, minC, minR);
                        auto br = gamepad::ComputeGridCellRect(hw.base.rect, hw.gridCols, hw.gridRows, maxC, maxR);
                        box = {tl.left, tl.top, br.right, br.bottom};
                    }
                }
            }
            g_overlay.showFocusBox = true;
            g_overlay.focusRect = box;
            g_overlay.focusColor = (GetNumActiveCursorItems() > 0) ? 0xFF44FF88 : 0xFFFFD700;
        } else {
            g_overlay.showFocusBox = true;
            g_overlay.focusRect = hw.base.rect;
            g_overlay.focusColor = 0xFFFFD700;
        }
    }

    // 7. Context-aware UI hint strip
    g_overlay.showHint = true;
    int gridCount = 0;
    for (const auto& w : widgets) {
        if (w.base.isGridbox) ++gridCount;
    }
    if (GetNumActiveCursorItems() > 0) {
        g_overlay.hintText = "[D-Pad/LS] Move Item   [A] Place   [LB/RB] Switch Panel   [B] Put Back";
    } else if (gridCount >= 2) {
        g_overlay.hintText = "[A] Pick Up   [X] Quick Move   [LB/RB] Switch Panel   [Y] Sort   [B] Close";
    } else if (GamepadUI_IsInventoryOpen()) {
        g_overlay.hintText = "[A] Pick Up   [X] Use/Equip   [LB/RB] Paperdoll/Inv   [Y] Sort   [B] Close";
    } else if (g_hudCursorToggle && IsWorldStateInGame() && !GamepadUI_IsAnyBlockingPanelOpen()) {
        g_overlay.hintText = "[HUD Cursor]  [LS/D-Pad] Aim  [A] Click  [X] Right-Click  [Back/B] Gameplay";
    } else {
        g_overlay.hintText = "[D-Pad/LS] Navigate   [A] Select   [RS] Scroll   [LB/RB] Panel   [B] Back";
    }
}

} // namespace gp_internal
