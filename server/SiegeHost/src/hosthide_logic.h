// hosthide_logic.h - pure, host-testable rules for hiding the dedicated ServerHost character.
// Included both by src/hosthide.cpp (DLL) and tests/hosthide_test.cpp (unit tests).
#pragma once
#include <cstdint>
#include <cwctype>

namespace hosthide {

// GoAspect::m_Flags (+0x3C) bit masks verified against GoAspect::Xfer (DSLOA.exe 0x00537722 / L297998).
constexpr uint32_t kAspectVisible              = 0x00000001u;
constexpr uint32_t kAspectDrawShadow           = 0x00000002u;
constexpr uint32_t kAspectDrawSelIndicator     = 0x00000008u;
// Note: 0x00000010 is checked as an override in FUN_00535b7c (L297413) — intentionally NEVER set.
constexpr uint32_t kAspectCollidable           = 0x00000020u;
constexpr uint32_t kAspectBlocksPath           = 0x00000040u;
constexpr uint32_t kAspectInvincible           = 0x00000100u;
constexpr uint32_t kAspectSelectable           = 0x00000200u;

constexpr uint32_t kAspectCloakClearMask =
    kAspectVisible | kAspectDrawShadow | kAspectDrawSelIndicator |
    kAspectCollidable | kAspectBlocksPath | kAspectSelectable;

constexpr uint32_t kAspectCloakSetMask = kAspectInvincible;

inline bool WideEqIgnoreCase(const wchar_t* a, const wchar_t* b) {
    if (!a || !b || !*a || !*b) return false;
    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == L'\0' && *b == L'\0';
}

// True when either the player's profile name (Player+0x04) or hero name (Player+0x58)
// matches the configured HostCharacter (default "ServerHost"), case-insensitively.
inline bool MatchesHostName(const wchar_t* playerName, const wchar_t* heroName, const wchar_t* hostName) {
    if (!hostName || !*hostName) return false;
    return WideEqIgnoreCase(playerName, hostName) || WideEqIgnoreCase(heroName, hostName);
}

// Decision for UI player-iteration hooks (staging lobby, \ scoreboard, overhead labels,
// loading progress, in-game players panel, chat dropdown, kick listbox, end-game summary).
// Never hides the local player themselves, even if misconfigured.
inline bool ShouldShowInUi(bool engineValid, bool hideHost, bool isLocalPlayer, bool isHostPlayer) {
    if (!engineValid) return false;
    if (hideHost && !isLocalPlayer && isHostPlayer) return false;
    return true;
}

// Decision for Player::RCSetWorldLocation ("%S has entered %S") chat announcements.
inline bool ShouldSuppressLocationChat(bool hideHost, bool isLocalPlayer, bool isHostPlayer) {
    return hideHost && !isLocalPlayer && isHostPlayer;
}

// Computes the row count for the `\` / `"` multiplayer_ranks scoreboard (0x00515770).
// Clamps to [1, maxRows] because 0x00515770 indexes `rows[count - 1]` unconditionally
// (L266691) and allocates fixed 8-slot UIWindow* arrays.
inline int ComputeRanksRowCount(int visibleNonHostHumans, int rawHumanCount, bool hideHost, int maxRows = 8) {
    int count = hideHost ? visibleNonHostHumans : rawHumanCount;
    if (count < 1) count = 1;
    if (maxRows > 0 && count > maxRows) count = maxRows;
    return count;
}

// Computes the cloaked GoAspect::m_Flags bitfield for ServerHost's 3D hero.
// Clears visibility, ground shadow, selection ring/minimap flag, collision, path blocking,
// and selectability; enables invincibility; leaves all other bits (including 0x10) untouched.
inline uint32_t ComputeCloakedAspectFlags(uint32_t currentFlags) {
    return (currentFlags & ~kAspectCloakClearMask) | kAspectCloakSetMask;
}

inline bool NeedsAspectCloak(uint32_t currentFlags) {
    return ComputeCloakedAspectFlags(currentFlags) != currentFlags;
}

}  // namespace hosthide
