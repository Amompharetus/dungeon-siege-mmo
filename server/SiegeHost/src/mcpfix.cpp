#include "siegehost.h"
#include "mcpfix.h"
#include <cmath>
#include <cstdint>
#include <cstring>

struct EngineSiegePos {
    float    x;
    float    y;
    float    z;
    uint32_t node;
};
static_assert(sizeof(EngineSiegePos) == 16, "EngineSiegePos must be 16 bytes");

extern "C" void SiegeHost_McpHeadToHeadCheck();
extern "C" void SiegeHost_McpMoveTargetHook();

extern "C" {
void* g_mcpHthContAddr          = (void*)0x0062176B;
void* g_mcpHthFailAddr          = (void*)0x00621D2C;
void* g_mcpFallbackOrigContAddr = (void*)0x0062A56F;
void* g_mcpFallbackDepAddr      = (void*)0x0062A6C1;
void* g_mcpFallbackRetAddr      = (void*)0x0062A707;
}

typedef EngineSiegePos* (__thiscall* Plan_GetPosAtTimeFn)(void* plan, EngineSiegePos* outPos, double* inOutTime, uint32_t bClamp, uint32_t zeroArg);
typedef EngineSiegePos* (__thiscall* Plan_GetEndPosFn)(void* plan, EngineSiegePos* outPos);
typedef double          (__thiscall* Plan_GetEndTimeFn)(void* plan);
typedef int             (__thiscall* Plan_MakeRequestFn)(void* plan, const EngineSiegePos* targetPos, float range, float maxTime, uint32_t reqFlags, uint32_t targetGoid);
typedef float*          (__thiscall* SiegeEngine_CalcDiffVecFn)(void* siegeEngine, float* outVec3, const EngineSiegePos* from, const EngineSiegePos* to);

static const auto s_planGetPosAtTime = (Plan_GetPosAtTimeFn)0x0061E9FF;
static const auto s_planGetEndPos    = (Plan_GetEndPosFn)0x0061EA76;
static const auto s_planGetEndTime   = (Plan_GetEndTimeFn)0x0061EAD6;
static const auto s_planMakeRequest  = (Plan_MakeRequestFn)0x0061FDF3;
static const auto s_calcDiffVector   = (SiegeEngine_CalcDiffVecFn)0x0066E6FE;

static bool ReadablePtr(const void* p, SIZE_T sz) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    uintptr_t start = (uintptr_t)p;
    uintptr_t end   = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (end - start) >= sz;
}

static bool WorldDiffVec(const EngineSiegePos& from, const EngineSiegePos& to, float outDiff[3]) {
    void* se = *(void**)0x007ACF80;
    if (!se || !ReadablePtr(se, 0x40)) return false;
    s_calcDiffVector(se, outDiff, &from, &to);
    return true;
}

static bool WorldDistSqOk(const EngineSiegePos& from, const EngineSiegePos& to, float* outDistSq) {
    float d[3] = { 0.0f, 0.0f, 0.0f };
    if (!WorldDiffVec(from, to, d)) return false;
    float dSq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (!std::isfinite(dSq)) return false;
    *outDistSq = dSq;
    return true;
}

static float WorldDistSq(const EngineSiegePos& from, const EngineSiegePos& to) {
    float dSq = 0.0f;
    return WorldDistSqOk(from, to, &dSq) ? dSq : 0.0f;
}

static float WorldDist(const EngineSiegePos& from, const EngineSiegePos& to) {
    return std::sqrt(WorldDistSq(from, to));
}

extern "C" uint32_t __cdecl McpFix_CanRunShortHeadToHead(const void* listHead, uint32_t listSize) {
    if (listSize >= 3) return 1;
    if (listSize < 2)  return 0;
    if (!ReadablePtr(listHead, 8)) return 0;
    const void* node0 = *(const void* const*)listHead;
    if (!node0 || node0 == listHead || !ReadablePtr(node0, 24)) return 0;
    const void* node1 = *(const void* const*)node0;
    if (!node1 || node1 == listHead || node1 == node0 || !ReadablePtr(node1, 24)) return 0;

    const EngineSiegePos& p0 = *(const EngineSiegePos*)((const char*)node0 + 8);
    const EngineSiegePos& p1 = *(const EngineSiegePos*)((const char*)node1 + 8);
    float dSq = 0.0f;
    if (!WorldDistSqOk(p0, p1, &dSq)) return 0;
    return mcpfix::ShouldRunHeadToHead(listSize, dSq) ? 1u : 0u;
}

extern "C" uint32_t __cdecl McpFix_SolveMeleeMovingTarget(void* frameEbp, void* origEsp) {
    const char* ebp = (const char*)frameEbp;
    char* esp = (char*)origEsp;

    void* attackerPlan = *(void**)(esp + 0x0C);
    void* targetPlan   = *(void**)(esp + 0x1C);
    if (!attackerPlan || !targetPlan) return mcpfix::REQ_OUT_OF_RANGE_12;

    uint32_t targetGoid   = *(const uint32_t*)(ebp + 0x0C);
    float    maxTimeParam = *(const float*)(ebp + 0x14);
    float    range        = *(const float*)(ebp + 0x18);
    float    lookAhead    = *(const float*)(ebp + 0x1C);
    uint32_t reqFlags     = *(const uint32_t*)(ebp + 0x24);

    float  attackerSpeed       = *(const float*)(esp + 0x20);
    double tStart              = *(const double*)(esp + 0x28);
    const EngineSiegePos& att0 = *(const EngineSiegePos*)(esp + 0x38);

    double tTargetEnd = s_planGetEndTime(targetPlan);

    EngineSiegePos targetPos0 = {};
    double t0Sample = tStart;
    s_planGetPosAtTime(targetPlan, &targetPos0, &t0Sample, 1, 0);
    float dist0Sq = 0.0f;
    if (!WorldDistSqOk(att0, targetPos0, &dist0Sq)) {
        return mcpfix::REQ_OUT_OF_RANGE_12;
    }
    float dist0 = std::sqrt(dist0Sq);

    // Already within real melee range at attacker's current plan end: attack without extra move.
    if (dist0 <= range + 0.05f) {
        *(double*)(esp + 0x10)         = tStart;
        *(EngineSiegePos*)(esp + 0x58) = targetPos0;
        return mcpfix::ClassifyFinalMeleeResult(mcpfix::REQ_OK_IN_RANGE, dist0, range);
    }

    auto sampleTarget = [&](double t) -> EngineSiegePos {
        EngineSiegePos p = {};
        double tt = t;
        s_planGetPosAtTime(targetPlan, &p, &tt, 1, 0);
        return p;
    };
    auto distFromStart = [&](const EngineSiegePos& p) -> float {
        return WorldDist(att0, p);
    };

    double tAim = mcpfix::SelectMeleeInterceptTime(
        tStart, tTargetEnd, attackerSpeed, range, lookAhead, maxTimeParam, dist0,
        sampleTarget, distFromStart
    );
    EngineSiegePos aimPos = (tAim == tStart) ? targetPos0 : sampleTarget(tAim);

    int reqRet = s_planMakeRequest(attackerPlan, &aimPos, range, maxTimeParam, reqFlags, targetGoid);
    if (mcpfix::IsRequestFailed(reqRet)) {
        return mcpfix::REQ_OUT_OF_RANGE_12;
    }

    EngineSiegePos attEndPos = {};
    s_planGetEndPos(attackerPlan, &attEndPos);
    double tAttEnd = s_planGetEndTime(attackerPlan);
    double tCheck  = (tAttEnd < 1e100 && tAttEnd >= tStart) ? tAttEnd : tStart;

    // Guard against aiming at a predicted point so close to att0 that MakeRequest made no step
    // while target is actually still out of range at tStart.
    if (WorldDist(att0, attEndPos) < 0.05f && dist0 > range + 0.05f) {
        reqRet = s_planMakeRequest(attackerPlan, &targetPos0, range, maxTimeParam, reqFlags, targetGoid);
        if (mcpfix::IsRequestFailed(reqRet)) {
            return mcpfix::REQ_OUT_OF_RANGE_12;
        }
        s_planGetEndPos(attackerPlan, &attEndPos);
        tAttEnd = s_planGetEndTime(attackerPlan);
        tCheck  = (tAttEnd < 1e100 && tAttEnd >= tStart) ? tAttEnd : tStart;
        aimPos  = targetPos0;
    }

    EngineSiegePos targetAtEnd = sampleTarget(tCheck);
    float realDist = WorldDist(attEndPos, targetAtEnd);
    uint32_t finalRet = mcpfix::ClassifyFinalMeleeResult(reqRet, realDist, range);

    *(double*)(esp + 0x10)         = tCheck;
    *(EngineSiegePos*)(esp + 0x58) = targetAtEnd;
    return finalRet;
}

__asm__(
    ".text\n"
    ".globl _SiegeHost_McpHeadToHeadCheck\n"
    "_SiegeHost_McpHeadToHeadCheck:\n"
    "  movl 0x40(%esp), %eax\n"
    "  cmpl $3, %eax\n"
    "  jge 1f\n"
    "  pushl %ecx\n"
    "  pushl %edx\n"
    "  pushl %eax\n"
    "  pushl 0x48(%esp)\n"
    "  call _McpFix_CanRunShortHeadToHead\n"
    "  addl $8, %esp\n"
    "  popl %edx\n"
    "  popl %ecx\n"
    "  testl %eax, %eax\n"
    "  jz 2f\n"
    "1:\n"
    "  movl 0x3C(%esp), %ecx\n"
    "  jmp *(_g_mcpHthContAddr)\n"
    "2:\n"
    "  movl $5, 0x0C(%esp)\n"
    "  jmp *(_g_mcpHthFailAddr)\n"
);

__asm__(
    ".text\n"
    ".globl _SiegeHost_McpMoveTargetHook\n"
    "_SiegeHost_McpMoveTargetHook:\n"
    "  cmpl $1, 0x28(%ebp)\n"
    "  jne 3f\n"
    "  cmpb $0, 0x20(%ebp)\n"
    "  je 3f\n"
    "  movl %esp, %eax\n"
    "  pushl %eax\n"
    "  pushl %ebp\n"
    "  call _McpFix_SolveMeleeMovingTarget\n"
    "  addl $8, %esp\n"
    "  cmpl $4, %eax\n"
    "  ja 2f\n"
    "  movl %eax, %esi\n"
    "  movl 0x0C(%esp), %edi\n"
    "  jmp *(_g_mcpFallbackDepAddr)\n"
    "2:\n"
    "  jmp *(_g_mcpFallbackRetAddr)\n"
    "3:\n"
    "  flds 0x1C(%ebp)\n"
    "  subl $0x18, %esp\n"
    "  fstps 0x14(%esp)\n"
    "  jmp *(_g_mcpFallbackOrigContAddr)\n"
);

static bool PatchJump(uintptr_t addr, const BYTE* expected, size_t len, void* target) {
    BYTE* p = (BYTE*)addr;
    if (!ReadablePtr(p, len) || memcmp(p, expected, len) != 0) {
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &oldProt)) {
        return false;
    }
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)target - (addr + 5));
    for (size_t i = 5; i < len; ++i) p[i] = 0x90;
    VirtualProtect(p, len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return true;
}

void McpFixInstall() {
    // 1. Head-to-Head waypoint count check at 0x00621757 (20 bytes ending before 0x0062176B):
    //    cmp dword ptr [esp+0x40], 3; jge 0x62176b; mov [esp+0x0c], 1; jmp 0x621d2c
    static const BYTE kExpectHth[20] = {
        0x83, 0x7C, 0x24, 0x40, 0x03,
        0x7D, 0x0D,
        0xC7, 0x44, 0x24, 0x0C, 0x01, 0x00, 0x00, 0x00,
        0xE9, 0xC1, 0x05, 0x00, 0x00
    };
    if (PatchJump(0x00621757, kExpectHth, sizeof(kExpectHth), (void*)&SiegeHost_McpHeadToHeadCheck)) {
        Log("mcpfix: hooked HeadToHead <3 short-circuit at 0x00621757");
    } else {
        Log("mcpfix: WARNING byte mismatch at 0x00621757; HeadToHead hook skipped");
    }

    // 2. Moving-target melee fallback at 0x0062A565 (10 bytes ending before 0x0062A56F):
    //    fld dword ptr [ebp+0x1c]; sub esp, 0x18; fstp dword ptr [esp+0x14]
    static const BYTE kExpectFallback[10] = {
        0xD9, 0x45, 0x1C,
        0x83, 0xEC, 0x18,
        0xD9, 0x5C, 0x24, 0x14
    };
    if (PatchJump(0x0062A565, kExpectFallback, sizeof(kExpectFallback), (void*)&SiegeHost_McpMoveTargetHook)) {
        Log("mcpfix: hooked moving-target melee fallback at 0x0062A565");
    } else {
        Log("mcpfix: WARNING byte mismatch at 0x0062A565; moving-target hook skipped");
    }
}
