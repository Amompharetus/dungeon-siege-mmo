// pvpzone.cpp - Zone-based PvP (The Endless Dunes, The Pit of Despair, The Eastern Island)
//             + Town Animal Pen PvP (Elddim, Meren, Hiroth Mule/Pasture/Pig pens, exact fence lines)
//             + dio_nothing (players keep items on death inside PvP zones/pens, normal drops elsewhere)
//             + "(PvP Zone)" chat suffix / enter-leave pen notifications
//             + Health & Rejuvenation potion disabling in PvP (always in town pens; in outdoor PvP
//               zones while another alive PvP enemy is within 40m or within 15s of PvP combat;
//               pure Mana potions always remain usable).
//
// Hooks installed:
//   [A] 0x0056D2B9  Player::IsEnemy(const Player*)          (Server + Client)
//   [B] PvpZoneTick FriendTo / FriendToMe pairwise sync     (Server, 100ms) + pen chat notices (Client)
//   [C] 0x00513451 & 0x00513EB3 friendly-spell team check   (Client)
//   [D] 0x005B58FA  Rules::DamageGo cross-player guard + 15s PvP combat timer (Server + Client)
//   [E] 0x0057803C, 0x0057815E, 0x005F0D46 zone/pen dio_nothing (Server + Client)
//   [F] 0x0056BBD1  Player::RCSetWorldLocation "(PvP Zone)" suffix (Server + Client)
//   [G] 0x005DEB17  DrinkPotionImpl (H/M hotkeys, HUD buttons, brain_hero auto-drink) (Server)
//   [H] 0x005DEBE3  call AIQuery::GetMaxN inside DrinkPotionImpl (strip rejuv on Mana drink) (Server)
//   [I] 0x00576E4E  GoInventory::SAutoUse (manual click / job_drink.skrit / job_use.skrit) (Server)
//   [J] 0x005F4C5D  call AIAction::RSDrinkLifeHealingPotions (H key / HUD life potion feedback) (Client)
//   [K1] 0x005124AC UIGame/Inventory drink dispatch #1 (manual potion click feedback) (Client)
//   [K2] 0x0051267D UIGame/Inventory drink dispatch #2 (manual potion click feedback) (Client)

#include "siegehost.h"
#include "engine.h"
#include "pvpzone_logic.h"
#include "sharedstash.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

struct SiegePos {
    float    x;
    float    y;
    float    z;
    uint32_t node;
};

struct GopColl {
    void** begin;
    void** end;
};

const auto RCSetFriendTo             = (void(__thiscall*)(void* pl, DWORD mask))0x0056D311;
const auto RCSetFriendToMe           = (void(__thiscall*)(void* pl, DWORD mask))0x0056D40D;
const auto GetPlayerTeam             = (void*(__thiscall*)(void* srv, DWORD playerId))0x0056FC53;
const auto Player_GetParty           = (void*(__thiscall*)(void* pl))0x0056C31A;
const auto GoByGoid                  = (void*(__cdecl*)(DWORD goid))0x005207E1;
const auto GoPlacement_GetPosition   = (const SiegePos*(__thiscall*)(void* placement))0x00540DFE;
const auto Go_GetLifeState           = (int(__thiscall*)(const void* go))0x00539405;
const auto GoMagic_IsHealthPotion    = (BYTE(__thiscall*)(void* goMagic))0x005D7054;
const auto GoMagic_IsRejuvPotion     = (BYTE(__thiscall*)(void* goMagic))0x005D6FFF;
const auto AIQuery_GetMaxN           = (BYTE(__thiscall*)(void* aiq, void* actor, int trait, int maxN, void* inList, void* outList))0x005E9EA4;
const auto AIQuery_IsInRange         = (BYTE(__thiscall*)(void* aiq, const void* goA, const void* goB, float range))0x005EADDF;

const int   kMaxPvpLocs                 = 32;
const float kOutdoorPvpPotionBlockRange = 40.0f;
const DWORD kPvpCombatWindowMs          = 15000;
const DWORD kPotionMsgCooldownMs        = 2500;

unsigned g_pvpLocs[kMaxPvpLocs];
int      g_numPvpLocs = 0;
bool     g_pvpOn      = false;
bool     g_dioOn      = false;
wchar_t  g_hostHero[64] = L"ServerHost";

DWORD g_pvpAttacksAllowed = 0;
DWORD g_pveDamageBlocked  = 0;
DWORD g_pvpPotionsBlocked = 0;

bool  g_filterManaRejuv   = false;
DWORD g_lastPotBlockMsgMs = 0;

struct PvpCombatEntry {
    DWORD pid;
    DWORD lastTick;
};
PvpCombatEntry g_pvpCombat[32] = {};

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

bool IsMultiplayerWorld() {
    if (void* wm = eng::WorldMap_Singleton()) {
        const char* mapName = eng::GpC(wm, 4);
        if (mapName && *mapName && _stricmp(mapName, "multiplayer_world") != 0) return false;
    }
    return true;
}

const char* LocationName(unsigned loc) {
    switch (loc) {
        case 402: return "the Endless Dunes";
        case 409: return "the Pit of Despair";
        case 901: return "the Eastern Island";
        default:  return "PvP Zone";
    }
}

bool IsPvpLocation(unsigned loc) {
    if (!g_pvpOn || loc == 0) return false;
    if (!IsMultiplayerWorld()) return false;
    for (int i = 0; i < g_numPvpLocs; ++i) {
        if (g_pvpLocs[i] == loc) return true;
    }
    return false;
}

bool IsHumanNonHost(void* pl) {
    if (!pl || !eng::Player_IsValid(pl)) return false;
    if (*(int*)((char*)pl + 0x14) != 1) return false;  // 1 = PC_HUMAN
    const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
    if (!w || !*w) return false;
    return wcscmp(w, g_hostHero) != 0;
}

void* GetPlayerHeroGo(void* pl) {
    if (!pl || !Readable(pl, 0xB8)) return nullptr;
    // Fast path: Player+0x60 (m_HeroGoid -- populated for all players on Server and local player on Client).
    DWORD goid = *(DWORD*)((char*)pl + 0x60);
    if (goid != 0 && goid != 0xFFFFFFFFu) {
        void* go = GoByGoid(goid);
        if (go && Readable(go, 0x80)) return go;
    }
    // Client remote-player path (exact path TeamPortraits::Rebuild uses at 0x00519FD7..0x00519FF6):
    // Player::GetParty() (0x0056C31A) -> party->m_Children[0] (GopColl at party+0x88).
    void* party = Player_GetParty(pl);
    if (party && Readable(party, 0x90)) {
        void** cb = *(void***)((char*)party + 0x88);
        void** ce = *(void***)((char*)party + 0x8C);
        if (cb && ce && ce > cb && Readable(cb, sizeof(void*))) {
            void* hero = *cb;
            if (hero && Readable(hero, 0x80)) return hero;
        }
    }
    return nullptr;
}

bool IsHeroAliveConscious(void* hero) {
    if (!hero || !Readable(hero, 0x10)) return false;
    return Go_GetLifeState(hero) == 1;  // 1 = LS_ALIVE_CONSCIOUS
}

// Returns 1..5 if player's hero is strictly inside one of the 5 town animal pens on multiplayer_world, else 0.
int GetPlayerPenId(void* pl) {
    if (!g_pvpOn || !IsMultiplayerWorld()) return 0;
    if (!IsHumanNonHost(pl)) return 0;
    void* hero = GetPlayerHeroGo(pl);
    if (!hero) return 0;
    void* placement = *(void**)((char*)hero + 0x40);
    if (!placement || !Readable(placement, 0x24)) return 0;
    const SiegePos* pos = GoPlacement_GetPosition(placement);
    if (!pos || !Readable(pos, sizeof(SiegePos))) return 0;
    return pvpzone::LookupPenAtNodePos(pos->node, pos->x, pos->z);
}

bool IsPvpCombatant(void* pl) {
    if (!IsHumanNonHost(pl)) return false;
    if (GetPlayerPenId(pl) != 0) return true;
    unsigned loc = *(unsigned*)((char*)pl + 0xF4);
    return IsPvpLocation(loc);
}

bool ArePvpEnemies(void* plA, void* plB) {
    if (!g_pvpOn || !plA || !plB || plA == plB) return false;
    if (!IsHumanNonHost(plA) || !IsHumanNonHost(plB)) return false;
    int  penA = GetPlayerPenId(plA);
    int  penB = GetPlayerPenId(plB);
    bool outA = IsPvpLocation(*(unsigned*)((char*)plA + 0xF4));
    bool outB = IsPvpLocation(*(unsigned*)((char*)plB + 0xF4));
    return pvpzone::ArePvpEnemiesState(outA, penA, outB, penB);
}

void MarkPvpCombat(void* pl) {
    if (!pl || !Readable(pl, 0x10)) return;
    DWORD pid = *(DWORD*)((char*)pl + 0x0C);
    if (pid == 0) return;
    DWORD now = GetTickCount();
    if (now == 0) now = 1;
    int freeSlot = -1;
    int oldestSlot = 0;
    DWORD oldestAge = 0;
    for (int i = 0; i < 32; ++i) {
        if (g_pvpCombat[i].pid == pid) {
            g_pvpCombat[i].lastTick = now;
            return;
        }
        if (g_pvpCombat[i].pid == 0 && freeSlot < 0) {
            freeSlot = i;
        } else if (g_pvpCombat[i].pid != 0) {
            DWORD age = now - g_pvpCombat[i].lastTick;
            if (age >= oldestAge) {
                oldestAge = age;
                oldestSlot = i;
            }
        }
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldestSlot;
    g_pvpCombat[slot].pid = pid;
    g_pvpCombat[slot].lastTick = now;
}

bool IsInRecentPvpCombat(void* pl) {
    if (!pl || !Readable(pl, 0x10)) return false;
    DWORD pid = *(DWORD*)((char*)pl + 0x0C);
    if (pid == 0) return false;
    DWORD now = GetTickCount();
    for (int i = 0; i < 32; ++i) {
        if (g_pvpCombat[i].pid == pid) {
            return g_pvpCombat[i].lastTick != 0 && (now - g_pvpCombat[i].lastTick) < kPvpCombatWindowMs;
        }
    }
    return false;
}

bool HasOutdoorPvpEnemyNearby(void* pl) {
    void* hero = GetPlayerHeroGo(pl);
    if (!hero) return false;
    void* aiq = *(void**)0x007AD268;
    if (!aiq) return false;
    void* srv = eng::Server_Singleton();
    if (!srv) return false;
    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    if (!b || !e || b >= e) return false;
    for (void** it = b; it != e; ++it) {
        void* other = *it;
        if (!other || other == pl) continue;
        if (!ArePvpEnemies(pl, other)) continue;
        void* otherHero = GetPlayerHeroGo(other);
        if (!otherHero || !IsHeroAliveConscious(otherHero)) continue;
        if (AIQuery_IsInRange(aiq, hero, otherHero, kOutdoorPvpPotionBlockRange)) {
            return true;
        }
    }
    return false;
}

bool AreHealthPotionsBlocked(void* pl) {
    if (!g_pvpOn || !IsHumanNonHost(pl)) return false;
    int pen = GetPlayerPenId(pl);
    if (pen != 0) return true;
    bool inOutdoor = IsPvpLocation(*(unsigned*)((char*)pl + 0xF4));
    if (!inOutdoor) return false;
    bool inCombat = IsInRecentPvpCombat(pl);
    bool nearby   = inCombat ? false : HasOutdoorPvpEnemyNearby(pl);
    return pvpzone::ShouldBlockHealthPotionsState(0, inOutdoor, nearby, inCombat);
}

bool IsHealthOrRejuvPotionGo(void* itemGo) {
    if (!itemGo || !Readable(itemGo, 0x34)) return false;
    void* goMagic = *(void**)((char*)itemGo + 0x30);
    if (!goMagic || !Readable(goMagic, 0x30)) return false;
    if (!*(void**)((char*)goMagic + 0x2C)) return false;
    return GoMagic_IsHealthPotion(goMagic) != 0 || GoMagic_IsRejuvPotion(goMagic) != 0;
}

bool IsRejuvPotionGo(void* itemGo) {
    if (!itemGo || !Readable(itemGo, 0x34)) return false;
    void* goMagic = *(void**)((char*)itemGo + 0x30);
    if (!goMagic || !Readable(goMagic, 0x30)) return false;
    if (!*(void**)((char*)goMagic + 0x2C)) return false;
    return GoMagic_IsRejuvPotion(goMagic) != 0;
}

// Local chat print on Client (same UIMultiplayer path used by characterabilities.cpp).
void PrintClientChat(const char* text) {
    if (Cfg().role != Role::Client || !text || !*text) return;
    void* uiMp = *(void**)0x007ACF94;
    if (!uiMp || !Readable(uiMp, 0x20)) return;

    eng::GpStr narrow(text);
    struct GpWStr {
        wchar_t* p{nullptr};
        char alloc{0};
        ~GpWStr() {
            if (p) {
                typedef void(__thiscall* DtorFn)(void*);
                ((DtorFn)0x00402FD9)(this);
            }
        }
    } wide;

    typedef void*(__cdecl* NarrowToWideFn)(void* out, const void* in, int codePage);
    ((NarrowToWideFn)0x0041257B)(&wide, &narrow.s, 0);

    typedef void(__thiscall* ChatFn)(void* thisPtr, const void* str);
    ((ChatFn)0x0048D2AA)(uiMp, &wide);
}

void PrintPotionBlockedChat() {
    DWORD now = GetTickCount();
    if (g_lastPotBlockMsgMs && (now - g_lastPotBlockMsgMs) < kPotionMsgCooldownMs) return;
    g_lastPotBlockMsgMs = now ? now : 1;
    PrintClientChat("Health and rejuvenation potions cannot be used during PvP.");
}

// Walks a Go* (and up to 3 parent links at Go+0x6C) to find an owning human Player* (Go+0x78).
void* HumanPlayerOfGo(void* go) {
    void* cur = go;
    for (int depth = 0; depth < 4 && Readable(cur, 0x7C); ++depth) {
        void* pl = *(void**)((char*)cur + 0x78);
        if (pl && Readable(pl, 0x18) && *(int*)((char*)pl + 0x14) == 1 && eng::Player_IsValid(pl)) {
            return pl;
        }
        void* parent = *(void**)((char*)cur + 0x6C);
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

void ParsePvpLocations(const std::string& spec) {
    g_numPvpLocs = 0;
    if (spec.empty() || _stricmp(spec.c_str(), "0") == 0 || _stricmp(spec.c_str(), "off") == 0 ||
        _stricmp(spec.c_str(), "none") == 0) {
        return;
    }
    const char* p = spec.c_str();
    while (*p && g_numPvpLocs < kMaxPvpLocs) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        if (!*p) break;
        unsigned v = (unsigned)strtoul(p, nullptr, 10);
        if (v > 0) {
            bool dup = false;
            for (int i = 0; i < g_numPvpLocs; ++i) if (g_pvpLocs[i] == v) dup = true;
            if (!dup) g_pvpLocs[g_numPvpLocs++] = v;
        }
        while (*p && *p != ',') ++p;
    }
}

void PvpZoneClientTick() {
    static DWORD lastTick = 0;
    static int   s_lastLocalPen = 0;
    DWORD now = GetTickCount();
    if (now - lastTick < 100) return;
    lastTick = now;

    void* wsObj = eng::WorldState_Singleton();
    if (!wsObj) { s_lastLocalPen = 0; return; }
    int ws = eng::WorldState_GetCurrentState(wsObj);
    if (ws != eng::WS_MP_INGAME && ws != eng::WS_MP_INGAME_JIP) {
        s_lastLocalPen = 0;
        return;
    }
    if (!g_pvpOn) return;

    void* localPl = eng::LocalPlayer();
    int pen = localPl ? GetPlayerPenId(localPl) : 0;
    if (pen != s_lastLocalPen) {
        char msg[160];
        if (pen != 0) {
            snprintf(msg, sizeof msg, "You have entered %s (PvP Zone).",
                     pvpzone::PenName(pen));
            PrintClientChat(msg);
            Log("PvpZone: local hero entered pen #%d (%s).", pen, pvpzone::PenName(pen));
        } else {
            snprintf(msg, sizeof msg, "You have left %s (Safe Zone).",
                     pvpzone::PenName(s_lastLocalPen));
            PrintClientChat(msg);
            Log("PvpZone: local hero left pen #%d (%s).", s_lastLocalPen, pvpzone::PenName(s_lastLocalPen));
        }
        s_lastLocalPen = pen;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// [A] Player::IsEnemy(const Player* other) hook at 0x0056D2B9
// ---------------------------------------------------------------------------------------------------
extern "C" int __cdecl SiegeHost_PlayerIsEnemyCheck(void* self, void* other) {
    if (!g_pvpOn || !self || !other) return -1;
    if (!Readable(self, 0x18) || !Readable(other, 0x18)) return -1;
    if (*(int*)((char*)self + 0x14) != 1 || *(int*)((char*)other + 0x14) != 1) return -1;
    if (self == other) return 0;
    return ArePvpEnemies(self, other) ? 1 : 0;
}

extern "C" void* g_playerIsEnemyTail = (void*)0x0056D2C2;
extern "C" void SiegeHost_PlayerIsEnemyStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_PlayerIsEnemyStub\n"
    "_SiegeHost_PlayerIsEnemyStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  movl 12(%esp), %eax\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_PlayerIsEnemyCheck\n"
    "  addl $8, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  cmpl $-1, %eax\n"
    "  je 1f\n"
    "  ret $4\n"
    "1:\n"
    "  pushl %esi\n"
    "  movl %ecx, %esi\n"
    "  movl 0x007ACFAC, %ecx\n"
    "  jmp *_g_playerIsEnemyTail\n");

// ---------------------------------------------------------------------------------------------------
// [C] Client-side friendly/healing spell team check at 0x00513451 and 0x00513EB3
// ---------------------------------------------------------------------------------------------------
extern "C" void* __cdecl SiegeHost_FriendlySpellTeam(void* srv, DWORD targetPlayerId, void* casterGo, void* targetGo) {
    void* team = GetPlayerTeam(srv, targetPlayerId);
    if (!g_pvpOn || !casterGo || !targetGo) return team;
    void* cPl = HumanPlayerOfGo(casterGo);
    void* tPl = HumanPlayerOfGo(targetGo);
    if (cPl && tPl && ArePvpEnemies(cPl, tPl)) {
        return (void*)-1;
    }
    return team;
}

extern "C" void SiegeHost_SpellTeamStubEdi();
extern "C" void SiegeHost_SpellTeamStubEbx();
__asm__(
    ".text\n"
    ".globl _SiegeHost_SpellTeamStubEdi\n"
    "_SiegeHost_SpellTeamStubEdi:\n"
    "  movl 4(%esp), %eax\n"
    "  pushl %edi\n"
    "  pushl %esi\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_FriendlySpellTeam\n"
    "  addl $16, %esp\n"
    "  ret $4\n"
    ".globl _SiegeHost_SpellTeamStubEbx\n"
    "_SiegeHost_SpellTeamStubEbx:\n"
    "  movl 4(%esp), %eax\n"
    "  pushl %ebx\n"
    "  pushl %esi\n"
    "  pushl %eax\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_FriendlySpellTeam\n"
    "  addl $16, %esp\n"
    "  ret $4\n");

// ---------------------------------------------------------------------------------------------------
// [D] Cross-player damage guard in Rules::DamageGo at 0x005B58FA
// ---------------------------------------------------------------------------------------------------
extern "C" char __cdecl SiegeHost_AllowDamageGo(void* victimGo, void* attackerGo, void* ownerGo, void* weaponGo) {
    if (!victimGo) return 0;
    void* vPl = HumanPlayerOfGo(victimGo);
    if (!vPl) return 1;

    void* aPl = HumanPlayerOfGo(attackerGo);
    if (!aPl) aPl = HumanPlayerOfGo(ownerGo);
    if (!aPl) aPl = HumanPlayerOfGo(weaponGo);
    if (!aPl || aPl == vPl) return 1;

    if (ArePvpEnemies(vPl, aPl)) {
        MarkPvpCombat(vPl);
        MarkPvpCombat(aPl);
        ++g_pvpAttacksAllowed;
        if (Cfg().role == Role::Server && g_pvpAttacksAllowed <= 20) {
            const wchar_t* aw = *(const wchar_t**)((char*)aPl + 0x58);
            const wchar_t* vw = *(const wchar_t**)((char*)vPl + 0x58);
            char anm[64] = "?", vnm[64] = "?";
            if (aw) WideCharToMultiByte(CP_ACP, 0, aw, -1, anm, sizeof anm, nullptr, nullptr);
            if (vw) WideCharToMultiByte(CP_ACP, 0, vw, -1, vnm, sizeof vnm, nullptr, nullptr);
            Log("PvpZone: PvP hit #%lu allowed ('%s' -> '%s').", g_pvpAttacksAllowed, anm, vnm);
        }
        return 1;
    }
    ++g_pveDamageBlocked;
    return 0;
}

extern "C" void* g_damageGoContinue = (void*)0x005B5902;
extern "C" void* g_damageGoAbort    = (void*)0x005B649D;
extern "C" void SiegeHost_DamageGoStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_DamageGoStub\n"
    "_SiegeHost_DamageGoStub:\n"
    "  andl $0, -0x10(%ebp)\n"
    "  pushal\n"
    "  pushl -0x24(%ebp)\n"
    "  pushl $0\n"
    "  pushl -0x08(%ebp)\n"
    "  pushl -0x04(%ebp)\n"
    "  call _SiegeHost_AllowDamageGo\n"
    "  addl $16, %esp\n"
    "  testb %al, %al\n"
    "  popal\n"
    "  jz 1f\n"
    "  cmpb $0, 0x24(%ebp)\n"
    "  jmp *_g_damageGoContinue\n"
    "1:\n"
    "  jmp *_g_damageGoAbort\n");

// ---------------------------------------------------------------------------------------------------
// [E] Zone-scoped dio_nothing at 0x0057803C, 0x0057815E, and 0x005F0D46
// ---------------------------------------------------------------------------------------------------
int GetDropInvOptionForGo(void* go) {
    int defOpt = 3;  // dio_inventory
    void* srv = *(void**)0x007ACF70;
    if (srv && Readable(srv, 0x30)) {
        int opt = *(int*)((char*)srv + 0x2C);
        if (opt >= 1 && opt <= 4) defOpt = opt;
    }
    if (!g_dioOn || !go) return defOpt;
    void* pl = HumanPlayerOfGo(go);
    if (pl && IsPvpCombatant(pl)) {
        return 1;  // dio_nothing inside configured PvP zones and town animal pens
    }
    return defOpt;
}

extern "C" int __cdecl SiegeHost_DropInvOptionGoInv(void* goInv, void* itemGo, void* retAddr) {
    if (retAddr == (void*)0x00578041 && itemGo && SharedStash_IsStashGo(itemGo)) {
        return 1;
    }
    void* go = (goInv && Readable(goInv, 8)) ? *(void**)((char*)goInv + 4) : nullptr;
    return GetDropInvOptionForGo(go);
}

extern "C" int __cdecl SiegeHost_DropInvOptionGoid(void* goidHolder) {
    void* go = nullptr;
    if (goidHolder && Readable(goidHolder, 4)) {
        DWORD goid = *(DWORD*)goidHolder;
        if (goid != 0) {
            go = GoByGoid(goid);
        }
    }
    return GetDropInvOptionForGo(go);
}

extern "C" void SiegeHost_DropInvGoInvStub();
extern "C" void SiegeHost_DropInvGoidStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_DropInvGoInvStub\n"
    "_SiegeHost_DropInvGoInvStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl 8(%esp)\n"
    "  pushl %edi\n"
    "  pushl %esi\n"
    "  call _SiegeHost_DropInvOptionGoInv\n"
    "  addl $12, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  ret\n"
    ".globl _SiegeHost_DropInvGoidStub\n"
    "_SiegeHost_DropInvGoidStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %esi\n"
    "  call _SiegeHost_DropInvOptionGoid\n"
    "  addl $4, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  ret\n");

// ---------------------------------------------------------------------------------------------------
// [F] On-enter location announcement suffix at 0x0056BBD1 in Player::RCSetWorldLocation
// ---------------------------------------------------------------------------------------------------
extern "C" void* g_reportSysScreenVa = (void*)0x0042C62E;

extern "C" const char* __cdecl SiegeHost_ChooseEnterFmt(void* callerEbp) {
    static const char kStockFmt[] = "$MSG$%S has entered %S.";
    static const char kPvpFmt[]   = "$MSG$%S has entered %S (PvP Zone).";

    if (!callerEbp || !Readable(callerEbp, 16)) return kStockFmt;

    unsigned loc = 0;
    if (Readable((char*)callerEbp - 0x28, 4)) {
        void* pl = *(void**)((char*)callerEbp - 0x28);
        if (pl && eng::Player_IsValid(pl)) {
            loc = *(unsigned*)((char*)pl + 0xF4);
        }
    }
    if (loc == 0 && Readable((char*)callerEbp + 8, 4)) {
        loc = *(unsigned*)((char*)callerEbp + 8);
    }

    if (IsPvpLocation(loc)) {
        Log("PvpZone: entered PvP zone %u (%s) -> using '%s'", loc, LocationName(loc), kPvpFmt);
        return kPvpFmt;
    }
    return kStockFmt;
}

extern "C" void SiegeHost_OnEnterLocationStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_OnEnterLocationStub\n"
    "_SiegeHost_OnEnterLocationStub:\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %ebp\n"
    "  call _SiegeHost_ChooseEnterFmt\n"
    "  addl $4, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  movl %eax, 4(%esp)\n"
    "  jmp *_g_reportSysScreenVa\n");

// ---------------------------------------------------------------------------------------------------
// [G] Server Hook: DrinkPotionImpl at 0x005DEB17 (6-byte prologue: 55 8B EC 83 EC 18, __thiscall ret 8)
// ---------------------------------------------------------------------------------------------------
extern "C" void* g_drinkPotionTail = (void*)0x005DEB1D;
extern "C" void SiegeHost_DrinkPotionOrig();
extern "C" void SiegeHost_DrinkPotionStub();

extern "C" void __cdecl SiegeHost_DrinkPotionHook(void* goMind, int origin, DWORD bLife) {
    void* go = (goMind && Readable(goMind, 8)) ? *(void**)((char*)goMind + 4) : nullptr;
    void* pl = go ? HumanPlayerOfGo(go) : nullptr;
    bool blocked = (pl && AreHealthPotionsBlocked(pl));
    if (blocked && (bLife & 0xFFu) != 0) {
        ++g_pvpPotionsBlocked;
        return;
    }
    bool prevFilter = g_filterManaRejuv;
    if (blocked && (bLife & 0xFFu) == 0) {
        g_filterManaRejuv = true;
    }
    typedef void(__thiscall* DrinkImplFn)(void* goMind, int origin, DWORD bLife);
    ((DrinkImplFn)(void*)&SiegeHost_DrinkPotionOrig)(goMind, origin, bLife);
    g_filterManaRejuv = prevFilter;
}

__asm__(
    ".text\n"
    ".globl _SiegeHost_DrinkPotionOrig\n"
    "_SiegeHost_DrinkPotionOrig:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x18, %esp\n"
    "  jmp *_g_drinkPotionTail\n"
    ".globl _SiegeHost_DrinkPotionStub\n"
    "_SiegeHost_DrinkPotionStub:\n"
    "  movl 8(%esp), %eax\n"
    "  movl 4(%esp), %edx\n"
    "  pushl %eax\n"
    "  pushl %edx\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_DrinkPotionHook\n"
    "  addl $12, %esp\n"
    "  ret $8\n");

// ---------------------------------------------------------------------------------------------------
// [H] Server Hook: call AIQuery::GetMaxN (0x005E9EA4) at 0x005DEBE3 inside DrinkPotionImpl
//     Strips Rejuvenation Potions in-place when g_filterManaRejuv is active so pressing M only uses
//     pure Mana potions and cleanly exits at 0x005DEBEA if only Rejuv potions exist (zero null-deref).
// ---------------------------------------------------------------------------------------------------
extern "C" BYTE __cdecl SiegeHost_DrinkGetMaxNHook(void* aiq, void* actor, int trait, int maxN, void* inList, void* outList) {
    BYTE ok = AIQuery_GetMaxN(aiq, actor, trait, maxN, inList, outList);
    if (!ok || !g_filterManaRejuv || !outList || !Readable(outList, sizeof(GopColl))) {
        return ok;
    }
    auto* coll = (GopColl*)outList;
    if (coll->begin && coll->end && coll->end > coll->begin) {
        void** dst = coll->begin;
        bool strippedAny = false;
        for (void** src = coll->begin; src < coll->end; ++src) {
            if (IsRejuvPotionGo(*src)) {
                strippedAny = true;
            } else {
                *dst++ = *src;
            }
        }
        coll->end = dst;
        if (dst == coll->begin) {
            if (strippedAny) ++g_pvpPotionsBlocked;
            return 0;
        }
    }
    return ok;
}

extern "C" void SiegeHost_DrinkGetMaxNStub();
__asm__(
    ".text\n"
    ".globl _SiegeHost_DrinkGetMaxNStub\n"
    "_SiegeHost_DrinkGetMaxNStub:\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl 20(%esp)\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_DrinkGetMaxNHook\n"
    "  addl $24, %esp\n"
    "  ret $0x14\n");

// ---------------------------------------------------------------------------------------------------
// [I] Server Hook: GoInventory::SAutoUse at 0x00576E4E (6-byte prologue: 55 8B EC 83 EC 38, __thiscall ret 0xC)
// ---------------------------------------------------------------------------------------------------
extern "C" void* g_sAutoUseTail = (void*)0x00576E54;
extern "C" void SiegeHost_SAutoUseOrig();
extern "C" void SiegeHost_SAutoUseStub();

extern "C" BYTE __cdecl SiegeHost_SAutoUseHook(void* goInv, DWORD itemGoid, int origin, DWORD bAllowAny) {
    if (g_pvpOn && itemGoid != 0 && itemGoid != 0xFFFFFFFFu) {
        void* itemGo = GoByGoid(itemGoid);
        if (itemGo && IsHealthOrRejuvPotionGo(itemGo)) {
            void* ownerGo = (goInv && Readable(goInv, 8)) ? *(void**)((char*)goInv + 4) : nullptr;
            void* pl = ownerGo ? HumanPlayerOfGo(ownerGo) : nullptr;
            if (pl && AreHealthPotionsBlocked(pl)) {
                ++g_pvpPotionsBlocked;
                return 0;
            }
        }
    }
    typedef BYTE(__thiscall* SAutoUseFn)(void* goInv, DWORD itemGoid, int origin, DWORD bAllowAny);
    return ((SAutoUseFn)(void*)&SiegeHost_SAutoUseOrig)(goInv, itemGoid, origin, bAllowAny);
}

__asm__(
    ".text\n"
    ".globl _SiegeHost_SAutoUseOrig\n"
    "_SiegeHost_SAutoUseOrig:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x38, %esp\n"
    "  jmp *_g_sAutoUseTail\n"
    ".globl _SiegeHost_SAutoUseStub\n"
    "_SiegeHost_SAutoUseStub:\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_SAutoUseHook\n"
    "  addl $16, %esp\n"
    "  ret $0xC\n");

// ---------------------------------------------------------------------------------------------------
// [J] Client Hook: call AIAction::RSDrinkLifeHealingPotions (0x005F4C88) at 0x005F4C5D (only caller)
//     MUST NOT place 0xE9 at byte +0 of 0x005F4C88: it is a PE-exported FuBi RPC, and
//     FuBi::SysExports::ImportBindings (0x004522F9) follows 0xE9 thunks into Mss32.dll,
//     removing 0x005F4C88 from m_FunctionsByAddr and breaking FuBi::ResolveFunction at 0x005F4CE0.
// ---------------------------------------------------------------------------------------------------
extern "C" void SiegeHost_RSDrinkLifeStub();

extern "C" DWORD __cdecl SiegeHost_RSDrinkLifeHook(void* aiAction, const void* goidBuf, DWORD bufBytes, int origin) {
    if (g_pvpOn && Cfg().role == Role::Client) {
        void* localPl = eng::LocalPlayer();
        if (localPl && AreHealthPotionsBlocked(localPl)) {
            ++g_pvpPotionsBlocked;
            PrintPotionBlockedChat();
            return 0xFFFFFFFFu;
        }
    }
    typedef DWORD(__thiscall* RSDrinkLifeFn)(void* aiAction, const void* goidBuf, DWORD bufBytes, int origin);
    return ((RSDrinkLifeFn)0x005F4C88)(aiAction, goidBuf, bufBytes, origin);
}

__asm__(
    ".text\n"
    ".globl _SiegeHost_RSDrinkLifeStub\n"
    "_SiegeHost_RSDrinkLifeStub:\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  pushl %ecx\n"
    "  call _SiegeHost_RSDrinkLifeHook\n"
    "  addl $16, %esp\n"
    "  ret $0xC\n");

// ---------------------------------------------------------------------------------------------------
// [K1] Client Hook: FUN_005124ac at 0x005124AC (6-byte prologue: 55 8B EC 83 EC 6C, __stdcall ret 8)
// [K2] Client Hook: FUN_0051267d at 0x0051267D (5-byte prologue: 55 8B EC 51 51, __stdcall ret 0xC)
// ---------------------------------------------------------------------------------------------------
extern "C" void* g_clientDrink1Tail = (void*)0x005124B2;
extern "C" void* g_clientDrink2Tail = (void*)0x00512682;
extern "C" void SiegeHost_ClientDrink1Orig();
extern "C" void SiegeHost_ClientDrink1Stub();
extern "C" void SiegeHost_ClientDrink2Orig();
extern "C" void SiegeHost_ClientDrink2Stub();

extern "C" void __cdecl SiegeHost_ClientDrink1Hook(DWORD itemGoid, DWORD bQueue) {
    if (g_pvpOn && Cfg().role == Role::Client && itemGoid != 0 && itemGoid != 0xFFFFFFFFu) {
        void* itemGo = GoByGoid(itemGoid);
        void* localPl = eng::LocalPlayer();
        if (itemGo && localPl && IsHealthOrRejuvPotionGo(itemGo) && AreHealthPotionsBlocked(localPl)) {
            ++g_pvpPotionsBlocked;
            PrintPotionBlockedChat();
            return;
        }
    }
    typedef void(__stdcall* ClientDrink1Fn)(DWORD itemGoid, DWORD bQueue);
    ((ClientDrink1Fn)(void*)&SiegeHost_ClientDrink1Orig)(itemGoid, bQueue);
}

extern "C" void __cdecl SiegeHost_ClientDrink2Hook(DWORD itemGoid, DWORD actorGoid, DWORD bQueue) {
    if (g_pvpOn && Cfg().role == Role::Client && itemGoid != 0 && itemGoid != 0xFFFFFFFFu) {
        void* itemGo = GoByGoid(itemGoid);
        void* localPl = eng::LocalPlayer();
        if (itemGo && localPl && IsHealthOrRejuvPotionGo(itemGo) && AreHealthPotionsBlocked(localPl)) {
            ++g_pvpPotionsBlocked;
            PrintPotionBlockedChat();
            return;
        }
    }
    typedef void(__stdcall* ClientDrink2Fn)(DWORD itemGoid, DWORD actorGoid, DWORD bQueue);
    ((ClientDrink2Fn)(void*)&SiegeHost_ClientDrink2Orig)(itemGoid, actorGoid, bQueue);
}

__asm__(
    ".text\n"
    ".globl _SiegeHost_ClientDrink1Orig\n"
    "_SiegeHost_ClientDrink1Orig:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x6C, %esp\n"
    "  jmp *_g_clientDrink1Tail\n"
    ".globl _SiegeHost_ClientDrink1Stub\n"
    "_SiegeHost_ClientDrink1Stub:\n"
    "  pushl 8(%esp)\n"
    "  pushl 8(%esp)\n"
    "  call _SiegeHost_ClientDrink1Hook\n"
    "  addl $8, %esp\n"
    "  ret $8\n"
    ".globl _SiegeHost_ClientDrink2Orig\n"
    "_SiegeHost_ClientDrink2Orig:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %ecx\n"
    "  pushl %ecx\n"
    "  jmp *_g_clientDrink2Tail\n"
    ".globl _SiegeHost_ClientDrink2Stub\n"
    "_SiegeHost_ClientDrink2Stub:\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  pushl 12(%esp)\n"
    "  call _SiegeHost_ClientDrink2Hook\n"
    "  addl $12, %esp\n"
    "  ret $0xC\n");

// ---------------------------------------------------------------------------------------------------
// Installation & Server/Client Tick
// ---------------------------------------------------------------------------------------------------
namespace {

bool PatchJmpChecked(DWORD va, const BYTE* expect, size_t len, void* stub, const char* label) {
    BYTE* p = (BYTE*)va;
    if (memcmp(p, expect, len) != 0) {
        Log("PvpZone: VERIFY FAILED at %s (0x%08lX).", label, va);
        return false;
    }
    DWORD o;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &o)) {
        Log("PvpZone: VirtualProtect failed at %s (0x%08lX, err=%lu).", label, va, GetLastError());
        return false;
    }
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((DWORD)stub - (va + 5));
    for (size_t i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(p, len, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

bool PatchCallChecked(DWORD va, DWORD expectedTarget, void* stub, const char* label) {
    BYTE* p = (BYTE*)va;
    if (p[0] != 0xE8 || (va + 5 + *(int32_t*)(p + 1) != expectedTarget)) {
        Log("PvpZone: VERIFY FAILED at %s (0x%08lX, expected call 0x%08lX).", label, va, expectedTarget);
        return false;
    }
    DWORD o;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
        Log("PvpZone: VirtualProtect failed at %s (0x%08lX, err=%lu).", label, va, GetLastError());
        return false;
    }
    *(int32_t*)(p + 1) = (int32_t)((DWORD)stub - (va + 5));
    VirtualProtect(p, 5, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    return true;
}

}  // namespace

void PvpZoneInstall() {
    MultiByteToWideChar(CP_ACP, 0, Cfg().hostCharacter.c_str(), -1, g_hostHero, 63);
    ParsePvpLocations(Cfg().pvpLocations);

    // 1. Install zone-scoped dio_nothing hooks (equipped items, inventory items, gold, and follower weapon keep)
    if (Cfg().dropInvNothing) {
        struct DioSite { DWORD va; void* stub; };
        const DioSite kDioSites[] = {
            {0x0057803C, (void*)&SiegeHost_DropInvGoInvStub},
            {0x0057815E, (void*)&SiegeHost_DropInvGoInvStub},
            {0x005F0D46, (void*)&SiegeHost_DropInvGoidStub},
        };
        static const BYTE kDioExpect[8] = {0xA1, 0x70, 0xCF, 0x7A, 0x00, 0x8B, 0x40, 0x2C};
        bool allOk = true;
        for (const auto& s : kDioSites) {
            if (memcmp((void*)s.va, kDioExpect, 8) != 0) {
                Log("PvpZone: dio_nothing VERIFY FAILED at 0x%08lX.", s.va);
                allOk = false;
            }
        }
        if (allOk) {
            for (const auto& s : kDioSites) {
                BYTE* p = (BYTE*)s.va;
                DWORD o;
                if (VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &o)) {
                    p[0] = 0xE8;
                    *(int32_t*)(p + 1) = (int32_t)((DWORD)s.stub - (s.va + 5));
                    p[5] = 0x90; p[6] = 0x90; p[7] = 0x90;
                    VirtualProtect(p, 8, o, &o);
                    FlushInstructionCache(GetCurrentProcess(), p, 8);
                }
            }
            g_dioOn = true;
            Log("PvpZone: zone-scoped dio_nothing armed (PvP zones + 5 town animal pens keep all gear/gold).");
        }
    }

    // 2. Install Zone + Town Animal Pen PvP hooks
    if (g_numPvpLocs <= 0) {
        Log("PvpZone: off (no PvP locations configured).");
        return;
    }

    // [A] Player::IsEnemy at 0x0056D2B9
    static const BYTE kIsEnemyExpect[9] = {0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0xAC, 0xCF, 0x7A, 0x00};
    if (!PatchJmpChecked(0x0056D2B9, kIsEnemyExpect, 9, (void*)&SiegeHost_PlayerIsEnemyStub, "Player::IsEnemy")) {
        return;
    }

    // [C] Client friendly/healing spell team check
    if (Cfg().role == Role::Client) {
        int hooked = 0;
        if (PatchCallChecked(0x00513451, 0x0056FC53, (void*)&SiegeHost_SpellTeamStubEdi, "SpellTeamEdi")) ++hooked;
        if (PatchCallChecked(0x00513EB3, 0x0056FC53, (void*)&SiegeHost_SpellTeamStubEbx, "SpellTeamEbx")) ++hooked;
        Log("PvpZone: client friendly-spell guard armed at %d of 2 sites.", hooked);
    }

    // [D] Rules::DamageGo cross-player damage guard at 0x005B58FA
    static const BYTE kDmgExpect[8] = {0x83, 0x65, 0xF0, 0x00, 0x80, 0x7D, 0x24, 0x00};
    if (PatchJmpChecked(0x005B58FA, kDmgExpect, 8, (void*)&SiegeHost_DamageGoStub, "Rules::DamageGo")) {
        Log("PvpZone: Rules::DamageGo cross-player filter armed at 0x005B58FA.");
    }

    // [F] On-enter PvP Zone chat suffix at 0x0056BBD1
    if (PatchCallChecked(0x0056BBD1, 0x0042C62E, (void*)&SiegeHost_OnEnterLocationStub, "OnEnterLocation")) {
        Log("PvpZone: on-enter chat suffix hook armed at 0x0056BBD1.");
    }

    // [G], [H], [I] Server-authoritative PvP health & rejuvenation potion guards
    if (Cfg().role == Role::Server) {
        static const BYTE kDrinkExpect[6]    = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18};
        static const BYTE kSAutoUseExpect[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x38};
        bool okG = PatchJmpChecked(0x005DEB17, kDrinkExpect, 6, (void*)&SiegeHost_DrinkPotionStub, "DrinkPotionImpl");
        bool okH = PatchCallChecked(0x005DEBE3, 0x005E9EA4, (void*)&SiegeHost_DrinkGetMaxNStub, "DrinkGetMaxN");
        bool okI = PatchJmpChecked(0x00576E4E, kSAutoUseExpect, 6, (void*)&SiegeHost_SAutoUseStub, "GoInventory::SAutoUse");
        Log("PvpZone: server potion guards armed [G=%d H=%d I=%d] (pens=always, outdoor=40m/15s).",
            okG ? 1 : 0, okH ? 1 : 0, okI ? 1 : 0);
    }

    // [J], [K1], [K2] Client preventative potion hooks + chat feedback
    if (Cfg().role == Role::Client) {
        static const BYTE kDrink1Expect[6]  = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x6C};
        static const BYTE kDrink2Expect[5]  = {0x55, 0x8B, 0xEC, 0x51, 0x51};
        bool okJ  = PatchCallChecked(0x005F4C5D, 0x005F4C88, (void*)&SiegeHost_RSDrinkLifeStub, "RSDrinkLife");
        bool okK1 = PatchJmpChecked(0x005124AC, kDrink1Expect, 6, (void*)&SiegeHost_ClientDrink1Stub, "ClientDrink1");
        bool okK2 = PatchJmpChecked(0x0051267D, kDrink2Expect, 5, (void*)&SiegeHost_ClientDrink2Stub, "ClientDrink2");
        Log("PvpZone: client potion guards armed [J=%d K1=%d K2=%d].",
            okJ ? 1 : 0, okK1 ? 1 : 0, okK2 ? 1 : 0);
    }

    g_pvpOn = true;
    std::string locList;
    for (int i = 0; i < g_numPvpLocs; ++i) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s%u (%s)", i ? ", " : "", g_pvpLocs[i], LocationName(g_pvpLocs[i]));
        locList += buf;
    }
    Log("PvpZone: armed on locations [%s] + 5 town animal pens (%u terrain nodes).",
        locList.c_str(), (unsigned)pvpzone::kPvpPenTileCount);
}

void PvpZoneTick() {
    if (Cfg().role == Role::Client) {
        PvpZoneClientTick();
        return;
    }
    if (Cfg().role != Role::Server) return;

    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (now - lastTick < 100) return;
    lastTick = now;

    void* wsObj = eng::WorldState_Singleton();
    if (!wsObj) return;
    int ws = eng::WorldState_GetCurrentState(wsObj);
    if (ws != eng::WS_MP_INGAME && ws != eng::WS_MP_INGAME_JIP) return;

    void* srv = eng::Server_Singleton();
    if (!srv) return;

    // Keep Server+0x2C set to dio_inventory (3) globally so PvE deaths outside PvP zones/pens drop inventory/gold.
    if (g_dioOn && *(int*)((char*)srv + 0x2C) != 3) {
        eng::Server_RCSetDropInvOption(srv, 3);
        Log("PvpZone: Server::RCSetDropInvOption(3 = dio_inventory outside PvP) -> Server+0x2C=%d",
            *(int*)((char*)srv + 0x2C));
    }

    if (!g_pvpOn) return;

    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    if (!b || !e || b >= e) return;

    static unsigned s_lastLoc[32] = {0};
    static int      s_lastPen[32] = {0};
    static bool     s_wasPvp[32]  = {false};

    void* players[32];
    DWORD ids[32];
    int   count = 0;
    DWORD allMask = 0;

    int slot = 0;
    for (void** it = b; it != e && slot < 32; ++it, ++slot) {
        void* pl = *it;
        if (!pl || !eng::Player_IsValid(pl)) {
            s_lastLoc[slot] = 0;
            s_lastPen[slot] = 0;
            s_wasPvp[slot]  = false;
            continue;
        }
        DWORD    pid = *(DWORD*)((char*)pl + 0x0C);
        unsigned loc = *(unsigned*)((char*)pl + 0xF4);
        int      pen = GetPlayerPenId(pl);
        bool     pvp = IsPvpCombatant(pl);

        if (loc != s_lastLoc[slot] || pen != s_lastPen[slot] || pvp != s_wasPvp[slot]) {
            if ((pvp != s_wasPvp[slot] || pen != s_lastPen[slot]) && IsHumanNonHost(pl)) {
                const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
                char nm[64] = "?";
                if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, nm, sizeof nm, nullptr, nullptr);
                if (pen != 0) {
                    Log("PvpZone: player '%s' (id=0x%08lX) ENTERED PvP pen #%d (%s).",
                        nm, pid, pen, pvpzone::PenName(pen));
                } else if (pvp) {
                    Log("PvpZone: player '%s' (id=0x%08lX) ENTERED PvP zone %u (%s).",
                        nm, pid, loc, LocationName(loc));
                } else {
                    Log("PvpZone: player '%s' (id=0x%08lX) LEFT PvP -> now in PvE location %u.",
                        nm, pid, loc);
                }
            }
            s_lastLoc[slot] = loc;
            s_lastPen[slot] = pen;
            s_wasPvp[slot]  = pvp;
        }

        players[count] = pl;
        ids[count]     = pid;
        allMask       |= pid;
        ++count;
    }

    // Pairwise FriendTo / FriendToMe sync (respects per-pen isolation and outdoor PvP zones).
    for (int i = 0; i < count; ++i) {
        void* pl  = players[i];
        DWORD pid = ids[i];
        DWORD wantFriend = pid;
        for (int j = 0; j < count; ++j) {
            if (i == j) continue;
            if (!ArePvpEnemies(pl, players[j])) {
                wantFriend |= ids[j];
            }
        }
        DWORD curFriendTo   = *(DWORD*)((char*)pl + 0xE8);
        DWORD curFriendToMe = *(DWORD*)((char*)pl + 0xEC);
        if ((curFriendTo & allMask) != wantFriend) {
            RCSetFriendTo(pl, (curFriendTo & ~allMask) | wantFriend);
        }
        if ((curFriendToMe & allMask) != wantFriend) {
            RCSetFriendToMe(pl, (curFriendToMe & ~allMask) | wantFriend);
        }
    }
}

std::string PvpZoneHeartbeatLines() {
    char b[224];
    snprintf(b, sizeof b,
             "pvpzone=%s+5pens\ndio_nothing=%d\npvp_hits=%lu\npve_blocked=%lu\npvp_pots_blocked=%lu\n",
             g_pvpOn ? Cfg().pvpLocations.c_str() : "off",
             g_dioOn ? 1 : 0,
             g_pvpAttacksAllowed,
             g_pveDamageBlocked,
             g_pvpPotionsBlocked);
    return b;
}
