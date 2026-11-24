#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "siegehost.h"
#include "engine.h"
#include "characterabilities.h"
#include "gamepad.h"
#include "gamepad_internal.h"

namespace gp_internal {

ControlMode g_mode = ControlMode::Inactive;
bool g_gamepadActiveSession = false;
bool g_hudCursorToggle = false;
OverlayState g_overlay{};

} // namespace gp_internal

using namespace gp_internal;

namespace {

// ----------------------------------------------------------------------------------------------
// Dynamic XInput loader (works on Windows 10/11, DS4Windows, Steam Input, Wine/Proton/Steam Deck)
// ----------------------------------------------------------------------------------------------
typedef DWORD (WINAPI* PFN_XInputGetState)(DWORD dwUserIndex, xinput_abi::XINPUT_STATE* pState);

PFN_XInputGetState g_pfnXInputGetState = nullptr;
bool g_xinputLoadAttempted = false;
bool g_initialized = false;

int g_activePort = -1;
DWORD g_lastScanMs = 0;
DWORD g_lastTickMs = 0;
bool g_loggedConnection = false;

WORD g_prevButtons = 0;
gamepad::TriggerHysteresis g_ltHyst{};
gamepad::TriggerHysteresis g_rtHyst{};

// Virtual cursor state (in game client-area pixels)
volatile LONG g_virtualCursorActive = 0;
float g_virtX = 512.0f;
float g_virtY = 384.0f;
bool g_virtCoordsInitialized = false;
POINT g_lastRealScreenPt = {-99999, -99999};
bool g_injectingSyntheticMsg = false;
void* g_hiddenCursorWin = nullptr;

// ----------------------------------------------------------------------------------------------
// user32!GetCursorPos inline hook (same proven trampoline pattern as InstallGetKeyboardLayoutHook)
// ----------------------------------------------------------------------------------------------
typedef BOOL (WINAPI* GetCursorPosFn)(LPPOINT);
GetCursorPosFn g_origGetCursorPos = nullptr;
bool g_getCursorPosHooked = false;

static bool IsAppModuleInRelativeMouseMode(uintptr_t appMod) {
    if (!appMod || !Readable((const void*)appMod, 0x160)) return false;
    int activeCnt = *(const int*)(appMod + 0x78);
    if (activeCnt <= 0) return false;
    bool fullscreen = (*(const char*)(appMod + 0x74) != 0);
    bool flagNoCapture = ((*(const uint8_t*)(appMod + 0x4C) & 1) != 0);
    uint16_t btnMask = (*(const uint16_t*)(appMod + 0x10C) & 0x0F80);
    bool forceRelative = (((*(const uint32_t*)(appMod + 0x48) >> 0x1C) & 1) != 0);
    return (fullscreen && !flagNoCapture) || (btnMask != 0) || forceRelative;
}

static BOOL WINAPI HookGetCursorPos(LPPOINT lpPoint) {
    if (!lpPoint) return g_origGetCursorPos ? g_origGetCursorPos(lpPoint) : FALSE;

    if (InterlockedCompareExchange(&g_virtualCursorActive, 0, 0) != 0 &&
        g_gameHwnd && IsWindow(g_gameHwnd) && GetForegroundWindow() == g_gameHwnd)
    {
        uintptr_t retAddr = (uintptr_t)__builtin_return_address(0);
        int vx = (int)std::lround(g_virtX);
        int vy = (int)std::lround(g_virtY);
        POINT clientPt = {vx, vy};

        // AppModule::PollMouse (0x00414B2A..0x00414EEA) is DSLOA.exe's sole GetCursorPos caller.
        // In fullscreen/relative-mouse mode (or while a mouse button is held), PollMouse computes
        // delta = (cursorClient - [appMod+0x134/0x138]) and adds it to [appMod+0x12C/0x130].
        // Returning (lastRef + desiredDelta) lands AppModule's internal cursor on exact (vx, vy)
        // in BOTH windowed and fullscreen modes without ever warping the OS mouse cursor!
        if (retAddr >= 0x00414B2Au && retAddr < 0x00414EEAu) {
            uintptr_t appMod = *(uintptr_t*)kAddrAppModule;
            if (IsAppModuleInRelativeMouseMode(appMod)) {
                int curX  = *(const int*)(appMod + 0x12C);
                int curY  = *(const int*)(appMod + 0x130);
                int lastX = *(const int*)(appMod + 0x134);
                int lastY = *(const int*)(appMod + 0x138);
                clientPt.x = lastX + (vx - curX);
                clientPt.y = lastY + (vy - curY);
            }
        }

        if (ClientToScreen(g_gameHwnd, &clientPt)) {
            *lpPoint = clientPt;
            return TRUE;
        }
    }

    return g_origGetCursorPos ? g_origGetCursorPos(lpPoint) : FALSE;
}

static void InstallGetCursorPosHook() {
    if (g_getCursorPosHooked) return;
    HMODULE u32 = GetModuleHandleA("user32.dll");
    BYTE* fn = u32 ? (BYTE*)GetProcAddress(u32, "GetCursorPos") : nullptr;
    static const BYTE prologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!fn || memcmp(fn, prologue, 5) != 0) {
        Log("Gamepad: user32!GetCursorPos prologue mismatch; will use SetCursorPos fallback.");
        return;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("Gamepad: VirtualAlloc failed for GetCursorPos hook.");
        return;
    }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD*)(tramp + 6) = (DWORD)((fn + 5) - (tramp + 10));
    g_origGetCursorPos = (GetCursorPosFn)tramp;

    DWORD old = 0;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
        Log("Gamepad: VirtualProtect failed for GetCursorPos hook.");
        g_origGetCursorPos = nullptr;
        return;
    }
    fn[0] = 0xE9;
    *(DWORD*)(fn + 1) = (DWORD)((BYTE*)HookGetCursorPos - (fn + 5));
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    g_getCursorPosHooked = true;
    Log("Gamepad: hooked user32!GetCursorPos (virtual cursor isolated to DSLOA + mods).");
}

static void LoadXInputOnce() {
    if (g_xinputLoadAttempted) return;
    g_xinputLoadAttempted = true;
    static const char* const kDlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* dll : kDlls) {
        HMODULE h = LoadLibraryA(dll);
        if (h) {
            auto fn = (PFN_XInputGetState)GetProcAddress(h, "XInputGetState");
            if (fn) {
                g_pfnXInputGetState = fn;
                Log("Gamepad: loaded %s (XInput ready — Xbox / DS4Windows / Steam Input compatible).", dll);
                return;
            }
        }
    }
    Log("Gamepad: no XInput DLL available; gamepad support inactive.");
}

// ----------------------------------------------------------------------------------------------
// Native UIShell Overlay (siegehost_gamepad_hud): focus box, target reticle, context hint strip
// ----------------------------------------------------------------------------------------------
static const char* kHudIface     = "siegehost_gamepad_hud";
static const char* kFocusWinName = "gp_focus_box";
static const char* kTgtWinName   = "gp_target_box";
static const char* kHintBgName   = "gp_hint_bg";
static const char* kHintTxtName  = "gp_hint_text";

static bool g_hudCreateFailed = false;
static std::string g_lastHintText;
static bool g_lastFocusVis = false;
static bool g_lastTargetVis = false;
static bool g_lastHintVis = false;

static void SetWinVisible(void* win, bool vis) {
    if (!win || !Readable(win, 0x110)) return;
    typedef void (__thiscall* SetVisFn)(void*, bool);
    ((SetVisFn)0x0047B06E)(win, vis);
}

static void* GetActiveGameCursor() {
    void* uiShell = eng::UIShell_Singleton();
    if (!uiShell || !Readable(uiShell, 0x108)) return nullptr;
    typedef void* (__thiscall* GetCurFn)(void*);
    void* cur = ((GetCurFn)0x0047B106)(uiShell);
    return (cur && Readable(cur, 0x110)) ? cur : nullptr;
}

static void SyncGameplayCursorHidden(bool hide) {
    void* activeCur = GetActiveGameCursor();
    if (hide) {
        if (g_hiddenCursorWin && g_hiddenCursorWin != activeCur) {
            SetWinVisible(g_hiddenCursorWin, true);
        }
        if (activeCur) {
            SetWinVisible(activeCur, false);
            g_hiddenCursorWin = activeCur;
        }
    } else {
        if (g_hiddenCursorWin) {
            SetWinVisible(g_hiddenCursorWin, true);
            g_hiddenCursorWin = nullptr;
        }
        if (activeCur) {
            SetWinVisible(activeCur, true);
        }
    }
}

static void SetWinRect(void* win, int left, int top, int right, int bottom) {
    if (!win || !Readable(win, 0x80)) return;
    // Verified in DSLOA.named.c (0x0047AFAB & member_labels 0x004A50F6):
    // UIWindow::SetRect(int left, int right, int top, int bottom, bool bAnimation)
    typedef void (__thiscall* SetRectFn)(void*, int, int, int, int, bool);
    ((SetRectFn)0x0047AFAB)(win, left, right, top, bottom, false);
}

static void SetWinBorder(void* win, bool enabled, DWORD argbColor, int padding) {
    if (!win || !Readable(win, 0x150)) return;
    typedef void (__thiscall* SetBorderFn)(void*, bool);
    typedef void (__thiscall* SetColorFn)(void*, uint32_t);
    typedef void (__thiscall* SetPadFn)(void*, int);
    ((SetBorderFn)0x0047B081)(win, enabled);
    ((SetColorFn)0x0047B095)(win, argbColor);
    ((SetPadFn)0x0047B0CA)(win, padding);
}

static void SetWinBackground(void* win, bool fill, DWORD argbColor, float alpha) {
    if (!win || !Readable(win, 0x150)) return;
    typedef void (__thiscall* SetFillFn)(void*, bool);
    typedef void (__thiscall* SetBgColorFn)(void*, uint32_t);
    typedef void (__thiscall* SetAlphaFn)(void*, float);
    ((SetFillFn)0x0047B0BD)(win, fill);
    ((SetBgColorFn)0x0047B0A9)(win, argbColor);
    ((SetAlphaFn)0x0047B05A)(win, alpha);
}

static void SetWinPassThrough(void* win, bool pass) {
    if (!win || !Readable(win, 0x150)) return;
    typedef void (__thiscall* SetPtFn)(void*, bool);
    ((SetPtFn)0x0047B264)(win, pass);
}

static void* CreateHudWindow(void* uiShell, const char* typeStr, const char* winName) {
    typedef void* (__thiscall* CreateWinFn)(void*, const eng::gpstring*);
    typedef void  (__thiscall* SetNameFn)(void*, const eng::gpstring*);
    typedef void  (__thiscall* AddWinFn)(void*, void*, const eng::gpstring*, bool);

    eng::GpStr typeGp(typeStr);
    void* win = ((CreateWinFn)0x006E9F06)(uiShell, &typeGp.s);
    if (!win || !Readable(win, 0x160)) return nullptr;

    eng::GpStr nameGp(winName);
    ((SetNameFn)0x0047AF1F)(win, &nameGp.s);

    eng::GpStr ifaceGp(kHudIface);
    ((AddWinFn)0x006E9A2D)(uiShell, win, &ifaceGp.s, false);

    SetWinPassThrough(win, true);
    SetWinVisible(win, false);
    return win;
}

static bool HasUIShellInterface(void* uiShell, const char* name) {
    if (!uiShell || !name || !Readable(uiShell, 0x10)) return false;
    void** begin = *(void***)((const char*)uiShell + 0x04);
    void** end   = *(void***)((const char*)uiShell + 0x08);
    if (!begin || !end || end <= begin || (size_t)(end - begin) > 256) return false;
    if (!Readable(begin, (size_t)(end - begin) * sizeof(void*))) return false;
    for (void** it = begin; it != end; ++it) {
        void* iface = *it;
        if (!iface || !Readable(iface, 0x10)) continue;
        const char* nm = eng::GpC(iface, 4);
        if (nm && _stricmp(nm, name) == 0) return true;
    }
    return false;
}

static bool EnsureHudOverlay() {
    if (g_hudCreateFailed) return false;
    void* uiShell = eng::UIShell_Singleton();
    if (!uiShell || !Readable(uiShell, 0x120)) return false;

    void* existing = eng::FindWindow(kFocusWinName, kHudIface);
    if (existing) {
        if (!eng::InterfaceVisible(kHudIface)) {
            eng::ShowInterface(kHudIface);
        }
        return true;
    }

    // Create blank top-most interface lazily (survives Frontend <-> Staging <-> In-Game resets)
    typedef void (__thiscall* AddBlankFn)(void*, const eng::gpstring*);
    typedef void (__thiscall* PlaceTopFn)(void*, const char*);

    if (!HasUIShellInterface(uiShell, kHudIface)) {
        eng::GpStr ifaceGp(kHudIface);
        ((AddBlankFn)0x006ED1EB)(uiShell, &ifaceGp.s);
    }

    void* wFocus = CreateHudWindow(uiShell, "window", kFocusWinName);
    void* wTgt   = CreateHudWindow(uiShell, "window", kTgtWinName);
    void* wBg    = CreateHudWindow(uiShell, "window", kHintBgName);
    void* wTxt   = CreateHudWindow(uiShell, "text",   kHintTxtName);

    if (!wFocus || !wTgt) {
        g_hudCreateFailed = true;
        Log("Gamepad: failed to create native UIShell overlay windows; continuing without visual overlay.");
        return false;
    }

    SetWinBorder(wFocus, true, 0xFFFFD700, 1);
    SetWinBackground(wFocus, true, 0x22FFD700, 1.0f);

    SetWinBorder(wTgt, true, 0xFFFF4444, 1);
    SetWinBackground(wTgt, true, 0x18FF4444, 1.0f);

    if (wBg) {
        SetWinBorder(wBg, true, 0xAA8A7348, 0);
        SetWinBackground(wBg, true, 0xB80E0C0A, 1.0f);
    }
    if (wTxt) {
        typedef void (__thiscall* SetFontFn)(void*, const eng::gpstring*);
        typedef void (__thiscall* SetColorFn)(void*, uint32_t);
        eng::GpStr fontGp("b_gui_fnt_12p_copperplate-light");
        ((SetFontFn)0x006F19B6)(wTxt, &fontGp.s);
        ((SetColorFn)0x0047F709)(wTxt, 0xFFF5E6C4);
    }

    eng::ShowInterface(kHudIface);
    ((PlaceTopFn)0x006ED77E)(uiShell, kHudIface);
    g_lastHintText.clear();
    g_lastFocusVis = false;
    g_lastTargetVis = false;
    g_lastHintVis = false;
    return true;
}

static void RenderOverlay() {
    bool wantAny = g_gamepadActiveSession &&
                   (g_overlay.showFocusBox || g_overlay.showTargetBox || (Cfg().gamepadShowHints && g_overlay.showHint));

    if (!wantAny) {
        if (g_lastFocusVis || g_lastTargetVis || g_lastHintVis) {
            if (void* w = eng::FindWindow(kFocusWinName, kHudIface)) SetWinVisible(w, false);
            if (void* w = eng::FindWindow(kTgtWinName,   kHudIface)) SetWinVisible(w, false);
            if (void* w = eng::FindWindow(kHintBgName,   kHudIface)) SetWinVisible(w, false);
            if (void* w = eng::FindWindow(kHintTxtName,  kHudIface)) SetWinVisible(w, false);
            g_lastFocusVis = g_lastTargetVis = g_lastHintVis = false;
        }
        return;
    }

    if (!EnsureHudOverlay()) return;

    void* uiShell = eng::UIShell_Singleton();
    if (uiShell && Readable(uiShell, 0x10)) {
        typedef void (__thiscall* PlaceTopFn)(void*, const char*);
        ((PlaceTopFn)0x006ED77E)(uiShell, kHudIface);
    }

    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);

    if (void* wFocus = eng::FindWindow(kFocusWinName, kHudIface)) {
        if (g_overlay.showFocusBox && g_overlay.focusRect.Valid()) {
            int l = g_overlay.focusRect.left - 2;
            int t = g_overlay.focusRect.top - 2;
            int r = g_overlay.focusRect.right + 2;
            int b = g_overlay.focusRect.bottom + 2;
            if (l < 0) l = 0;
            if (t < 0) t = 0;
            if (sw > 0 && r > sw - 1) r = sw - 1;
            if (sh > 0 && b > sh - 1) b = sh - 1;
            SetWinRect(wFocus, l, t, r, b);
            SetWinBorder(wFocus, true, g_overlay.focusColor, 1);
            SetWinBackground(wFocus, true, (g_overlay.focusColor & 0x00FFFFFFu) | 0x20000000u, 1.0f);
            SetWinVisible(wFocus, true);
            g_lastFocusVis = true;
        } else {
            SetWinVisible(wFocus, false);
            g_lastFocusVis = false;
        }
    }

    if (void* wTgt = eng::FindWindow(kTgtWinName, kHudIface)) {
        if (g_overlay.showTargetBox && g_overlay.targetRect.Valid()) {
            SetWinRect(wTgt, g_overlay.targetRect.left, g_overlay.targetRect.top,
                       g_overlay.targetRect.right, g_overlay.targetRect.bottom);
            SetWinBorder(wTgt, true, g_overlay.targetColor, 1);
            SetWinBackground(wTgt, true, (g_overlay.targetColor & 0x00FFFFFFu) | 0x18000000u, 1.0f);
            SetWinVisible(wTgt, true);
            g_lastTargetVis = true;
        } else {
            SetWinVisible(wTgt, false);
            g_lastTargetVis = false;
        }
    }

    bool showHint = Cfg().gamepadShowHints && g_overlay.showHint && !g_overlay.hintText.empty() && sw > 300 && sh > 200;
    void* wBg  = eng::FindWindow(kHintBgName, kHudIface);
    void* wTxt = eng::FindWindow(kHintTxtName, kHudIface);
    if (wBg && wTxt) {
        if (showHint) {
            int barW = (int)g_overlay.hintText.size() * 7 + 28;
            if (barW > sw - 40) barW = sw - 40;
            if (barW < 200) barW = 200;
            int barH = 22;
            int l = (sw - barW) / 2;
            int t = sh - barH - 8;
            SetWinRect(wBg, l, t, l + barW, t + barH);
            SetWinRect(wTxt, l + 10, t + 4, l + barW - 10, t + barH - 2);
            if (g_overlay.hintText != g_lastHintText) {
                g_lastHintText = g_overlay.hintText;
                typedef void (__thiscall* UITextSetTextFn)(void*, const eng::gpstring*, bool);
                eng::GpStr gp(g_overlay.hintText.c_str());
                ((UITextSetTextFn)0x0047B4C4)(wTxt, &gp.s, false);
            }
            SetWinVisible(wBg, true);
            SetWinVisible(wTxt, true);
            g_lastHintVis = true;
        } else {
            SetWinVisible(wBg, false);
            SetWinVisible(wTxt, false);
            g_lastHintVis = false;
        }
    }
}

// ----------------------------------------------------------------------------------------------
// Physical mouse detection (yield virtual cursor cleanly whenever user grabs physical mouse)
// ----------------------------------------------------------------------------------------------
static void PollPhysicalMouseTakeover() {
    if (!g_origGetCursorPos || !g_gameHwnd || GetForegroundWindow() != g_gameHwnd) return;
    POINT realPt{};
    if (!g_origGetCursorPos(&realPt)) return;

    if (g_lastRealScreenPt.x == -99999) {
        g_lastRealScreenPt = realPt;
        return;
    }

    int dx = realPt.x - g_lastRealScreenPt.x;
    int dy = realPt.y - g_lastRealScreenPt.y;
    g_lastRealScreenPt = realPt;

    // In fullscreen mode, AppModule resets the OS cursor to window center each frame;
    // ignore OS cursor jumps straight onto the window center.
    RECT rc{};
    if (GetClientRect(g_gameHwnd, &rc)) {
        POINT center = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
        ClientToScreen(g_gameHwnd, &center);
        if (std::abs(realPt.x - center.x) <= 2 && std::abs(realPt.y - center.y) <= 2) {
            return;
        }
    }

    if (dx * dx + dy * dy >= 36) { // >= 6px real physical mouse movement
        if (InterlockedCompareExchange(&g_virtualCursorActive, 0, 0) != 0) {
            DisableVirtualCursor();
        }
        g_gamepadActiveSession = false;
        g_hudCursorToggle = false;
        SyncGameplayCursorHidden(false);
    }
}

} // namespace

// ----------------------------------------------------------------------------------------------
// Shared Helpers (gp_internal)
// ----------------------------------------------------------------------------------------------
namespace gp_internal {

void GetScreenDimensions(int& outW, int& outH) {
    outW = 0;
    outH = 0;
    void* uiShell = eng::UIShell_Singleton();
    if (uiShell && Readable(uiShell, 0x70)) {
        int w = *(const int*)((const char*)uiShell + 0x60);
        int h = *(const int*)((const char*)uiShell + 0x64);
        if (w > 100 && h > 100 && w <= 8192 && h <= 8192) {
            outW = w;
            outH = h;
            return;
        }
    }
    if (g_gameHwnd && IsWindow(g_gameHwnd)) {
        RECT rc{};
        if (GetClientRect(g_gameHwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            outW = rc.right - rc.left;
            outH = rc.bottom - rc.top;
            return;
        }
    }
    outW = 1024;
    outH = 768;
}

bool IsWorldStateInGame() {
    const char* ws = eng::CurrentWorldState();
    if (!ws) return false;
    return _stricmp(ws, "ws_sp_ingame") == 0 ||
           _stricmp(ws, "ws_mp_ingame") == 0 ||
           _stricmp(ws, "ws_mp_ingame_jip") == 0;
}

bool IsInMenuOrPanelOpen() {
    if (!IsWorldStateInGame()) return true;
    return GamepadUI_IsAnyBlockingPanelOpen();
}

void SetVirtualCursorClient(float x, float y, bool activate, bool dispatchMove) {
    int sw = 0, sh = 0;
    GetScreenDimensions(sw, sh);
    if (x < 1.0f) x = 1.0f;
    if (y < 1.0f) y = 1.0f;
    if (sw > 2 && x > (float)(sw - 2)) x = (float)(sw - 2);
    if (sh > 2 && y > (float)(sh - 2)) y = (float)(sh - 2);

    g_virtX = x;
    g_virtY = y;
    g_virtCoordsInitialized = true;

    if (activate) {
        InterlockedExchange(&g_virtualCursorActive, 1);
    }

    int ix = (int)std::lround(g_virtX);
    int iy = (int)std::lround(g_virtY);

    // Fallback if GetCursorPos couldn't be hooked
    if (!g_getCursorPosHooked && activate && g_gameHwnd && GetForegroundWindow() == g_gameHwnd) {
        POINT sp = {ix, iy};
        if (ClientToScreen(g_gameHwnd, &sp)) {
            SetCursorPos(sp.x, sp.y);
            g_lastRealScreenPt = sp;
        }
    }

    if (dispatchMove && g_gameHwnd && IsWindow(g_gameHwnd)) {
        // Also mirror directly into UIShell mouse coordinates so hit-testing sees it immediately this frame
        void* uiShell = eng::UIShell_Singleton();
        if (uiShell && Readable(uiShell, 0x88)) {
            typedef void (__thiscall* SetMouseFn)(void*, int);
            ((SetMouseFn)0x0047211E)(uiShell, ix);
            ((SetMouseFn)0x0047BC01)(uiShell, iy);
        }
        g_injectingSyntheticMsg = true;
        SiegeHost_SendGameWndMsg(WM_MOUSEMOVE, 0, MAKELPARAM(ix, iy));
        g_injectingSyntheticMsg = false;
    }
}

void GetVirtualCursorClientF(float& outX, float& outY) {
    if (!g_virtCoordsInitialized) {
        int sw = 0, sh = 0;
        GetScreenDimensions(sw, sh);
        g_virtX = sw * 0.5f;
        g_virtY = sh * 0.5f;
        void* uiShell = eng::UIShell_Singleton();
        if (uiShell && Readable(uiShell, 0x84)) {
            int mx = *(const int*)((const char*)uiShell + 0x78);
            int my = *(const int*)((const char*)uiShell + 0x7C);
            if (mx > 0 && mx < sw && my > 0 && my < sh) {
                g_virtX = (float)mx;
                g_virtY = (float)my;
            }
        }
        g_virtCoordsInitialized = true;
    }
    outX = g_virtX;
    outY = g_virtY;
}

void DisableVirtualCursor() {
    InterlockedExchange(&g_virtualCursorActive, 0);
}

static void SetAppShiftBit(bool on, uint32_t& savedFlags, bool& modified) {
    modified = false;
    uintptr_t appMod = *(uintptr_t*)kAddrAppModule;
    if (!appMod || !Readable((void*)(appMod + 0x10C), 4)) return;
    uint32_t* pFlags = (uint32_t*)(appMod + 0x10C);
    savedFlags = *pFlags;
    if (on) *pFlags |= 1u;
    else    *pFlags &= ~1u;
    modified = true;
}

static void RestoreAppShiftBit(uint32_t savedFlags, bool modified) {
    if (!modified) return;
    uintptr_t appMod = *(uintptr_t*)kAddrAppModule;
    if (!appMod || !Readable((void*)(appMod + 0x10C), 4)) return;
    *(uint32_t*)(appMod + 0x10C) = savedFlags;
}

void InjectMouseClick(bool rightButton, bool down) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    float fx = 0.0f, fy = 0.0f;
    GetVirtualCursorClientF(fx, fy);
    int ix = (int)std::lround(fx);
    int iy = (int)std::lround(fy);

    // Ensure UIShell + AppModule cursor is synced to (ix, iy) before dispatching click
    SetVirtualCursorClient(fx, fy, true, true);

    UINT msg = rightButton ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                           : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    WPARAM wp = down ? (rightButton ? MK_RBUTTON : MK_LBUTTON) : 0;

    g_injectingSyntheticMsg = true;
    SiegeHost_SendGameWndMsg(msg, wp, MAKELPARAM(ix, iy));
    g_injectingSyntheticMsg = false;
}

void InjectMouseTap(bool rightButton, bool withShift) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    uint32_t savedFlags = 0;
    bool mod = false;
    if (withShift) {
        SetAppShiftBit(true, savedFlags, mod);
        InjectKeyEvent(VK_SHIFT, true, false);
    }
    InjectMouseClick(rightButton, true);
    InjectMouseClick(rightButton, false);
    if (withShift) {
        InjectKeyEvent(VK_SHIFT, false, false);
        RestoreAppShiftBit(savedFlags, mod);
    }
}

void InjectMouseDoubleClick() {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    float fx = 0.0f, fy = 0.0f;
    GetVirtualCursorClientF(fx, fy);
    int ix = (int)std::lround(fx);
    int iy = (int)std::lround(fy);
    SetVirtualCursorClient(fx, fy, true, true);
    g_injectingSyntheticMsg = true;
    SiegeHost_SendGameWndMsg(WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(ix, iy));
    SiegeHost_SendGameWndMsg(WM_LBUTTONUP, 0, MAKELPARAM(ix, iy));
    g_injectingSyntheticMsg = false;
}

void InjectMouseWheel(int wheelNotches) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd) || wheelNotches == 0) return;
    float fx = 0.0f, fy = 0.0f;
    GetVirtualCursorClientF(fx, fy);
    POINT sp = {(int)std::lround(fx), (int)std::lround(fy)};
    ClientToScreen(g_gameHwnd, &sp);
    WPARAM wp = MAKEWPARAM(0, (short)(wheelNotches * WHEEL_DELTA));
    g_injectingSyntheticMsg = true;
    SiegeHost_SendGameWndMsg(WM_MOUSEWHEEL, wp, MAKELPARAM(sp.x, sp.y));
    g_injectingSyntheticMsg = false;
}

void InjectKeyEvent(UINT vk, bool down, bool withShift) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    uint32_t savedFlags = 0;
    bool mod = false;
    if (withShift) SetAppShiftBit(true, savedFlags, mod);

    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) & 0xFF;
    LPARAM lp = 1 | (LPARAM)(scan << 16);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
        vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
        vk == VK_PRIOR || vk == VK_NEXT) {
        lp |= (1 << 24); // Extended key flag
    }
    if (!down) {
        lp |= (1 << 30) | (1 << 31);
    }

    g_injectingSyntheticMsg = true;
    SiegeHost_SendGameWndMsg(down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    g_injectingSyntheticMsg = false;

    if (withShift) RestoreAppShiftBit(savedFlags, mod);
}

void InjectKeyTap(UINT vk, bool withShift) {
    InjectKeyEvent(vk, true, withShift);
    InjectKeyEvent(vk, false, withShift);
}

} // namespace gp_internal

// ----------------------------------------------------------------------------------------------
// Public API (called from siegehost.cpp)
// ----------------------------------------------------------------------------------------------
void Gamepad_Init() {
    if (g_initialized) return;
    g_initialized = true;

    if (Cfg().role == Role::Server) return;
    if (!Cfg().gamepadEnabled) {
        Log("Gamepad: disabled by config ([Gamepad] Enabled=0).");
        return;
    }

    // Never call LoadLibraryA inside DllMain (loader lock): XInput DLL is loaded lazily on the first main-thread tick.
    InstallGetCursorPosHook();
    GamepadWorld_InstallHooks();
    Log("Gamepad: hooks armed (deadzone=%.2f, camSpeed=%.2f, cursorSpeed=%.2f, hints=%d).",
        Cfg().gamepadDeadzone, Cfg().gamepadCameraSpeed, Cfg().gamepadCursorSpeed, Cfg().gamepadShowHints);
}

void Gamepad_FilterMessage(LPMSG msg) {
    if (!msg || g_injectingSyntheticMsg) return;
    if (Cfg().role == Role::Server || !Cfg().gamepadEnabled) return;

    // Any real physical mouse button press immediately hands cursor control back to the mouse
    if (msg->message == WM_LBUTTONDOWN || msg->message == WM_RBUTTONDOWN || msg->message == WM_MBUTTONDOWN) {
        if (InterlockedCompareExchange(&g_virtualCursorActive, 0, 0) != 0) {
            DisableVirtualCursor();
        }
        g_gamepadActiveSession = false;
        g_hudCursorToggle = false;
        SyncGameplayCursorHidden(false);
    }
}

bool Gamepad_IsVirtualCursorActive() {
    return InterlockedCompareExchange(&g_virtualCursorActive, 0, 0) != 0;
}

bool Gamepad_GetVirtualCursorClient(POINT* outPt) {
    if (!outPt || !Gamepad_IsVirtualCursorActive()) return false;
    outPt->x = (LONG)std::lround(g_virtX);
    outPt->y = (LONG)std::lround(g_virtY);
    return true;
}

void Gamepad_Tick() {
    if (Cfg().role == Role::Server || !Cfg().gamepadEnabled) return;
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;

    // Load XInput safely on main thread (outside DllMain loader lock)
    LoadXInputOnce();
    if (!g_pfnXInputGetState) return;

    // Never process gamepad input while DSLOA window is unfocused (avoid ghost input during Alt-Tab)
    if (GetForegroundWindow() != g_gameHwnd) {
        if (g_mode == ControlMode::Gameplay || g_mode == ControlMode::FreeAim) {
            GamepadWorld_OnLeaveGameplay();
        }
        SyncGameplayCursorHidden(false);
        DisableVirtualCursor();
        return;
    }

    PollPhysicalMouseTakeover();

    DWORD now = GetTickCount();
    float dt = 0.016f;
    if (g_lastTickMs != 0) {
        DWORD delta = now - g_lastTickMs;
        if (delta > 0 && delta < 250) {
            dt = (float)delta / 1000.0f;
        }
    }
    g_lastTickMs = now ? now : 1;

    // Poll active controller; if none connected, scan ports 0..3 once every 1000ms (<1us overhead)
    xinput_abi::XINPUT_STATE st{};
    bool connected = false;
    if (g_activePort >= 0) {
        if (g_pfnXInputGetState((DWORD)g_activePort, &st) == ERROR_SUCCESS) {
            connected = true;
        } else {
            Log("Gamepad: controller disconnected from port %d.", g_activePort);
            g_activePort = -1;
            g_loggedConnection = false;
            g_gamepadActiveSession = false;
            g_hudCursorToggle = false;
            GamepadWorld_OnLeaveGameplay();
            GamepadUI_OnExitUI();
            SyncGameplayCursorHidden(false);
            DisableVirtualCursor();
            g_overlay = {};
            RenderOverlay();
            g_lastScanMs = now;
            return;
        }
    } else if (g_lastScanMs == 0 || (DWORD)(now - g_lastScanMs) >= 1000) {
        g_lastScanMs = now ? now : 1;
        for (int p = 0; p < 4; ++p) {
            if (g_pfnXInputGetState((DWORD)p, &st) == ERROR_SUCCESS) {
                g_activePort = p;
                connected = true;
                if (!g_loggedConnection) {
                    g_loggedConnection = true;
                    Log("Gamepad: XInput controller detected on port %d.", p);
                }
                break;
            }
        }
    }

    if (!connected) return;

    // Build normalized frame input
    PadFrame pad{};
    pad.connected   = true;
    pad.port        = g_activePort;
    pad.nowMs       = now;
    pad.dtSec       = dt;
    pad.buttons     = st.Gamepad.wButtons;
    pad.prevButtons = g_prevButtons;
    pad.leftStick   = gamepad::ProcessStick(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, Cfg().gamepadDeadzone, 1.35f);
    pad.rightStick  = gamepad::ProcessStick(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, Cfg().gamepadDeadzone, 1.35f);
    pad.ltValue     = (float)st.Gamepad.bLeftTrigger / 255.0f;
    pad.rtValue     = (float)st.Gamepad.bRightTrigger / 255.0f;
    pad.prevLtDown  = g_ltHyst.pressed;
    pad.prevRtDown  = g_rtHyst.pressed;
    pad.ltDown      = g_ltHyst.Update(st.Gamepad.bLeftTrigger);
    pad.rtDown      = g_rtHyst.Update(st.Gamepad.bRightTrigger);

    g_prevButtons = pad.buttons;

    bool anyPadInput = (pad.buttons != 0) || pad.ltDown || pad.rtDown ||
                       pad.leftStick.magnitude > 0.0f || pad.rightStick.magnitude > 0.0f;
    if (anyPadInput) {
        g_gamepadActiveSession = true;
    }

    // Determine active control mode automatically from game & UI state
    ControlMode prevMode = g_mode;
    ControlMode nextMode = ControlMode::Inactive;

    if (!g_gamepadActiveSession) {
        nextMode = ControlMode::Inactive;
    } else if (IsChatActive()) {
        nextMode = ControlMode::Chat;
    } else if (!IsWorldStateInGame() || GamepadUI_IsAnyBlockingPanelOpen() || g_hudCursorToggle) {
        nextMode = ControlMode::MenuUI;
    } else if (pad.rtDown) {
        nextMode = ControlMode::FreeAim;
    } else {
        nextMode = ControlMode::Gameplay;
    }

    // Handle mode transitions cleanly
    if (prevMode != nextMode) {
        if (prevMode == ControlMode::Gameplay || prevMode == ControlMode::FreeAim) {
            GamepadWorld_OnLeaveGameplay();
        }
        if (prevMode == ControlMode::MenuUI) {
            GamepadUI_OnExitUI();
        }
        if (nextMode == ControlMode::Gameplay) {
            // Park virtual cursor in neutral top-center band (away from edge-scroll borders,
            // HUD buttons, and center combat viewport) to prevent phantom hover tooltips while hidden.
            int sw = 0, sh = 0;
            GetScreenDimensions(sw, sh);
            SetVirtualCursorClient(sw * 0.5f, 28.0f, true, true);
        }
        g_mode = nextMode;
    }

    // Reset overlay descriptors each frame; active mode populates what it needs
    g_overlay = {};

    switch (g_mode) {
        case ControlMode::Inactive:
            break;

        case ControlMode::Chat:
            // While typing in chat / character name / console, stay out of the way;
            // allow B or Start to dismiss chat cleanly with Esc, A to submit with Enter.
            if (pad.Pressed(xinput_abi::kB) || pad.Pressed(xinput_abi::kStart)) {
                InjectKeyTap(VK_ESCAPE);
            } else if (pad.Pressed(xinput_abi::kA)) {
                InjectKeyTap(VK_RETURN);
            }
            g_overlay.showHint = true;
            g_overlay.hintText = "[Keyboard] Type   [A] Send   [B] Close";
            break;

        case ControlMode::Gameplay:
            if (pad.Pressed(xinput_abi::kBack)) {
                g_hudCursorToggle = true;
                g_mode = ControlMode::MenuUI;
                GamepadWorld_OnLeaveGameplay();
                GamepadUI_Tick(pad);
            } else {
                GamepadWorld_TickGameplay(pad);
            }
            break;

        case ControlMode::FreeAim:
            GamepadWorld_TickFreeAim(pad);
            break;

        case ControlMode::MenuUI:
            if (g_hudCursorToggle && IsWorldStateInGame() && !GamepadUI_IsAnyBlockingPanelOpen() &&
                pad.Pressed(xinput_abi::kBack))
            {
                g_hudCursorToggle = false;
                GamepadUI_OnExitUI();
                int sw = 0, sh = 0;
                GetScreenDimensions(sw, sh);
                SetVirtualCursorClient(sw * 0.5f, 28.0f, true, true);
                g_mode = ControlMode::Gameplay;
            } else {
                GamepadUI_Tick(pad);
            }
            break;
    }

    // Hide the Dungeon Siege sword cursor only during active 3D gameplay;
    // keep it visible in menus, inventory, HUD cursor mode, Free-Aim, chat, or mouse/KB use.
    SyncGameplayCursorHidden(g_gamepadActiveSession && g_mode == ControlMode::Gameplay);

    RenderOverlay();
}
