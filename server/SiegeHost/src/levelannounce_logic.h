#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace levelannounce {

constexpr uint32_t kDefaultSettleMs = 5000;  // 5s silent baseline window after hero first seen (plus JoinGuard's 10s when active)
constexpr int      kMaxJumpPerTick  = 5;     // sanity cap against uninitialized/corrupted memory spikes
constexpr int      kMaxValidLevel   = 200;

struct ObservedPlayer {
    uint32_t    dpnid     = 0;
    uint32_t    heroGoid  = 0;
    std::string account;
    std::string character;
    int         uberLevel = 0;
};

struct LevelUpEvent {
    std::string account;
    std::string character;
    int         oldLevel = 0;
    int         newLevel = 0;
};

struct TrackedPlayer {
    uint32_t    heroGoid    = 0;
    std::string account;
    std::string character;
    uint32_t    firstSeenMs = 0;
    bool        settled     = false;
    int         lastLevel   = 0;
};

// Pure state machine that tracks online human heroes and emits LevelUpEvent strictly on
// real in-game Uber Level increases (never on join/load, never on regressions, never on garbage spikes).
class LevelTracker {
public:
    explicit LevelTracker(uint32_t settleMs = kDefaultSettleMs) : settleMs_(settleMs) {}

    std::vector<LevelUpEvent> Update(const std::vector<ObservedPlayer>& online, uint32_t nowMs) {
        std::vector<LevelUpEvent> events;
        std::map<uint32_t, bool> seen;

        for (const auto& obs : online) {
            if (obs.dpnid == 0 || obs.heroGoid == 0 || obs.character.empty()) continue;
            if (obs.uberLevel < 0 || obs.uberLevel > kMaxValidLevel) continue;
            seen[obs.dpnid] = true;

            auto it = tracked_.find(obs.dpnid);
            if (it == tracked_.end() ||
                it->second.heroGoid != obs.heroGoid ||
                it->second.character != obs.character) {
                TrackedPlayer tp;
                tp.heroGoid    = obs.heroGoid;
                tp.account     = obs.account;
                tp.character   = obs.character;
                tp.firstSeenMs = nowMs;
                tp.settled     = (settleMs_ == 0);
                tp.lastLevel   = obs.uberLevel;
                tracked_[obs.dpnid] = tp;
                continue;
            }

            TrackedPlayer& tp = it->second;
            if (!obs.account.empty()) {
                tp.account = obs.account;
            }

            if (!tp.settled) {
                if (obs.uberLevel > tp.lastLevel) {
                    tp.lastLevel = obs.uberLevel;
                }
                if ((uint32_t)(nowMs - tp.firstSeenMs) >= settleMs_) {
                    tp.settled = true;
                }
                continue;
            }

            if (obs.uberLevel > tp.lastLevel) {
                int delta = obs.uberLevel - tp.lastLevel;
                int prev  = tp.lastLevel;
                tp.lastLevel = obs.uberLevel;
                if (delta <= kMaxJumpPerTick && obs.uberLevel >= 1) {
                    LevelUpEvent ev;
                    ev.account   = tp.account;
                    ev.character = tp.character;
                    ev.oldLevel  = prev;
                    ev.newLevel  = obs.uberLevel;
                    events.push_back(ev);
                }
            }
        }

        for (auto it = tracked_.begin(); it != tracked_.end(); ) {
            if (seen.find(it->first) == seen.end()) {
                it = tracked_.erase(it);
            } else {
                ++it;
            }
        }

        return events;
    }

    size_t TrackedCount() const { return tracked_.size(); }
    void Reset() { tracked_.clear(); }

private:
    uint32_t settleMs_;
    std::map<uint32_t, TrackedPlayer> tracked_;
};

inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

inline std::string BuildLevelUpJson(const LevelUpEvent& ev) {
    return "{\"account\":\"" + JsonEscape(ev.account)
         + "\",\"character\":\"" + JsonEscape(ev.character)
         + "\",\"level\":" + std::to_string(ev.newLevel)
         + ",\"old_level\":" + std::to_string(ev.oldLevel) + "}";
}

inline bool ParsePortalUrl(const std::string& url, std::string& host, std::string& port) {
    host.clear();
    port.clear();
    if (url.empty()) return false;
    std::string s = url;
    if (s.rfind("http://", 0) == 0) {
        s = s.substr(7);
    } else if (s.rfind("https://", 0) == 0) {
        return false; // raw WinSock HTTP only (matches SharedStash / SiegeServer HubUrl)
    }
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        s = s.substr(0, slash);
    }
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        host = s.substr(0, colon);
        port = s.substr(colon + 1);
    } else {
        host = s;
        port = "80";
    }
    return !host.empty() && !port.empty();
}

} // namespace levelannounce
