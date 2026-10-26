// characterabilities.cpp — DS2-inspired Character Abilities for Dungeon Siege 1 MMO.
// 4 Stat-Tied Abilities unlocked at Discipline Level 30+:
//   [Q] Melee Ability:        Whirling Cleave (360° weapon sweep + physical AoE damage)
//   [W] Ranged Ability:       Shrapnel Blast (fan burst + piercing AoE damage)
//   [E] Nature Magic Ability: Aegis of Renewal (protective orbital shield + Armor Rating buff + burst healing + defensive pulse)
//   [R] Combat Magic Ability: Flame Nova (expanding elemental blast + heavy fire damage)

#include "characterabilities.h"
#include "siegehost.h"
#include "engine.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

extern "C" void __fastcall Orig_GoDefend_ResetModifiers(void* defend);
__asm__(
    ".text\n"
    ".globl _Orig_GoDefend_ResetModifiers\n"
    "_Orig_GoDefend_ResetModifiers:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %ecx\n"
    "  pushl %esi\n"
    "  movl %ecx, %esi\n"
    "  movl $0x005D3F54, %eax\n"
    "  jmp *%eax\n"
);

namespace {

struct SiegePos {
    float x;
    float y;
    float z;
    uint32_t node;
};

struct SiegeRot {
    float x;
    float y;
    float z;
    float w;
    uint32_t node;
};

struct AnimReq {
    int chore;
    int subAnim;
    int stance;
    int block;
    int flags;
};

#pragma pack(push, 1)
struct AbilityPacket {
    uint32_t magic;       // 0x41435344 ('DSCA')
    uint32_t heroGoid;    // Goid of the hero activating the ability
    uint8_t  slot;        // 0 = Q, 1 = W, 2 = E, 3 = R
    uint8_t  isBroadcast; // 0 = Client -> Server request, 1 = Server -> Client broadcast
    uint16_t reserved;    // 0
};
#pragma pack(pop)

const uint32_t kAbilityMagic = 0x41435344; // 'DSCA'

// DSLOA.exe v1.50 engine function pointers
const auto IsServerLocal               = (bool(__cdecl*)())0x00450B04;
const auto GetScreenHero               = (void*(__thiscall*)(void* srv))0x00570565;
const auto FindGo                      = (void*(__thiscall*)(void* godb, uint32_t goid))0x00550A6B;
const auto GoActor_GetSkillLevel       = (float(__thiscall*)(void* actor, const char* skill))0x0047FC6E;
const auto GoPlacement_GetPosition     = (const SiegePos*(__thiscall*)(void* placement))0x00540DFE;
const auto GoMind_IsEnemy              = (bool(__thiscall*)(void* mind, const void* targetGo))0x005DF1BE;
const auto GoMind_RSStop               = (void(__thiscall*)(void* mind, int origin))0x005DE632;
const auto AIQuery_IsInRange           = (bool(__stdcall*)(const void* goA, const void* goB, float range))0x005EADDF;
const auto MakeAnimReq                 = (AnimReq*(__cdecl*)(int chore))0x005BFCCC;
const auto GoBody_SAnimate             = (void(__thiscall*)(void* body, const AnimReq* req))0x005C0083;
const auto WorldFx_RunScript           = (void*(__thiscall*)(void* wfx, const char* script, uint32_t target, uint32_t source, const char* bone, uint32_t other, int event))0x00472F79;
const auto Rules_CalculateDamage       = (float(__thiscall*)(void* rules, uint32_t attacker, uint32_t weapon, uint32_t victim, float minDmg, float maxDmg, float totalDef, bool bPiercing))0x005B4678;
const auto Rules_DamageGo              = (bool(__thiscall*)(void* rules, uint32_t victim, uint32_t attacker, uint32_t weapon, float damage, bool piercing, bool canExplode))0x0047BE87;
const auto Rules_CalculateExperience   = (float(__thiscall*)(void* rules, const uint32_t* attacker, const uint32_t* weapon, const uint32_t* victim, float damage))0x005B6728;
const auto Rules_RCAwardPrimaryExperience = (void(__thiscall*)(void* rules, const uint32_t* goid, const char* skill, double xp, bool share))0x005B7194;
const auto Rules_RCPlayLevelUpSoundAndText = (void(__thiscall*)(void* rules, const uint32_t* goid, const char* skill))0x005B69A3;
const auto Rules_SetPackMemberStats    = (void(__thiscall*)(void* rules, const uint32_t* goid))0x005B7B57;
const auto Rules_ChangeLife            = (void(__thiscall*)(void* rules, const uint32_t* goid, float delta, unsigned long rpcId))0x005B802D;
const auto Rules_ChangeMana            = (void(__thiscall*)(void* rules, const uint32_t* goid, float delta, unsigned long rpcId))0x005B82EA;
const auto GoAspect_SSetNaturalMaxLife = (void(__thiscall*)(void* aspect, float maxLife))0x00534B1A;
const auto GoAspect_SSetNaturalMaxMana = (void(__thiscall*)(void* aspect, float maxMana))0x00534FD8;
const auto Aspect_SetCurrentLifeLocal  = (void(__thiscall*)(void* aspect, float life))0x005349D6;
const auto SSetCurrentLife             = (void(__thiscall*)(void* aspect, float life))0x005348D7;
const auto Sim_SExplodeGo              = (bool(__thiscall*)(void* sim, uint32_t goid, float magnitude, const float* dirVec3))0x005A3224;
const auto WorldMessage_Ctor           = (void*(__thiscall*)(void* msg, int event, uint32_t data1, uint32_t from, uint32_t to))0x005967F9;
const auto WorldMessage_Send           = (void(__thiscall*)(void* msg, int flags))0x005969E0;

struct PowerDef {
    char keyChar;
    const char* name;
    const char* statKey;
    const char* statDisplay;
    float reqLevel;
    DWORD cooldownMs;
    DWORD lastUsedMs;
};

PowerDef g_powers[4] = {
    { 'Q', "Whirling Cleave",  "melee",        "Melee",        30.0f, 30000, 0 },
    { 'W', "Shrapnel Blast",   "ranged",       "Ranged",       30.0f, 30000, 0 },
    { 'E', "Aegis of Renewal", "nature magic", "Nature Magic", 30.0f, 40000, 0 },
    { 'R', "Flame Nova",       "combat magic", "Combat Magic", 30.0f, 30000, 0 }
};

bool g_eatNextChar = false;
WPARAM g_eatCharVal = 0;

bool Readable(const void* p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0;
}

// ---- Active Aegis of Renewal Armor Rating Buffs (per Goid, works on both Client & Server) ----
struct AegisBuff {
    uint32_t goid;
    float    boost;
    DWORD    endTime;
};
const int kMaxAegisBuffs = 32;
AegisBuff g_aegisBuffs[kMaxAegisBuffs] = {};

float GetAegisBonus(uint32_t goid) {
    if (!goid) return 0.0f;
    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxAegisBuffs; ++i) {
        if (g_aegisBuffs[i].goid == goid) {
            if (g_aegisBuffs[i].endTime != 0 && (int32_t)(g_aegisBuffs[i].endTime - now) > 0) {
                return g_aegisBuffs[i].boost;
            }
            g_aegisBuffs[i].goid = 0;
            g_aegisBuffs[i].boost = 0.0f;
            g_aegisBuffs[i].endTime = 0;
            return 0.0f;
        }
    }
    return 0.0f;
}

void SetAegisBonus(void* hero, uint32_t goid, float boost, DWORD durationMs) {
    if (!goid) return;
    DWORD now = GetTickCount();
    DWORD endTime = now + durationMs;
    if (endTime == 0) endTime = 1;

    float oldBoost = 0.0f;
    int slot = -1;
    for (int i = 0; i < kMaxAegisBuffs; ++i) {
        if (g_aegisBuffs[i].goid == goid) {
            if (g_aegisBuffs[i].endTime != 0 && (int32_t)(g_aegisBuffs[i].endTime - now) > 0) {
                oldBoost = g_aegisBuffs[i].boost;
            }
            slot = i;
            break;
        }
        if (slot == -1 && g_aegisBuffs[i].goid == 0) slot = i;
    }
    if (slot == -1) slot = 0;

    g_aegisBuffs[slot].goid = goid;
    g_aegisBuffs[slot].boost = boost;
    g_aegisBuffs[slot].endTime = endTime;

    // Immediately apply delta to hero->m_Defend (+0x18) for instant 0-frame UI and combat update
    if (hero && Readable(hero, 0x40)) {
        void* defend = *(void**)((char*)hero + 0x18);
        if (defend && Readable(defend, 0x30)) {
            float delta = boost - oldBoost;
            if (delta != 0.0f) {
                *(float*)((char*)defend + 0x28) += delta;
            }
        }
    }
}

// ---- Award Experience to Specific Discipline Stat on Server ----
void AwardAbilityExperience(void* hero, uint32_t heroGoid, const char* statKey, double xp) {
    if (!hero || !heroGoid || !statKey || xp <= 0.0) return;
    void* gRules = *(void**)0x007AD0F4;
    void* actor  = *(void**)((char*)hero + 0x00);
    void* aspect = *(void**)((char*)hero + 0x04);
    if (!gRules || !actor || !aspect || !Readable(actor, 0x40) || !Readable(aspect, 0xD0)) return;

    int oldFloor = (int)floorf(GoActor_GetSkillLevel(actor, statKey));
    Rules_RCAwardPrimaryExperience(gRules, &heroGoid, statKey, xp, false);
    int newFloor = (int)floorf(GoActor_GetSkillLevel(actor, statKey));

    if (newFloor > oldFloor) {
        Rules_RCPlayLevelUpSoundAndText(gRules, &heroGoid, statKey);
        GoAspect_SSetNaturalMaxLife(aspect, *(float*)((char*)aspect + 0xA8));
        GoAspect_SSetNaturalMaxMana(aspect, *(float*)((char*)aspect + 0xC4));
        float lifeDelta = *(float*)((char*)aspect + 0xAC) - *(float*)((char*)aspect + 0xB0);
        float manaDelta = *(float*)((char*)aspect + 0xC8) - *(float*)((char*)aspect + 0xCC);
        if (lifeDelta > 0.0f) Rules_ChangeLife(gRules, &heroGoid, lifeDelta, 0);
        if (manaDelta > 0.0f) Rules_ChangeMana(gRules, &heroGoid, manaDelta, 0);
        Rules_SetPackMemberStats(gRules, &heroGoid);
    }
}

// ---- Radial AoE Scan (Damage + XP + Stagger / Attack Interrupt + Fatal Ragdoll) ----
void ApplyRadialAoE(void* hero, uint32_t heroGoid, const SiegePos* heroPos, int slot, float radius, float rawDamage, bool piercing, bool isServerAuth) {
    if (!hero || !heroPos) return;
    void* godb   = *(void**)0x007ACF4C;
    void* gRules = *(void**)0x007AD0F4;
    void* sim    = *(void**)0x007ACF78;
    void* heroMind = *(void**)((char*)hero + 0x34);
    if (!godb || !Readable(godb, 0x340)) return;

    uint32_t heroMask = *(uint32_t*)((char*)hero + 0x68);
    float radiusSq = radius * radius;

    const int kGoDbGoList = 0x338;
    const int kGoNext = 0x70;
    void* head = *(void**)((char*)godb + kGoDbGoList);
    if (!head || !Readable(head, 0x80)) return;
    void* first = *(void**)((char*)head + kGoNext);
    if (!first || !Readable(first, 0x80)) return;

    // Collect targets first so mutating life state / octree during iteration never invalidates the GoDb walk
    struct HitTarget {
        void*    go;
        uint32_t goid;
    };
    HitTarget targets[24];
    int numTargets = 0;

    int guard = 0;
    for (void* go = first; go && go != head && guard < 8192 && numTargets < 24; ++guard) {
        if (!Readable(go, 0x80)) break;
        void* next = *(void**)((char*)go + kGoNext);

        if (go != hero) {
            uint32_t targetMask = *(uint32_t*)((char*)go + 0x68);
            if ((heroMask & targetMask) != 0) {
                void* actor     = *(void**)((char*)go + 0x00);
                void* aspect    = *(void**)((char*)go + 0x04);
                void* placement = *(void**)((char*)go + 0x40);
                if (actor && aspect && placement && Readable(aspect, 0xB8) && Readable(placement, 0x30)) {
                    // Must be alive & conscious (LS_ALIVE_CONSCIOUS == 1) and not invincible
                    if (*(int*)((char*)aspect + 0xA0) == 1 && (*(uint32_t*)((char*)aspect + 0x3C) & 0x100) == 0) {
                        bool isEnemy = false;
                        if (heroMind && Readable(heroMind, 0x60)) {
                            isEnemy = GoMind_IsEnemy(heroMind, go);
                        } else {
                            void* heroActor = *(void**)((char*)hero + 0x00);
                            if (heroActor && Readable(heroActor, 0x20)) {
                                int heroAlign = *(int*)((char*)heroActor + 0x14);
                                int targetAlign = *(int*)((char*)actor + 0x14);
                                isEnemy = (targetAlign != 1 && targetAlign != heroAlign);
                            }
                        }

                        if (isEnemy) {
                            const SiegePos* targetPos = GoPlacement_GetPosition(placement);
                            if (targetPos) {
                                bool inRange = false;
                                if (targetPos->node == heroPos->node) {
                                    float dx = targetPos->x - heroPos->x;
                                    float dy = targetPos->y - heroPos->y;
                                    float dz = targetPos->z - heroPos->z;
                                    inRange = (dx * dx + dy * dy + dz * dz <= radiusSq);
                                } else {
                                    inRange = AIQuery_IsInRange(hero, go, radius);
                                }

                                if (inRange) {
                                    uint32_t targetGoid = *(uint32_t*)((char*)go + 0x4C);
                                    if (targetGoid) {
                                        targets[numTargets++] = { go, targetGoid };
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        go = next;
    }

    uint32_t nullGoid = *(uint32_t*)0x007ADC88;
    void* wfx = *(void**)0x007ACFB4;
    float totalXpAwarded = 0.0f;
    const float kMaxAbilityXp = 300.0f;

    for (int i = 0; i < numTargets; ++i) {
        void* targetGo      = targets[i].go;
        uint32_t targetGoid = targets[i].goid;
        if (!Readable(targetGo, 0x80)) continue;
        void* targetAspect    = *(void**)((char*)targetGo + 0x04);
        void* targetPlacement = *(void**)((char*)targetGo + 0x40);
        if (!targetAspect || !Readable(targetAspect, 0xB8)) continue;

        if (isServerAuth && rawDamage > 0.0f && gRules && Readable(gRules, 0x80)) {
            float def = ((float(__thiscall*)(void*, uint32_t))0x005B4422)(gRules, targetGoid);
            float reduction = def / (def + 150.0f);
            if (piercing) reduction *= 0.5f;
            float finalDmg = rawDamage * (1.0f - reduction);
            if (finalDmg < rawDamage * 0.40f) finalDmg = rawDamage * 0.40f;

            Log("Ability hit begin. Target=0x%08lX Dmg=%.0f.", targetGoid, finalDmg);
            float xp = Rules_CalculateExperience(gRules, &heroGoid, &heroGoid, &targetGoid, finalDmg);
            if (xp <= 0.0f) xp = finalDmg * 0.25f;

            // Multi-target diminishing returns: 100% on first 2 targets, 25% on subsequent targets, capped at 300 total XP
            float xpMult = (i < 2) ? 1.0f : 0.25f;
            xp *= xpMult;
            if (totalXpAwarded + xp > kMaxAbilityXp) {
                xp = kMaxAbilityXp - totalXpAwarded;
                if (xp < 0.0f) xp = 0.0f;
            }
            if (xp > 0.0f) {
                totalXpAwarded += xp;
                AwardAbilityExperience(hero, heroGoid, g_powers[slot].statKey, (double)xp);
            }

            // Pass nullGoid as weapon so engine's native DamageGo doesn't award weapon-based XP to equipped weapons
            Rules_DamageGo(gRules, targetGoid, heroGoid, nullGoid, finalDmg, piercing, true);
            Log("Ability hit done. Target=0x%08lX.", targetGoid);
        }

        // Spawn hit visual effect on the target (local only)
        if (wfx && Readable(wfx, 0x20)) {
            if (slot == 0) WorldFx_RunScript(wfx, "star_sparks", targetGoid, targetGoid, "", nullGoid, 0);
            else if (slot == 1) {
                WorldFx_RunScript(wfx, "arrow_tracer", targetGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "metal_shard_dust", targetGoid, targetGoid, "", nullGoid, 0);
            }
        }
    }
}

// ---- Apply Aegis of Renewal Buff & Heal to Nearby Living Allies ----
void ApplyAegisToAllies(void* hero, uint32_t heroGoid, const SiegePos* heroPos, float radius, float boost, DWORD durationMs, float level, bool isServerAuth) {
    if (!hero || !heroPos) return;
    void* godb   = *(void**)0x007ACF4C;
    void* gRules = *(void**)0x007AD0F4;
    void* wfx    = *(void**)0x007ACFB4;
    void* heroMind = *(void**)((char*)hero + 0x34);
    if (!godb || !Readable(godb, 0x340)) return;

    uint32_t heroMask = *(uint32_t*)((char*)hero + 0x68);
    float radiusSq = radius * radius;

    const int kGoDbGoList = 0x338;
    const int kGoNext = 0x70;
    void* head = *(void**)((char*)godb + kGoDbGoList);
    if (!head || !Readable(head, 0x80)) return;
    void* first = *(void**)((char*)head + kGoNext);
    if (!first || !Readable(first, 0x80)) return;

    struct AllyTarget {
        void*    go;
        uint32_t goid;
    };
    AllyTarget allies[24];
    int numAllies = 0;

    int guard = 0;
    for (void* go = first; go && go != head && guard < 8192 && numAllies < 24; ++guard) {
        if (!Readable(go, 0x80)) break;
        void* next = *(void**)((char*)go + kGoNext);

        if (go != hero) {
            uint32_t targetMask = *(uint32_t*)((char*)go + 0x68);
            if ((heroMask & targetMask) == 0) {
                void* actor     = *(void**)((char*)go + 0x00);
                void* aspect    = *(void**)((char*)go + 0x04);
                void* placement = *(void**)((char*)go + 0x40);
                if (actor && aspect && placement && Readable(aspect, 0xB8) && Readable(placement, 0x30)) {
                    if (*(int*)((char*)aspect + 0xA0) == 1) { // conscious
                        bool isEnemy = false;
                        if (heroMind && Readable(heroMind, 0x60)) {
                            isEnemy = GoMind_IsEnemy(heroMind, go);
                        }
                        if (!isEnemy) {
                            const SiegePos* targetPos = GoPlacement_GetPosition(placement);
                            if (targetPos) {
                                bool inRange = false;
                                if (targetPos->node == heroPos->node) {
                                    float dx = targetPos->x - heroPos->x;
                                    float dy = targetPos->y - heroPos->y;
                                    float dz = targetPos->z - heroPos->z;
                                    inRange = (dx * dx + dy * dy + dz * dz <= radiusSq);
                                } else {
                                    inRange = AIQuery_IsInRange(hero, go, radius);
                                }

                                if (inRange) {
                                    uint32_t targetGoid = *(uint32_t*)((char*)go + 0x4C);
                                    if (targetGoid) {
                                        allies[numAllies++] = { go, targetGoid };
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        go = next;
    }

    float totalAllyHealXp = 0.0f;

    for (int i = 0; i < numAllies; ++i) {
        void* allyGo = allies[i].go;
        uint32_t allyGoid = allies[i].goid;
        if (!Readable(allyGo, 0x80)) continue;
        void* allyAspect = *(void**)((char*)allyGo + 0x04);
        if (!allyAspect || !Readable(allyAspect, 0xC0)) continue;

        // Apply identical Armor Rating defense boost and duration to ally
        SetAegisBonus(allyGo, allyGoid, boost, durationMs);

        // Spawn barrier VFX on ally
        if (wfx && Readable(wfx, 0x20)) {
            WorldFx_RunScript(wfx, "armor_boost", allyGoid, allyGoid, "", *(uint32_t*)0x007ADC88, 0);
            WorldFx_RunScript(wfx, "shield", allyGoid, allyGoid, "", *(uint32_t*)0x007ADC88, 0);
        }

        // Heal ally with identical missing life formula
        float maxLife = *(float*)((char*)allyAspect + 0xAC);
        float curLife = *(float*)((char*)allyAspect + 0xB0);
        if (curLife < maxLife) {
            float heal = maxLife * 0.40f + level * 8.0f;
            float newLife = curLife + heal;
            if (newLife > maxLife) newLife = maxLife;
            float actualHeal = newLife - curLife;
            if (isServerAuth) {
                if (gRules && Readable(gRules, 0x80)) {
                    Rules_ChangeLife(gRules, &allyGoid, actualHeal, 0);
                } else {
                    SSetCurrentLife(allyAspect, newLife);
                }
                // Ally heal XP: 0.25x multiplier, max 50 XP per ally, capped at 100 XP total across all allies
                if (actualHeal > 0.0f && totalAllyHealXp < 100.0f) {
                    float allyXp = actualHeal * 0.25f;
                    if (allyXp > 50.0f) allyXp = 50.0f;
                    if (totalAllyHealXp + allyXp > 100.0f) allyXp = 100.0f - totalAllyHealXp;
                    if (allyXp > 0.0f) {
                        totalAllyHealXp += allyXp;
                        AwardAbilityExperience(hero, heroGoid, "nature magic", (double)allyXp);
                    }
                }
            } else {
                Aspect_SetCurrentLifeLocal(allyAspect, newLife);
            }
        }
    }
}

// ---- DirectPlay8 Packet Queue & Network Helpers ----
CRITICAL_SECTION g_pktCs;
bool g_pktCsInit = false;
AbilityPacket g_pktQueue[32] = {};
int g_pktHead = 0;
int g_pktTail = 0;
void* g_dp8Client = nullptr;
void* g_dp8Server = nullptr;

void SendAbilityPacketToServer(uint32_t heroGoid, int slot) {
    if (!::CharacterAbilities_IsServerSupported()) return;
    void* dpClient = g_dp8Client;
    if (!dpClient) {
        void* srv = *(void**)0x007ACF70;
        if (srv && Readable(srv, 0x100)) {
            dpClient = *(void**)((char*)srv + 0xE4);
        }
    }
    if (!dpClient || !Readable(dpClient, 4)) return;
    void** vtbl = *(void***)dpClient;
    if (!vtbl || !Readable(vtbl, 9 * sizeof(void*))) return;

    AbilityPacket pkt{};
    pkt.magic       = kAbilityMagic;
    pkt.heroGoid    = heroGoid;
    pkt.slot        = (uint8_t)slot;
    pkt.isBroadcast = 0;
    pkt.reserved    = 0;

    struct DpnBufferDesc {
        DWORD size;
        BYTE* data;
    } desc = { sizeof(pkt), (BYTE*)&pkt };

    DWORD asyncHandle = 0;
    typedef HRESULT(WINAPI* SendFn)(void* self, const DpnBufferDesc* buf, DWORD cBuf, DWORD timeout, void* ctx, DWORD* hAsync, DWORD flags);
    HRESULT hr = ((SendFn)vtbl[8])(dpClient, &desc, 1, 0, nullptr, &asyncHandle, 0x08 /*DPNSEND_GUARANTEED*/);
    if (SUCCEEDED(hr)) {
        Log("Ability packet sent to server. Hero=0x%08lX slot=%d.", heroGoid, slot);
    }
}

void BroadcastAbilityPacketToClients(uint32_t heroGoid, int slot) {
    void* dpServer = g_dp8Server;
    if (!dpServer) {
        void* srv = *(void**)0x007ACF70;
        if (srv && Readable(srv, 0x100)) {
            dpServer = *(void**)((char*)srv + 0xE8);
        }
    }
    if (!dpServer || !Readable(dpServer, 4)) return;
    void** vtbl = *(void***)dpServer;
    if (!vtbl || !Readable(vtbl, 15 * sizeof(void*))) return;

    AbilityPacket pkt{};
    pkt.magic       = kAbilityMagic;
    pkt.heroGoid    = heroGoid;
    pkt.slot        = (uint8_t)slot;
    pkt.isBroadcast = 1;
    pkt.reserved    = 0;

    struct DpnBufferDesc {
        DWORD size;
        BYTE* data;
    } desc = { sizeof(pkt), (BYTE*)&pkt };

    DWORD asyncHandle = 0;
    typedef HRESULT(WINAPI* SendToFn)(void* self, DWORD dpnid, const DpnBufferDesc* buf, DWORD cBuf, DWORD timeout, void* ctx, DWORD* hAsync, DWORD flags);
    HRESULT hr = ((SendToFn)vtbl[14])(dpServer, 0 /*DPNID_ALL_PLAYERS_GROUP*/, &desc, 1, 0, nullptr, &asyncHandle, 0x08 /*DPNSEND_GUARANTEED*/);
    if (SUCCEEDED(hr)) {
        Log("Ability broadcast to clients. Hero=0x%08lX slot=%d.", heroGoid, slot);
    }
}

bool IsChatActive() {
    if (eng::WindowVisible("console_edit", "game_console_mp")) return true;
    if (eng::WindowVisible("staging_char_name_edit_box", "staging_area_create_character")) return true;
    if (eng::WindowVisible("staging_area_chat_editbox", "staging_area")) return true;
    if (eng::WindowVisible("end_game_chat_editbox", "end_game_summary")) return true;
    return false;
}

// Print feedback directly into the in-game chat window / console box
void PrintGameChat(const char* text) {
    if (!text || !*text) return;
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

// 3-second cooldown per message type (same idea as the out-of-mana message).
// "requires level" and "cooling down" each have their own independent timer.
const DWORD kMsgCooldownMs = 3000;
DWORD g_lastReqMsg  = 0;
DWORD g_lastCoolMsg = 0;

void PrintThrottled(DWORD& last, const char* text) {
    DWORD now = GetTickCount();
    if (last && now - last < kMsgCooldownMs) return;
    last = now ? now : 1;
    PrintGameChat(text);
}

bool IsCharacterAliveAndConscious(void* character) {
    if (!character || !Readable(character, 0x80)) return false;
    void* aspect = *(void**)((char*)character + 0x04);
    if (!aspect || !Readable(aspect, 0xC0)) return false;
    // Must be LS_ALIVE_CONSCIOUS (1) — blocks unconscious (2), dead/decay (3..8), and ghost (9)
    if (*(int*)((char*)aspect + 0xA0) != 1) return false;
    if (*(float*)((char*)aspect + 0xB0) <= 0.0f) return false;
    return true;
}

void ExecuteCharacterAbility(int slot, void* hero, float level, bool isServerAuth) {
    if (slot < 0 || slot > 3 || !IsCharacterAliveAndConscious(hero)) return;

    void* placement   = *(void**)((char*)hero + 0x40);
    void* body        = *(void**)((char*)hero + 0x0C);
    void* aspect      = *(void**)((char*)hero + 0x04);
    uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
    uint32_t nullGoid = *(uint32_t*)0x007ADC88;

    void* wfx    = *(void**)0x007ACFB4;
    void* gRules = *(void**)0x007AD0F4;

    const SiegePos* pos = nullptr;
    if (placement && Readable(placement, 0x20)) {
        pos = GoPlacement_GetPosition(placement);
    }

    switch (slot) {
        case 0: { // [Q] Melee — Whirling Cleave
            if (wfx && Readable(wfx, 0x20)) {
                WorldFx_RunScript(wfx, "fury_spin", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "fury_earth_stomp", heroGoid, heroGoid, "", nullGoid, 0);
            }
            if (pos) {
                float extraLevel = (level > 30.0f) ? (level - 30.0f) : 0.0f;
                float damage = 225.0f + extraLevel * 6.0f;
                float radius = 5.8f; // Closely aligns with the 5.55m outer tip of the whirling fury cylinder
                ApplyRadialAoE(hero, heroGoid, pos, 0, radius, damage, false, isServerAuth);
            }
            Log("Whirling Cleave activated. Level=%.1f.", level);
            break;
        }

        case 1: { // [W] Ranged — Shrapnel Blast
            if (wfx && Readable(wfx, 0x20)) {
                WorldFx_RunScript(wfx, "trap_dust_explosion", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "dust_explosion_large", heroGoid, heroGoid, "", nullGoid, 0);
            }
            if (pos) {
                float extraLevel = (level > 30.0f) ? (level - 30.0f) : 0.0f;
                float damage = 162.5f + extraLevel * 4.5f;
                float radius = 6.5f; // Closely aligns with the outer edge of the shrapnel and dust shockwave
                ApplyRadialAoE(hero, heroGoid, pos, 1, radius, damage, true, isServerAuth);
            }
            Log("Shrapnel Blast activated. Level=%.1f.", level);
            break;
        }

        case 2: { // [E] Nature Magic — Aegis of Renewal
            DWORD durMs = 10000; // 10-second duration
            float boost = 80.0f + level * 6.0f;
            SetAegisBonus(hero, heroGoid, boost, durMs);

            if (wfx && Readable(wfx, 0x20)) {
                WorldFx_RunScript(wfx, "recharge_party", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "armor_boost", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "shield", heroGoid, heroGoid, "", nullGoid, 0);
            }

            // Burst Life Heal hero (if damaged)
            if (aspect && Readable(aspect, 0xC0)) {
                float maxLife = *(float*)((char*)aspect + 0xAC);
                float curLife = *(float*)((char*)aspect + 0xB0);
                if (curLife < maxLife) {
                    float heal = maxLife * 0.40f + level * 8.0f;
                    float newLife = curLife + heal;
                    if (newLife > maxLife) newLife = maxLife;
                    float actualHeal = newLife - curLife;
                    if (isServerAuth) {
                        if (gRules && Readable(gRules, 0x80)) {
                            Rules_ChangeLife(gRules, &heroGoid, actualHeal, 0);
                        } else {
                            SSetCurrentLife(aspect, newLife);
                        }
                        if (actualHeal > 0.0f) {
                            float heroXp = actualHeal * 0.25f;
                            if (heroXp > 150.0f) heroXp = 150.0f;
                            AwardAbilityExperience(hero, heroGoid, "nature magic", (double)heroXp);
                        }
                    } else {
                        Aspect_SetCurrentLifeLocal(aspect, newLife);
                    }
                }
            }

            if (pos) {
                // Apply Aegis of Renewal barrier, heal, and defense buff to all nearby allies (10.0m moderate range)
                ApplyAegisToAllies(hero, heroGoid, pos, 10.0f, boost, durMs, level, isServerAuth);

                // Defensive Outward Stagger Pulse (interrupts encroaching enemies' attack swings)
                float radius = 7.0f;
                ApplyRadialAoE(hero, heroGoid, pos, 2, radius, 0.0f, false, isServerAuth);
            }
            Log("Aegis of Renewal activated. Level=%.1f.", level);
            break;
        }

        case 3: { // [R] Combat Magic — Flame Nova
            if (wfx && Readable(wfx, 0x20)) {
                WorldFx_RunScript(wfx, "death_blast", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "fire_blast", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "fire_blast", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "fire_blast", heroGoid, heroGoid, "", nullGoid, 0);
                WorldFx_RunScript(wfx, "fire_blast", heroGoid, heroGoid, "", nullGoid, 0);
            }
            if (pos) {
                float extraLevel = (level > 30.0f) ? (level - 30.0f) : 0.0f;
                float damage = 277.5f + extraLevel * 7.5f;
                float radius = 8.5f; // Aligned with the 8.5m outer reach of the 360-degree flame nova
                ApplyRadialAoE(hero, heroGoid, pos, 3, radius, damage, true, isServerAuth);
            }
            Log("Flame Nova activated. Level=%.1f.", level);
            break;
        }
    }
}

} // namespace

extern "C" void __fastcall Hook_GoDefend_ResetModifiers(void* defend) {
    Orig_GoDefend_ResetModifiers(defend);
    if (!defend || !Readable(defend, 0x30)) return;
    void* go = *(void**)((char*)defend + 0x04);
    if (!go || !Readable(go, 0x50)) return;
    uint32_t goid = *(uint32_t*)((char*)go + 0x4C);
    float bonus = GetAegisBonus(goid);
    if (bonus > 0.0f) {
        *(float*)((char*)defend + 0x28) += bonus;
    }
}

static char g_connectedHostUrl[600] = "";

void CharacterAbilities_NotifyConnect(const char* hostUrl) {
    if (hostUrl) {
        strncpy(g_connectedHostUrl, hostUrl, sizeof(g_connectedHostUrl) - 1);
        g_connectedHostUrl[sizeof(g_connectedHostUrl) - 1] = '\0';
    }
}

bool CharacterAbilities_IsServerSupported() {
    // Both NA and EU shards (and single-player / local host) now run CharacterAbilities.
    // Because on-demand AWS EC2 shards receive new public IPv4 addresses after idle stop/start cycles,
    // do not gate the client HUD on a static IPv4 string.
    return true;
}

void CharacterAbilities_SetDirectPlay(void* dpClient, void* dpServer) {
    if (dpClient) g_dp8Client = dpClient;
    if (dpServer) g_dp8Server = dpServer;
}

void CharacterAbilities_Init() {
    if (!g_pktCsInit) {
        InitializeCriticalSection(&g_pktCs);
        g_pktCsInit = true;
    }

    // Hook GoDefend::ResetModifiers at 0x005D3F4D (7 bytes: 55 8B EC 51 56 8B F1)
    {
        const BYTE kExpect[7] = { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1 };
        BYTE* p = (BYTE*)0x005D3F4D;
        if (memcmp(p, kExpect, 7) == 0) {
            DWORD o;
            if (VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &o)) {
                p[0] = 0xE9;
                *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&Hook_GoDefend_ResetModifiers - (0x005D3F4D + 5));
                p[5] = 0x90;
                p[6] = 0x90;
                VirtualProtect(p, 7, o, &o);
                FlushInstructionCache(GetCurrentProcess(), p, 7);
            }
        }
    }

    Log("initialized - 4 stat-tied abilities ready (Q: Cleave, W: Shrapnel, E: Aegis, R: Nova).");
}

bool CharacterAbilities_OnReceivePacket(bool isServer, DWORD /*senderDpnid*/, const BYTE* data, DWORD size) {
    if (!data || size != sizeof(AbilityPacket)) return false;
    const AbilityPacket* pkt = (const AbilityPacket*)data;
    if (pkt->magic != kAbilityMagic || pkt->slot > 3) return false;
    if (!CharacterAbilities_IsServerSupported()) return true; // Consume to avoid native dispatcher crash!
    if (isServer && pkt->isBroadcast != 0) return false;
    if (!isServer && pkt->isBroadcast == 0) return false;

    if (isServer) {
        Log("Ability packet received on server. Hero=0x%08lX slot=%d.", pkt->heroGoid, (int)pkt->slot);
    }

    if (g_pktCsInit) {
        EnterCriticalSection(&g_pktCs);
        int nextTail = (g_pktTail + 1) % 32;
        if (nextTail != g_pktHead) {
            g_pktQueue[g_pktTail] = *pkt;
            g_pktTail = nextTail;
        }
        LeaveCriticalSection(&g_pktCs);
    }
    return true;
}

void CharacterAbilities_Tick() {
    if (!CharacterAbilities_IsServerSupported()) return;
    // Process queued network packets (Client -> Server requests or Server -> Client broadcasts)
    if (!g_pktCsInit) return;
    AbilityPacket localPkts[16];
    int count = 0;

    EnterCriticalSection(&g_pktCs);
    while (g_pktHead != g_pktTail && count < 16) {
        localPkts[count++] = g_pktQueue[g_pktHead];
        g_pktHead = (g_pktHead + 1) % 32;
    }
    LeaveCriticalSection(&g_pktCs);

    if (count == 0) return;

    void* godb = *(void**)0x007ACF4C;
    if (!godb || !Readable(godb, 0x40)) return;

    bool serverLocal = IsServerLocal();
    uint32_t myHeroGoid = 0;
    if (!serverLocal) {
        void* srv = *(void**)0x007ACF70;
        if (srv && Readable(srv, 0x20)) {
            void* myHero = GetScreenHero(srv);
            if (myHero && Readable(myHero, 0x80)) {
                myHeroGoid = *(uint32_t*)((char*)myHero + 0x4C);
            }
        }
    }

    for (int i = 0; i < count; ++i) {
        const AbilityPacket& pkt = localPkts[i];
        if (!serverLocal && pkt.heroGoid == myHeroGoid) {
            // Ignore broadcast echo of our own ability (already executed locally at keypress)
            continue;
        }

        void* hero = FindGo(godb, pkt.heroGoid);
        if (!hero || !Readable(hero, 0x80)) continue;
        if (!IsCharacterAliveAndConscious(hero)) continue;
        void* actor = *(void**)((char*)hero + 0x00);
        if (!actor || !Readable(actor, 0x40)) continue;

        float level = GoActor_GetSkillLevel(actor, g_powers[pkt.slot].statKey);
        if (level < g_powers[pkt.slot].reqLevel) continue;

        ExecuteCharacterAbility(pkt.slot, hero, level, serverLocal);

        if (serverLocal && pkt.isBroadcast == 0) {
            BroadcastAbilityPacketToClients(pkt.heroGoid, pkt.slot);
        }
    }
}

bool CharacterAbilities_FilterKey(LPMSG msg) {
    if (!msg) return false;
    if (!CharacterAbilities_IsServerSupported()) return false;

    // Handle character eating to avoid letter leaking if key was consumed
    if (msg->message == WM_CHAR) {
        if (g_eatNextChar && (msg->wParam == g_eatCharVal ||
                              msg->wParam == 'q' || msg->wParam == 'Q' ||
                              msg->wParam == 'w' || msg->wParam == 'W' ||
                              msg->wParam == 'e' || msg->wParam == 'E' ||
                              msg->wParam == 'r' || msg->wParam == 'R')) {
            g_eatNextChar = false;
            g_eatCharVal = 0;
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
            return true;
        }
        return false;
    }

    if (msg->message == WM_KEYUP) {
        if (msg->wParam == 'Q' || msg->wParam == 'W' || msg->wParam == 'E' || msg->wParam == 'R') {
            if (!IsChatActive()) {
                msg->message = WM_NULL;
                msg->wParam = 0;
                msg->lParam = 0;
                return true;
            }
        }
        return false;
    }

    if (msg->message != WM_KEYDOWN) return false;

    // Check which slot
    WPARAM key = msg->wParam;
    int slot = -1;
    switch (key) {
        case 'Q': slot = 0; break;
        case 'W': slot = 1; break;
        case 'E': slot = 2; break;
        case 'R': slot = 3; break;
        default: return false;
    }

    // If chat edit box is open, never intercept: allow normal typing
    if (IsChatActive()) return false;

    // Must be in game and controlling a hero
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x20)) return false;

    void* hero = GetScreenHero(srv);
    if (!hero || !Readable(hero, 0x80)) return false;

    void* actor = *(void**)((char*)hero + 0x00);
    if (!actor || !Readable(actor, 0x40)) return false;

    // Cannot use abilities while unconscious, dead, or a ghost (no chat messages)
    if (!IsCharacterAliveAndConscious(hero)) {
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
        g_eatNextChar = true;
        g_eatCharVal = key;
        return true;
    }

    // Check discipline level requirement
    float level = GoActor_GetSkillLevel(actor, g_powers[slot].statKey);
    if (level < g_powers[slot].reqLevel) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s requires %s level %.1f (Current: %.1f).",
                 g_powers[slot].name, g_powers[slot].statDisplay, g_powers[slot].reqLevel, level);
        PrintThrottled(g_lastReqMsg, buf);

        // Neutralize key so vanilla hotkey does not fire
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
        g_eatNextChar = true;
        g_eatCharVal = key;
        return true;
    }

    // Check cooldown
    DWORD now = GetTickCount();
    if (g_powers[slot].lastUsedMs != 0 && now - g_powers[slot].lastUsedMs < g_powers[slot].cooldownMs) {
        float remaining = (float)(g_powers[slot].cooldownMs - (now - g_powers[slot].lastUsedMs)) / 1000.0f;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s cooling down (%.1fs remaining).",
                 g_powers[slot].name, remaining);
        PrintThrottled(g_lastCoolMsg, buf);

        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
        g_eatNextChar = true;
        g_eatCharVal = key;
        return true;
    }

    // Successfully trigger ability!
    g_powers[slot].lastUsedMs = now;

    bool serverLocal = IsServerLocal();
    ExecuteCharacterAbility(slot, hero, level, serverLocal);
    if (!serverLocal) {
        uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
        SendAbilityPacketToServer(heroGoid, slot);
    }

    // Consume input
    msg->message = WM_NULL;
    msg->wParam = 0;
    msg->lParam = 0;
    g_eatNextChar = true;
    g_eatCharVal = key;
    return true;
}
