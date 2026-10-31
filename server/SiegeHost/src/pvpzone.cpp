// pvpzone.cpp - 1.5.9: Zone-based PvP (The Endless Dunes, The Pit of Despair, The Eastern Island)
//                      + dio_nothing (players keep items on death inside PvP zones, normal drops elsewhere)
//                      + "(PvP Zone)" chat suffix when entering a PvP zone.
//
// ---------------------------------------------------------------------------------------------------
// 1. ZONE-BASED PVP (Locations 402, 409, 901 on Utraean Peninsula / multiplayer_world)
// ---------------------------------------------------------------------------------------------------
// Every Player object keeps its current world_locations.gas ID at Player+0xF4, set on the server and
// broadcast to all clients by Player::RCSetWorldLocation (0x0056BA42) whenever a character crosses a
// set_player_world_location trigger.
//
// In MpWorld.dsmap (world/maps/multiplayer_world/info/world_locations.gas):
//   * 402 = "the Endless Dunes"   (endless_dunes)
//   * 409 = "the Pit of Despair"  (pit_of_despair)
//   * 901 = "the Eastern Island"  (island)
//
// Two human players are PvP enemies if and only if:
//   - Both are valid human players (Player+0x14 == 1) and distinct (plA != plB),
//   - Neither is the dedicated server's dummy character ("ServerHost"),
//   - Both are currently inside a configured PvP location (Player+0xF4 in PvpLocations).
// Everywhere else (or when one player is inside a PvP zone and the other is outside), they remain
// friends (PvE co-op).
//
// Four places enforce this across both Server and Client:
//   [A] Player::IsEnemy(const Player* other) at 0x0056D2B9 (thiscall, ret 4):
//       Called by Player::IsFriend (0x0056D300), GoMind::IsEnemy (0x005DF1BE), and GoMind::IsFriend
//       (0x005DF22D). When both `this` and `other` are human players, returns true (1) if they are
//       PvP enemies and false (0) otherwise. When either side is the computer player (monster/NPC),
//       falls through to the engine's stock check.
//   [B] Server-side FriendTo / FriendToMe bitmask sync in PvpZoneTick():
//       Calls Player::RCSetFriendTo (0x0056D311) and Player::RCSetFriendToMe (0x0056D40D) whenever
//       a player enters or leaves a PvP zone so Player+0xE8 and Player+0xEC stay in sync on all
//       connected machines.
//   [C] Client-side defensive/healing spell team checks at 0x00513451 and 0x00513EB3:
//       When casting a defensive/healing spell, UIPartyManager checks whether caster and target are
//       on the same team via Server::GetPlayerTeam (0x0056FC53). If caster and target are currently
//       PvP enemies, returns (void*)-1 so players do not get a heal cursor on their opponent while
//       still allowing cross-player healing in PvE zones.
//   [D] Server-side cross-player damage guard in Rules::DamageGo at 0x005B58FA:
//       All damage paths (melee, ranged, projectiles, magic, AoE volume damage, particles) funnel
//       into Rules::DamageGo (0x005B58CD). Right after victim ([ebp-4]), attacker ([ebp-8]), and
//       weapon ([ebp-0x24]) Go* pointers are resolved (and before any branch or shield check), if the
//       victim belongs to a human player and the attacker or weapon belongs to a different human
//       player, damage is allowed ONLY when the two players are currently PvP enemies. This also
//       prevents stray/in-flight projectiles or AoE spells from hurting players who are in PvE.
//   [E] Zone-scoped dio_nothing at 0x0057803C, 0x0057815E, and 0x005F0D46:
//       Players keep all items and gold on death inside PvP zones, but drop them normally elsewhere.
//   [F] On-enter PvP Zone chat suffix at 0x0056BBD1 in Player::RCSetWorldLocation:
//       Replaces "$MSG$%S has entered %S." with "$MSG$%S has entered %S (PvP Zone)." when entering a PvP zone.
//
// ---------------------------------------------------------------------------------------------------
// 2. DROP NOTHING ON DEATH (dio_nothing = 1)
// ---------------------------------------------------------------------------------------------------
// In DSLOA.exe, eDropInvOption values are:
//   1 = dio_nothing, 2 = dio_equipped, 3 = dio_inventory, 4 = dio_all
// stored at Server+0x2C and replicated by Server::RCSetDropInvOption (0x005707A8).
//
// When a player party member dies in multiplayer, three sites read Server+0x2C via the 8-byte load
// `mov eax, [0x007ACF70]; mov eax, [eax+0x2C]` (A1 70 CF 7A 00 8B 40 2C):
//   * 0x0057803C in GoInventory::DropItemsFor (equipped + inventory item drop; je skips when eax == 1)
//   * 0x0057815E in GoInventory::DropItemsFor (gold drop; je skips when eax == 1)
//   * 0x005F0D46 in GoFollower death handler  (keeps weapons equipped when eax == 1)
// (Monster deaths branch over 0x0057803C and 0x0057815E after Go::IsPlayerPartyMember returns 0, so
// monster loot is completely unaffected.)
//
// Patching those three loads to `mov eax, 1; nop; nop; nop` AND setting Server::RCSetDropInvOption(1)
// guarantees that players never drop equipped items, inventory items, or gold when they die.

#include "siegehost.h"
#include "engine.h"
#include "sharedstash.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

namespace {

const auto RCSetFriendTo        = (void(__thiscall*)(void* pl, DWORD mask))0x0056D311;
const auto RCSetFriendToMe      = (void(__thiscall*)(void* pl, DWORD mask))0x0056D40D;
const auto GetPlayerTeam        = (void*(__thiscall*)(void* srv, DWORD playerId))0x0056FC53;

const int kMaxPvpLocs = 32;
unsigned g_pvpLocs[kMaxPvpLocs];
int g_numPvpLocs = 0;
bool g_pvpOn = false;
bool g_dioOn = false;
wchar_t g_hostHero[64] = L"ServerHost";

DWORD g_pvpAttacksAllowed = 0;
DWORD g_pveDamageBlocked = 0;

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
    // Only match on multiplayer_world (Utraean Peninsula) if WorldMap has a map name loaded.
    if (void* wm = eng::WorldMap_Singleton()) {
        const char* mapName = eng::GpC(wm, 4);
        if (mapName && *mapName && _stricmp(mapName, "multiplayer_world") != 0) return false;
    }
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

bool IsPvpCombatant(void* pl) {
    if (!IsHumanNonHost(pl)) return false;
    unsigned loc = *(unsigned*)((char*)pl + 0xF4);
    return IsPvpLocation(loc);
}

bool ArePvpEnemies(void* plA, void* plB) {
    if (!g_pvpOn || !plA || !plB || plA == plB) return false;
    return IsPvpCombatant(plA) && IsPvpCombatant(plB);
}

// Walks a Go* (and up to 3 parent links at Go+0x60) to find an owning human Player* (Go+0x78).
void* HumanPlayerOfGo(void* go) {
    void* cur = go;
    for (int depth = 0; depth < 4 && Readable(cur, 0x7C); ++depth) {
        void* pl = *(void**)((char*)cur + 0x78);
        if (pl && Readable(pl, 0x18) && *(int*)((char*)pl + 0x14) == 1 && eng::Player_IsValid(pl)) {
            return pl;
        }
        void* parent = *(void**)((char*)cur + 0x60);
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

}  // namespace

// ---------------------------------------------------------------------------------------------------
// [A] Player::IsEnemy(const Player* other) hook at 0x0056D2B9
// ---------------------------------------------------------------------------------------------------
// Returns:
//   1 = enemies (both are human players in a PvP zone)
//   0 = friends (both are human players, not both in a PvP zone)
//  -1 = fall through to original Player::IsEnemy (monster / computer player involved)
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
    "  movl 12(%esp), %eax\n"          // arg0: const Player* other
    "  pushl %eax\n"
    "  pushl %ecx\n"                   // this: Player* self
    "  call _SiegeHost_PlayerIsEnemyCheck\n"
    "  addl $8, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  cmpl $-1, %eax\n"
    "  je 1f\n"
    "  ret $4\n"
    "1:\n"
    "  pushl %esi\n"                   // original 9 bytes at 0x0056D2B9..0x0056D2C1
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
        return (void*)-1;  // mismatch caster's team so healing/defensive spells cannot target a PvP enemy
    }
    return team;
}

extern "C" void SiegeHost_SpellTeamStubEdi();
extern "C" void SiegeHost_SpellTeamStubEbx();
__asm__(
    ".text\n"
    ".globl _SiegeHost_SpellTeamStubEdi\n"
    "_SiegeHost_SpellTeamStubEdi:\n"
    "  movl 4(%esp), %eax\n"           // targetPlayerId
    "  pushl %edi\n"                   // targetGo (in edi at 0x00513451)
    "  pushl %esi\n"                   // casterGo (in esi at 0x00513451)
    "  pushl %eax\n"
    "  pushl %ecx\n"                   // Server*
    "  call _SiegeHost_FriendlySpellTeam\n"
    "  addl $16, %esp\n"
    "  ret $4\n"
    ".globl _SiegeHost_SpellTeamStubEbx\n"
    "_SiegeHost_SpellTeamStubEbx:\n"
    "  movl 4(%esp), %eax\n"           // targetPlayerId
    "  pushl %ebx\n"                   // targetGo (in ebx at 0x00513EB3)
    "  pushl %esi\n"                   // casterGo (in esi at 0x00513EB3)
    "  pushl %eax\n"
    "  pushl %ecx\n"                   // Server*
    "  call _SiegeHost_FriendlySpellTeam\n"
    "  addl $16, %esp\n"
    "  ret $4\n");

// ---------------------------------------------------------------------------------------------------
// [D] Cross-player damage guard in Rules::DamageGo at 0x005B58FA
// ---------------------------------------------------------------------------------------------------
// At 0x005B58FA..0x005B5901 (8 bytes):
//   005B58FA: 83 65 F0 00     and dword ptr [ebp - 0x10], 0
//   005B58FE: 80 7D 24 00     cmp byte ptr [ebp + 0x24], 0
//   005B5902: 0F 84 33 01 00 00 je 0x005B5A3B
// No branch in Rules::DamageGo targets 0x005B58FA..0x005B5901, and [ebp-4] (victim Go*),
// [ebp-8] (attacker Go*), and [ebp-0x24] (weapon Go*) have already been resolved.
extern "C" char __cdecl SiegeHost_AllowDamageGo(void* victimGo, void* attackerGo, void* ownerGo, void* weaponGo) {
    if (!victimGo) return 0;
    void* vPl = HumanPlayerOfGo(victimGo);
    if (!vPl) return 1;  // victim is a monster/object -> allow normal damage

    void* aPl = HumanPlayerOfGo(attackerGo);
    if (!aPl) aPl = HumanPlayerOfGo(ownerGo);
    if (!aPl) aPl = HumanPlayerOfGo(weaponGo);
    if (!aPl || aPl == vPl) return 1;  // monster attacker or self-damage -> allow

    if (ArePvpEnemies(vPl, aPl)) {
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
    "  andl $0, -0x10(%ebp)\n"           // original 4 bytes at 0x005B58FA: and dword ptr [ebp-0x10], 0
    "  pushal\n"
    "  pushl -0x24(%ebp)\n"              // weapon Go*
    "  pushl $0\n"                       // owner Go* (resolved via parent chain of attacker/weapon)
    "  pushl -0x08(%ebp)\n"              // attacker Go*
    "  pushl -0x04(%ebp)\n"              // victim Go*
    "  call _SiegeHost_AllowDamageGo\n"
    "  addl $16, %esp\n"
    "  testb %al, %al\n"
    "  popal\n"
    "  jz 1f\n"
    "  cmpb $0, 0x24(%ebp)\n"            // original 4 bytes at 0x005B58FE: cmp byte ptr [ebp+0x24], 0
    "  jmp *_g_damageGoContinue\n"       // resume at 0x005B5902 (je 0x005B5A3B)
    "1:\n"
    "  jmp *_g_damageGoAbort\n");        // abort at 0x005B649D (xor al, al; pop edi/esi/ebx; leave; ret 0x20)

// ---------------------------------------------------------------------------------------------------
// [E] Zone-scoped dio_nothing at 0x0057803C, 0x0057815E, and 0x005F0D46
// ---------------------------------------------------------------------------------------------------
// At all three player death-drop check sites, the engine loads `Server+0x2C` (`m_DropInvOption`) via the
// 8-byte sequence `A1 70 CF 7A 00 8B 40 2C` (`mov eax, [0x007ACF70]; mov eax, [eax+0x2C]`):
//   * 0x0057803C in GoInventory::DropItemsFor (equipped + inventory items): `esi` is `GoInventory*`, `[esi+4]` is `Go*`.
//   * 0x0057815E in GoInventory::DropItemsFor (gold drop):                  `esi` is `GoInventory*`, `[esi+4]` is `Go*`.
//   * 0x005F0D46 in GoMind/GoFollower death handler:                        `esi` holds `Goid` at `[esi]`, resolved via `0x005207E1`.
// If the dying character belongs to a human player currently inside a configured PvP zone (402, 409, 901),
// return 1 (dio_nothing — keep all equipped items, inventory items, and gold). Otherwise return the
// server's normal drop option (4 = dio_all — drop items and gold as usual in PvE zones).
int GetDropInvOptionForGo(void* go) {
    int defOpt = 4;  // dio_all (normal multiplayer drop)
    void* srv = *(void**)0x007ACF70;
    if (srv && Readable(srv, 0x30)) {
        int opt = *(int*)((char*)srv + 0x2C);
        if (opt >= 1 && opt <= 4) defOpt = opt;
    }
    if (!g_dioOn || !go) return defOpt;
    void* pl = HumanPlayerOfGo(go);
    if (pl && IsPvpCombatant(pl)) {
        return 1;  // dio_nothing inside configured PvP zones (402, 409, 901)
    }
    return defOpt;  // normal drop everywhere else in the wild
}

extern "C" int __cdecl SiegeHost_DropInvOptionGoInv(void* goInv, void* itemGo, void* retAddr) {
    if (retAddr == (void*)0x00578041 && itemGo && SharedStash_IsStashGo(itemGo)) {
        return 1;  // dio_nothing inside item drop loop for stashGo (skips drop at 0x0057803C)
    }
    void* go = (goInv && Readable(goInv, 8)) ? *(void**)((char*)goInv + 4) : nullptr;
    return GetDropInvOptionForGo(go);
}

extern "C" int __cdecl SiegeHost_DropInvOptionGoid(void* goidHolder) {
    void* go = nullptr;
    if (goidHolder && Readable(goidHolder, 4)) {
        DWORD goid = *(DWORD*)goidHolder;
        if (goid != 0) {
            const auto GetGo = (void*(__cdecl*)(DWORD))0x005207E1;
            go = GetGo(goid);
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
// At 0x0056BBD1, Player::RCSetWorldLocation calls ReportSys::Screen (0x0042C62E) with:
//   arg0 = "$MSG$%S has entered %S." (0x0079126C)
//   arg1 = hero_name (const wchar_t*)
//   arg2 = loc_name (const wchar_t*)
// When entering a configured PvP zone (402, 409, 901), we replace arg0 with
// "$MSG$%S has entered %S (PvP Zone)."
extern "C" void* g_reportSysScreenVa = (void*)0x0042C62E;

extern "C" const char* __cdecl SiegeHost_ChooseEnterFmt(void* callerEbp) {
    static const char kStockFmt[] = "$MSG$%S has entered %S.";
    static const char kPvpFmt[]   = "$MSG$%S has entered %S (PvP Zone).";

    if (!callerEbp || !Readable(callerEbp, 16)) return kStockFmt;

    unsigned loc = 0;
    // Check [ebp - 0x28] for Player* this
    if (Readable((char*)callerEbp - 0x28, 4)) {
        void* pl = *(void**)((char*)callerEbp - 0x28);
        if (pl && eng::Player_IsValid(pl)) {
            loc = *(unsigned*)((char*)pl + 0xF4);
        }
    }
    // Fallback: check [ebp + 8] for the loc argument passed to Player::RCSetWorldLocation
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
// Installation & Server Tick
// ---------------------------------------------------------------------------------------------------
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
            Log("PvpZone: zone-scoped dio_nothing armed - players keep all equipped items, inventory items, and gold "
                "when dying inside PvP zones, and drop as usual everywhere else (patched 0x0057803C, 0x0057815E, 0x005F0D46).");
        }
    }

    // 2. Install Zone-Based PvP hooks
    if (g_numPvpLocs <= 0) {
        Log("PvpZone: off (no PvP locations configured).");
        return;
    }

    // [A] Hook Player::IsEnemy at 0x0056D2B9 (9 bytes: 56 8B F1 8B 0D AC CF 7A 00)
    static const BYTE kIsEnemyExpect[9] = {0x56, 0x8B, 0xF1, 0x8B, 0x0D, 0xAC, 0xCF, 0x7A, 0x00};
    BYTE* pEnemy = (BYTE*)0x0056D2B9;
    if (memcmp(pEnemy, kIsEnemyExpect, 9) != 0) {
        Log("PvpZone: VERIFY FAILED at Player::IsEnemy (0x0056D2B9) - PvP zones not armed.");
        return;
    }
    {
        DWORD o;
        if (!VirtualProtect(pEnemy, 9, PAGE_EXECUTE_READWRITE, &o)) {
            Log("PvpZone: VirtualProtect failed at 0x0056D2B9 (%lu).", GetLastError());
            return;
        }
        pEnemy[0] = 0xE9;
        *(int32_t*)(pEnemy + 1) = (int32_t)((DWORD)(void*)&SiegeHost_PlayerIsEnemyStub - (0x0056D2B9 + 5));
        pEnemy[5] = 0x90; pEnemy[6] = 0x90; pEnemy[7] = 0x90; pEnemy[8] = 0x90;
        VirtualProtect(pEnemy, 9, o, &o);
        FlushInstructionCache(GetCurrentProcess(), pEnemy, 9);
    }

    // [C] On Client: hook friendly/healing spell team checks at 0x00513451 and 0x00513EB3
    if (Cfg().role == Role::Client) {
        struct SpellSite { DWORD va; void* stub; };
        const SpellSite kSpellSites[] = {
            {0x00513451, (void*)&SiegeHost_SpellTeamStubEdi},
            {0x00513EB3, (void*)&SiegeHost_SpellTeamStubEbx},
        };
        int hooked = 0;
        for (const auto& s : kSpellSites) {
            BYTE* p = (BYTE*)s.va;
            if (p[0] == 0xE8 && s.va + 5 + *(int32_t*)(p + 1) == 0x0056FC53) {
                DWORD o;
                if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
                    *(int32_t*)(p + 1) = (int32_t)((DWORD)s.stub - (s.va + 5));
                    VirtualProtect(p, 5, o, &o);
                    FlushInstructionCache(GetCurrentProcess(), p, 5);
                    ++hooked;
                }
            } else {
                Log("PvpZone: VERIFY FAILED on friendly spell check at 0x%08lX.", s.va);
            }
        }
        Log("PvpZone: client friendly-spell guard armed at %d of 2 sites (0x00513451, 0x00513EB3).", hooked);
    }

    // [D] Hook Rules::DamageGo cross-player damage guard at 0x005B58FA (8 bytes: 83 65 F0 00 80 7D 24 00)
    {
        static const BYTE kDmgExpect[8] = {0x83, 0x65, 0xF0, 0x00, 0x80, 0x7D, 0x24, 0x00};
        BYTE* pDmg = (BYTE*)0x005B58FA;
        if (memcmp(pDmg, kDmgExpect, 8) == 0) {
            DWORD o;
            if (VirtualProtect(pDmg, 8, PAGE_EXECUTE_READWRITE, &o)) {
                pDmg[0] = 0xE9;
                *(int32_t*)(pDmg + 1) = (int32_t)((DWORD)(void*)&SiegeHost_DamageGoStub - (0x005B58FA + 5));
                pDmg[5] = 0x90; pDmg[6] = 0x90; pDmg[7] = 0x90;
                VirtualProtect(pDmg, 8, o, &o);
                FlushInstructionCache(GetCurrentProcess(), pDmg, 8);
                Log("PvpZone: Rules::DamageGo cross-player filter armed at 0x005B58FA.");
            }
        } else {
            Log("PvpZone: VERIFY FAILED at Rules::DamageGo (0x005B58FA).");
        }
    }

    // [F] Hook on-enter PvP Zone chat suffix at 0x0056BBD1 in Player::RCSetWorldLocation
    // Replaces the 5-byte call to ReportSys::Screen (0x0042C62E) with SiegeHost_OnEnterLocationStub.
    {
        BYTE* p = (BYTE*)0x0056BBD1;
        if (p[0] == 0xE8 && (0x0056BBD1 + 5 + *(int32_t*)(p + 1) == 0x0042C62E)) {
            DWORD o;
            if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
                *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&SiegeHost_OnEnterLocationStub - (0x0056BBD1 + 5));
                VirtualProtect(p, 5, o, &o);
                FlushInstructionCache(GetCurrentProcess(), p, 5);
                Log("PvpZone: on-enter chat suffix hook armed at 0x0056BBD1.");
            } else {
                Log("PvpZone: VirtualProtect failed at 0x0056BBD1 (%lu).", GetLastError());
            }
        } else {
            Log("PvpZone: VERIFY FAILED at 0x0056BBD1 (expected call 0x0042C62E).");
        }
    }

    g_pvpOn = true;
    std::string locList;
    for (int i = 0; i < g_numPvpLocs; ++i) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s%u (%s)", i ? ", " : "", g_pvpLocs[i], LocationName(g_pvpLocs[i]));
        locList += buf;
    }
    Log("PvpZone: armed on locations [%s] (hooked Player::IsEnemy at 0x0056D2B9).", locList.c_str());
}

void PvpZoneTick() {
    if (Cfg().role != Role::Server) return;
    static DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (now - lastTick < 250) return;
    lastTick = now;

    // Only run while the server is in game.
    void* wsObj = eng::WorldState_Singleton();
    if (!wsObj) return;
    int ws = eng::WorldState_GetCurrentState(wsObj);
    if (ws != eng::WS_MP_INGAME && ws != eng::WS_MP_INGAME_JIP) return;

    void* srv = eng::Server_Singleton();
    if (!srv) return;

    // Keep Server+0x2C set to dio_all (4) globally so PvE deaths outside PvP zones drop items as usual.
    if (g_dioOn && *(int*)((char*)srv + 0x2C) != 4) {
        eng::Server_RCSetDropInvOption(srv, 4);
        Log("PvpZone: Server::RCSetDropInvOption(4 = dio_all outside PvP zones) -> Server+0x2C=%d", *(int*)((char*)srv + 0x2C));
    }

    if (!g_pvpOn) return;

    void** b = *(void***)((char*)srv + 4);
    void** e = *(void***)((char*)srv + 8);
    if (!b || !e || b >= e) return;

    // Track per-slot PvP state to log zone transitions and keep FriendTo/FriendToMe synced.
    static unsigned s_lastLoc[32] = {0};
    static bool s_wasPvp[32] = {false};

    void* players[32];
    DWORD ids[32];
    bool inPvp[32];
    int count = 0;
    DWORD allMask = 0;
    DWORD pvpMask = 0;

    int slot = 0;
    for (void** it = b; it != e && slot < 32; ++it, ++slot) {
        void* pl = *it;
        if (!pl || !eng::Player_IsValid(pl)) {
            s_lastLoc[slot] = 0;
            s_wasPvp[slot] = false;
            continue;
        }
        DWORD pid = *(DWORD*)((char*)pl + 0x0C);
        unsigned loc = *(unsigned*)((char*)pl + 0xF4);
        bool pvp = IsPvpCombatant(pl);

        if (loc != s_lastLoc[slot] || pvp != s_wasPvp[slot]) {
            if (pvp != s_wasPvp[slot] && IsHumanNonHost(pl)) {
                const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
                char nm[64] = "?";
                if (w) WideCharToMultiByte(CP_ACP, 0, w, -1, nm, sizeof nm, nullptr, nullptr);
                if (pvp) {
                    Log("PvpZone: player '%s' (id=0x%08lX) ENTERED PvP zone %u (%s).", nm, pid, loc, LocationName(loc));
                } else {
                    Log("PvpZone: player '%s' (id=0x%08lX) LEFT PvP zone -> now in PvE location %u.", nm, pid, loc);
                }
            }
            s_lastLoc[slot] = loc;
            s_wasPvp[slot] = pvp;
        }

        players[count] = pl;
        ids[count] = pid;
        inPvp[count] = pvp;
        allMask |= pid;
        if (pvp) pvpMask |= pid;
        ++count;
    }

    // For each player:
    // - If in a PvP zone: friendly with self and anyone NOT in a PvP zone; hostile to other players in a PvP zone.
    // - If in a PvE zone (or ServerHost): friendly with all players not in PvP, and if another player is in PvP they are still non-hostile to PvE players.
    for (int i = 0; i < count; ++i) {
        void* pl = players[i];
        DWORD pid = ids[i];
        DWORD wantFriend = inPvp[i] ? ((allMask & ~pvpMask) | pid) : allMask;
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
    char b[160];
    snprintf(b, sizeof b, "pvpzone=%s\ndio_nothing=%d\npvp_hits=%lu\npve_blocked=%lu\n",
             g_pvpOn ? Cfg().pvpLocations.c_str() : "off", g_dioOn ? 1 : 0, g_pvpAttacksAllowed, g_pveDamageBlocked);
    return b;
}
