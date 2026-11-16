#pragma once

// Pure BuffUI helpers (icon cache keyed by spellGoid with stateName fallback, and >99m timer formatting).
// Header-only and Windows-free so native host tests can exercise it directly.

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace buffui {

inline int AsciiCaseCmp(const char* a, const char* b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        unsigned char ca = static_cast<unsigned char>(*a++);
        unsigned char cb = static_cast<unsigned char>(*b++);
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return static_cast<int>(ca) - static_cast<int>(cb);
    }
    return static_cast<int>(static_cast<unsigned char>(*a)) - static_cast<int>(static_cast<unsigned char>(*b));
}

struct StateIconCache {
    static constexpr int kCapacity = 128;

    struct Entry {
        uint32_t spellGoid;
        char     stateName[64];
        char     activeIcon[64];
    };

    Entry entries[kCapacity] = {};
    int   count = 0;
    int   nextEvict = 0;

    void Record(uint32_t spellGoid, const char* sName, const char* icon) {
        if (!sName || !*sName || !icon || !*icon) return;
        for (int i = 0; i < count; ++i) {
            bool matchByGoid = (spellGoid != 0 && entries[i].spellGoid == spellGoid);
            bool matchNoGoid = (spellGoid == 0 && entries[i].spellGoid == 0 && AsciiCaseCmp(entries[i].stateName, sName) == 0);
            if (matchByGoid || matchNoGoid) {
                std::strncpy(entries[i].stateName, sName, 63);
                entries[i].stateName[63] = '\0';
                std::strncpy(entries[i].activeIcon, icon, 63);
                entries[i].activeIcon[63] = '\0';
                return;
            }
        }
        int idx;
        if (count < kCapacity) {
            idx = count++;
        } else {
            idx = nextEvict;
            nextEvict = (nextEvict + 1) % kCapacity;
        }
        entries[idx].spellGoid = spellGoid;
        std::strncpy(entries[idx].stateName, sName, 63);
        entries[idx].stateName[63] = '\0';
        std::strncpy(entries[idx].activeIcon, icon, 63);
        entries[idx].activeIcon[63] = '\0';
    }

    const char* Lookup(uint32_t spellGoid, const char* sName) const {
        if (spellGoid != 0) {
            for (int i = 0; i < count; ++i) {
                if (entries[i].spellGoid == spellGoid && entries[i].activeIcon[0] != '\0') {
                    return entries[i].activeIcon;
                }
            }
        }
        if (sName && *sName) {
            for (int i = count - 1; i >= 0; --i) {
                if (AsciiCaseCmp(entries[i].stateName, sName) == 0 && entries[i].activeIcon[0] != '\0') {
                    return entries[i].activeIcon;
                }
            }
        }
        return nullptr;
    }
};

// Format icon overlay timer lines when slotTime > 5999 (>99m59s) into the 15px-wide 2-line i_time box:
//   - 100m..9h59m: line0 = "<H>h" (e.g. "1h"), line1 = "<MM>" (e.g. "40")
//   - >= 10h:      line0 = "99", line1 = "++"
inline void FormatOverNinetyNineMinutes(int slotTime, char line0[16], char line1[16]) {
    int hours  = slotTime / 3600;
    int remMin = (slotTime % 3600) / 60;
    if (hours >= 1 && hours <= 9) {
        std::snprintf(line0, 16, "%dh", hours);
        std::snprintf(line1, 16, "%02d", remMin);
    } else {
        std::strcpy(line0, "99");
        std::strcpy(line1, "++");
    }
}

} // namespace buffui
