// HasteFix — apply active Haste / Group Haste speed modifier (Blender+0x7C) to weapon
// ReloadDelay and spell CastReloadDelay on the server so crossbows, longbows, miniguns,
// and Nature/Combat Magic spells receive the full +25% speed bonus alongside melee and short bows.
//
// In vanilla DSLOA.exe v1.50:
//   - spell_speed_affector.skrit (used by spell_haste and spell_group_haste) sets walk velocity
//     to 1.25x and calls nema::Blender::ChangeSpeedModifier(0.8f) (0x006A788C -> Blender+0x7C).
//   - Blender::GetBaseDuration (0x006A7044) multiplies animation duration by [Blender+0x7C],
//     giving +25% speed to melee (reload_delay=0) and short/LoA bows (reload_delay=0).
//   - However, combat jobs compute total attack/cast cycle time as:
//       job_attack_object_ranged.skrit : m_Weapon$.Go.Attack.ReloadDelay + Blender.GetBaseDuration(...)
//       job_attack_object_melee.skrit  : m_Weapon$.Attack.ReloadDelay    + Blender.GetBaseDuration(...)
//       job_cast.skrit / _position     : m_Spell$.Magic.CastReloadDelay  + Blender.GetBaseDuration(...)
//     Because ReloadDelay and CastReloadDelay are not scaled by Blender+0x7C, crossbows/miniguns
//     only gained ~1-4% speed and spells only gained ~3-10% speed under Haste.
//
// Fix (Role::Server only — leaves client UI tooltips displaying unmodified base values):
//   - Hook GoAttack::GetReloadDelay (0x005CCB01) and GoMagic::GetCastReloadDelay (0x005D5C63).
//   - Walk GoComponent+0x04 -> Go -> Go::GetParent (+0x6C, up to 4 hops) until finding the owning
//     actor (Go::GetActor at +0x00 != nullptr).
//   - Read Go::GetAspect (+0x04) -> GoAspect::GetAspectPtr (+0x44) -> Aspect::GetBlender (+0xE8)
//     -> Blender::m_SpeedModifier (+0x7C).
//   - When speedMod is strictly in (0.1f, 0.999f) (i.e. a speedup like Haste 0.8f; ignores normal
//     1.0f and Slow > 1.0f), multiply the returned delay by speedMod so total cycle time is
//     scaled by exactly 0.8x (+25% speed).

#include "siegehost.h"
#include <atomic>
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
        Log("HasteFix: detour verify failed for %s at 0x%08lX", name, va);
        return false;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("HasteFix: VirtualAlloc failed for %s", name);
        return false;
    }
    memcpy(tramp, src, len);
    tramp[len] = 0xE9;
    *(int32_t*)(tramp + len + 1) = (int32_t)((va + len) - (DWORD)(tramp + len + 5));
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);

    DWORD old = 0;
    if (!VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &old)) {
        Log("HasteFix: VirtualProtect failed for %s (%lu)", name, GetLastError());
        return false;
    }
    src[0] = 0xE9;
    *(int32_t*)(src + 1) = (int32_t)((DWORD)hook - (va + 5));
    for (size_t i = 5; i < len; ++i) src[i] = 0x90;
    VirtualProtect(src, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, len);

    *outOrig = tramp;
    Log("HasteFix: hooked %s at 0x%08lX (len=%u)", name, va, (unsigned)len);
    return true;
}

// Verified DSLOA.exe v1.50 offsets (all confirmed via PE exports):
static constexpr DWORD  kVaGetReloadDelay     = 0x005CCB01; // GoAttack::GetReloadDelay
static constexpr DWORD  kVaGetCastReloadDelay = 0x005D5C63; // GoMagic::GetCastReloadDelay
static constexpr size_t kOffCompGo            = 0x04;       // GoComponent::GetGo (0x004E8A92)
static constexpr size_t kOffGoActor           = 0x00;       // Go::GetActor (0x00470810)
static constexpr size_t kOffGoAspect          = 0x04;       // Go::GetAspect (0x004E8A92)
static constexpr size_t kOffGoParent          = 0x6C;       // Go::GetParent (0x006E7F8E)
static constexpr size_t kOffAspectNemaPtr     = 0x44;       // GoAspect::GetAspectPtr (0x004468C3)
static constexpr size_t kOffNemaBlender       = 0xE8;       // nema::Aspect::GetBlender (0x00571DDC)
static constexpr size_t kOffBlenderSpeedMod   = 0x7C;       // nema::Blender::ChangeSpeedModifier (0x006A788C)

using FloatGetterFn = float (__fastcall*)(void* self, void* edx);

static FloatGetterFn g_origGetReloadDelay     = nullptr;
static FloatGetterFn g_origGetCastReloadDelay = nullptr;
static std::atomic<uint32_t> g_scaledAttackCount{0};
static std::atomic<uint32_t> g_scaledMagicCount{0};

// Returns the owning actor's Blender speed modifier if it is speeding the actor up
// (strictly between 0.1f and 0.999f, e.g. 0.8f under Haste / Group Haste).
// Returns 1.0f in all other cases (no haste, Slow > 1.0f, ground item, invalid pointers).
static float GetOwnerHasteSpeedMod(void* goComponent) {
    if (!Readable(goComponent, kOffCompGo + sizeof(void*))) return 1.0f;

    void* go = *(void* const*)((const char*)goComponent + kOffCompGo);
    void* actorGo = nullptr;

    // Walk up to 4 hops: 0 = actor's own GoAttack, 1 = weapon/innate spell -> actor,
    // 2 = spell -> spellbook -> actor, 3 = nested container safety margin.
    for (int depth = 0; depth < 4 && go != nullptr; ++depth) {
        if (!Readable(go, kOffGoParent + sizeof(void*))) return 1.0f;
        void* actorComp = *(void* const*)((const char*)go + kOffGoActor);
        if (actorComp != nullptr) {
            actorGo = go;
            break;
        }
        go = *(void* const*)((const char*)go + kOffGoParent);
    }
    if (!actorGo) return 1.0f;

    void* aspect = *(void* const*)((const char*)actorGo + kOffGoAspect);
    if (!Readable(aspect, kOffAspectNemaPtr + sizeof(void*))) return 1.0f;

    void* nemaAspect = *(void* const*)((const char*)aspect + kOffAspectNemaPtr);
    if (!Readable(nemaAspect, kOffNemaBlender + sizeof(void*))) return 1.0f;

    void* blender = *(void* const*)((const char*)nemaAspect + kOffNemaBlender);
    if (!Readable(blender, kOffBlenderSpeedMod + sizeof(float))) return 1.0f;

    float speedMod = *(const float*)((const char*)blender + kOffBlenderSpeedMod);
    if (speedMod > 0.1f && speedMod < 0.999f) {
        return speedMod;
    }
    return 1.0f;
}

static float __fastcall Hook_GetReloadDelay(void* goAttack, void* edx) {
    float delay = g_origGetReloadDelay(goAttack, edx);
    if (delay > 0.0f) {
        float mod = GetOwnerHasteSpeedMod(goAttack);
        if (mod < 0.999f) {
            uint32_t n = g_scaledAttackCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5 || (n % 250) == 0) {
                Log("HasteFix: scaled weapon ReloadDelay %.3f -> %.3f (mod=%.3f, count=%u)",
                    delay, delay * mod, mod, n);
            }
            return delay * mod;
        }
    }
    return delay;
}

static float __fastcall Hook_GetCastReloadDelay(void* goMagic, void* edx) {
    float delay = g_origGetCastReloadDelay(goMagic, edx);
    if (delay > 0.0f) {
        float mod = GetOwnerHasteSpeedMod(goMagic);
        if (mod < 0.999f) {
            uint32_t n = g_scaledMagicCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 5 || (n % 250) == 0) {
                Log("HasteFix: scaled spell CastReloadDelay %.3f -> %.3f (mod=%.3f, count=%u)",
                    delay, delay * mod, mod, n);
            }
            return delay * mod;
        }
    }
    return delay;
}

} // namespace

void HasteFixInstall() {
    // 0x005CCB01: test byte ptr [0x007B0B84], 1 (7 bytes, absolute address)
    static const BYTE kExpectReload[7] = { 0xF6, 0x05, 0x84, 0x0B, 0x7B, 0x00, 0x01 };
    // 0x005D5C63: test byte ptr [0x007B0DF0], 1 (7 bytes, absolute address)
    static const BYTE kExpectCastReload[7] = { 0xF6, 0x05, 0xF0, 0x0D, 0x7B, 0x00, 0x01 };

    bool ok1 = InstallDetour(
        kVaGetReloadDelay, kExpectReload, sizeof(kExpectReload),
        (void*)Hook_GetReloadDelay, (void**)&g_origGetReloadDelay,
        "GoAttack::GetReloadDelay");
    bool ok2 = InstallDetour(
        kVaGetCastReloadDelay, kExpectCastReload, sizeof(kExpectCastReload),
        (void*)Hook_GetCastReloadDelay, (void**)&g_origGetCastReloadDelay,
        "GoMagic::GetCastReloadDelay");

    if (ok1 && ok2) {
        Log("HasteFix: active (Haste / Group Haste scales ReloadDelay and CastReloadDelay by Blender speed modifier).");
    } else {
        Log("HasteFix: WARNING partial install (reload=%d castReload=%d)", ok1 ? 1 : 0, ok2 ? 1 : 0);
    }
}
