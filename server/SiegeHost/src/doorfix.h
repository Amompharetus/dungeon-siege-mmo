#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace doorfix {

static constexpr uint32_t kDoorSyncMagic      = 0x52444853u;  // "SHDR"
static constexpr uint8_t  kDoorSyncVersion    = 1u;
static constexpr uint8_t  kMaxReportedDoors   = 64u;
static constexpr uint16_t kFlagTruncated      = 0x0001u;

static constexpr uint32_t kMissingGraceMs     = 1200u;  // must be absent across >=2 reports spanning >=1.2s
static constexpr uint32_t kResyncCooldownMs   = 5000u;  // minimum gap between resyncs for same (dpnid, goid)
static constexpr uint32_t kStaleEntryMs       = 30000u; // prune untracked entries after 30s
static constexpr uint8_t  kMaxResyncAttempts  = 5u;

#pragma pack(push, 1)
struct DoorSyncReport {
    uint32_t magic;
    uint8_t  version;
    uint8_t  count;
    uint16_t flags;
    uint32_t heroNode;
    uint32_t doorGoids[kMaxReportedDoors];
};
#pragma pack(pop)

static constexpr size_t kDoorSyncHeaderBytes = 12u;

inline size_t SerializedReportSize(uint8_t count) {
    if (count > kMaxReportedDoors) count = kMaxReportedDoors;
    return kDoorSyncHeaderBytes + static_cast<size_t>(count) * sizeof(uint32_t);
}

inline bool ValidateReportPacket(const uint8_t* data, size_t size, DoorSyncReport* out) {
    if (!data || !out || size < kDoorSyncHeaderBytes) return false;
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != kDoorSyncMagic) return false;
    uint8_t ver = data[4];
    uint8_t cnt = data[5];
    if (ver != kDoorSyncVersion || cnt > kMaxReportedDoors) return false;
    if (size < SerializedReportSize(cnt)) return false;

    std::memset(out, 0, sizeof(*out));
    std::memcpy(out, data, SerializedReportSize(cnt));
    return true;
}

inline bool ReportContainsGoid(const DoorSyncReport& rep, uint32_t goid) {
    uint8_t n = rep.count <= kMaxReportedDoors ? rep.count : kMaxReportedDoors;
    for (uint8_t i = 0; i < n; ++i) {
        if (rep.doorGoids[i] == goid) return true;
    }
    return false;
}

inline bool StartsWithNoCase(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (!*s) return false;
        if (std::tolower(static_cast<unsigned char>(*s)) !=
            std::tolower(static_cast<unsigned char>(*prefix))) {
            return false;
        }
        ++s;
        ++prefix;
    }
    return true;
}

inline bool EqualsNoCase(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Strips optional difficulty/mod prefixes ("2W_", "3W_", "mmo_loa_") before matching.
inline const char* StripTemplatePrefixes(const char* tpl) {
    if (!tpl) return "";
    if (StartsWithNoCase(tpl, "2W_") || StartsWithNoCase(tpl, "3W_")) {
        tpl += 3;
    }
    if (StartsWithNoCase(tpl, "mmo_loa_")) {
        tpl += 8;
    }
    return tpl;
}

// Matches all 107 vanilla DS1 + LoA base_door subclasses without false-matching non-doors.
inline bool IsDoorTemplateName(const char* tpl) {
    if (!tpl || !*tpl) return false;
    const char* s = StripTemplatePrefixes(tpl);
    return StartsWithNoCase(s, "door_") ||
           EqualsNoCase(s, "base_door") ||
           StartsWithNoCase(s, "dsx_gear_door") ||
           StartsWithNoCase(s, "dsx_ice_gate_") ||
           StartsWithNoCase(s, "dsx_nassirom_door");
}

inline bool IsGearDoorTemplateName(const char* tpl) {
    if (!tpl || !*tpl) return false;
    return StartsWithNoCase(StripTemplatePrefixes(tpl), "dsx_gear_door");
}

// Tracks missing (dpnid, goid) pairs with a grace window, cooldown, and retry cap so
// freshly streamed doors in flight on the wire are never falsely double-synced.
class DoorMissingTracker {
public:
    struct Entry {
        uint32_t dpnid;
        uint32_t goid;
        uint32_t firstMissingMs;
        uint32_t lastSeenMissingMs;
        uint32_t lastResyncMs;
        uint16_t missingReports;
        uint8_t  resyncCount;
    };

    void MarkPresent(uint32_t dpnid, uint32_t goid) {
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].dpnid == dpnid && entries_[i].goid == goid) {
                entries_[i] = entries_.back();
                entries_.pop_back();
                return;
            }
        }
    }

    void ClearPlayer(uint32_t dpnid) {
        size_t out = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].dpnid != dpnid) {
                entries_[out++] = entries_[i];
            }
        }
        entries_.resize(out);
    }

    void PruneStale(uint32_t nowMs) {
        size_t out = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if ((nowMs - entries_[i].lastSeenMissingMs) < kStaleEntryMs) {
                entries_[out++] = entries_[i];
            }
        }
        entries_.resize(out);
    }

    // Returns true iff the server should call SyncGoToClient for (dpnid, goid) now.
    bool RecordMissing(uint32_t dpnid, uint32_t goid, uint32_t nowMs) {
        Entry* e = nullptr;
        for (auto& cur : entries_) {
            if (cur.dpnid == dpnid && cur.goid == goid) {
                e = &cur;
                break;
            }
        }
        if (!e) {
            if (entries_.size() >= 256) PruneStale(nowMs);
            Entry ne{};
            ne.dpnid             = dpnid;
            ne.goid              = goid;
            ne.firstMissingMs    = nowMs;
            ne.lastSeenMissingMs = nowMs;
            ne.lastResyncMs      = 0;
            ne.missingReports    = 1;
            ne.resyncCount       = 0;
            entries_.push_back(ne);
            return false;
        }

        e->lastSeenMissingMs = nowMs;
        if (e->missingReports < 0xFFFFu) ++e->missingReports;

        if (e->resyncCount >= kMaxResyncAttempts) return false;
        if (e->missingReports < 2u) return false;
        if ((nowMs - e->firstMissingMs) < kMissingGraceMs) return false;
        if (e->resyncCount > 0 && (nowMs - e->lastResyncMs) < kResyncCooldownMs) return false;

        e->lastResyncMs = nowMs;
        ++e->resyncCount;
        return true;
    }

    size_t Size() const { return entries_.size(); }

private:
    std::vector<Entry> entries_;
};

}  // namespace doorfix

#ifdef _WIN32
void DoorFixInstall();
void DoorFixTick();
void DoorFix_SetDirectPlay(void* dpClient, void* dpServer);
bool DoorFix_OnReceivePacket(bool isServer, unsigned long senderDpnid, const unsigned char* data, unsigned long size);
void DoorFix_OnPlayerDestroyed(unsigned long dpnid);
#endif
