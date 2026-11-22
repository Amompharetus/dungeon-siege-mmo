// PrefsFix — persist "Tutorial Tips" (UIMenuManager+0x40) in prefs.gas across multiplayer sessions.
//
// In vanilla DSLOA.exe v1.50:
//   - UIMenuManager singleton lives at [0x007ACFA4]; m_bTutorialTips is BYTE at +0x40 (ctor 0x00494527 sets 1).
//   - UIMenuManager::SetTutorialTips(BYTE bEnable, BYTE bShowChatHint) is at 0x004974FC.
//   - UIGame::Xfer (0x004EFF3B) serializes "tutorial_tips" only into single-player .dssave files.
//   - TattooGame::LoadPrefs (0x00474A4A) and TattooGame::SavePrefs (0x0047548C) read/write UIMenuManager's
//     text_scroll (+0x20) and max_text (+0x24) in prefs.gas, but omit tutorial_tips completely.
//   - Additionally, SiegeHost's ExitAfterGame terminates the process after leaving an MP game, bypassing
//     TattooGame::Shutdown's SavePrefs call if a player toggled tips off via the tutorial popup (tip_off).
//
// Fix (client / standalone only):
//   1. Hook TattooGame::LoadPrefs (0x00474A4A) to load "tutorial_tips" from [prefs] if present (preserving
//      the vanilla default of true for brand-new profiles).
//   2. Hook TattooGame::SavePrefs (0x0047548C) to write "tutorial_tips" into [prefs] right before the
//      stock function writes all other keys and flushes prefs.gas to disk.
//   3. Hook UIMenuManager::SetTutorialTips (0x004974FC) so popup toggles (bShowChatHint != 0, used only by
//      tip_off/tip_on) immediately flush prefs.gas, surviving ExitAfterGame / Alt+F4.

#include "siegehost.h"
#include <cstdint>
#include <cstring>

namespace {

static bool Readable(const void* ptr, size_t len) {
    if (!ptr || (uintptr_t)ptr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    return (uintptr_t)ptr + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static bool InstallDetour(DWORD va, const BYTE* expect, size_t len, void* hook, void** outOrig, const char* name) {
    BYTE* src = (BYTE*)va;
    if (!Readable(src, len) || len < 5 || memcmp(src, expect, len) != 0) {
        Log("PrefsFix: detour verify failed for %s at 0x%08lX", name, va);
        return false;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("PrefsFix: VirtualAlloc failed for %s", name);
        return false;
    }
    memcpy(tramp, src, len);
    tramp[len] = 0xE9;
    *(int32_t*)(tramp + len + 1) = (int32_t)((va + len) - (DWORD)(tramp + len + 5));
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);

    DWORD old = 0;
    if (!VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &old)) {
        Log("PrefsFix: VirtualProtect failed for %s (%lu)", name, GetLastError());
        return false;
    }
    src[0] = 0xE9;
    *(int32_t*)(src + 1) = (int32_t)((DWORD)hook - (va + 5));
    for (size_t i = 5; i < len; ++i) src[i] = 0x90;
    VirtualProtect(src, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, len);

    *outOrig = tramp;
    Log("PrefsFix: hooked %s at 0x%08lX (len=%u)", name, va, (unsigned)len);
    return true;
}

static constexpr DWORD kTattooGameSingletonPtr = 0x007A99B0;
static constexpr DWORD kUiMenuMgrSingletonPtr  = 0x007ACFA4;
static constexpr size_t kTutorialTipsOffset    = 0x40;
static constexpr DWORD kFvpBool                = 0x20;  // returned by 0x00440FF9

using GetPrefsFuelHandleFn = void* (__thiscall*)(void* tattooGame, void** outBlock, BYTE bCreate);
using FuelGetBoolFn        = int   (__thiscall*)(void* fuelBlock, const char* key, BYTE* outVal, BYTE bOptional);
using FuelSetBoolFn        = BYTE  (__thiscall*)(void* fuelBlock, const char* key, const BYTE* valPtr, DWORD fvpFlags);

using LoadPrefsFn       = void (__fastcall*)(void* tattooGame, void* edx);
using SavePrefsFn       = void (__fastcall*)(void* tattooGame, void* edx, BYTE bFlush);
using SetTutorialTipsFn = void (__fastcall*)(void* uiMenuMgr, void* edx, BYTE bEnable, BYTE bShowChatHint);

static LoadPrefsFn       g_origLoadPrefs       = nullptr;
static SavePrefsFn       g_origSavePrefs       = nullptr;
static SetTutorialTipsFn g_origSetTutorialTips = nullptr;
static bool              g_inPopupSave         = false;

static void* GetUiMenuMgr() {
    void* p = *(void**)kUiMenuMgrSingletonPtr;
    return (p && Readable(p, kTutorialTipsOffset + 1)) ? p : nullptr;
}

static void* GetTattooGame() {
    void* p = *(void**)kTattooGameSingletonPtr;
    return (p && Readable(p, 0x10)) ? p : nullptr;
}

static void* OpenPrefsBlock(void* tattooGame, bool create) {
    if (!tattooGame) return nullptr;
    void* block = nullptr;
    auto fn = (GetPrefsFuelHandleFn)0x00475A8E;
    fn(tattooGame, &block, create ? 1 : 0);
    return block;
}

static void __fastcall Hook_LoadPrefs(void* tattooGame, void* edx) {
    g_origLoadPrefs(tattooGame, edx);

    void* uiMenu = GetUiMenuMgr();
    void* block  = OpenPrefsBlock(tattooGame, false);
    if (!uiMenu || !block) return;

    BYTE val = 1;
    auto getBool = (FuelGetBoolFn)0x0047A3A9;
    if (getBool(block, "tutorial_tips", &val, 1)) {
        BYTE normalized = val ? 1 : 0;
        if (g_origSetTutorialTips) {
            g_origSetTutorialTips(uiMenu, nullptr, normalized, 0);
        } else {
            *((BYTE*)uiMenu + kTutorialTipsOffset) = normalized;
        }
        Log("PrefsFix: loaded tutorial_tips=%s from prefs.gas", normalized ? "true" : "false");
    }
}

static void __fastcall Hook_SavePrefs(void* tattooGame, void* edx, BYTE bFlush) {
    void* uiMenu = GetUiMenuMgr();
    if (uiMenu && tattooGame) {
        void* block = OpenPrefsBlock(tattooGame, true);
        if (block) {
            BYTE val = (*((const BYTE*)uiMenu + kTutorialTipsOffset)) ? 1 : 0;
            auto setBool = (FuelSetBoolFn)0x0047A430;
            setBool(block, "tutorial_tips", &val, kFvpBool);
            Log("PrefsFix: saved tutorial_tips=%s to prefs.gas (flush=%u)",
                val ? "true" : "false", (unsigned)(bFlush ? 1 : 0));
        }
    }
    g_origSavePrefs(tattooGame, edx, bFlush);
}

static void __fastcall Hook_SetTutorialTips(void* uiMenuMgr, void* edx, BYTE bEnable, BYTE bShowChatHint) {
    g_origSetTutorialTips(uiMenuMgr, edx, bEnable, bShowChatHint);

    // Only popup toggles (tip_off at 0x004F23D5 / tip_on at 0x004F23F4) pass bShowChatHint != 0.
    // Options-menu toggles, Defaults, and Cancel pass bShowChatHint == 0 (persisted on OK via SavePrefs).
    if (bShowChatHint && !g_inPopupSave && g_origSavePrefs) {
        if (void* tg = GetTattooGame()) {
            g_inPopupSave = true;
            Hook_SavePrefs(tg, nullptr, 1);
            g_inPopupSave = false;
        }
    }
}

}  // namespace

void PrefsFixInstall() {
    if (Cfg().role == Role::Server) return;

    static const BYTE kLoadPrefsExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C };
    static const BYTE kSavePrefsExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
    static const BYTE kSetTutExpect[9]    = { 0x8A, 0x44, 0x24, 0x04, 0x84, 0xC0, 0x88, 0x41, 0x40 };

    bool okLoad = InstallDetour(0x00474A4A, kLoadPrefsExpect, sizeof(kLoadPrefsExpect),
                                (void*)&Hook_LoadPrefs, (void**)&g_origLoadPrefs, "TattooGame::LoadPrefs");
    bool okSave = InstallDetour(0x0047548C, kSavePrefsExpect, sizeof(kSavePrefsExpect),
                                (void*)&Hook_SavePrefs, (void**)&g_origSavePrefs, "TattooGame::SavePrefs");
    bool okSet  = InstallDetour(0x004974FC, kSetTutExpect, sizeof(kSetTutExpect),
                                (void*)&Hook_SetTutorialTips, (void**)&g_origSetTutorialTips, "UIMenuManager::SetTutorialTips");
    if (okLoad && okSave && okSet) {
        Log("PrefsFix: installed (tutorial_tips persisted in prefs.gas)");
    }
}
