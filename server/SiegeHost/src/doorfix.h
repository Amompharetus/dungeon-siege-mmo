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

static constexpr uint32_t kMissingGraceMs     = 3500u;  // must be absent across >=2 reports spanning >=3.5s (avoids streaming-in races)
static constexpr uint32_t kResyncCooldownMs   = 5000u;  // minimum gap between resyncs for same (dpnid, goid)
static constexpr uint32_t kStaleEntryMs       = 30000u; // prune untracked entries after 30s
static constexpr uint8_t  kMaxResyncAttempts  = 5u;
static constexpr size_t   kMaxFubiFlushBytes  = 24576u; // chunk oversized FuBi flushes well under DP8 64 KB cap

// Poison delta-compression baselines before packing GoCreateReq (0x005451B1) or GoCloneReq (0x005459C9)
// on the server so no field is ever delta-elided. This is 100% wire-compatible with stock/older clients:
// every packed RPC carries full m_RegionId, m_PlayerId, m_StartingPos, m_StartingRot (and m_CloneSourceGoid /
// m_TemplateName for clones), making GoDb+0x3D8/+0x3E0 delta desync impossible and immediately self-healing
// any stale client entry on the very next packet.
//
// Critical XferPlayerId (0x00545FC1) bit-framing invariant:
//   When prev->m_PlayerId == 0, XferPlayerId omits the 2nd "changed" bit on BOTH packer and unpacker.
//   Therefore we preserve 0 as 0 (packet #1 of a session), and whenever prev->m_PlayerId != 0 we poison it
//   to a non-zero sentinel guaranteed != req->m_PlayerId (0x80000000u or 0x40000000u).
inline void PoisonGoCreateReqPrev(const void* req, void* prev) {
    if (!req || !prev) return;
    const auto* r = static_cast<const uint8_t*>(req);
    auto*       p = static_cast<uint8_t*>(prev);

    uint32_t reqReg = 0;
    std::memcpy(&reqReg, r + 0x04, 4);
    uint32_t poisonReg = reqReg ^ 1u;
    std::memcpy(p + 0x04, &poisonReg, 4);

    uint32_t prevPl = 0;
    std::memcpy(&prevPl, p + 0x08, 4);
    if (prevPl != 0u) {
        uint32_t reqPl = 0;
        std::memcpy(&reqPl, r + 0x08, 4);
        uint32_t poisonPl = (reqPl == 0x80000000u) ? 0x40000000u : 0x80000000u;
        std::memcpy(p + 0x08, &poisonPl, 4);
    }

    uint32_t reqPos0 = 0;
    std::memcpy(&reqPos0, r + 0x10, 4);
    uint32_t poisonPos0 = reqPos0 ^ 0xFFFFFFFFu;
    std::memcpy(p + 0x10, &poisonPos0, 4);

    uint32_t reqRot0 = 0;
    std::memcpy(&reqRot0, r + 0x20, 4);
    uint32_t poisonRot0 = reqRot0 ^ 0xFFFFFFFFu;
    std::memcpy(p + 0x20, &poisonRot0, 4);
}

inline void PoisonGoCloneReqPrev(const void* req, void* prev) {
    if (!req || !prev) return;
    const auto* r = static_cast<const uint8_t*>(req);
    auto*       p = static_cast<uint8_t*>(prev);

    // +0x00 m_CloneSourceGoid (XferGoid3Way 0x005467FE compares against prev+0x00)
    uint32_t reqSrc = 0;
    std::memcpy(&reqSrc, r + 0x00, 4);
    uint32_t poisonSrc = reqSrc ^ 1u;
    std::memcpy(p + 0x00, &poisonSrc, 4);

    // +0x08 m_PlayerId (preserve 0 for XferPlayerId framing; poison any non-zero prev)
    uint32_t prevPl = 0;
    std::memcpy(&prevPl, p + 0x08, 4);
    if (prevPl != 0u) {
        uint32_t reqPl = 0;
        std::memcpy(&reqPl, r + 0x08, 4);
        uint32_t poisonPl = (reqPl == 0x80000000u) ? 0x40000000u : 0x80000000u;
        std::memcpy(p + 0x08, &poisonPl, 4);
    }

    // +0x10 m_StartingPos, +0x20 m_StartingRot
    uint32_t reqPos0 = 0;
    std::memcpy(&reqPos0, r + 0x10, 4);
    uint32_t poisonPos0 = reqPos0 ^ 0xFFFFFFFFu;
    std::memcpy(p + 0x10, &poisonPos0, 4);

    uint32_t reqRot0 = 0;
    std::memcpy(&reqRot0, r + 0x20, 4);
    uint32_t poisonRot0 = reqRot0 ^ 0xFFFFFFFFu;
    std::memcpy(p + 0x20, &poisonRot0, 4);

    // +0x3C const char* m_TemplateName (XferStringDelta 0x00659268 sends bit1=1, bit2=1 + full
    // string when *prevPtr == NULL; GoCloneReqFull::copy at 0x00545D81 safely manages prev+0x40 gpstring)
    const char* nullPtr = nullptr;
    std::memcpy(p + 0x3C, &nullPtr, sizeof(nullPtr));
}

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

// Pure invariants for GoDb+0x300 (m_bFullXfer) leak recovery around GoDb::CloneGoImpl (0x0054A7BA)
// and top-level CreateGo_Client / CloneGo_Client entry points.
inline bool ShouldClearLeakedFullXferFlag(uint8_t pre300, uint8_t post300) {
    return pre300 == 0u && post300 != 0u;
}

inline bool ShouldClearStuckFullXferAtIdle(uint32_t depth308, uint8_t flag300) {
    return depth308 == 0u && flag300 != 0u;
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
        uint32_t lastHeroNode;
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
    bool RecordMissing(uint32_t dpnid, uint32_t goid, uint32_t heroNode, uint32_t nowMs) {
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
            ne.lastHeroNode      = heroNode;
            ne.missingReports    = 1;
            ne.resyncCount       = 0;
            entries_.push_back(ne);
            return false;
        }

        e->lastSeenMissingMs = nowMs;
        if (e->missingReports < 0xFFFFu) ++e->missingReports;

        if (e->resyncCount >= kMaxResyncAttempts) {
            // If the player moved to a different terrain node (e.g. approaching the door
            // after earlier attempts ran while far away), grant fresh resync attempts
            // once the normal cooldown has elapsed.
            if (heroNode != 0u && heroNode != e->lastHeroNode &&
                (nowMs - e->lastResyncMs) >= kResyncCooldownMs) {
                e->resyncCount  = 0;
                e->lastHeroNode = heroNode;
            } else {
                return false;
            }
        }
        if (e->missingReports < 2u) return false;
        if ((nowMs - e->firstMissingMs) < kMissingGraceMs) return false;
        if (e->resyncCount > 0 && (nowMs - e->lastResyncMs) < kResyncCooldownMs) return false;

        e->lastResyncMs = nowMs;
        e->lastHeroNode = heroNode;
        ++e->resyncCount;
        return true;
    }

    bool RecordMissing(uint32_t dpnid, uint32_t goid, uint32_t nowMs) {
        return RecordMissing(dpnid, goid, 0u, nowMs);
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
