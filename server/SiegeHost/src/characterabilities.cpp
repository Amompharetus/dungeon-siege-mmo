// characterabilities.cpp — DS2-inspired Character Abilities for Dungeon Siege 1 MMO.
// 4 Stat-Tied Abilities unlocked at Discipline Level 30+:
//   [Q] Melee Ability:        Whirling Cleave (360° weapon sweep + physical AoE damage)
//   [W] Ranged Ability:       Shrapnel Blast (fan burst + piercing AoE damage)
//   [E] Nature Magic Ability: Aegis of Renewal (protective orbital shield + Armor Rating buff + burst healing + defensive pulse)
//   [R] Combat Magic Ability: Flame Nova (expanding elemental blast + heavy fire damage)

#include "characterabilities.h"
#include "sharedstash.h"
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

extern "C" bool __fastcall Orig_GoMind_Validate(void* thisMind, void* unused_edx, const void* jobReq);
__asm__(
    ".text\n"
    ".globl _Orig_GoMind_Validate\n"
    ".globl @Orig_GoMind_Validate@12\n"
    "_Orig_GoMind_Validate:\n"
    "@Orig_GoMind_Validate@12:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0x14, %esp\n"
    "  movl $0x005DAE91, %eax\n"
    "  jmp *%eax\n"
);

extern "C" bool __fastcall Orig_UIPartyManager_IsPlayerPartyMemberBusy(void* thisMgr, void* unused_edx, void* go, uint32_t bIgnoreCharWin);
__asm__(
    ".text\n"
    ".globl _Orig_UIPartyManager_IsPlayerPartyMemberBusy\n"
    ".globl @Orig_UIPartyManager_IsPlayerPartyMemberBusy@16\n"
    "_Orig_UIPartyManager_IsPlayerPartyMemberBusy:\n"
    "@Orig_UIPartyManager_IsPlayerPartyMemberBusy@16:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  pushl %ecx\n"
    "  pushl %ecx\n"
    "  movl $0x004A68E7, %eax\n"
    "  jmp *%eax\n"
);

extern "C" void __fastcall Orig_GoActor_AddGenericState(void* thisActor, void* unused_edx, const char* sName, const char* sDescription, uint32_t expireLo, uint32_t expireHi, uint32_t casterGoid, uint32_t spellGoid, float spellLevel);
__asm__(
    ".text\n"
    ".globl _Orig_GoActor_AddGenericState\n"
    ".globl @Orig_GoActor_AddGenericState@36\n"
    "_Orig_GoActor_AddGenericState:\n"
    "@Orig_GoActor_AddGenericState@36:\n"
    "  pushl %ebp\n"
    "  movl %esp, %ebp\n"
    "  subl $0xA0, %esp\n"
    "  movl $0x005BD32A, %eax\n"
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
    uint32_t magic;            // 0x41435344 ('DSCA')
    uint32_t heroGoid;         // Goid of the hero activating the ability
    uint8_t  slot;             // 0 = Q, 1 = W, 2 = E, 3 = R
    uint8_t  isBroadcast;      // 0 = Client -> Server request, 1 = Server -> Client broadcast
    uint16_t skillLevelTenths; // Server-authoritative skill level * 10 on broadcast (0 on client request)
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
const auto Rules_CalculateExperience   = (float(__thiscall*)(void* rules, uint32_t attacker, uint32_t weapon, uint32_t victim, float damage))0x005B6728;
const auto Rules_RCAwardPrimaryExperience = (void(__thiscall*)(void* rules, uint32_t goid, const char* skill, double xp, bool share))0x005B7194;
const auto Rules_RCPlayLevelUpSoundAndText = (void(__thiscall*)(void* rules, uint32_t goid, const char* skill))0x005B69A3;
const auto Rules_SetPackMemberStats    = (void(__thiscall*)(void* rules, uint32_t goid))0x005B7B57;
const auto Rules_ChangeLife            = (void(__thiscall*)(void* rules, uint32_t goid, float delta, unsigned long rpcId))0x005B802D;
const auto Rules_ChangeMana            = (void(__thiscall*)(void* rules, uint32_t goid, float delta, unsigned long rpcId))0x005B82EA;
const auto GoAspect_SSetNaturalMaxLife = (void(__thiscall*)(void* aspect, float maxLife))0x00534B1A;
const auto GoAspect_SSetNaturalMaxMana = (void(__thiscall*)(void* aspect, float maxMana))0x00534FD8;
const auto Aspect_SetCurrentLifeLocal  = (void(__thiscall*)(void* aspect, float life))0x005349D6;
const auto SSetCurrentLife             = (void(__thiscall*)(void* aspect, float life))0x005348D7;
const auto Sim_SExplodeGo              = (bool(__thiscall*)(void* sim, uint32_t goid, float magnitude, const float* dirVec3))0x005A3224;
const auto Go_PlayVoiceSound           = (uint32_t(__thiscall*)(void* go, const char* eventName, bool distanced))0x00539D77;
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
    { 'E', "Aegis of Renewal", "nature magic", "Nature Magic", 30.0f, 30000, 0 },
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
    if (!(mbi.Protect & ok)) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}

// ---- Active Aegis of Renewal Armor Rating Buffs (per Target + Caster pair, stacks independently across casters) ----
struct AegisBuff {
    uint32_t targetGoid;
    uint32_t casterGoid;
    float    boost;
    DWORD    endTime;
};
const int kMaxAegisBuffs = 256;
AegisBuff g_aegisBuffs[kMaxAegisBuffs] = {};

float GetAegisBonus(uint32_t targetGoid) {
    if (!targetGoid) return 0.0f;
    DWORD now = GetTickCount();
    float total = 0.0f;
    for (int i = 0; i < kMaxAegisBuffs; ++i) {
        if (g_aegisBuffs[i].targetGoid == targetGoid) {
            if (g_aegisBuffs[i].endTime != 0 && (int32_t)(g_aegisBuffs[i].endTime - now) > 0) {
                total += g_aegisBuffs[i].boost;
            } else {
                g_aegisBuffs[i].targetGoid = 0;
                g_aegisBuffs[i].casterGoid = 0;
                g_aegisBuffs[i].boost = 0.0f;
                g_aegisBuffs[i].endTime = 0;
            }
        }
    }
    return total;
}

void SetAegisBonus(void* targetGo, uint32_t targetGoid, uint32_t casterGoid, float boost, DWORD durationMs) {
    if (!targetGoid || !casterGoid) return;
    DWORD now = GetTickCount();
    DWORD endTime = now + durationMs;
    if (endTime == 0) endTime = 1;

    float oldBoost = 0.0f;
    int slot = -1;
    for (int i = 0; i < kMaxAegisBuffs; ++i) {
        // Reclaim expired entries opportunistically so slots stay clean
        if (g_aegisBuffs[i].targetGoid != 0 && g_aegisBuffs[i].endTime != 0 &&
            (int32_t)(g_aegisBuffs[i].endTime - now) <= 0 &&
            !(g_aegisBuffs[i].targetGoid == targetGoid && g_aegisBuffs[i].casterGoid == casterGoid)) {
            if (slot == -1) slot = i;
            continue;
        }
        if (g_aegisBuffs[i].targetGoid == targetGoid && g_aegisBuffs[i].casterGoid == casterGoid) {
            if (g_aegisBuffs[i].endTime != 0 && (int32_t)(g_aegisBuffs[i].endTime - now) > 0) {
                oldBoost = g_aegisBuffs[i].boost;
            }
            slot = i;
            break;
        }
        if (slot == -1 && g_aegisBuffs[i].targetGoid == 0) slot = i;
    }
    if (slot == -1) slot = 0;

    g_aegisBuffs[slot].targetGoid = targetGoid;
    g_aegisBuffs[slot].casterGoid = casterGoid;
    g_aegisBuffs[slot].boost = boost;
    g_aegisBuffs[slot].endTime = endTime;

    // Immediately apply delta to targetGo->m_Defend (+0x18) for instant 0-frame UI and combat update
    if (targetGo && Readable(targetGo, 0x40)) {
        void* defend = *(void**)((char*)targetGo + 0x18);
        if (defend && Readable(defend, 0x30)) {
            float delta = boost - oldBoost;
            if (delta != 0.0f) {
                *(float*)((char*)defend + 0x28) += delta;
            }
        }
    }
}

void ExpireAegisBuffs() {
    DWORD now = GetTickCount();
    void* godb = *(void**)0x007ACF4C;
    for (int i = 0; i < kMaxAegisBuffs; ++i) {
        if (g_aegisBuffs[i].targetGoid != 0 && g_aegisBuffs[i].endTime != 0 &&
            (int32_t)(g_aegisBuffs[i].endTime - now) <= 0) {
            uint32_t goid = g_aegisBuffs[i].targetGoid;
            float expiredBoost = g_aegisBuffs[i].boost;
            g_aegisBuffs[i].targetGoid = 0;
            g_aegisBuffs[i].casterGoid = 0;
            g_aegisBuffs[i].boost = 0.0f;
            g_aegisBuffs[i].endTime = 0;
            if (expiredBoost > 0.0f && godb && Readable(godb, 0x40)) {
                void* go = FindGo(godb, goid);
                if (go && Readable(go, 0x40)) {
                    void* defend = *(void**)((char*)go + 0x18);
                    if (defend && Readable(defend, 0x30)) {
                        float newDef = *(float*)((char*)defend + 0x28) - expiredBoost;
                        if (newDef < 0.0f) newDef = 0.0f;
                        *(float*)((char*)defend + 0x28) = newDef;
                    }
                }
            }
        }
    }
}

// Experience table directly from formulas.gas (levels 0 to 150)
static const double kExperienceTable[] = {
    0.0, 70.0, 650.0, 1450.0, 2350.0, 3350.0, 4650.0, 6650.0, 9450.0, 12950.0, 17100.0, 22000.0, 27800.0,
    34300.0, 41300.0, 49191.0, 58327.0, 68882.0, 81054.0, 95071.0, 111190.0, 129706.0, 150954.0,
    175317.0, 203567.0, 236338.0, 274352.0, 318449.0, 369600.0, 428936.0, 497766.0, 577609.0,
    670226.0, 777662.0, 902288.0, 1046855.0, 1214551.0, 1409080.0, 1634732.0, 1896489.0,
    2200128.0, 2552348.0, 2960924.0, 3434872.0, 3984651.0, 4622395.0, 5362179.0, 6220327.0,
    7215779.0, 8370504.0, 9709985.0, 11263782.0, 13066188.0, 15156978.0, 17582294.0,
    20395661.0, 23659167.0, 27444833.0, 31836207.0, 36930200.0, 42839232.0, 49780637.0,
    58283859.0, 68700306.0, 81460453.0, 97091634.0, 116239830.0, 139696370.0, 168430631.0,
    203630102.0, 246749453.0, 299570658.0, 364276635.0, 443541456.0, 540640862.0, 659587634.0,
    805297430.0, 983791930.0, 1202447693.0, 1470301002.0, 1798421306.0, 2200368678.0,
    2692754209.0, 3295926485.0, 4034812522.0, 4939947918.0, 6048738778.0, 7407007581.0,
    9070886865.0, 11109138988.0, 13605997839.0, 16664649931.0, 20411498744.0,
    25001388540.0, 30624003540.0, 37511706915.0, 45949143550.0, 56285003427.0,
    68946431776.0, 84456681504.0, 103456737421.0, 126731805919.0, 155243764829.0,
    190170914494.0, 232956672834.0, 285369226800.0, 349574605408.0, 428226194203.0,
    524574390477.0, 642600930913.0, 787183442946.0, 964297020188.0, 1181261152309.0,
    1447042214156.0, 1772624014920.0, 2171461720855.0, 2660037910626.0,
    3258543743095.0, 3991713387870.0, 4889846202719.0, 5990058900910.0,
    7337819456193.0, 8988826136414.0, 11011309319686.0, 13488851219194.0,
    16523840046091.0, 20241701359039.0, 24796081467402.0, 30375197100145.0,
    37209613750256.0, 45581774146643.0, 55837670632215.0, 68401143827042.0,
    83791398490705.0, 102644460453692.0, 125739461358351.0,
    154030837466559.0, 188687773199113.0, 231142519471492.0,
    283149583655156.0, 346858237280144.0, 424901337970755.0,
    520504136316754.0, 637617564290602.0, 781081513558566.0,
    956824851411821.0, 1172110440282060.0, 1435835286648100.0,
    1758898223446500.0, 2154650321024540.0, 2639446640557650.0
};

static double CalculateLevelToXP(float level) {
    if (level <= 0.0f) return 0.0;
    int maxIdx = (int)(sizeof(kExperienceTable) / sizeof(kExperienceTable[0])) - 1;
    if (level >= (float)maxIdx) return kExperienceTable[maxIdx];
    int fl = (int)floorf(level);
    int ce = fl + 1;
    if (ce > maxIdx) ce = maxIdx;
    double frac = (double)(level - (float)fl);
    return kExperienceTable[fl] + frac * (kExperienceTable[ce] - kExperienceTable[fl]);
}

// ---- Award Experience to Specific Discipline Stat on Server ----
void AwardAbilityExperience(void* hero, uint32_t heroGoid, const char* statKey, double xp) {
    if (!hero || !heroGoid || !statKey || xp <= 0.0) return;
    void* gRules = *(void**)0x007AD0F4;
    void* actor  = *(void**)((char*)hero + 0x00);
    void* aspect = *(void**)((char*)hero + 0x04);
    if (!gRules || !actor || !aspect || !Readable(actor, 0x40) || !Readable(aspect, 0xD0)) return;

    int oldFloor = (int)floorf(GoActor_GetSkillLevel(actor, statKey));
    float oldNatLife = *(float*)((char*)aspect + 0xA8);
    float oldNatMana = *(float*)((char*)aspect + 0xC4);
    Rules_RCAwardPrimaryExperience(gRules, heroGoid, statKey, xp, false);
    int newFloor = (int)floorf(GoActor_GetSkillLevel(actor, statKey));
    float newNatLife = *(float*)((char*)aspect + 0xA8);
    float newNatMana = *(float*)((char*)aspect + 0xC4);

    if (newFloor > oldFloor) {
        Rules_RCPlayLevelUpSoundAndText(gRules, heroGoid, statKey);
        GoAspect_SSetNaturalMaxLife(aspect, newNatLife);
        GoAspect_SSetNaturalMaxMana(aspect, newNatMana);
        float lifeDelta = *(float*)((char*)aspect + 0xAC) - *(float*)((char*)aspect + 0xB0);
        float manaDelta = *(float*)((char*)aspect + 0xC8) - *(float*)((char*)aspect + 0xCC);
        if (lifeDelta > 0.0f) Rules_ChangeLife(gRules, heroGoid, lifeDelta, 0);
        if (manaDelta > 0.0f) Rules_ChangeMana(gRules, heroGoid, manaDelta, 0);
        Rules_SetPackMemberStats(gRules, heroGoid);
    } else if (newNatLife > oldNatLife || newNatMana > oldNatMana) {
        // Secondary attribute (Strength / Dexterity / Intelligence) leveled up inside RCAwardPrimaryExperience
        GoAspect_SSetNaturalMaxLife(aspect, newNatLife);
        GoAspect_SSetNaturalMaxMana(aspect, newNatMana);
        float lifeDelta = *(float*)((char*)aspect + 0xAC) - *(float*)((char*)aspect + 0xB0);
        float manaDelta = *(float*)((char*)aspect + 0xC8) - *(float*)((char*)aspect + 0xCC);
        if (lifeDelta > 0.0f) Rules_ChangeLife(gRules, heroGoid, lifeDelta, 0);
        if (manaDelta > 0.0f) Rules_ChangeMana(gRules, heroGoid, manaDelta, 0);
        Rules_SetPackMemberStats(gRules, heroGoid);
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

    // Dynamic next-level XP cap based on formulas.gas (later_levels = 0.025 = 2.5% of next level)
    void* heroActor = *(void**)((char*)hero + 0x00);
    float heroSkillLevel = (heroActor && Readable(heroActor, 0x40)) ? GoActor_GetSkillLevel(heroActor, g_powers[slot].statKey) : 30.0f;
    if (heroSkillLevel < 30.0f) heroSkillLevel = 30.0f;
    int curLevelFloor = (int)floorf(heroSkillLevel);
    double curLevelXp = CalculateLevelToXP((float)curLevelFloor);
    double nextLevelXp = CalculateLevelToXP((float)(curLevelFloor + 1));
    double deltaXp = nextLevelXp - curLevelXp;
    const float kMaxAbilityXp = (deltaXp > 0.0) ? (float)(deltaXp * 0.025) : 2000.0f;

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
            if (piercing) {
                // W (slot 1): ignores 30% of enemy armor reduction
                // R (slot 3): ignores 50% of enemy armor reduction
                float armorIgnore = (slot == 1) ? 0.30f : 0.50f;
                reduction *= (1.0f - armorIgnore);
            }
            float finalDmg = rawDamage * (1.0f - reduction);
            if (finalDmg < rawDamage * 0.40f) finalDmg = rawDamage * 0.40f;

            Log("Ability hit begin. Target=0x%08lX Dmg=%.0f.", targetGoid, finalDmg);

            // Calculate XP from monster aspect properties (0x50 = total XP, 0x54 = remaining XP pool, 0xAC = max life)
            float targetBaseXp = *(float*)((char*)targetAspect + 0x50);
            float targetCurXp  = *(float*)((char*)targetAspect + 0x54);
            float targetMaxHp  = *(float*)((char*)targetAspect + 0xAC);
            float xp = 0.0f;
            if (targetMaxHp > 0.0f && targetBaseXp > 0.0f) {
                xp = targetBaseXp * (finalDmg / targetMaxHp);
                if (targetCurXp > 0.0f && xp > targetCurXp) xp = targetCurXp;
            } else {
                xp = finalDmg * 0.25f;
            }

            // Deduct awarded raw XP from monster's available pool
            if (targetCurXp > 0.0f && xp > 0.0f) {
                float remaining = targetCurXp - xp;
                if (remaining < 0.0f) remaining = 0.0f;
                *(float*)((char*)targetAspect + 0x54) = remaining;
            }

            // Multi-target diminishing returns: 100% on first 2 targets, 25% on subsequent targets, capped at 2.5% next-level XP
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
            Log("Ability hit done. Target=0x%08lX XP=%.1f.", targetGoid, xp);
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

// ---- Helper: Resolve non-host human Player* owning a character, companion, or summon ----
void* GetOwningHumanPlayer(void* go) {
    void* cur = go;
    for (int depth = 0; depth < 4 && cur && Readable(cur, 0x80); ++depth) {
        void* pl = *(void**)((char*)cur + 0x78);
        if (pl && Readable(pl, 0x100) && eng::Player_IsValid(pl)) {
            if (*(int*)((char*)pl + 0x14) != 1) return nullptr; // 1 = PC_HUMAN (excludes PC_COMPUTER critters/NPCs/monsters)
            const wchar_t* w = *(const wchar_t**)((char*)pl + 0x58);
            if (w && Readable(w, sizeof(wchar_t)) && *w) {
                if (_wcsicmp(w, L"ServerHost") == 0) return nullptr;
            }
            return pl;
        }
        void* next = *(void**)((char*)cur + 0x6C);
        if (!next || next == cur) break;
        cur = next;
    }
    return nullptr;
}

// ---- Helper: True when casterGo and targetGo belong to allied human players (everywhere except PvP zones) ----
bool AreActorsAllied(void* casterGo, void* heroMind, void* targetGo) {
    void* casterPl = GetOwningHumanPlayer(casterGo);
    void* targetPl = GetOwningHumanPlayer(targetGo);
    if (!casterPl || !targetPl) return false;
    if (casterPl == targetPl) return true;

    // Player::IsEnemy (0x0056D2B9) is hooked by PvpZone on both Client and Server:
    // returns true ONLY when both human players are inside one of the 3 PvP zones, and false everywhere else.
    typedef bool(__thiscall* PlayerIsEnemyFn)(void*, const void*);
    if (((PlayerIsEnemyFn)0x0056D2B9)(casterPl, targetPl)) {
        return false;
    }
    if (heroMind && Readable(heroMind, 0x60) && GoMind_IsEnemy(heroMind, targetGo)) {
        return false;
    }
    return true;
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
    AllyTarget allies[64];
    int numAllies = 0;

    int guard = 0;
    for (void* go = first; go && go != head && guard < 8192 && numAllies < 64; ++guard) {
        if (!Readable(go, 0x80)) break;
        void* next = *(void**)((char*)go + kGoNext);

        if (go != hero) {
            uint32_t targetMask = *(uint32_t*)((char*)go + 0x68);
            if ((heroMask & targetMask) != 0) {
                void* actor     = *(void**)((char*)go + 0x00);
                void* aspect    = *(void**)((char*)go + 0x04);
                void* placement = *(void**)((char*)go + 0x40);
                if (actor && aspect && placement && Readable(aspect, 0xB8) && Readable(placement, 0x30)) {
                    if (*(int*)((char*)aspect + 0xA0) == 1 && (*(uint32_t*)((char*)aspect + 0x3C) & 0x100) == 0) { // conscious & not invincible
                        if (AreActorsAllied(hero, heroMind, go)) {
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
    int curLevelFloor = (int)floorf(level);
    double curLevelXp = CalculateLevelToXP((float)curLevelFloor);
    double nextLevelXp = CalculateLevelToXP((float)(curLevelFloor + 1));
    double deltaXp = nextLevelXp - curLevelXp;
    if (deltaXp <= 0.0) deltaXp = 79843.0; // Level 30 delta fallback
    float maxAllyTotalXp = (float)(deltaXp * 0.005); // 0.5% next-level cap across allies
    float maxPerAllyXp   = maxAllyTotalXp * 0.5f;    // 0.25% max per individual ally

    for (int i = 0; i < numAllies; ++i) {
        void* allyGo = allies[i].go;
        uint32_t allyGoid = allies[i].goid;
        if (!Readable(allyGo, 0x80)) continue;
        void* allyAspect = *(void**)((char*)allyGo + 0x04);
        if (!allyAspect || !Readable(allyAspect, 0xC0)) continue;

        // Apply identical Armor Rating defense boost and independent duration per caster to ally
        SetAegisBonus(allyGo, allyGoid, heroGoid, boost, durationMs);

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
                    Rules_ChangeLife(gRules, allyGoid, actualHeal, 0);
                } else {
                    SSetCurrentLife(allyAspect, newLife);
                }
                // Ally heal XP: 0.25x multiplier, awarded exclusively to the caster
                if (actualHeal > 0.0f && totalAllyHealXp < maxAllyTotalXp) {
                    float allyXp = actualHeal * 0.25f;
                    if (allyXp > maxPerAllyXp) allyXp = maxPerAllyXp;
                    if (totalAllyHealXp + allyXp > maxAllyTotalXp) allyXp = maxAllyTotalXp - totalAllyHealXp;
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
    pkt.magic            = kAbilityMagic;
    pkt.heroGoid         = heroGoid;
    pkt.slot             = (uint8_t)slot;
    pkt.isBroadcast      = 0;
    pkt.skillLevelTenths = 0;

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

void BroadcastAbilityPacketToClients(uint32_t heroGoid, int slot, float level) {
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

    float clamped = (level < 0.0f) ? 0.0f : ((level > 6500.0f) ? 6500.0f : level);
    AbilityPacket pkt{};
    pkt.magic            = kAbilityMagic;
    pkt.heroGoid         = heroGoid;
    pkt.slot             = (uint8_t)slot;
    pkt.isBroadcast      = 1;
    pkt.skillLevelTenths = (uint16_t)(clamped * 10.0f + 0.5f);

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
            SetAegisBonus(hero, heroGoid, heroGoid, boost, durMs);

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
                            Rules_ChangeLife(gRules, heroGoid, actualHeal, 0);
                        } else {
                            SSetCurrentLife(aspect, newLife);
                        }
                        if (actualHeal > 0.0f) {
                            int curLevelFloor = (int)floorf(level);
                            double curLevelXp = CalculateLevelToXP((float)curLevelFloor);
                            double nextLevelXp = CalculateLevelToXP((float)(curLevelFloor + 1));
                            double deltaXp = nextLevelXp - curLevelXp;
                            if (deltaXp <= 0.0) deltaXp = 79843.0; // Level 30 delta fallback
                            float maxSelfXp = (float)(deltaXp * 0.010); // 1.0% next-level cap for self-heal
                            float heroXp = actualHeal * 0.25f;
                            if (heroXp > maxSelfXp) heroXp = maxSelfXp;
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

bool IsChatActive() {
    // 1. In-game console chat edit box
    if (eng::WindowVisible("console_edit", "game_console_mp")) return true;
    if (eng::WindowVisible("staging_char_name_edit_box", "staging_area_create_character")) return true;
    if (eng::WindowVisible("staging_area_chat_editbox", "staging_area")) return true;
    if (eng::WindowVisible("end_game_chat_editbox", "end_game_summary")) return true;

    // 2. In-game UI text / chat input lock flag on WorldOptions
    void* wo = *(void**)0x007ACFD4;
    if (wo && Readable(wo, 0x50) && *((const unsigned char*)wo + 0x46) != 0) {
        return true;
    }

    // 3. Single-player / dev console active flag on Game
    void* game = *(void**)0x007ACF8C;
    if (game && Readable(game, 0x100) && *((const unsigned char*)game + 0xFC) != 0) {
        return true;
    }

    return false;
}

bool IsActorDrinkingPotion(void* go) {
    if (!go || !Readable(go, 0x80)) return false;
    void* aspect = *(void**)((char*)go + 0x04);
    if (!aspect || !Readable(aspect, 0xC0)) return false;
    // Must be LS_ALIVE_CONSCIOUS (1)
    if (*(int*)((char*)aspect + 0xA0) != 1) return false;

    // 1. Check active front action job on GoMind (+0x34) (JQ_ACTION span at +0x68..+0x6C)
    void* mind = *(void**)((char*)go + 0x34);
    if (mind && Readable(mind, 0x70)) {
        void** jb = *(void***)((char*)mind + 0x68);
        void** je = *(void***)((char*)mind + 0x6C);
        if (jb && je && je > jb && (size_t)(je - jb) <= 64 &&
            Readable(jb, (size_t)(je - jb) * sizeof(void*))) {
            for (void** it = jb; it != je; ++it) {
                void* job = *it;
                if (!job || !Readable(job, 0x14)) break;
                if ((*((const unsigned char*)job + 0x10) & 0x02) == 0) {
                    if (*(const uint32_t*)job == 0x0E /*jat_drink*/) {
                        return true;
                    }
                    break;
                }
            }
        }
    }

    // 2. Check if ES_WEAPON_HAND (GoInventory+0x38) holds a potion (Multiplayer Client & Server during Drinking$)
    void* inv = *(void**)((char*)go + 0x2C);
    if (inv && Readable(inv, 0x60)) {
        void* weapItem = *(void**)((char*)inv + 0x38); // ES_WEAPON_HAND = slot 6 (0x20 + 6*4 = 0x38)
        if (weapItem && Readable(weapItem, 0x60)) {
            // Go::m_Potion is at +0x44 (Go::IsPotion @ 0x005396CF)
            if (*(void**)((char*)weapItem + 0x44) != nullptr) {
                return true;
            }
        }
    }
    return false;
}

extern "C" bool __fastcall Hook_GoMind_Validate(void* thisMind, void* unused_edx, const void* jobReq) {
    if (thisMind && jobReq && Readable(thisMind, 0x20) && Readable(jobReq, 0x10)) {
        uint32_t jat = *(const uint32_t*)jobReq;
        uint32_t q   = *(const uint32_t*)((const char*)jobReq + 0x04);
        if (q == 1 /*JQ_ACTION*/) {
            void* go = *(void**)((char*)thisMind + 0x04);
            if (IsActorDrinkingPotion(go)) {
                // Allow only involuntary life-state/cutscene jobs (jat_die=0x0C, jat_unconscious=0x20, jat_gain_consciousness=0x15, jat_do_se_command=0x0D)
                if (jat != 0x0C && jat != 0x20 && jat != 0x15 && jat != 0x0D) {
                    return false;
                }
            }
        }
    }
    return Orig_GoMind_Validate(thisMind, unused_edx, jobReq);
}

extern "C" bool __fastcall Hook_UIPartyManager_IsPlayerPartyMemberBusy(void* thisMgr, void* unused_edx, void* go, uint32_t bIgnoreCharWin) {
    if (IsActorDrinkingPotion(go)) {
        return true;
    }
    return Orig_UIPartyManager_IsPlayerPartyMemberBusy(thisMgr, unused_edx, go, bIgnoreCharWin);
}

// ---- BuffUI State Icon Cache & GoActor::AddGenericState Hook ----
struct StateIconCacheEntry {
    char stateName[64];
    char activeIcon[64];
};
static StateIconCacheEntry s_stateIconCache[64] = {};
static int s_stateIconCacheCount = 0;

static void CacheStateActiveIcon(const char* sName, const char* icon) {
    if (!sName || !*sName || !icon || !*icon) return;
    for (int i = 0; i < s_stateIconCacheCount; ++i) {
        if (_stricmp(s_stateIconCache[i].stateName, sName) == 0) {
            strncpy(s_stateIconCache[i].activeIcon, icon, 63);
            s_stateIconCache[i].activeIcon[63] = '\0';
            return;
        }
    }
    int idx = (s_stateIconCacheCount < 64) ? s_stateIconCacheCount++ : 0;
    strncpy(s_stateIconCache[idx].stateName, sName, 63);
    s_stateIconCache[idx].stateName[63] = '\0';
    strncpy(s_stateIconCache[idx].activeIcon, icon, 63);
    s_stateIconCache[idx].activeIcon[63] = '\0';
}

static const char* LookupCachedStateIcon(const char* sName) {
    if (!sName || !*sName) return nullptr;
    for (int i = 0; i < s_stateIconCacheCount; ++i) {
        if (_stricmp(s_stateIconCache[i].stateName, sName) == 0 && s_stateIconCache[i].activeIcon[0] != '\0') {
            return s_stateIconCache[i].activeIcon;
        }
    }
    return nullptr;
}

extern "C" void __fastcall Hook_GoActor_AddGenericState(void* thisActor, void* unused_edx, const char* sName, const char* sDescription, uint32_t expireLo, uint32_t expireHi, uint32_t casterGoid, uint32_t spellGoid, float spellLevel) {
    if (thisActor && sName && Readable(thisActor, 0x60) && Readable(sName, 1) && *sName) {
        // Cache spell's active_icon while the spell Go* still exists in GoDb
        if (spellGoid != 0) {
            void* godb = *(void**)0x007ACF4C;
            if (godb && Readable(godb, 0x40)) {
                void* spellGo = FindGo(godb, spellGoid);
                if (spellGo && Readable(spellGo, 0x40)) {
                    void* goGui = *(void**)((char*)spellGo + 0x28);
                    if (goGui && Readable(goGui, 0x20)) {
                        typedef const eng::gpstring* (__thiscall* GetActiveIconFn)(const void*);
                        static const auto s_getActiveIcon = (GetActiveIconFn)0x00541B78;
                        const eng::gpstring* iconStr = s_getActiveIcon(goGui);
                        if (iconStr && Readable(iconStr, 4) && iconStr->p && Readable(iconStr->p, 1) && *iconStr->p) {
                            CacheStateActiveIcon(sName, iconStr->p);
                        }
                    }
                }
            }
        }

        // Fix vanilla GoActor::AddGenericState duration-extension bug:
        // std::map::insert does not overwrite an existing state key, so remove any existing same-named state first.
        typedef bool (__thiscall* HasGenericStateFn)(const void*, const char*);
        typedef void (__thiscall* RemoveGenericStateFn)(void*, const char*);
        static const auto s_hasState    = (HasGenericStateFn)0x005BD57A;
        static const auto s_removeState = (RemoveGenericStateFn)0x005BD53A;
        for (int guard = 0; guard < 8 && s_hasState(thisActor, sName); ++guard) {
            s_removeState(thisActor, sName);
        }
    }
    Orig_GoActor_AddGenericState(thisActor, unused_edx, sName, sDescription, expireLo, expireHi, casterGoid, spellGoid, spellLevel);
}

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

    // Hook GoDefend::ResetModifiers at 0x005D3F4D on both Client and Server (7 bytes: 55 8B EC 51 56 8B F1)
    {
        const BYTE kExpect[7] = { 0x55, 0x8B, 0xEC, 0x51, 0x56, 0x8B, 0xF1 };
        BYTE* p = (BYTE*)0x005D3F4D;
        if (Readable(p, 7) && memcmp(p, kExpect, 7) == 0) {
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

    // Hook GoMind::Validate at 0x005DAE8B on both Client and Server (6 bytes: 55 8B EC 83 EC 14)
    // Prevents canceling jat_drink (0x0E) potion drinking animation via Z (CollectLoot), S (Stop), or other JQ_ACTION jobs
    {
        const BYTE kExpect[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14 };
        BYTE* p = (BYTE*)0x005DAE8B;
        if (Readable(p, 6) && memcmp(p, kExpect, 6) == 0) {
            DWORD o;
            if (VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &o)) {
                p[0] = 0xE9;
                *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&Hook_GoMind_Validate - (0x005DAE8B + 5));
                p[5] = 0x90;
                VirtualProtect(p, 6, o, &o);
                FlushInstructionCache(GetCurrentProcess(), p, 6);
                Log("hooked GoMind::Validate at 0x005DAE8B (potion animation cancel fix)");
            }
        }
    }

    // Hook UIPartyManager::IsPlayerPartyMemberBusy at 0x004A68E2 on Client (5 bytes: 55 8B EC 51 51)
    // Prevents weapon/spell slot switching (1/2/3/4) and UI commands from interrupting potion drinking
    {
        const BYTE kExpect[5] = { 0x55, 0x8B, 0xEC, 0x51, 0x51 };
        BYTE* p = (BYTE*)0x004A68E2;
        if (Readable(p, 5) && memcmp(p, kExpect, 5) == 0) {
            DWORD o;
            if (VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &o)) {
                p[0] = 0xE9;
                *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&Hook_UIPartyManager_IsPlayerPartyMemberBusy - (0x004A68E2 + 5));
                VirtualProtect(p, 5, o, &o);
                FlushInstructionCache(GetCurrentProcess(), p, 5);
                Log("hooked UIPartyManager::IsPlayerPartyMemberBusy at 0x004A68E2 (potion busy guard)");
            }
        }
    }

    // Hook GoActor::AddGenericState at 0x005BD321 on both Client and Server (9 bytes: 55 8B EC 81 EC A0 00 00 00)
    // Fixes vanilla duration-extension overwrite bug and caches spell active icons for Native BuffUI
    {
        const BYTE kExpect[9] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00 };
        BYTE* p = (BYTE*)0x005BD321;
        if (Readable(p, 9) && memcmp(p, kExpect, 9) == 0) {
            DWORD o;
            if (VirtualProtect(p, 9, PAGE_EXECUTE_READWRITE, &o)) {
                p[0] = 0xE9;
                *(int32_t*)(p + 1) = (int32_t)((DWORD)(void*)&Hook_GoActor_AddGenericState - (0x005BD321 + 5));
                p[5] = 0x90;
                p[6] = 0x90;
                p[7] = 0x90;
                p[8] = 0x90;
                VirtualProtect(p, 9, o, &o);
                FlushInstructionCache(GetCurrentProcess(), p, 9);
                Log("hooked GoActor::AddGenericState at 0x005BD321 (Native BuffUI state sync)");
            }
        }
    }

    Log("initialized - 4 stat-tied abilities ready (Q: Cleave, W: Shrapnel, E: Aegis, R: Nova) with shared 30s cooldown & Native BuffUI.");
}

bool CharacterAbilities_OnReceivePacket(bool isServer, DWORD /*senderDpnid*/, const BYTE* data, DWORD size) {
    if (!data || size != sizeof(AbilityPacket)) return false;
    const AbilityPacket* pkt = (const AbilityPacket*)data;
    if (pkt->magic != kAbilityMagic) return false;
    if (pkt->slot > 3) return true; // Consume to avoid native dispatcher crash!
    if (!CharacterAbilities_IsServerSupported()) return true; // Consume to avoid native dispatcher crash!
    if (isServer && pkt->isBroadcast != 0) return true; // Consume and ignore: server ignores broadcast packets
    if (!isServer && pkt->isBroadcast == 0) return true; // Consume and ignore: client ignores request packets

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

static void UpdateAbilityUI();
static void UpdateBuffUI();
static bool TriggerAbility(int slot);
static volatile int g_queuedUiSlot = -1;

struct ServerHeroSharedCd {
    uint32_t heroGoid;
    DWORD    lastUsedMs;
};
static ServerHeroSharedCd s_serverHeroCds[64] = {};

static bool CheckAndRecordServerSharedCd(uint32_t heroGoid) {
    if (!heroGoid) return false;
    DWORD now = GetTickCount();
    int freeSlot = -1;
    for (int i = 0; i < 64; ++i) {
        if (s_serverHeroCds[i].heroGoid == heroGoid) {
            // Allow 1.5s tolerance for network latency jitter on the 30s shared cooldown
            if (s_serverHeroCds[i].lastUsedMs != 0 && (DWORD)(now - s_serverHeroCds[i].lastUsedMs) < 28500) {
                return false;
            }
            s_serverHeroCds[i].lastUsedMs = now ? now : 1;
            return true;
        }
        if (freeSlot == -1 && (s_serverHeroCds[i].heroGoid == 0 || (DWORD)(now - s_serverHeroCds[i].lastUsedMs) >= 60000)) {
            freeSlot = i;
        }
    }
    if (freeSlot == -1) freeSlot = 0;
    s_serverHeroCds[freeSlot].heroGoid   = heroGoid;
    s_serverHeroCds[freeSlot].lastUsedMs = now ? now : 1;
    return true;
}

void CharacterAbilities_QueueTrigger(int slot) {
    if (slot >= 0 && slot <= 3) {
        g_queuedUiSlot = slot;
    }
}

void CharacterAbilities_Tick() {
    if (!CharacterAbilities_IsServerSupported()) return;

    ExpireAegisBuffs();

    if (Cfg().role != Role::Server) {
        int qSlot = g_queuedUiSlot;
        if (qSlot >= 0 && qSlot <= 3) {
            g_queuedUiSlot = -1;
            TriggerAbility(qSlot);
        }
        UpdateAbilityUI();
        UpdateBuffUI();
    }

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
        if (IsActorDrinkingPotion(hero)) continue;
        void* actor = *(void**)((char*)hero + 0x00);
        if (!actor || !Readable(actor, 0x40)) continue;

        float level = (!serverLocal && pkt.isBroadcast != 0 && pkt.skillLevelTenths > 0)
            ? ((float)pkt.skillLevelTenths * 0.1f)
            : GoActor_GetSkillLevel(actor, g_powers[pkt.slot].statKey);
        if (level < g_powers[pkt.slot].reqLevel) continue;

        if (serverLocal && pkt.isBroadcast == 0) {
            if (!CheckAndRecordServerSharedCd(pkt.heroGoid)) {
                continue;
            }
        }

        ExecuteCharacterAbility(pkt.slot, hero, level, serverLocal);

        if (serverLocal && pkt.isBroadcast == 0) {
            BroadcastAbilityPacketToClients(pkt.heroGoid, pkt.slot, level);
        }
    }
}

static void SetWindowVisible(void* w, bool vis) {
    if (!w) return;
    typedef void (__thiscall* SetVisFn)(void*, bool);
    static const auto s_setVis = (SetVisFn)0x0047B06E;
    s_setVis(w, vis);
    *(bool*)((char*)w + 0x108) = vis;
}

static void EnsureButtonClickable(void* w) {
    if (!w || !Readable(w, 0x140)) return;
    *(bool*)((char*)w + 0x137) = true; // m_bHitDetect
    *(bool*)((char*)w + 0x139) = true; // m_bEnabled
}

static void SetWindowAlpha(void* w, float alpha) {
    if (!w) return;
    *(float*)((char*)w + 0x104) = alpha;
}

static void SetTextWindow(void* w, const char* text, DWORD color, char* cachedStr) {
    if (!w) return;
    *(DWORD*)((char*)w + 0x1B4) = color;
    if (cachedStr && strcmp(cachedStr, text) == 0) return;
    if (cachedStr) {
        strncpy(cachedStr, text, 31);
        cachedStr[31] = '\0';
    }
    typedef void (__thiscall* SetTextFn)(void*, const eng::gpstring*, bool);
    static const auto s_setText = (SetTextFn)0x0047B4C4;
    eng::GpStr g(text);
    s_setText(w, &g.s, false);
}

static char s_cachedAwpCd[4][32] = {};
static char s_cachedAwpKey[4][32] = {};

static bool s_abilityHovered[4] = { false, false, false, false };
static bool s_stashHovered = false;

void CharacterAbilities_SetHover(int slot, bool hovered) {
    if (slot >= 0 && slot < 4) {
        s_abilityHovered[slot] = hovered;
    }
}

void CharacterAbilities_SetStashHover(bool hovered) {
    s_stashHovered = hovered;
}

static void UpdateAbilityUI() {
    const char* ws = eng::CurrentWorldState();
    if (!strstr(ws, "_ingame")) return;

    void* ui = eng::UI();
    if (!ui) return;

    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x20)) return;

    void* hero = GetScreenHero(srv);
    bool inGame = (hero && Readable(hero, 0x80));
    if (!inGame) {
        const char* const kHov[4] = { "awp_ability_hover_0", "awp_ability_hover_1", "awp_ability_hover_2", "awp_ability_hover_3" };
        for (int i = 0; i < 4; ++i) {
            void* h = eng::FindWindow(kHov[i], "character_awp");
            if (h) SetWindowVisible(h, false);
            s_abilityHovered[i] = false;
        }
        void* sh = eng::FindWindow("awp_stash_hover_1", "character_awp");
        if (sh) SetWindowVisible(sh, false);
        s_stashHovered = false;
        return;
    }
    void* actor = *(void**)((char*)hero + 0x00);

    // Reset client cooldown if hero Goid changed (e.g. switched characters / rejoined)
    static uint32_t s_lastClientHeroGoid = 0;
    uint32_t curHeroGoid = *(uint32_t*)((char*)hero + 0x4C);
    if (curHeroGoid != 0 && curHeroGoid != s_lastClientHeroGoid) {
        s_lastClientHeroGoid = curHeroGoid;
        for (int i = 0; i < 4; ++i) {
            g_powers[i].lastUsedMs = 0;
        }
    }

    void* slotsPanel = eng::FindWindow("window_slots_panel_1", "character_awp");
    bool slotsVis = slotsPanel ? eng::UIWindow_GetVisible(slotsPanel) : true;
    bool charVis = eng::InterfaceVisible("character");
    bool showOption1 = slotsVis && !charVis;

    const char* const kAwpIcons[4]  = { "awp_ability_icon_0",  "awp_ability_icon_1",  "awp_ability_icon_2",  "awp_ability_icon_3"  };
    const char* const kAwpBtns[4]   = { "awp_ability_btn_0",   "awp_ability_btn_1",   "awp_ability_btn_2",   "awp_ability_btn_3"   };
    const char* const kAwpHovers[4] = { "awp_ability_hover_0", "awp_ability_hover_1", "awp_ability_hover_2", "awp_ability_hover_3" };
    const char* const kAwpCds[4]    = { "awp_ability_cd_0",    "awp_ability_cd_1",    "awp_ability_cd_2",    "awp_ability_cd_3"    };
    const char* const kAwpKeys[4]   = { "awp_ability_key_0",   "awp_ability_key_1",   "awp_ability_key_2",   "awp_ability_key_3"   };

    void* awpPanel = eng::FindWindow("awp_ability_subpanel_1", "character_awp");
    if (awpPanel) SetWindowVisible(awpPanel, showOption1);
    void* stashBtn = eng::FindWindow("awp_stash_btn_1", "character_awp");
    if (stashBtn) {
        SetWindowVisible(stashBtn, showOption1);
        if (showOption1) EnsureButtonClickable(stashBtn);
    }

    HWND hwnd = g_gameHwnd ? g_gameHwnd : GetActiveWindow();
    POINT mousePt = { -9999, -9999 };
    if (hwnd && GetCursorPos(&mousePt)) {
        ScreenToClient(hwnd, &mousePt);
    }

    void* stashHover = eng::FindWindow("awp_stash_hover_1", "character_awp");
    if (stashHover) {
        bool mouseInStash = false;
        if (showOption1 && stashBtn && Readable(stashBtn, 0x80)) {
            int left   = *(int*)((char*)stashBtn + 0x60);
            int top    = *(int*)((char*)stashBtn + 0x64);
            int right  = *(int*)((char*)stashBtn + 0x68);
            int bottom = *(int*)((char*)stashBtn + 0x6C);
            mouseInStash = (mousePt.x >= left && mousePt.x <= right && mousePt.y >= top && mousePt.y <= bottom);
        }
        bool isStashHovered = showOption1 && (s_stashHovered || mouseInStash);
        SetWindowVisible(stashHover, isStashHovered);
        if (isStashHovered) SetWindowAlpha(stashHover, 1.0f);
    }

    static void* s_lastFirstBtn = nullptr;
    void* firstBtn = eng::FindWindow(kAwpBtns[0], "character_awp");
    if (firstBtn != s_lastFirstBtn) {
        s_lastFirstBtn = firstBtn;
        memset(s_cachedAwpCd, 0, sizeof(s_cachedAwpCd));
        memset(s_cachedAwpKey, 0, sizeof(s_cachedAwpKey));
    }

    DWORD now = GetTickCount();
    bool isAliveConscious = IsCharacterAliveAndConscious(hero);

    for (int i = 0; i < 4; ++i) {
        float level = (actor && Readable(actor, 0x40)) ? GoActor_GetSkillLevel(actor, g_powers[i].statKey) : 0.0f;
        bool isLocked = (level < g_powers[i].reqLevel);

        float cdRemaining = 0.0f;
        if (g_powers[i].lastUsedMs != 0 && (DWORD)(now - g_powers[i].lastUsedMs) < g_powers[i].cooldownMs) {
            cdRemaining = (float)(g_powers[i].cooldownMs - (DWORD)(now - g_powers[i].lastUsedMs)) / 1000.0f;
        }

        bool justOffCd = false;
        if (g_powers[i].lastUsedMs != 0 && cdRemaining <= 0.0f) {
            DWORD elapsed = (DWORD)(now - g_powers[i].lastUsedMs);
            if (elapsed >= g_powers[i].cooldownMs && elapsed < g_powers[i].cooldownMs + 400) {
                justOffCd = true;
            }
        }

        float alpha = 1.0f;
        char cdStr[16] = "";
        DWORD cdColor = 0xFFFFFFFF;
        DWORD keyColor = 0xFFCCCCCC;

        if (isLocked) {
            // Disabled because required discipline level is not met: never display cooldown timer
            alpha = 0.35f;
            cdStr[0] = '\0';
            cdColor = 0xFFFFFFFF;
            keyColor = 0xFF555555; // Dimmed hotkey
        } else if (cdRemaining > 0.0f) {
            alpha = 0.50f;
            int sec = (int)ceilf(cdRemaining);
            snprintf(cdStr, sizeof(cdStr), "%d", sec);
            cdColor = 0xFFFFFFFF;
            keyColor = 0xFF888888;
        } else if (!isAliveConscious) {
            alpha = 0.50f;
            cdStr[0] = '\0';
            cdColor = 0xFFFFFFFF;
            keyColor = 0xFF888888;
        } else if (justOffCd) {
            alpha = 1.0f;
            cdStr[0] = '\0';
            keyColor = 0xFFFFD700; // Gold flash when just ready
        } else {
            alpha = 1.0f;
            cdStr[0] = '\0';
            keyColor = 0xFFD8D8D8;
        }

        // Option 1 (Top-Left active weapon panel)
        void* awpIcon  = eng::FindWindow(kAwpIcons[i], "character_awp");
        void* awpBtn   = eng::FindWindow(kAwpBtns[i], "character_awp");
        void* awpHover = eng::FindWindow(kAwpHovers[i], "character_awp");
        void* awpCd    = eng::FindWindow(kAwpCds[i], "character_awp");
        void* awpKey   = eng::FindWindow(kAwpKeys[i], "character_awp");
        if (awpIcon) {
            SetWindowVisible(awpIcon, showOption1);
            if (showOption1) SetWindowAlpha(awpIcon, alpha);
        }
        if (awpBtn) {
            SetWindowVisible(awpBtn, showOption1);
            if (showOption1) {
                SetWindowAlpha(awpBtn, awpIcon ? 1.0f : alpha);
                EnsureButtonClickable(awpBtn);
            }
        }
        if (awpHover) {
            bool mouseInBtn = false;
            if (showOption1 && awpBtn && Readable(awpBtn, 0x80)) {
                int left   = *(int*)((char*)awpBtn + 0x60);
                int top    = *(int*)((char*)awpBtn + 0x64);
                int right  = *(int*)((char*)awpBtn + 0x68);
                int bottom = *(int*)((char*)awpBtn + 0x6C);
                mouseInBtn = (mousePt.x >= left && mousePt.x <= right && mousePt.y >= top && mousePt.y <= bottom);
            }
            bool isHovered = showOption1 && (s_abilityHovered[i] || mouseInBtn);
            SetWindowVisible(awpHover, isHovered);
            if (isHovered) SetWindowAlpha(awpHover, 1.0f);
        }
        if (awpCd) {
            SetWindowVisible(awpCd, showOption1);
            if (showOption1) SetTextWindow(awpCd, cdStr, cdColor, s_cachedAwpCd[i]);
        }
        if (awpKey) {
            SetWindowVisible(awpKey, showOption1);
            if (showOption1) {
                char kbuf[2] = { g_powers[i].keyChar, '\0' };
                SetTextWindow(awpKey, kbuf, keyColor, s_cachedAwpKey[i]);
            }
        }
    }
}

// ---- Native BuffUI Update (Drives [BuffUI] from GoActor::m_GenericStateMap + Active Aegis of Renewal Buffs) ----
static void UpdateBuffUI() {
    const char* ws = eng::CurrentWorldState();
    if (!ws || !strstr(ws, "_ingame")) return;
    if (!eng::InterfaceVisible("BuffUI")) return;

    static DWORD s_lastBuffUiTick = 0;
    DWORD nowTick = GetTickCount();
    if (s_lastBuffUiTick != 0 && (DWORD)(nowTick - s_lastBuffUiTick) < 200) {
        return;
    }
    s_lastBuffUiTick = nowTick ? nowTick : 1;

    void* godb = *(void**)0x007ACF4C;
    void* srv  = *(void**)0x007ACF70;
    if (!godb || !Readable(godb, 0x40) || !srv || !Readable(srv, 0x20)) return;

    // Resolve focused hero (supports single-player party selection via GoDb::GetFocusGo, fallback to GetScreenHero)
    typedef uint32_t (__thiscall* GetFocusGoFn)(const void*);
    static const auto s_getFocusGo = (GetFocusGoFn)0x0054EB1F;
    uint32_t focusGoid = s_getFocusGo(godb);
    void* hero = focusGoid ? FindGo(godb, focusGoid) : nullptr;
    if (!hero || !Readable(hero, 0x80)) {
        hero = GetScreenHero(srv);
    }
    if (!hero || !Readable(hero, 0x80)) return;

    void* actor  = *(void**)((char*)hero + 0x00);
    void* common = *(void**)((char*)hero + 0x10);
    uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
    if (!actor || !Readable(actor, 0x60)) return;

    struct ActiveBuffEntry {
        int  remSec;
        char desc[128];
        char icon[64];
    };
    ActiveBuffEntry entries[24];
    int numEntries = 0;

    double curWorldTime = eng::WorldTime();

    typedef const eng::gpstring* (__thiscall* GetActiveIconFn)(const void*);
    static const auto s_getActiveIcon = (GetActiveIconFn)0x00541B78;
    typedef void (__thiscall* TreeNextFn)(void**);
    static const auto s_treeNext = (TreeNextFn)0x005BEC9B;

    // 1. Collect spell buffs from GoActor::m_GenericStateMap (red-black tree _Head at actor + 0x4C)
    void* head = *(void**)((char*)actor + 0x4C);
    if (head && Readable(head, 0x10)) {
        void* node = *(void**)head; // _Head->_Left (begin())
        int guard = 0;
        while (node && node != head && guard < 128 && numEntries < 24) {
            if (!Readable(node, 0x40)) break;
            const char*    stateKey   = *(const char**)((char*)node + 0x0C);
            const wchar_t* wDesc      = *(const wchar_t**)((char*)node + 0x14);
            uint32_t       spellGoid  = *(uint32_t*)((char*)node + 0x20);
            double         expireTime = *(double*)((char*)node + 0x2C);
            bool           bNoExpire  = *(uint8_t*)((char*)node + 0x3C) != 0;

            int remSec = bNoExpire ? 0 : (int)floor(expireTime - curWorldTime + 0.5);
            if (!bNoExpire && remSec >= 0) {
                ActiveBuffEntry& e = entries[numEntries];
                e.remSec = remSec;
                e.desc[0] = '\0';
                e.icon[0] = '\0';

                if (wDesc && Readable(wDesc, sizeof(wchar_t)) && *wDesc) {
                    WideCharToMultiByte(CP_ACP, 0, wDesc, -1, e.desc, sizeof(e.desc) - 1, NULL, NULL);
                    e.desc[sizeof(e.desc) - 1] = '\0';
                }
                if (e.desc[0] == '\0' && stateKey && Readable(stateKey, 1) && *stateKey) {
                    strncpy(e.desc, stateKey, sizeof(e.desc) - 1);
                    e.desc[sizeof(e.desc) - 1] = '\0';
                }

                // Resolve active icon from live spell Go* or cached state table
                if (spellGoid != 0) {
                    void* spellGo = FindGo(godb, spellGoid);
                    if (spellGo && Readable(spellGo, 0x40)) {
                        void* goGui = *(void**)((char*)spellGo + 0x28);
                        if (goGui && Readable(goGui, 0x20)) {
                            const eng::gpstring* iconStr = s_getActiveIcon(goGui);
                            if (iconStr && Readable(iconStr, 4) && iconStr->p && Readable(iconStr->p, 1) && *iconStr->p) {
                                strncpy(e.icon, iconStr->p, sizeof(e.icon) - 1);
                                e.icon[sizeof(e.icon) - 1] = '\0';
                                if (stateKey && Readable(stateKey, 1) && *stateKey) {
                                    CacheStateActiveIcon(stateKey, e.icon);
                                }
                            }
                        }
                    }
                }
                if (e.icon[0] == '\0' && stateKey && Readable(stateKey, 1) && *stateKey) {
                    const char* cached = LookupCachedStateIcon(stateKey);
                    if (cached) {
                        strncpy(e.icon, cached, sizeof(e.icon) - 1);
                        e.icon[sizeof(e.icon) - 1] = '\0';
                    }
                }
                if (e.icon[0] == '\0') {
                    strcpy(e.icon, "b_gui_ig_mnu_nature-magic");
                }
                ++numEntries;
            }

            s_treeNext(&node);
            ++guard;
        }
    }

    // 2. Collect active [E] Aegis of Renewal buffs on heroGoid (supports multiple independent ally stacks)
    if (heroGoid != 0) {
        for (int i = 0; i < kMaxAegisBuffs && numEntries < 24; ++i) {
            if (g_aegisBuffs[i].targetGoid == heroGoid && g_aegisBuffs[i].endTime != 0) {
                int32_t remMs = (int32_t)(g_aegisBuffs[i].endTime - nowTick);
                if (remMs > 0) {
                    ActiveBuffEntry& e = entries[numEntries++];
                    e.remSec = (int)((remMs + 500) / 1000);
                    snprintf(e.desc, sizeof(e.desc), "Aegis of Renewal (+%.0f Armor)", g_aegisBuffs[i].boost);
                    strcpy(e.icon, "b_gui_ig_mnu_nature-magic");
                }
            }
        }
    }

    // 3. Sort active buffs by remaining duration ascending (shortest remaining time first, matching BufUI)
    for (int i = 0; i < numEntries - 1; ++i) {
        for (int j = i + 1; j < numEntries; ++j) {
            if (entries[j].remSec < entries[i].remSec) {
                ActiveBuffEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    // 4. Update BuffUI title ("<Hero>'s active buffs (<N>)")
    // GoCommon + 0x14 (m_ScreenName) is a GoWString (pointer to gpwstring, which itself holds wchar_t*).
    char heroName[64] = "Hero";
    bool gotName = false;
    if (common && Readable(common, 0x20)) {
        const wchar_t* const* ppName = *(const wchar_t* const**)((char*)common + 0x14);
        const wchar_t* wName = (ppName && Readable(ppName, sizeof(void*))) ? *ppName : nullptr;
        if (wName && Readable(wName, sizeof(wchar_t)) && *wName) {
            int n = WideCharToMultiByte(CP_ACP, 0, wName, -1, heroName, sizeof(heroName) - 1, NULL, NULL);
            if (n > 1) {
                heroName[sizeof(heroName) - 1] = '\0';
                gotName = true;
            }
        }
    }
    if (!gotName && hero && Readable(hero, 0x80)) {
        void* player = *(void**)((char*)hero + 0x78);
        if (player && Readable(player, 0x60)) {
            const wchar_t* wPlayerName = *(const wchar_t**)((char*)player + 0x58);
            if (wPlayerName && Readable(wPlayerName, sizeof(wchar_t)) && *wPlayerName) {
                int n = WideCharToMultiByte(CP_ACP, 0, wPlayerName, -1, heroName, sizeof(heroName) - 1, NULL, NULL);
                if (n > 1) {
                    heroName[sizeof(heroName) - 1] = '\0';
                }
            }
        }
    }
    size_t nameLen = strlen(heroName);
    bool endsWithS = (nameLen > 0 && (heroName[nameLen - 1] == 's' || heroName[nameLen - 1] == 'S'));
    char titleBuf[128];
    if (numEntries > 0) {
        snprintf(titleBuf, sizeof(titleBuf), "%s'%s active buffs (%d)", heroName, endsWithS ? "" : "s", numEntries);
    } else {
        snprintf(titleBuf, sizeof(titleBuf), "%s'%s active buffs", heroName, endsWithS ? "" : "s");
    }

    typedef void (__thiscall* UITextSetTextFn)(void*, const eng::gpstring*, bool);
    typedef int  (__thiscall* UITextBoxSetLineTextFn)(void*, int, const eng::gpstring*, DWORD);
    typedef void (__thiscall* UIWindowLoadTextureFn)(void*, const eng::gpstring*, bool);
    static const auto s_uiTextSetText        = (UITextSetTextFn)0x0047B4C4;
    static const auto s_uiTextBoxSetLineText = (UITextBoxSetLineTextFn)0x0047F8AA;
    static const auto s_uiWindowLoadTexture  = (UIWindowLoadTextureFn)0x0047B039;

    void* wTitleI    = eng::FindWindow("i_title", "BuffUI");
    void* wTitleT    = eng::FindWindow("t_title", "BuffUI");
    void* wBuffsList = eng::FindWindow("t_buffs", "BuffUI");
    void* wTimesList = eng::FindWindow("t_times", "BuffUI");
    void* wExtBg     = eng::FindWindow("j_background", "BuffUI");

    eng::GpStr titleGp(titleBuf);
    if (wTitleI) s_uiTextSetText(wTitleI, &titleGp.s, true);
    if (wTitleT) s_uiTextSetText(wTitleT, &titleGp.s, true);

    eng::GpStr emptyGp("");
    eng::GpStr emptyIconGp("b_gui_ig_mnu_bufui_empty");

    for (int line = 0; line < 24; ++line) {
        char iconWinName[16];
        char timeWinName[16];
        snprintf(iconWinName, sizeof(iconWinName), "i_icon%d", line);
        snprintf(timeWinName, sizeof(timeWinName), "i_time%d", line);
        void* wIcon = eng::FindWindow(iconWinName, "BuffUI");
        void* wTime = eng::FindWindow(timeWinName, "BuffUI");

        if (line < numEntries) {
            int slotTime = entries[line].remSec;
            int timeHrs  = slotTime / 3600;
            int timeMin  = (slotTime - timeHrs * 3600) / 60;
            int timeSec  = slotTime % 60;

            char hmsBuf[32];
            char secBuf[16];
            if (timeHrs > 0) {
                snprintf(hmsBuf, sizeof(hmsBuf), "%d:%02d:%02d", timeHrs, timeMin, timeSec);
            } else if (slotTime > 59) {
                snprintf(hmsBuf, sizeof(hmsBuf), "%2d:%02d", timeMin, timeSec);
            } else {
                snprintf(hmsBuf, sizeof(hmsBuf), "%2d", timeSec);
            }
            if (slotTime > 59) {
                snprintf(secBuf, sizeof(secBuf), "%02d", timeSec);
            } else {
                snprintf(secBuf, sizeof(secBuf), "%2d", timeSec);
            }

            DWORD timeColor = (slotTime > 10) ? 0xFF4999FF : 0xFFFF0000;
            eng::GpStr hmsGp(hmsBuf);
            eng::GpStr descGp(entries[line].desc);
            if (wTimesList) s_uiTextBoxSetLineText(wTimesList, line, &hmsGp.s, timeColor);
            if (wBuffsList) s_uiTextBoxSetLineText(wBuffsList, line, &descGp.s, 0xFFFFFFFF);

            if (wIcon) {
                eng::GpStr iconGp(entries[line].icon);
                s_uiWindowLoadTexture(wIcon, &iconGp.s, false);
            }
            if (wTime) {
                if (slotTime > 5999) {
                    eng::GpStr p99("99");
                    eng::GpStr ppp("++");
                    s_uiTextBoxSetLineText(wTime, 0, &p99.s, 0xFF4999FF);
                    s_uiTextBoxSetLineText(wTime, 1, &ppp.s, 0xFF4999FF);
                } else {
                    int totalMin = slotTime / 60;
                    char minBuf[16] = "";
                    if (totalMin > 0) {
                        snprintf(minBuf, sizeof(minBuf), "%d", totalMin);
                    }
                    eng::GpStr minGp(minBuf);
                    eng::GpStr secGp(secBuf);
                    s_uiTextBoxSetLineText(wTime, 0, &minGp.s, 0xFF4999FF);
                    s_uiTextBoxSetLineText(wTime, 1, &secGp.s, timeColor);
                }
            }
        } else {
            if (wTimesList) s_uiTextBoxSetLineText(wTimesList, line, &emptyGp.s, 0xFFFFFFFF);
            if (wBuffsList) s_uiTextBoxSetLineText(wBuffsList, line, &emptyGp.s, 0xFFFFFFFF);
            if (wIcon) s_uiWindowLoadTexture(wIcon, &emptyIconGp.s, false);
            if (wTime) {
                s_uiTextBoxSetLineText(wTime, 0, &emptyGp.s, 0xFFFFFFFF);
                s_uiTextBoxSetLineText(wTime, 1, &emptyGp.s, 0xFFFFFFFF);
            }
        }
    }

    if (wExtBg) {
        if (numEntries <= 12) {
            s_uiWindowLoadTexture(wExtBg, &emptyIconGp.s, false);
        } else {
            eng::GpStr fullBgGp("b_gui_ig_mnu_bufui");
            s_uiWindowLoadTexture(wExtBg, &fullBgGp.s, false);
        }
    }
}

static bool TriggerAbility(int slot) {
    if (slot < 0 || slot > 3) return false;
    if (!CharacterAbilities_IsServerSupported()) return false;

    // Must be in game and controlling a hero
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x20)) return false;

    void* hero = GetScreenHero(srv);
    if (!hero || !Readable(hero, 0x80)) return false;

    void* actor = *(void**)((char*)hero + 0x00);
    if (!actor || !Readable(actor, 0x40)) return false;

    // Cannot use abilities while unconscious, dead, a ghost, or drinking a potion
    if (!IsCharacterAliveAndConscious(hero)) {
        return false;
    }
    if (IsActorDrinkingPotion(hero)) {
        return false;
    }

    // Check discipline level requirement
    float level = GoActor_GetSkillLevel(actor, g_powers[slot].statKey);
    if (level < g_powers[slot].reqLevel) {
        Go_PlayVoiceSound(hero, "order_out_of_mana", false);
        char buf[128];
        snprintf(buf, sizeof(buf), "%s requires %s level %.0f (Current: %.1f).",
                 g_powers[slot].name, g_powers[slot].statDisplay, g_powers[slot].reqLevel, level);
        PrintThrottled(g_lastReqMsg, buf);
        return false;
    }

    // Check shared 30-second cooldown
    DWORD now = GetTickCount();
    if (g_powers[slot].lastUsedMs != 0 && (DWORD)(now - g_powers[slot].lastUsedMs) < g_powers[slot].cooldownMs) {
        Go_PlayVoiceSound(hero, "order_out_of_mana", false);
        float remaining = (float)(g_powers[slot].cooldownMs - (DWORD)(now - g_powers[slot].lastUsedMs)) / 1000.0f;
        char buf[128];
        snprintf(buf, sizeof(buf), "%s cooling down (%.1fs remaining).",
                 g_powers[slot].name, remaining);
        PrintThrottled(g_lastCoolMsg, buf);
        return false;
    }

    // Successfully trigger ability — put all 4 character abilities on the shared 30-second cooldown!
    DWORD stamp = now ? now : 1;
    for (int i = 0; i < 4; ++i) {
        g_powers[i].lastUsedMs = stamp;
    }

    bool serverLocal = IsServerLocal();
    ExecuteCharacterAbility(slot, hero, level, serverLocal);
    if (!serverLocal) {
        uint32_t heroGoid = *(uint32_t*)((char*)hero + 0x4C);
        SendAbilityPacketToServer(heroGoid, slot);
    }
    return true;
}

static bool IsInGame() {
    const char* ws = eng::CurrentWorldState();
    if (ws && strstr(ws, "_ingame") != nullptr) return true;
    void* srv = *(void**)0x007ACF70;
    if (srv && Readable(srv, 0x20)) {
        void* hero = GetScreenHero(srv);
        if (hero && Readable(hero, 0x80)) return true;
    }
    return false;
}

bool CharacterAbilities_FilterMouse(LPMSG msg) {
    (void)msg;
    return false;
}

bool CharacterAbilities_FilterKey(LPMSG msg) {
    if (!msg) return false;
    if (!CharacterAbilities_IsServerSupported()) return false;
    if (Cfg().role == Role::Server) return false;
    if (!IsInGame()) return false;

    // If chat or any text input is active, never intercept keys or eat characters
    if (IsChatActive()) {
        g_eatNextChar = false;
        g_eatCharVal = 0;
        return false;
    }

    // Handle character eating to avoid letter leaking if key was consumed
    if (msg->message == WM_CHAR) {
        if (g_eatNextChar && (msg->wParam == g_eatCharVal ||
                              msg->wParam == 'q' || msg->wParam == 'Q' ||
                              msg->wParam == 'w' || msg->wParam == 'W' ||
                              msg->wParam == 'e' || msg->wParam == 'E' ||
                              msg->wParam == 'r' || msg->wParam == 'R' ||
                              msg->wParam == ';' || msg->wParam == ':')) {
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
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
            return true;
        }
        return false;
    }

    if (msg->message != WM_KEYDOWN) return false;

    // Support Shift + ; (VK_OEM_1) hotkey to toggle BuffUI panel
    if (msg->wParam == VK_OEM_1 && (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        if ((msg->lParam & 0x40000000) == 0) {
            bool vis = eng::InterfaceVisible("BuffUI");
            if (vis) {
                eng::HideInterface("BuffUI");
            } else {
                eng::ShowInterface("BuffUI");
            }
            void* btn = eng::FindWindow("buffui_button", "data_bar");
            if (btn) {
                eng::UICheckbox_SetCheckState(btn, !vis);
            }
        }
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
        g_eatNextChar = true;
        g_eatCharVal = ':';
        return true;
    }

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

    if ((msg->lParam & 0x40000000) == 0) {
        TriggerAbility(slot);
    }

    // Consume input
    msg->message = WM_NULL;
    msg->wParam = 0;
    msg->lParam = 0;
    g_eatNextChar = true;
    g_eatCharVal = key;
    return true;
}
