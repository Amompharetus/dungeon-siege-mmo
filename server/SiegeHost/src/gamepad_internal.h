#pragma once
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include "gamepad_math.h"

// Minimal XInput ABI definitions (matches xinput1_4.dll / xinput1_3.dll / xinput9_1_0.dll)
namespace xinput_abi {

struct XINPUT_GAMEPAD {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE {
    DWORD          dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};

constexpr WORD kDpadUp        = 0x0001;
constexpr WORD kDpadDown      = 0x0002;
constexpr WORD kDpadLeft      = 0x0004;
constexpr WORD kDpadRight     = 0x0008;
constexpr WORD kStart         = 0x0010;
constexpr WORD kBack          = 0x0020;
constexpr WORD kLeftThumb     = 0x0040;
constexpr WORD kRightThumb    = 0x0080;
constexpr WORD kLeftShoulder  = 0x0100;
constexpr WORD kRightShoulder = 0x0200;
constexpr WORD kA             = 0x1000;
constexpr WORD kB             = 0x2000;
constexpr WORD kX             = 0x4000;
constexpr WORD kY             = 0x8000;

} // namespace xinput_abi

namespace gp_internal {

struct Vec3 {
    float x, y, z;
};

struct SiegePos {
    float x, y, z;
    uint32_t node;
};

enum class ControlMode : uint8_t {
    Inactive = 0, // No controller connected or mouse/KB currently owns focus in menus
    Gameplay = 1, // In-game 3D world control
    FreeAim  = 2, // In-game RT-held ground/spell reticle aiming
    MenuUI   = 3, // Menus, inventory, shop, stash, spellbook, dialogue, journal, HUD cursor
    Chat     = 4, // Active text editbox (chat / character name / console)
};

struct PadFrame {
    bool connected = false;
    int port = -1;
    uint32_t nowMs = 0;
    float dtSec = 0.016f;

    WORD buttons = 0;
    WORD prevButtons = 0;

    gamepad::Stick2D leftStick{};
    gamepad::Stick2D rightStick{};

    bool ltDown = false;
    bool prevLtDown = false;
    bool rtDown = false;
    bool prevRtDown = false;
    float ltValue = 0.0f;
    float rtValue = 0.0f;

    bool Pressed(WORD mask) const { return (buttons & mask) != 0 && (prevButtons & mask) == 0; }
    bool Released(WORD mask) const { return (buttons & mask) == 0 && (prevButtons & mask) != 0; }
    bool Down(WORD mask) const { return (buttons & mask) != 0; }
    bool LtPressed() const { return ltDown && !prevLtDown; }
    bool RtPressed() const { return rtDown && !prevRtDown; }
    bool RtReleased() const { return !rtDown && prevRtDown; }
};

struct OverlayState {
    bool showFocusBox = false;
    gamepad::UiRect focusRect{};
    DWORD focusColor = 0xFFFFD700; // Gold

    bool showTargetBox = false;
    gamepad::UiRect targetRect{};
    DWORD targetColor = 0xFFFF4444;

    bool showHint = false;
    std::string hintText;
};

// Safe memory readability guard
inline bool Readable(const void* p, size_t n = 4) {
    return p && (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, n);
}

// Engine globals (DSLOA.exe v1.50 verified)
constexpr uintptr_t kAddrAppModule     = 0x007A99A0; // AppModule (cursor, fullscreen, flags, mouse history)
constexpr uintptr_t kAddrRapiOwner     = 0x007ACFC8; // RapiOwner (+0x1E0 = Rapi*)
constexpr uintptr_t kAddrUIShell       = 0x007ACFE4;
constexpr uintptr_t kAddrUICamera      = 0x007ACF84;
constexpr uintptr_t kAddrUIGame        = 0x007ACF8C;
constexpr uintptr_t kAddrUIPartyMgr    = 0x007ACF90;
constexpr uintptr_t kAddrSiegeEngine   = 0x007ACF80;
constexpr uintptr_t kAddrServer        = 0x007ACF70;
constexpr uintptr_t kAddrGoDb          = 0x007ACF4C;
constexpr uintptr_t kAddrWorldState    = 0x007ACF44;
constexpr uintptr_t kAddrNullGoid      = 0x007ADC88;
constexpr uintptr_t kAddrUiTreeNil     = 0x007B3B10;

// Shared state and utilities implemented in gamepad.cpp
extern ControlMode g_mode;
extern bool g_gamepadActiveSession; // true after gamepad input; yields to mouse/KB on physical mouse move
extern bool g_hudCursorToggle;      // Back button toggles HUD cursor mode during gameplay
extern OverlayState g_overlay;

void GetScreenDimensions(int& outW, int& outH);
bool IsWorldStateInGame();
bool IsWorldStateMegaMap();
bool IsInMenuOrPanelOpen();
void SetVirtualCursorClient(float x, float y, bool activate, bool dispatchMove = true);
void GetVirtualCursorClientF(float& outX, float& outY);
void DisableVirtualCursor();
void InjectMouseClick(bool rightButton, bool down);
void InjectMouseTap(bool rightButton, bool withShift = false);
void InjectMouseDoubleClick();
void InjectMouseWheel(int wheelNotches);
void InjectKeyTap(UINT vk, bool withShift = false);
void InjectKeyEvent(UINT vk, bool down, bool withShift = false);

// World / gameplay subsystem (gamepad_world.cpp)
void GamepadWorld_InstallHooks();
void GamepadWorld_TickGameplay(const PadFrame& pad);
void GamepadWorld_TickFreeAim(const PadFrame& pad);
void GamepadWorld_OnLeaveGameplay();
bool GamepadWorld_GetCameraAnalog(float& outYaw, float& outPitch, float& outZoom);
void* GamepadWorld_GetLocalHero();
bool GamepadWorld_IsHeroInTown(void* hero);
void GamepadWorld_TriggerEscape();
void GamepadWorld_ToggleMegaMap();
int  GamepadWorld_GetActiveWeaponSlot();
void GamepadWorld_GetLabStats(int& outMoveOrders, int& outMoveFails, int& outWalkFlags,
                              int& outTgtScanned, int& outTgtCand, int& outTgtOnScr);

// UI subsystem (gamepad_ui.cpp)
void GamepadUI_Tick(const PadFrame& pad);
void GamepadUI_OnExitUI();
bool GamepadUI_IsAnyBlockingPanelOpen();
bool GamepadUI_IsInventoryOpen();

} // namespace gp_internal
