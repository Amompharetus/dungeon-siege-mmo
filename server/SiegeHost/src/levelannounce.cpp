// levelannounce.cpp — 1.5.41: server-side real-time Uber Level watcher.
//
// Polls online, verified human heroes once per second on the server main thread, reads each hero's
// natural (unbuffed) "Uber" skill level directly from GoActor's skill vector (+0x30), and pushes
// genuine level-ups asynchronously on a background worker thread to SiegePortal POST /api/shard/levelup.
// Completely dormant on clients (Role::Client / Role::None).
#include "levelannounce.h"
#include "levelannounce_logic.h"
#include "siegehost.h"
#include "engine.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

bool MemReadable(const void* p, size_t n) {
    if (!p || (uintptr_t)p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

bool IsWorldInGame() {
    void* wsObj = *(void**)0x007ACF44;
    if (!MemReadable(wsObj, 0x24)) return false;
    int ws = *(int*)((const char*)wsObj + 0x20);
    return ws == eng::WS_MP_INGAME || ws == eng::WS_MP_INGAME_JIP;
}

std::string ReadHeroNameUtf8(void* pl) {
    if (!MemReadable((const char*)pl + 0x58, sizeof(void*))) return "";
    const wchar_t* w = *(const wchar_t**)((const char*)pl + 0x58);
    if (!MemReadable(w, sizeof(wchar_t))) return "";
    char name[128] = "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, name, (int)sizeof(name) - 1, nullptr, nullptr);
    if (n <= 1) return "";
    name[sizeof(name) - 1] = '\0';
    return std::string(name);
}

// Reads the hero's natural (unbuffed, pure XP-derived) "Uber" skill level from GoActor's skill table.
// Verified in RE-04-join-guard.md: GoActor+0x18/0x1C vector of 0x48-byte Skill records, +0x00 name, +0x30 float natural level.
bool ReadHeroNaturalUberLevel(void* heroGo, int& outLevel) {
    outLevel = 0;
    if (!MemReadable(heroGo, 0x98)) return false;
    if ((*((const unsigned char*)heroGo + 0x96) & 8) != 0) return false; // marked for deletion
    void* actor = *(void**)heroGo;
    if (!MemReadable(actor, 0x24)) return false;

    char* sb = *(char**)((char*)actor + 0x18);
    char* se = *(char**)((char*)actor + 0x1C);
    if (!sb || !se || se < sb || (size_t)(se - sb) > 0x1000) return false;

    for (char* s = sb; s + 0x38 <= se; s += 0x48) {
        if (!MemReadable(s, 0x38)) break;
        const char* nm = *(const char**)s;
        if (!nm || !MemReadable(nm, 5)) continue;
        if (_stricmp(nm, "Uber") == 0) {
            float lvl = *(const float*)(s + 0x30);
            if (!(lvl >= 0.0f && lvl <= (float)levelannounce::kMaxValidLevel)) return false;
            outLevel = (int)floorf(lvl);
            return true;
        }
    }
    return false;
}

bool PostLevelUpHttp(const levelannounce::LevelUpEvent& ev, int& outStatus) {
    outStatus = 0;
    const Config& cfg = Cfg();
    if (cfg.portalHubUrl.empty() || cfg.portalShardId.empty() || cfg.portalShardKey.empty()) {
        return false;
    }
    std::string host, port;
    if (!levelannounce::ParsePortalUrl(cfg.portalHubUrl, host, port)) {
        return false;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        Log("LevelAnnounce: getaddrinfo failed for %s:%s", host.c_str(), port.c_str());
        return false;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        Log("LevelAnnounce: connect failed to %s:%s", host.c_str(), port.c_str());
        closesocket(s);
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);

    std::string body = levelannounce::BuildLevelUpJson(ev);
    std::string req  = "POST /api/shard/levelup HTTP/1.1\r\n"
                       "Host: " + host + ":" + port + "\r\n"
                       "X-Shard-Id: " + cfg.portalShardId + "\r\n"
                       "X-Shard-Key: " + cfg.portalShardKey + "\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(body.size()) + "\r\n"
                       "Connection: close\r\n\r\n" + body;

    size_t totalSent = 0;
    while (totalSent < req.size()) {
        int n = send(s, req.data() + totalSent, (int)(req.size() - totalSent), 0);
        if (n <= 0) {
            closesocket(s);
            return false;
        }
        totalSent += (size_t)n;
    }

    std::string resp;
    char buf[1024];
    int n = 0;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
        resp.append(buf, (size_t)n);
        if (resp.size() >= 4096) break;
    }
    closesocket(s);

    if (resp.size() < 12) return false;
    size_t sp = resp.find(' ');
    if (sp == std::string::npos || sp + 3 >= resp.size()) return false;
    outStatus = atoi(resp.c_str() + sp + 1);
    return true;
}

CRITICAL_SECTION g_cs;
bool             g_csInit   = false;
HANDLE           g_event    = nullptr;
HANDLE           g_thread   = nullptr;
bool             g_enabled  = false;
std::deque<levelannounce::LevelUpEvent> g_queue;
levelannounce::LevelTracker             g_tracker;

DWORD WINAPI WorkerThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_event, 2000);
        for (;;) {
            levelannounce::LevelUpEvent ev;
            bool hasItem = false;
            EnterCriticalSection(&g_cs);
            if (!g_queue.empty()) {
                ev = g_queue.front();
                g_queue.pop_front();
                hasItem = true;
            }
            LeaveCriticalSection(&g_cs);
            if (!hasItem) break;

            int status = 0;
            bool ok = PostLevelUpHttp(ev, status);
            if (ok && status == 200) {
                Log("LevelAnnounce: reported '%s' (account '%s') Uber Level %d -> portal HTTP 200",
                    ev.character.c_str(), ev.account.c_str(), ev.newLevel);
            } else {
                Log("LevelAnnounce: failed reporting '%s' (account '%s') Uber Level %d (ok=%d status=%d)",
                    ev.character.c_str(), ev.account.c_str(), ev.newLevel, (int)ok, status);
            }
        }
    }
    return 0;
}

void EnqueueEvent(const levelannounce::LevelUpEvent& ev) {
    if (!g_csInit || !g_event) return;
    EnterCriticalSection(&g_cs);
    if (g_queue.size() < 64) {
        g_queue.push_back(ev);
    }
    LeaveCriticalSection(&g_cs);
    SetEvent(g_event);
}

} // namespace

void LevelAnnounce_Init() {
    if (Cfg().role != Role::Server) return;
    if (Cfg().portalHubUrl.empty() || Cfg().portalShardId.empty() || Cfg().portalShardKey.empty()) {
        Log("LevelAnnounce: disabled (Portal HubUrl/ShardId/ShardKey not configured).");
        return;
    }
    std::string host, port;
    if (!levelannounce::ParsePortalUrl(Cfg().portalHubUrl, host, port)) {
        Log("LevelAnnounce: disabled (unsupported Portal HubUrl '%s').", Cfg().portalHubUrl.c_str());
        return;
    }

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    if (!g_event) {
        g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    if (!g_thread && g_event) {
        g_thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    }
    g_tracker.Reset();
    g_enabled = (g_thread != nullptr);
    if (g_enabled) {
        Log("LevelAnnounce: enabled (server-only, posting Uber Level ups to %s:%s as shard '%s').",
            host.c_str(), port.c_str(), Cfg().portalShardId.c_str());
    }
}

void LevelAnnounce_Tick() {
    if (!g_enabled || Cfg().role != Role::Server) return;

    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (lastTick != 0 && (DWORD)(now - lastTick) < 1000) return;
    lastTick = now;

    if (!IsWorldInGame()) return;

    void* srv = eng::Server_Singleton();
    if (!MemReadable(srv, 0x20)) return;
    void* localPl = eng::Server_GetLocalHumanPlayer(srv);

    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    if (!b || !e || e < b || (size_t)(e - b) > 64) return;

    const std::string& hostName = Cfg().hostCharacter;
    std::vector<levelannounce::ObservedPlayer> online;
    online.reserve(32);

    for (void** it = b; it < e; ++it) {
        if (!MemReadable(it, sizeof(void*))) break;
        void* pl = *it;
        if (!pl || pl == localPl || !MemReadable(pl, 0xD0)) continue;
        if (!eng::Player_IsValid(pl)) continue;
        if (*(const int*)((const char*)pl + 0x14) != 1) continue; // humans only

        DWORD mach = *(const DWORD*)((const char*)pl + 0x10);
        if (mach == 0) continue;
        if (!JG_IsDpnidVerified(mach)) continue;

        std::string heroName = ReadHeroNameUtf8(pl);
        if (heroName.empty()) continue;
        if (!hostName.empty() && _stricmp(heroName.c_str(), hostName.c_str()) == 0) continue;

        void** partyBegin = *(void***)((char*)pl + 0xAC);
        void** partyEnd   = *(void***)((char*)pl + 0xB0);
        if (!partyBegin || !partyEnd || partyBegin >= partyEnd) continue;
        if (!MemReadable(partyBegin, sizeof(void*))) continue;
        void* party = *partyBegin;
        if (!MemReadable(party, 0x90)) continue;

        void** cb = *(void***)((char*)party + 0x88);
        void** ce = *(void***)((char*)party + 0x8C);
        if (!cb || !ce || cb >= ce || !MemReadable(cb, sizeof(void*))) continue;
        void* heroGo = *cb;
        if (!MemReadable(heroGo, 0x98)) continue;

        uint32_t heroGoid = *(const uint32_t*)((const char*)heroGo + 0x4C);
        if (heroGoid == 0) continue;

        int uberLevel = 0;
        if (!ReadHeroNaturalUberLevel(heroGo, uberLevel)) continue;

        levelannounce::ObservedPlayer obs;
        obs.dpnid     = mach;
        obs.heroGoid  = heroGoid;
        obs.account   = JG_GetAccountForDpnid(mach);
        obs.character = heroName;
        obs.uberLevel = uberLevel;
        online.push_back(obs);
    }

    auto events = g_tracker.Update(online, (uint32_t)now);
    for (const auto& ev : events) {
        Log("LevelAnnounce: detected '%s' (account '%s') reached Uber Level %d (was %d)",
            ev.character.c_str(), ev.account.c_str(), ev.newLevel, ev.oldLevel);
        EnqueueEvent(ev);
    }
}
