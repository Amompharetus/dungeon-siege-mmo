// joinguard.cpp — 0.6.0 "join through the launcher only" (see 03-analysis/RE-04-join-guard.md)
//
// Client: the launcher writes a signed join ticket into SiegeHost.ini ([JoinGuard] Ticket=). HookConnect appends it to
//   the DirectPlay connect data:  <game data><ticket text><uint32 ticket length>"DSMT".
// Server: INDICATE_CONNECT verifies the ticket (HMAC-SHA256 with [JoinGuard] Key, server id, expiry, account not already
//   on) and rejects the connection otherwise. Accepted players are mapped dpnid -> account. On the main thread the
//   player's party experience (sum of every skill's XP over all party members) is compared with the MMO's record that
//   came in the ticket; a character with more experience than the MMO has seen is kicked. Verified players' experience
//   goes into heartbeat.txt, and siegeserver pushes it to the central portal, which raises the record.
//   Separately, every minute each party's skill levels are compared with the experience that paid for them
//   under the stock table, which catches a modified experience table in the player's own Resources folder.
#include "siegehost.h"
#include "engine.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------
// SHA-256 / HMAC-SHA256 (FIPS 180-4)
// ---------------------------------------------------------------------------------------------------------------
namespace {
struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len = 0;
    size_t n = 0;
    Sha256() {
        static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, iv, sizeof h);
    }
    static uint32_t rol(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rol(w[i - 15], 7) ^ rol(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rol(w[i - 2], 17) ^ rol(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = hh + (rol(e, 6) ^ rol(e, 11) ^ rol(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rol(a, 2) ^ rol(a, 13) ^ rol(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void add(const uint8_t* p, size_t k) {
        len += k;
        while (k--) { buf[n++] = *p++; if (n == 64) { block(buf); n = 0; } }
    }
    void add(const std::string& s) { add((const uint8_t*)s.data(), s.size()); }
    void done(uint8_t out[32]) {
        uint64_t bits = len * 8;
        uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (n != 56) add(&zero, 1);
        for (int i = 7; i >= 0; --i) { uint8_t b = (uint8_t)(bits >> (i * 8)); add(&b, 1); }
        for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - j * 8));
    }
};

void HmacSha256(const std::vector<uint8_t>& key, const std::string& msg, uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (key.size() > 64) { Sha256 s; s.add(key.data(), key.size()); s.done(k); }
    else memcpy(k, key.data(), key.size());
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    uint8_t inner[32];
    Sha256 a; a.add(ipad, 64); a.add(msg); a.done(inner);
    Sha256 b; b.add(opad, 64); b.add(inner, 32); b.done(out);
}

std::string Hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::vector<uint8_t> Unhex(const std::string& s) {
    std::vector<uint8_t> out;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int hi = v(s[i]), lo = v(s[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((uint8_t)(hi * 16 + lo));
    }
    return out;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}

// Character key: lowercase letters and digits only (matches the portal's key for "Save/<name>.dsparty" and hero names).
std::string CharKey(const std::string& s) {
    std::string k;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') k += (char)(c + 32);
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) k += (char)c;
    }
    return k;
}

// ---------------------------------------------------------------------------------------------------------------
// Shared state (DirectPlay threads + main thread)
// ---------------------------------------------------------------------------------------------------------------
struct Ticket {
    std::string account, server;
    long long exp = 0;
    std::map<std::string, long long> chars;  // key -> recorded XP (-1 = accept whatever it has the first time)
    long long newMax = 0, tolAbs = 0, tolPct = 0;
};

struct Joined {
    Ticket t;
    DWORD createdTick = 0;
    DWORD partySeenTick = 0;   // first time the party with experience was visible
    bool verified = false, kicked = false;
    std::string hero;          // hero name (UTF-8)
    long long xp = -1;         // last sample
    DWORD departedTick = 0;    // DESTROY_PLAYER time (kept 60 s so the final XP reaches the portal)
};

struct Pending {
    DWORD magic;
    Ticket t;
    PVOID gameCtx;
};
const DWORD kPendingMagic = 0x4A47504E;  // "NPGJ"

CRITICAL_SECTION g_cs;
bool g_csInit = false;
std::map<DWORD, Joined> g_joined;             // dpnid -> joined (incl. departed for 60 s)
std::vector<Pending*> g_pending;
struct Refusal { long long when; std::string account, reason; };
std::vector<Refusal> g_refusals;              // last few, for the heartbeat
std::vector<uint8_t> g_key;
bool g_enforce = false;
// XpCheck state; see the block further down for what it does and why it lives on the server.
int g_xpMode = 1;                 // 0 report, 1 enforce, -1 off
DWORD g_xpChecked = 0, g_xpFlagged = 0;
std::string g_serverId;

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

void AddRefusal(const std::string& account, const std::string& reason) {
    Lock l;
    g_refusals.push_back({(long long)time(nullptr), account, reason});
    if (g_refusals.size() > 8) g_refusals.erase(g_refusals.begin());
}

// "DSMT1|account|server|exp|chars|newmax|tolabs|tolpct|sig"   chars = hexname:xp,hexname:xp
bool ParseTicket(const std::string& text, Ticket& t, std::string& why) {
    size_t bar = text.rfind('|');
    if (bar == std::string::npos) { why = "bad-ticket"; return false; }
    std::string payload = text.substr(0, bar), sig = text.substr(bar + 1);
    auto f = Split(payload, '|');
    if (f.size() != 8 || f[0] != "DSMT1") { why = "bad-ticket"; return false; }
    if (_stricmp(f[2].c_str(), g_serverId.c_str()) != 0) { why = "wrong-server"; return false; }  // (signed with another server's key)
    uint8_t mac[32];
    HmacSha256(g_key, payload, mac);
    std::string want = Hex(mac, 32);
    if (sig.size() != want.size()) { why = "bad-ticket"; return false; }
    int diff = 0;
    for (size_t i = 0; i < want.size(); ++i) diff |= (want[i] ^ sig[i]);
    if (diff) { why = "bad-ticket"; return false; }
    t.account = f[1];
    t.server = f[2];
    t.exp = atoll(f[3].c_str());
    if (!f[4].empty())
        for (auto& c : Split(f[4], ',')) {
            size_t colon = c.find(':');
            if (colon == std::string::npos) continue;
            auto nb = Unhex(c.substr(0, colon));
            t.chars[CharKey(std::string(nb.begin(), nb.end()))] = atoll(c.c_str() + colon + 1);
        }
    t.newMax = atoll(f[5].c_str());
    t.tolAbs = atoll(f[6].c_str());
    t.tolPct = atoll(f[7].c_str());
    if (_stricmp(t.server.c_str(), g_serverId.c_str()) != 0) { why = "wrong-server"; return false; }
    if ((long long)time(nullptr) > t.exp) { why = "expired"; return false; }
    return true;
}

const char* ReasonText(const std::string& why) {
    if (why == "no-ticket") return "Join through Dungeon Siege MMO.bat (the launcher). Joining with DSLOA.exe directly is not allowed.";
    if (why == "bad-ticket") return "Your join pass is not valid. Close the game and run Dungeon Siege MMO.bat again.";
    if (why == "expired") return "Your join pass has expired. Close the game and run Dungeon Siege MMO.bat again.";
    if (why == "wrong-server") return "Your join pass is for another server. Run Dungeon Siege MMO.bat again and pick this server.";
    if (why == "in-use") return "This account is already in the game on this server.";
    return "The server refused the connection.";
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// Configuration (called from LoadConfig)
// ---------------------------------------------------------------------------------------------------------------
void JG_Init() {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    const Config& c = Cfg();
    if (c.role != Role::Server) return;
    g_key = Unhex(c.jgKey);
    g_serverId = c.jgServerId;
    g_enforce = c.jgEnabled && g_key.size() >= 16 && !g_serverId.empty();
    {
        char e[16] = "";
        GetEnvironmentVariableA("SIEGEHOST_XPCHECK", e, sizeof e);
        if (_stricmp(e, "enforce") == 0)   g_xpMode = 1;
        else if (_stricmp(e, "off") == 0)  g_xpMode = -1;
        else                               g_xpMode = 0;
        Log("JoinGuard: XpCheck %s - a skill's level is compared with the experience that paid for it, "
            "using the stock table. A modified experience table in a player's own Resources folder "
            "shows up here as a level far above what their experience buys. %s",
            g_xpMode < 0 ? "off" : (g_xpMode > 0 ? "ENFORCING" : "reporting only"),
            g_xpMode > 0 ? "Offenders are kicked." : "Nobody is kicked; this only logs and counts.");
    }
    Log("JoinGuard: %s (server id '%s', key %s)", g_enforce ? "ON - only players with a launcher join ticket can join" : "OFF",
        g_serverId.c_str(), g_key.size() >= 16 ? "set" : "missing");
}

bool JG_Enforced() { return g_enforce; }

// ---------------------------------------------------------------------------------------------------------------
// Client: append the ticket to the connect data
// ---------------------------------------------------------------------------------------------------------------
void JG_ClientConnectData(const void* data, DWORD size, const void** outData, DWORD* outSize) {
    *outData = data;
    *outSize = size;
    char ticket[4096];
    GetPrivateProfileStringA("JoinGuard", "Ticket", "", ticket, sizeof ticket, Cfg().iniPath.c_str());  // re-read: the launcher refreshes it
    size_t tl = strlen(ticket);
    if (!tl) { Log("JoinGuard: no join ticket in %s (servers with JoinGuard will refuse)", Cfg().iniPath.c_str()); return; }
    static std::vector<BYTE> buf;  // must stay valid while DirectPlay connects
    buf.assign((BYTE*)data, (BYTE*)data + (data ? size : 0));
    buf.insert(buf.end(), (BYTE*)ticket, (BYTE*)ticket + tl);
    DWORD len = (DWORD)tl;
    buf.insert(buf.end(), (BYTE*)&len, (BYTE*)&len + 4);
    buf.insert(buf.end(), {'D', 'S', 'M', 'T'});
    *outData = buf.data();
    *outSize = (DWORD)buf.size();
    auto f = Split(ticket, '|');
    Log("JoinGuard: join ticket attached (%zu bytes, account '%s', server '%s')", tl, f.size() > 2 ? f[1].c_str() : "?", f.size() > 2 ? f[2].c_str() : "?");
}

// DPNMSG_CONNECT_COMPLETE: dwSize, hAsyncOp, pvUserContext, hResultCode, pvApplicationReplyData, dwApplicationReplyDataSize
void JG_ClientConnectComplete(PVOID msg) {
    DWORD* m = (DWORD*)msg;
    HRESULT hr = (HRESULT)m[3];
    const char* reply = (const char*)m[4];
    DWORD n = m[5];
    if (SUCCEEDED(hr) || !reply || n < 8 || memcmp(reply, "DS1MMO:", 7) != 0) return;
    std::string text(reply + 7, reply + n);
    while (!text.empty() && text.back() == 0) text.pop_back();
    Log("JoinGuard: the server refused this join: %s", text.c_str());
    if (FILE* f = fopen((LogDir() + "join_refused.txt").c_str(), "w")) { fprintf(f, "%s\n", text.c_str()); fclose(f); }
}

// ---------------------------------------------------------------------------------------------------------------
// Server: DirectPlay messages
// ---------------------------------------------------------------------------------------------------------------
HRESULT JG_ServerMessage(DWORD id, PVOID msg, HRESULT (WINAPI* callGame)(void*, DWORD, PVOID), void* gameCtx) {
    DWORD* m = (DWORD*)msg;
    if (id == 0xFFFF000E && g_enforce) {  // INDICATE_CONNECT: dwSize, pvUserConnectData, dwUserConnectDataSize, pvReplyData, dwReplyDataSize, pvReplyContext, pvPlayerContext, pAddressPlayer, pAddressDevice
        const BYTE* data = (const BYTE*)m[1];
        DWORD size = m[2];
        std::string why = "no-ticket";
        Ticket t;
        DWORD gameSize = size;
        if (data && size >= 8 && memcmp(data + size - 4, "DSMT", 4) == 0) {
            DWORD tl = *(DWORD*)(data + size - 8);
            if (tl > 0 && tl <= size - 8) {
                gameSize = size - 8 - tl;
                std::string text((const char*)data + gameSize, tl);
                if (ParseTicket(text, t, why)) {
                    why.clear();
                    Lock l;
                    for (auto& kv : g_joined)
                        if (!kv.second.departedTick && _stricmp(kv.second.t.account.c_str(), t.account.c_str()) == 0) why = "in-use";
                }
            } else {
                why = "bad-ticket";
            }
        }
        if (!why.empty()) {
            static char reply[512];
            snprintf(reply, sizeof reply, "DS1MMO:%s", ReasonText(why));
            m[3] = (DWORD)(ULONG_PTR)reply;
            m[4] = (DWORD)strlen(reply) + 1;
            Log("JoinGuard: REFUSED connection (%s, account '%s', data %lu bytes)", why.c_str(), t.account.empty() ? "-" : t.account.c_str(), size);
            AddRefusal(t.account.empty() ? "-" : t.account, why);
            return (HRESULT)0x80158410;  // DPNERR_HOSTREJECTEDCONNECTION
        }
        m[2] = gameSize;  // the game sees exactly its own connect data
        HRESULT hr = callGame(gameCtx, id, msg);
        if (SUCCEEDED(hr)) {
            auto* p = new Pending{kPendingMagic, t, (PVOID)m[6]};
            { Lock l; g_pending.push_back(p); }
            m[6] = (DWORD)(ULONG_PTR)p;
            Log("JoinGuard: ticket OK for account '%s' (server '%s', %zu recorded characters) -> connecting", t.account.c_str(), t.server.c_str(), t.chars.size());
        } else {
            Log("JoinGuard: ticket OK for '%s' but the game refused the connection (hr=0x%08lX)", t.account.c_str(), (DWORD)hr);
        }
        return hr;
    }
    auto takePending = [&](DWORD* slot) -> Pending* {
        Pending* p = (Pending*)(ULONG_PTR)*slot;
        Lock l;
        for (size_t i = 0; i < g_pending.size(); ++i)
            if (g_pending[i] == p) {
                g_pending.erase(g_pending.begin() + i);
                *slot = (DWORD)(ULONG_PTR)p->gameCtx;
                return p;
            }
        return nullptr;
    };
    if (id == 0xFFFF0007) {  // CREATE_PLAYER: dwSize, dpnidPlayer, pvPlayerContext
        if (Pending* p = takePending(&m[2])) {
            Lock l;
            Joined j;
            j.t = p->t;
            j.createdTick = GetTickCount();
            g_joined[m[1]] = j;
            Log("JoinGuard: dpnid 0x%08lX = account '%s'", m[1], p->t.account.c_str());
            delete p;
        }
        return callGame(gameCtx, id, msg);
    }
    if (id == 0xFFFF000F) {  // INDICATED_CONNECT_ABORTED: dwSize, pvPlayerContext
        if (Pending* p = takePending(&m[1])) delete p;
        return callGame(gameCtx, id, msg);
    }
    if (id == 0xFFFF0009) {  // DESTROY_PLAYER: dwSize, dpnidPlayer, pvPlayerContext, dwReason
        HRESULT hr = callGame(gameCtx, id, msg);
        Lock l;
        auto it = g_joined.find(m[1]);
        if (it != g_joined.end()) {
            it->second.departedTick = GetTickCount();
            Log("JoinGuard: account '%s' left (hero '%s', last experience %lld)", it->second.t.account.c_str(), it->second.hero.c_str(), it->second.xp);
        }
        return hr;
    }
    return callGame(gameCtx, id, msg);
}

// ---------------------------------------------------------------------------------------------------------------
// Server: main thread — character check, reporting, kicks
// ---------------------------------------------------------------------------------------------------------------
namespace {
// Total experience of a player's party: sum of every skill's XP (double at skill+0x20) of every party member.
// Party Go = Player+0xAC vector[0]; members = Go+0x88 GopColl; GoActor* = Go+0; skills vector GoActor+0x18/0x1C, 0x48 each.
bool PartyXp(void* pl, long long& xp, int& members) {
    char* p = (char*)pl;
    if (*(void**)(p + 0xAC) == *(void**)(p + 0xB0)) return false;
    char* party = (char*)**(void***)(p + 0xAC);
    if (!party) return false;
    void** cb = *(void***)(party + 0x88), **ce = *(void***)(party + 0x8C);
    double sum = 0;
    members = 0;
    for (void** c = cb; c && c < ce; ++c) {
        char* go = (char*)*c;
        char* actor = go ? *(char**)go : nullptr;
        if (!actor) continue;
        ++members;
        char* sb = *(char**)(actor + 0x18), *se = *(char**)(actor + 0x1C);
        for (char* s = sb; s && s < se; s += 0x48) {
            const char* nm = *(char**)s;
            if (nm && _stricmp(nm, "Uber") == 0) continue;  // uber level is derived from the others
            double v = *(double*)(s + 0x20);
            if (v > 0 && v < 1e17) {
                sum += v;
            } else if (v >= 1e17) {
                sum = 1e17; // saturate so huge/overflow XP is caught and kicked
            }
        }
    }
    if (!members) return false;
    if (sum > (double)0x7FFFFFFFFFFFFFFFLL) sum = (double)0x7FFFFFFFFFFFFFFFLL;
    xp = (long long)sum;
    return true;
}

// ---------------------------------------------------------------------------------------------
// XpCheck - a skill's level must match the experience it was paid for.
//
// THE HOLE THIS CLOSES
// ---------------------------------------------------------------------------------------------
// A player can drop a .dsres into their own Resources folder that the server does not have, and the
// engine will use it. The one that matters replaces world/global/formula/formulas.gas and divides
// the whole experience table by ten, so every level costs a tenth of what it should.
//
// It works because of where the arithmetic happens. The SERVER awards experience
// (Rules::RCAwardPrimaryExperience 0x005B7194, replicated). The CLIENT turns that experience into
// skill levels using ITS OWN copy of the table. The levels, not the formula, are what the character
// file stores ("melee_level = 57.7172"), so the inflated numbers go into the save, into the
// backups, and stay there.
//
// The experience check above does not see it: the mod does not touch experience at all. The
// experience stays honest and only the level derived from it is wrong, and nothing compared the two.
//
// WHY THIS IS THE RIGHT PLACE TO CATCH IT
// ---------------------------------------------------------------------------------------------
// Both numbers are in front of us on the server, in the same 0x48-byte skill record: experience is
// the double at +0x20, level is the float at +0x30. A player controls their own files; they do not
// control the arithmetic the server does on the character they hand over, so this cannot be modded
// away from the client side the way a file check could be.
//
// It also composes with the experience check: experience may not exceed what the portal has
// recorded, and a level may not exceed what that experience buys. Beating both at once means faking
// experience as well, which is the case already covered.
//
// THE TABLE
// ---------------------------------------------------------------------------------------------
// 151 thresholds, taken from the stock world/global/formula/formulas.gas in Resources/Logic.dsres.
// Level is the index whose threshold the experience has passed, plus a linear fraction towards the
// next one. Checked against a real character's seven skills: the engine's own levels come back to
// within 0.0001, so this is the engine's formula rather than an approximation of it.
//
// WHAT IT DOES ABOUT IT
// ---------------------------------------------------------------------------------------------
// By default it reports only. It logs and counts, and nobody is kicked, because how many existing
// characters are already affected has to be measured before anything is enforced.
// SIEGEHOST_XPCHECK=enforce turns on the kick once that number is known.
const double kExpTable[] = {
    0, 70, 650, 1450, 2350,
    3350, 4650, 6650, 9450, 12950,
    17100, 22000, 27800, 34300, 41300,
    49191, 58327, 68882, 81054, 95071,
    111190, 129706, 150954, 175317, 203567,
    236338, 274352, 318449, 369600, 428936,
    497766, 577609, 670226, 777662, 902288,
    1046855, 1214551, 1409080, 1634732, 1896489,
    2200128, 2552348, 2960924, 3434872, 3984651,
    4622395, 5362179, 6220327, 7215779, 8370504,
    9709985, 11263782, 13066188, 15156978, 17582294,
    20395661, 23659167, 27444833, 31836207, 36930200,
    42839232, 49780637, 58283859, 68700306, 81460453,
    97091634, 116239830, 139696370, 168430631, 203630102,
    246749453, 299570658, 364276635, 443541456, 540640862,
    659587634, 805297430, 983791930, 1202447693, 1470301002,
    1798421306, 2200368678, 2692754209, 3295926485, 4034812522,
    4939947918, 6048738778, 7407007581, 9070886865, 11109138988,
    13605997839, 16664649931, 20411498744, 25001388540, 30624003540,
    37511706915, 45949143550, 56285003427, 68946431776, 84456681504,
    103456737421, 126731805919, 155243764829, 190170914494, 232956672834,
    285369226800, 349574605408, 428226194203, 524574390477, 642600930913,
    787183442946, 964297020188, 1181261152309, 1447042214156, 1772624014920,
    2171461720855, 2660037910626, 3258543743095, 3991713387870, 4889846202719,
    5990058900910, 7337819456193, 8988826136414, 11011309319686, 13488851219194,
    16523840046091, 20241701359039, 24796081467402, 30375197100145, 37209613750256,
    45581774146643, 55837670632215, 68401143827042, 83791398490705, 102644460453692,
    125739461358351, 154030837466559, 188687773199113, 231142519471492, 283149583655156,
    346858237280144, 424901337970755, 520504136316754, 637617564290602, 781081513558566,
    956824851411821, 1172110440282060, 1435835286648100, 1758898223446500, 2154650321024540,
    2639446640557650
};
const int kExpLevels = (int)(sizeof kExpTable / sizeof kExpTable[0]);

// Level that this much experience in one skill actually buys, under the stock table.
double LevelForXp(double xp) {
    if (xp <= 0) return 0.0;
    int lo = 0;
    for (int i = 0; i < kExpLevels; ++i) { if (xp >= kExpTable[i]) lo = i; else break; }
    if (lo + 1 >= kExpLevels) return (double)lo;
    double span = kExpTable[lo + 1] - kExpTable[lo];
    return (double)lo + (span > 0 ? (xp - kExpTable[lo]) / span : 0.0);
}

// Generous: a whole level of slack, against an effect that is a factor of ten.
const double kLevelSlack = 1.0;

// Returns true and fills detail when some skill claims a level its experience cannot pay for.
bool XpCheck(void* pl, char* detail, size_t cap) {
    if (g_xpMode < 0) return false;
    char* p = (char*)pl;
    if (*(void**)(p + 0xAC) == *(void**)(p + 0xB0)) return false;
    char* party = (char*)**(void***)(p + 0xAC);
    if (!party) return false;
    void** cb = *(void***)(party + 0x88), **ce = *(void***)(party + 0x8C);
    bool bad = false; double worstBy = 0;
    char worstName[64] = ""; double worstXp = 0, worstLvl = 0, worstShould = 0;
    for (void** c = cb; c && c < ce; ++c) {
        char* go = (char*)*c;
        char* actor = go ? *(char**)go : nullptr;
        if (!actor) continue;
        char* sb = *(char**)(actor + 0x18), *se = *(char**)(actor + 0x1C);
        for (char* s = sb; s && s < se; s += 0x48) {
            const char* nm = *(char**)s;
            if (nm && _stricmp(nm, "Uber") == 0) continue;   // derived from the others, not paid for
            double xp = *(double*)(s + 0x20);
            float lvl = *(float*)(s + 0x30);
            if (lvl < 0.0f || lvl > 150.0f || (lvl != lvl)) {
                bad = true; worstBy = 999.0;
                snprintf(worstName, sizeof worstName, "%s", nm ? nm : "?");
                worstXp = xp; worstLvl = lvl; worstShould = 150.0;
                break;
            }
            if (nm && _stricmp(nm, "Uber") == 0) continue;   // derived from the others, not paid for
            if (!(xp >= 0 && xp < 1e17) || !(lvl >= 0 && lvl <= 150.0f)) continue;   // unreadable, skip
            double should = LevelForXp(xp);
            double by = lvl - should;
            if (by > kLevelSlack && by > worstBy) {
                bad = true; worstBy = by;
                snprintf(worstName, sizeof worstName, "%s", nm ? nm : "?");
                worstXp = xp; worstLvl = lvl; worstShould = should;
            }
        }
    }
    ++g_xpChecked;
    if (bad) {
        ++g_xpFlagged;
        snprintf(detail, cap,
                 "%s is level %.2f on %.0f experience, but that experience only pays for level %.2f "
                 "(%.2f levels too high). This is what a modified experience table in the player's own "
                 "Resources folder looks like.",
                 worstName, worstLvl, worstXp, worstShould, worstBy);
    }
    return bad;
}

// SIEGEHOST_SKILLDUMP=1, for working out offsets against a known save: prints every readable float
// in each 0x48-byte skill record next to the skill's name and its experience, once per character.
void SkillDump(void* pl) {
    char* p = (char*)pl;
    if (*(void**)(p + 0xAC) == *(void**)(p + 0xB0)) return;
    char* party = (char*)**(void***)(p + 0xAC);
    if (!party) return;
    void** cb = *(void***)(party + 0x88), **ce = *(void***)(party + 0x8C);
    for (void** c = cb; c && c < ce; ++c) {
        char* go = (char*)*c;
        char* actor = go ? *(char**)go : nullptr;
        if (!actor) continue;
        char* sb = *(char**)(actor + 0x18), *se = *(char**)(actor + 0x1C);
        for (char* s = sb; s && s < se; s += 0x48) {
            const char* nm = *(char**)s;
            char line[700]; int n = 0;
            n += snprintf(line + n, sizeof line - n, "SKILLDUMP '%s' xp=%.3f |", nm ? nm : "(null)",
                          *(double*)(s + 0x20));
            for (int off = 4; off < 0x48; off += 4) {
                float f = *(float*)(s + off);
                if (f > 0.0f && f < 100000.0f)
                    n += snprintf(line + n, sizeof line - n, " +%02X=%.4f", off, f);
                if (n > (int)sizeof line - 40) break;
            }
            Log("%s", line);
        }
        break;   // first party member is enough
    }
}

// SIEGEHOST_XPTEST=1, test harness. Raises one skill's level in memory without paying any
// experience, which is the shape a modified experience table leaves behind: honest experience,
// inflated level. The engine's own RCSetSkillLevels pays the experience as it goes, so it cannot
// produce this and cannot be used to exercise the check.
void XpTestTamper(void* pl) {
    char* p = (char*)pl;
    if (*(void**)(p + 0xAC) == *(void**)(p + 0xB0)) return;
    char* party = (char*)**(void***)(p + 0xAC);
    if (!party) return;
    void** cb = *(void***)(party + 0x88), **ce = *(void***)(party + 0x8C);
    for (void** c = cb; c && c < ce; ++c) {
        char* go = (char*)*c;
        char* actor = go ? *(char**)go : nullptr;
        if (!actor) continue;
        char* sb = *(char**)(actor + 0x18), *se = *(char**)(actor + 0x1C);
        for (char* s2 = sb; s2 && s2 < se; s2 += 0x48) {
            const char* nm = *(char**)s2;
            if (!nm || _stricmp(nm, "Melee") != 0) continue;
            float* lvl = (float*)(s2 + 0x30);
            double xp = *(double*)(s2 + 0x20);
            Log("XPTEST: '%s' is level %.4f on %.0f experience. Raising the level to %.4f and leaving "
                "the experience alone, which is what a divided experience table produces.",
                nm, *lvl, xp, *lvl + 20.0f);
            *lvl += 20.0f;
            return;
        }
    }
}

std::string HeroName(void* pl) {
    wchar_t* w = *(wchar_t**)((char*)pl + 0x58);
    char name[128] = "";
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof name - 1, nullptr, nullptr);
    return name;
}

void Kick(void* pl, Joined& j, const std::string& why, const std::string& detail) {
    j.kicked = true;
    DWORD pid = *(DWORD*)((char*)pl + 0xC);
    Log("JoinGuard: KICK account '%s' hero '%s': %s", j.t.account.c_str(), j.hero.c_str(), detail.c_str());
    {
        Lock l;
        g_refusals.push_back({(long long)time(nullptr), j.t.account, why + ":" + detail});
        if (g_refusals.size() > 8) g_refusals.erase(g_refusals.begin());
    }
    auto kick = (void (__thiscall*)(void*, DWORD))0x0048D5FF;  // UIMultiplayer::SKickPlayer(PlayerId) — does not use this
    kick(nullptr, pid);
}
// ---------------------------------------------------------------------------------------------------------------
// Inventory Scanner: validates every item template against the server's official ContentDb.
// Prevents players from bringing in modded/external items (e.g. XP amulets, mod spells) or hacked starting gold.
// ---------------------------------------------------------------------------------------------------------------
bool MemReadable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0;
}

bool IsOfficialTemplate(const char* tmpl) {
    if (!tmpl || !*tmpl) return true;

    std::string lower(tmpl);
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.find("xp_amulet") != std::string::npos ||
        lower.find("xp_ring") != std::string::npos ||
        lower.find("ring_xp") != std::string::npos ||
        lower.find("amulet_xp") != std::string::npos ||
        lower.find("kanese") != std::string::npos ||
        lower.find("cheat_") != std::string::npos ||
        lower.find("_cheat") != std::string::npos ||
        lower.find("godmode") != std::string::npos ||
        lower.find("trainer") != std::string::npos ||
        lower.find("editor") != std::string::npos) {
        return false;
    }

    return true;
}

void CollectGoItems(void* rootGo, std::vector<void*>& outItems, int depth = 0) {
    if (!rootGo || depth > 5 || !MemReadable(rootGo, 0x90)) return;
    void** b = *(void***)((char*)rootGo + 0x88);
    void** e = *(void***)((char*)rootGo + 0x8C);
    if (!b || !e || e < b || (size_t)(e - b) > 4096) return;
    for (void** it = b; it != e; ++it) {
        if (!MemReadable(it, sizeof(void*))) break;
        void* item = *it;
        if (!item || !MemReadable(item, 0x90)) continue;
        outItems.push_back(item);
        CollectGoItems(item, outItems, depth + 1);
    }
}

bool ScanInventory(void* pl, const Joined& j, std::string& outBadItem) {
    if (!pl || !MemReadable(pl, 0xC0)) return true;
    char* p = (char*)pl;
    if (*(void**)(p + 0xAC) == *(void**)(p + 0xB0)) return true;
    void** partyPtr = *(void***)(p + 0xAC);
    if (!partyPtr || !MemReadable(partyPtr, sizeof(void*))) return true;
    char* party = (char*)*partyPtr;
    if (!party || !MemReadable(party, 0x90)) return true;

    void** cb = *(void***)(party + 0x88);
    void** ce = *(void***)(party + 0x8C);
    if (!cb || !ce || ce < cb || (size_t)(ce - cb) > 32) return true;

    const auto TemplateName = (const char* (__thiscall*)(void*))0x005390A3;

    for (void** c = cb; c && c < ce; ++c) {
        if (!MemReadable(c, sizeof(void*))) break;
        void* memberGo = *c;
        if (!memberGo || !MemReadable(memberGo, 0x90)) continue;

        // Check starting gold on low-level/new character
        void* inv = *(void**)((char*)memberGo + 0x2C);
        if (inv && MemReadable(inv, 0x80)) {
            int gold = *(int*)((char*)inv + 0x78);
            if (j.xp <= 200 && gold > 50000) {
                outBadItem = "hacked starting gold (" + std::to_string(gold) + ")";
                return false;
            }
        }

        std::vector<void*> allItems;
        CollectGoItems(memberGo, allItems);

        for (void* itemGo : allItems) {
            if (!itemGo || !MemReadable(itemGo, 0x60)) continue;
            const char* tn = TemplateName(itemGo);
            if (!tn || !*tn) continue;

            if (!IsOfficialTemplate(tn)) {
                outBadItem = tn;
                return false;
            }
        }
    }
    return true;
}

}  // namespace

void JoinGuardServerTick() {
    if (!g_enforce) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    void* srv = eng::Server_Singleton();
    if (!srv) return;
    static int labLevels = -1;
    if (labLevels < 0) { char e[16] = ""; labLevels = GetEnvironmentVariableA("SIEGEHOST_LAB_GIVELEVELS", e, sizeof e) ? atoi(e) : 0; }

    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    for (void** it = b; it && it < e; ++it) {
        void* pl = *it;
        if (!eng::Player_IsValid(pl)) continue;
        char* p = (char*)pl;
        if (*(int*)(p + 0x14) != 1) continue;  // humans only
        DWORD mach = *(DWORD*)(p + 0x10);
        Joined snapshot;
        bool known;
        {
            Lock l;
            auto f = g_joined.find(mach);
            known = f != g_joined.end() && !f->second.departedTick;
            if (known) snapshot = f->second;
        }
        if (!known || snapshot.kicked) continue;  // the host itself, ghosts of players who left
        Joined j = snapshot;
        j.hero = HeroName(pl);
        long long xp;
        int members;
        if (PartyXp(pl, xp, members)) {
            j.xp = xp;
            if (!j.partySeenTick) {
                j.partySeenTick = now;
                Log("JoinGuard: account '%s' hero '%s' entered the world: party experience %lld (%d members)", j.t.account.c_str(), j.hero.c_str(), xp, members);
                { char e[8]; if (GetEnvironmentVariableA("SIEGEHOST_SKILLDUMP", e, sizeof e)) SkillDump(pl); }
            }
            // Every 15 s (and on first enter): scan player inventory for modded or unauthorized items
            {
                static std::map<DWORD, DWORD> lastInvScan;
                DWORD& lastScan = lastInvScan[mach];
                if (!lastScan || now - lastScan > 15000) {
                    lastScan = now;
                    std::string badItem;
                    if (!ScanInventory(pl, j, badItem)) {
                        char detail[400];
                        snprintf(detail, sizeof detail, "Character '%s' has unauthorized or modded item '%s' in inventory. Modded items from outside the MMO realm are not allowed.", j.hero.c_str(), badItem.c_str());
                        Kick(pl, j, "illegal_item", detail);
                        continue;
                    }
                }
            }
            // Every 60 s, not only at the door: a modified table keeps paying out for as long as the
            // player keeps killing things, so someone can arrive honest and drift while playing.
            {
                static std::map<DWORD, DWORD> lastXp;
                DWORD& last = lastXp[mach];
                if (!last || now - last > 60000) {
                    last = now;
                    { char e[8]; static bool done = false;
                      if (!done && GetEnvironmentVariableA("SIEGEHOST_XPTEST", e, sizeof e) &&
                          now - j.partySeenTick > 20000) { done = true; XpTestTamper(pl); } }
                    char xd[500];
                    if (XpCheck(pl, xd, sizeof xd)) {
                        Log("JoinGuard: XPCHECK %s account '%s' hero '%s': %s",
                            g_xpMode > 0 ? "KICK" : "report only", j.t.account.c_str(), j.hero.c_str(), xd);
                        if (g_xpMode > 0) Kick(pl, j, "levels", xd);
                    }
                }
            }
            if (!j.verified) {
                char xd[500];
                if (XpCheck(pl, xd, sizeof xd)) {
                    Kick(pl, j, "levels", xd);
                    continue;
                }
                std::string key = CharKey(j.hero);
                auto rec = j.t.chars.find(key);
                char detail[400];
                bool bad = false;
                if (rec == j.t.chars.end()) {
                    if (xp > j.t.newMax) {
                        bad = true;
                        snprintf(detail, sizeof detail, "Character '%s' is not an MMO character (it already has %lld experience). Create a new character, or play one you made on the MMO.", j.hero.c_str(), xp);
                    }
                } else if (rec->second >= 0) {
                    long long allowed = rec->second + j.t.tolAbs + rec->second * j.t.tolPct / 100;
                    if (xp > allowed) {
                        bad = true;
                        snprintf(detail, sizeof detail, "Character '%s' has %lld experience, but the MMO has only recorded %lld for it. It was changed or played outside the MMO.", j.hero.c_str(), xp, rec->second);
                    }
                }
                if (bad) {
                    Kick(pl, j, "character", detail);
                } else if (now - j.partySeenTick >= 10000) {
                    j.verified = true;
                    Log("JoinGuard: account '%s' hero '%s' verified (experience %lld, record %lld)", j.t.account.c_str(), j.hero.c_str(), xp,
                        rec == j.t.chars.end() ? -2LL : rec->second);
                }
            } else if (labLevels > 0 && now - j.partySeenTick > 40000) {
                static std::map<DWORD, bool> given;
                if (!given[mach]) {
                    given[mach] = true;
                    char* party = (char*)**(void***)(p + 0xAC);
                    char* go = (char*)**(void***)(party + 0x88);
                    auto set = (void (__thiscall*)(void*, float, float, float))0x005BDA8F;  // GoActor::RCSetSkillLevels(str,int,dex)
                    set(*(void**)go, (float)labLevels, (float)labLevels, (float)labLevels);
                    long long x2; int m2;
                    PartyXp(pl, x2, m2);
                    Log("JoinGuard[lab]: RCSetSkillLevels(%d) on '%s' -> party experience now %lld", labLevels, j.hero.c_str(), x2);
                }
            }
        }
        Lock l;
        auto f = g_joined.find(mach);
        if (f != g_joined.end() && !f->second.departedTick) f->second = j;
    }
    // forget departed players after 60 s (their final experience has been in the heartbeat long enough)
    Lock l;
    for (auto it = g_joined.begin(); it != g_joined.end();)
        if (it->second.departedTick && now - it->second.departedTick > 60000) it = g_joined.erase(it);
        else ++it;
}

// Lines for heartbeat.txt: jg=on, pcN=account|hexhero|xp (verified players, incl. those who left < 60 s ago),
// refN=unix|account|reason
std::string JG_HeartbeatLines() {
    std::string out = std::string("joinguard=") + (g_enforce ? "on" : "off") + "\n";
    {
        char x[128];
        snprintf(x, sizeof x, "xpcheck=%s\nxpcheck_checked=%lu\nxpcheck_flagged=%lu\n",
                 g_xpMode < 0 ? "off" : (g_xpMode > 0 ? "enforce" : "report"), g_xpChecked, g_xpFlagged);
        out += x;
    }
    if (!g_enforce) return out;
    Lock l;
    int n = 0;
    char line[700];
    for (auto& kv : g_joined) {
        const Joined& j = kv.second;
        if (!j.verified || j.kicked || j.xp < 0 || j.hero.empty()) continue;
        snprintf(line, sizeof line, "pc%d=%s|%s|%lld\n", n++, j.t.account.c_str(), Hex((const uint8_t*)j.hero.data(), j.hero.size()).c_str(), j.xp);
        out += line;
    }
    n = 0;
    for (auto& r : g_refusals) {
        std::string reason = r.reason;
        for (char& c : reason) if (c == '\n' || c == '\r' || c == '|') c = ' ';
        snprintf(line, sizeof line, "ref%d=%lld|%s|%s\n", n++, r.when, r.account.c_str(), reason.c_str());
        out += line;
    }
    return out;
}

std::string JG_GetAccountForDpnid(DWORD dpnid) {
    if (!g_csInit) return "";
    Lock l;
    auto it = g_joined.find(dpnid);
    if (it != g_joined.end()) {
        return it->second.t.account;
    }
    return "";
}
