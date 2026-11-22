#include "siegehost.h"
#include "engine.h"
#include "doorfix.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

static bool Readable(const void* ptr, size_t len) {
    if (!ptr || (uintptr_t)ptr < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    uintptr_t end = (uintptr_t)ptr + len;
    uintptr_t regEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return end <= regEnd;
}

template <typename T>
static bool ReadVal(const void* base, size_t off, T* out) {
    const void* addr = (const BYTE*)base + off;
    if (!Readable(addr, sizeof(T))) return false;
    memcpy(out, addr, sizeof(T));
    return true;
}

static bool InstallDetour(DWORD va, const BYTE* expect, size_t len, void* hook, void** outOrig, const char* name) {
    BYTE* src = (BYTE*)va;
    if (!Readable(src, len) || len < 5 || memcmp(src, expect, len) != 0) {
        Log("DoorFix: detour verify failed for %s at 0x%08lX", name, va);
        return false;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, len + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) {
        Log("DoorFix: VirtualAlloc failed for %s", name);
        return false;
    }
    memcpy(tramp, src, len);
    tramp[len] = 0xE9;
    *(int32_t*)(tramp + len + 1) = (int32_t)((va + len) - (DWORD)(tramp + len + 5));
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);

    DWORD old = 0;
    if (!VirtualProtect(src, len, PAGE_EXECUTE_READWRITE, &old)) {
        Log("DoorFix: VirtualProtect failed for %s (%lu)", name, GetLastError());
        return false;
    }
    src[0] = 0xE9;
    *(int32_t*)(src + 1) = (int32_t)((DWORD)hook - (va + 5));
    for (size_t i = 5; i < len; ++i) src[i] = 0x90;
    VirtualProtect(src, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, len);

    *outOrig = tramp;
    Log("DoorFix: hooked %s at 0x%08lX (len=%u)", name, va, (unsigned)len);
    return true;
}

// ---- Engine helpers (DSLOA.exe v1.50) ----

struct EngineSiegePos {
    float x, y, z;
    DWORD node;
};

static void* GoByGoid(DWORD goid) {
    if (!goid || goid == 0xFFFFFFFF) return nullptr;
    auto fn = (void* (__cdecl*)(DWORD))0x005207E1;
    return fn(goid);
}

static DWORD FindGoidByScid(void* godb, DWORD scid) {
    if (!godb || !scid || scid == 0xFFFFFFFF) return 0xFFFFFFFF;
    auto fn = (DWORD (__thiscall*)(void*, DWORD, void*))0x0054FA42;
    return fn(godb, scid, nullptr);
}

static const char* GoTemplateName(void* go) {
    if (!go) return "";
    auto fn = (const char* (__thiscall*)(void*))0x005390A3;
    const char* s = fn(go);
    return (s && Readable(s, 1)) ? s : "";
}

static void* ScreenHero() {
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x20)) return nullptr;
    auto fn = (void* (__thiscall*)(void*))0x00570565;
    return fn(srv);
}

static bool ReadGoNode(const void* go, DWORD* outNode) {
    void* pl = nullptr;
    if (!go || !ReadVal(go, 0x40, &pl) || !pl) return false;
    auto getPos = (const EngineSiegePos* (__thiscall*)(void*))0x00540DFE;
    const EngineSiegePos* p = getPos(pl);
    if (!p || !Readable(p, sizeof(EngineSiegePos))) return false;
    *outNode = p->node;
    return true;
}

static void* GetLoadedNode(DWORD nodeGuid) {
    void* se = *(void**)0x007ACF80;
    if (!se || !nodeGuid) return nullptr;
    auto fn = (void* (__thiscall*)(void*, const DWORD*))0x006711C9;
    return fn(se, &nodeGuid);
}

static bool IsPlayerValid(void* pl) {
    if (!pl) return false;
    auto fn = (BYTE (__cdecl*)(void*))0x00569054;
    return fn(pl) != 0;
}

static bool IsPlayerHuman(void* pl) {
    if (!pl) return false;
    auto fn = (BYTE (__thiscall*)(void*))0x00487EE0;
    return fn(pl) != 0;
}

static void* FindPlayerByMachineId(DWORD dpnid) {
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x10)) return nullptr;
    auto fn = (void* (__thiscall*)(void*, DWORD))0x0056EB90;
    return fn(srv, dpnid);
}

static bool GetScidBit(void* godb, DWORD scid, int bit) {
    if (!godb || !scid || scid == 0xFFFFFFFF) return false;
    auto fn = (BYTE (__thiscall*)(void*, DWORD, int))0x00471618;
    return fn(godb, scid, bit) != 0;
}

static void SetDoesBlockPath(void* aspect, bool val) {
    if (!aspect) return;
    auto fn = (void (__thiscall*)(void*, BYTE))0x00532006;
    fn(aspect, val ? 1 : 0);
}

static void SetIsVisible(void* aspect, bool val) {
    if (!aspect) return;
    auto fn = (void (__thiscall*)(void*, BYTE))0x0053218F;
    fn(aspect, val ? 1 : 0);
}

static bool NemaReconstitute(void* nema, int level, DWORD bForce) {
    if (!nema) return false;
    auto fn = (BYTE (__thiscall*)(void*, int, DWORD))0x0069B245;
    return fn(nema, level, bForce) != 0;
}

static void NemaResetPose(void* nema) {
    if (!nema) return;
    auto fn = (void (__thiscall*)(void*))0x0069E637;
    fn(nema);
}

static void SyncGoToClient(void* godb, void* player, void* go, DWORD bFullSync) {
    if (!godb || !player || !go) return;
    auto fn = (void (__thiscall*)(void*, void*, void*, DWORD))0x0054B055;
    fn(godb, player, go, bFullSync);
}

static void GoXferSyncData(void* go, void* xferBuf, DWORD xferSz) {
    if (!go || !xferBuf || !xferSz) return;
    auto fn = (void (__thiscall*)(void*, void*, DWORD))0x0053C0D6;
    fn(go, xferBuf, xferSz);
}

static bool MarkGoForDeletion(void* godb, DWORD goid) {
    if (!godb || !goid || goid == 0xFFFFFFFFu) return false;
    auto fn = (BYTE (__thiscall*)(void*, DWORD, DWORD, DWORD, DWORD))0x0054C179;
    return fn(godb, goid, 0, 0, 0) != 0;
}

static bool IsPrimaryThread() {
    auto fn = (BYTE (__cdecl*)())0x00520656;
    return fn() != 0;
}

static void RbTreeNext(BYTE** nodePtr) {
    auto fn = (void (__thiscall*)(BYTE**))0x00557026;
    fn(nodePtr);
}

// ---- Part 1: Broadcast stack-overflow guard for GoDb::SCreateGo & GoDb::SCloneGo (>8 remote players) ----
// GoDb::BuildPlayerList (0x00549B8F) writes Player* pointers into a fixed 9-DWORD stack buffer
// inside SCreateGo (0x00549A0E) and SCloneGo (0x0054A9E6). When >8 remote human players are online
// during a broadcast (targetMachineId == 0 or 0xFFFFFFFE), split into per-player sends so no stack
// corruption or recipient truncation occurs.

typedef DWORD (__fastcall* SCreateGoFn)(void* godb, void* edx, DWORD targetMachine, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6);
typedef DWORD (__fastcall* SCloneGoFn)(void* godb, void* edx, DWORD targetMachine, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7);

static SCreateGoFn g_origSCreateGo = nullptr;
static SCloneGoFn  g_origSCloneGo  = nullptr;
static DWORD       g_splitLogCount = 0;

static void CollectRemoteHumanMachines(std::vector<DWORD>& out) {
    out.clear();
    void* srv = *(void**)0x007ACF70;
    if (!srv || !Readable(srv, 0x10)) return;
    void** begin = nullptr;
    void** end   = nullptr;
    if (!ReadVal(srv, 0x04, &begin) || !ReadVal(srv, 0x08, &end)) return;
    if (!begin || end < begin) return;
    size_t count = (size_t)(end - begin);
    if (count > 64 || !Readable(begin, count * sizeof(void*))) return;
    for (size_t i = 0; i < count; ++i) {
        void* pl = begin[i];
        if (!pl || !Readable(pl, 0x20)) continue;
        if (!IsPlayerValid(pl) || !IsPlayerHuman(pl)) continue;
        DWORD ctrl = 0;
        if (!ReadVal(pl, 0x14, &ctrl) || ctrl == 2) continue;  // skip host/server player
        DWORD machineId = 0;
        if (ReadVal(pl, 0x10, &machineId) && machineId != 0) {
            out.push_back(machineId);
        }
    }
}

static DWORD LocalRpcMachineId() {
    DWORD* p = nullptr;
    if (!ReadVal((const void*)0x007ACB68, 0, &p) || !p || !Readable(p, 4)) return 0;
    return *p;
}

static DWORD __fastcall Hook_SCreateGo(void* godb, void* edx, DWORD targetMachine,
                                       DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6) {
    if (targetMachine == 0 || targetMachine == 0xFFFFFFFEu) {
        std::vector<DWORD> remotes;
        CollectRemoteHumanMachines(remotes);
        if (remotes.size() > 8) {
            if (++g_splitLogCount <= 5) {
                Log("DoorFix: splitting broadcast SCreateGo across %u remote players (prevents BuildPlayerList overflow)",
                    (unsigned)remotes.size());
            }
            for (DWORD mid : remotes) {
                g_origSCreateGo(godb, edx, mid, a1, a2, a3, a4, a5, a6);
            }
            if (targetMachine == 0) {
                DWORD localId = LocalRpcMachineId();
                if (localId != 0) {
                    return g_origSCreateGo(godb, edx, localId, a1, a2, a3, a4, a5, a6);
                }
            }
            return 0xFFFFFFFFu;
        }
    }
    return g_origSCreateGo(godb, edx, targetMachine, a1, a2, a3, a4, a5, a6);
}

static DWORD __fastcall Hook_SCloneGo(void* godb, void* edx, DWORD targetMachine,
                                      DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7) {
    if (targetMachine == 0 || targetMachine == 0xFFFFFFFEu) {
        std::vector<DWORD> remotes;
        CollectRemoteHumanMachines(remotes);
        if (remotes.size() > 8) {
            if (++g_splitLogCount <= 5) {
                Log("DoorFix: splitting broadcast SCloneGo across %u remote players (prevents BuildPlayerList overflow)",
                    (unsigned)remotes.size());
            }
            for (DWORD mid : remotes) {
                g_origSCloneGo(godb, edx, mid, a1, a2, a3, a4, a5, a6, a7);
            }
            if (targetMachine == 0) {
                DWORD localId = LocalRpcMachineId();
                if (localId != 0) {
                    return g_origSCloneGo(godb, edx, localId, a1, a2, a3, a4, a5, a6, a7);
                }
            }
            return 0xFFFFFFFFu;
        }
    }
    return g_origSCloneGo(godb, edx, targetMachine, a1, a2, a3, a4, a5, a6, a7);
}

// ---- Part 1b: Delta-compression desync elimination for GoCreateReq::Xfer & GoCloneReq::Xfer ----
// GoDb::SCreateGo (0x00549A0E) and GoDb::SCloneGo (0x0054A9E6) delta-compress m_RegionId, m_PlayerId,
// m_StartingPos, m_StartingRot (and m_CloneSourceGoid / m_TemplateName for clones) against mutable
// per-player baselines at GoDb+0x3D8[bsr(plId)-1] and GoDb+0x3E0[bsr(plId)-1].
// If server and client baselines ever diverge (dropped flush, race, out-of-order retry, etc.), every
// subsequent static object in the same region (such as a9_r3_clockroom doors) sends bit=0 ("unchanged"),
// causing the client to decode a stale region/player and fail ContentDb::CreateGo permanently — even on
// DoorFix resyncs. Poisoning the server baseline before packing forces full wire transmission of all
// delta-compressed fields (100% wire-compatible with stock clients) and immediately heals any stale client
// baseline on the very next packet. A dedicated leaf lock serializes access to GoDb+0x3D8/+0x3E0.
typedef void (__fastcall* GoReqXferFn)(void* req, void* edx, void* bitpacker, void* prev);
typedef BYTE (__fastcall* NetFuBiPackQueueFn)(void* self, void* edx, void* queue, void* outBuf);
typedef BYTE (__fastcall* CreateGoClientFn)(void* godb, void* edx, DWORD goid, DWORD seed, void* req,
                                            DWORD bForce, void* xferBuf, DWORD xferSz, DWORD a7, DWORD a8);
typedef BYTE (__fastcall* CloneGoClientFn)(void* godb, void* edx, DWORD goid, DWORD seed, void* req,
                                           const char* tplName, DWORD bForce, void* xferBuf, DWORD xferSz,
                                           DWORD a8, DWORD a9);
typedef DWORD (__fastcall* CloneGoImplFn)(void* godb, void* edx, void* req,
                                          DWORD bForce, DWORD bIsContent, DWORD bDontNotify);

static GoReqXferFn        g_origCreateReqXfer   = nullptr;
static GoReqXferFn        g_origCloneReqXfer    = nullptr;
static NetFuBiPackQueueFn g_origPackQueue       = nullptr;
static CreateGoClientFn   g_origCreateGoCli     = nullptr;
static CloneGoClientFn    g_origCloneGoCli      = nullptr;
static CloneGoImplFn      g_origCloneGoImpl     = nullptr;
static CRITICAL_SECTION   g_xferLock;
static bool               g_xferLockInit        = false;
static DWORD              g_chunkLogCount       = 0;
static DWORD              g_cliFailLogCount     = 0;
static DWORD              g_cliDupSkipLogCount  = 0;
static DWORD              g_cliStaleDelLogCount = 0;
static DWORD              g_cliCloneDupLogCount = 0;
static DWORD              g_cliCloneFailLogCnt  = 0;
static DWORD              g_cloneLeakFixCount   = 0;
static DWORD              g_stuckFlagFixCount   = 0;

static void __fastcall Hook_GoCreateReq_Xfer(void* req, void* edx, void* bitpacker, void* prev) {
    if (bitpacker && prev && req && Readable(bitpacker, 1) && *(const BYTE*)bitpacker != 0 &&
        Readable(req, 0x34) && Readable(prev, 0x34)) {
        if (g_xferLockInit) EnterCriticalSection(&g_xferLock);
        doorfix::PoisonGoCreateReqPrev(req, prev);
        g_origCreateReqXfer(req, edx, bitpacker, prev);
        if (g_xferLockInit) LeaveCriticalSection(&g_xferLock);
        return;
    }
    g_origCreateReqXfer(req, edx, bitpacker, prev);
}

static void __fastcall Hook_GoCloneReq_Xfer(void* req, void* edx, void* bitpacker, void* prev) {
    if (bitpacker && prev && req && Readable(bitpacker, 1) && *(const BYTE*)bitpacker != 0 &&
        Readable(req, 0x44) && Readable(prev, 0x44)) {
        if (g_xferLockInit) EnterCriticalSection(&g_xferLock);
        doorfix::PoisonGoCloneReqPrev(req, prev);
        g_origCloneReqXfer(req, edx, bitpacker, prev);
        if (g_xferLockInit) LeaveCriticalSection(&g_xferLock);
        return;
    }
    g_origCloneReqXfer(req, edx, bitpacker, prev);
}

// ---- Part 1c: Oversized NetFuBiSend flush chunking (prevents DirectPlay8 64 KB DPNERR_SENDTOOLARGE) ----
// NetFuBiSend::PackQueue (0x00594D71) packs all queued RPCs for one player into a single NetPipe::Send
// buffer and marks each packed RPC with [rpc+0x20]=1; EraseSent (0x00594E05) then removes ONLY marked
// entries. During dense region stream-ins (e.g. a9_r3_clockroom's 716 nodes / 388 objects) on a busy
// server tick, packing everything into one packet can approach or exceed DirectPlay8's 64 KB ceiling,
// causing NetPipe::Send to silently discard the entire flush. Temporarily capping forward traversal at
// kMaxFubiFlushBytes (24 KB, always sending at least 1 RPC) leaves tail RPCs unmarked in FIFO order for
// the next 15.6 ms tick without touching head->prev or allocating memory.
static BYTE __fastcall Hook_NetFuBiSend_PackQueue(void* self, void* edx, void* queue, void* outBuf) {
    struct ListNode {
        ListNode* next;
        ListNode* prev;
        BYTE*     rpc;
    };
    if (!queue || !Readable(queue, 0x0C)) {
        return g_origPackQueue(self, edx, queue, outBuf);
    }
    ListNode* head = *(ListNode* const*)((const BYTE*)queue + 4);
    if (!head || !Readable(head, sizeof(ListNode))) {
        return g_origPackQueue(self, edx, queue, outBuf);
    }

    size_t    totalBytes = 5;  // outer FuBi frame header
    int       packed     = 0;
    int       deferred   = 0;
    ListNode* splitAfter = nullptr;
    ListNode* cur        = head->next;
    int       safety     = 0;
    while (cur && cur != head && ++safety <= 16384) {
        if (!Readable(cur, sizeof(ListNode))) {
            splitAfter = nullptr;
            break;
        }
        size_t rpcBytes = 5;
        BYTE* rpc = cur->rpc;
        if (rpc && Readable(rpc, 0x10)) {
            const BYTE* b = *(const BYTE* const*)(rpc + 0x08);
            const BYTE* e = *(const BYTE* const*)(rpc + 0x0C);
            if (b && e >= b) rpcBytes += (size_t)(e - b);
        }
        if (!splitAfter && packed > 0 && totalBytes + rpcBytes > doorfix::kMaxFubiFlushBytes) {
            splitAfter = cur->prev;
        }
        if (!splitAfter) {
            totalBytes += rpcBytes;
            ++packed;
        } else {
            ++deferred;
        }
        cur = cur->next;
    }

    if (!splitAfter || splitAfter == head || !Readable(splitAfter, sizeof(ListNode))) {
        return g_origPackQueue(self, edx, queue, outBuf);
    }

    ListNode* savedNext = splitAfter->next;
    splitAfter->next = head;
    BYTE ret = g_origPackQueue(self, edx, queue, outBuf);
    splitAfter->next = savedNext;

    if (++g_chunkLogCount <= 10) {
        Log("DoorFix[SRV]: chunked oversized FuBi flush at %u bytes (%d RPCs sent, %d deferred to next tick)",
            (unsigned)totalBytes, packed, deferred);
    }
    return ret;
}

// ---- Part 1d: Client-side CreateGo_Client duplicate-GOID zombie guard & failure diagnostics ----
// Stock GoDb::CreateGo_Client (0x00549E47) has a fatal duplicate-creation bug: when a Go with the
// same GOID already exists on the client (either committed in GlobalGoDb or queued in m_PendingGos),
// CreateGo -> AllocAt fails and CreateGo_Client calls CleanupFailedCreate (0x005510B8 -> 0x00551158),
// which executes FreeGoid(godb, goid, 0) on the LIVE existing Go WITHOUT deleting it. That orphans
// the live visual door as an un-deleteable closed zombie invisible to GoByGoid/SHDR, causing a second
// resync to spawn a second door on top of it (duplicate closed-door bug).
// Short-circuiting live/pending duplicate GOIDs here (applying xfer sync data cleanly and mirroring
// CreateGo_Client's scratch/m_LastCreatedGoid tail) permanently prevents visual zombie duplicates.
static BYTE __fastcall Hook_CreateGoClient(void* godb, void* edx, DWORD goid, DWORD seed, void* req,
                                           DWORD bForce, void* xferBuf, DWORD xferSz, DWORD a7, DWORD a8) {
    DWORD scid = 0, regId = 0, plId = 0, reqNode = 0;
    BYTE  reqFlags = 0;
    if (req && Readable(req, 0x0C)) {
        ReadVal(req, 0x00, &scid);
        ReadVal(req, 0x04, &regId);
        ReadVal(req, 0x08, &plId);
        if (Readable(req, 0x31)) {
            ReadVal(req, 0x1C, &reqNode);
            ReadVal(req, 0x30, &reqFlags);
        }
    }

    // Guard 1: If goid is already live or pending on this client, NEVER call g_origCreateGoCli
    // (which would fail AllocAt -> CleanupFailedCreate -> FreeGoid on the live Go).
    void* existGo = GoByGoid(goid);
    if (existGo && Readable(existGo, 0x9C)) {
        BYTE f96 = 0;
        ReadVal(existGo, 0x96, &f96);
        if ((f96 & 0x08u) == 0 && xferBuf && xferSz) {
            GoXferSyncData(existGo, xferBuf, xferSz);
        }
        if (godb && Readable(godb, 0x334)) {
            DWORD scratchBase = 0;
            if (ReadVal(godb, 0x32C, &scratchBase)) {
                *(DWORD*)((BYTE*)godb + 0x330) = scratchBase;
            }
            DWORD lastOff = IsPrimaryThread() ? 0x2F0u : 0x2F4u;
            *(DWORD*)((BYTE*)godb + lastOff) = goid;
        }
        if (++g_cliDupSkipLogCount <= 30) {
            Log("DoorFix[CLI]: skipped duplicate CreateGo_Client for live goid=0x%08lX scid=0x%08lX tpl=%s f96=0x%02X xferSz=%lu",
                goid, scid, GoTemplateName(existGo), (unsigned)f96, xferSz);
        }
        return 1;
    }

    // Guard 2: If a different stale GOID already occupies this door SCID on the client,
    // mark the stale door for clean deletion before creating the new GOID so doors never overlap.
    if (scid != 0 && scid != 0xFFFFFFFFu) {
        DWORD oldGoid = FindGoidByScid(godb, scid);
        if (oldGoid != 0 && oldGoid != 0xFFFFFFFFu && oldGoid != goid) {
            void* oldGo = GoByGoid(oldGoid);
            if (oldGo && Readable(oldGo, 0x9C)) {
                const char* oldTpl = GoTemplateName(oldGo);
                if (doorfix::IsDoorTemplateName(oldTpl)) {
                    BYTE oldF96 = 0;
                    ReadVal(oldGo, 0x96, &oldF96);
                    if ((oldF96 & 0x08u) == 0) {
                        MarkGoForDeletion(godb, oldGoid);
                        if (++g_cliStaleDelLogCount <= 30) {
                            Log("DoorFix[CLI]: marked stale door goid=0x%08lX for deletion before recreating scid=0x%08lX as goid=0x%08lX (tpl=%s)",
                                oldGoid, scid, goid, oldTpl);
                        }
                    }
                }
            }
        }
    }

    // Guard 3: Defensive reset if [GoDb+0x300] (m_bFullXfer) is somehow stuck non-zero at depth 0.
    BYTE  pre300 = 0, post300 = 0;
    DWORD pre308 = 0, post308 = 0;
    if (godb && Readable(godb, 0x310)) {
        ReadVal(godb, 0x300, &pre300);
        ReadVal(godb, 0x308, &pre308);
        if (doorfix::ShouldClearStuckFullXferAtIdle(pre308, pre300)) {
            *(BYTE*)((BYTE*)godb + 0x300) = 0;
            if (++g_stuckFlagFixCount <= 30) {
                Log("DoorFix[CLI]: cleared stuck GoDb+0x300=%u at depth=0 before CreateGo_Client goid=0x%08lX scid=0x%08lX",
                    (unsigned)pre300, goid, scid);
            }
            pre300 = 0;
        }
    }

    BYTE ret = g_origCreateGoCli(godb, edx, goid, seed, req, bForce, xferBuf, xferSz, a7, a8);
    if (!ret && ++g_cliFailLogCount <= 50) {
        if (godb && Readable(godb, 0x310)) {
            ReadVal(godb, 0x300, &post300);
            ReadVal(godb, 0x308, &post308);
        }
        void* postExist = GoByGoid(goid);
        DWORD scidGoid = FindGoidByScid(godb, scid);
        void* snode = reqNode ? GetLoadedNode(reqNode) : nullptr;
        BYTE nodeLoaded = 0;
        if (snode && Readable(snode, 0x101)) {
            ReadVal(snode, 0x100, &nodeLoaded);
        }
        Log("DoorFix[CLI]: CreateGo_Client FAILED goid=0x%08lX scid=0x%08lX reg=0x%lX pl=0x%lX reqNode=0x%08lX(snode=%p,ld=%u) fl=0x%02X bF=%u f300=%u/%u d308=%lu/%lu xferSz=%lu existGo=%p scidGoid=0x%08lX",
            goid, scid, regId, plId, reqNode, snode, (unsigned)nodeLoaded, (unsigned)reqFlags,
            (unsigned)(bForce & 0xFFu), (unsigned)pre300, (unsigned)post300, pre308, post308, xferSz, postExist, scidGoid);
    }
    return ret;
}

// ---- Part 1e: Root-cause fix for GoDb::CloneGoImpl (0x0054A7BA) stuck GoDb+0x300 leak ----
// Stock GoDb::CloneGoImpl (0x0054A7BA) sets [GoDb+0x300] = bForce at 0x0054A820, then calls
// GoDb::AllocGoid (0x00552C50) at 0x0054A826. If AllocGoid returns GOID_INVALID (e.g. duplicate
// RCCloneGo for an already-live clone/sub-clone, or slot collision), 0x0054A836 jumps directly
// to 0x0054A816 -> 0x0054A9AB, completely skipping 0x0054A990 (`and BYTE PTR [ebx+0x300], 0`).
// Once [GoDb+0x300] is stuck at 1, IsForceCreateActive (0x00549853) returns TRUE on EVERY future
// CreateGo_Client and CloneGo_Client call (because AutoForceGoid sets [GoDb+0x308]=1), permanently
// failing 100% of global object creations (doors, use_points, actors, containers) for the entire
// rest of the session while CreateLocalGo (0x00549FBD) continues working because it bypasses 0x549853.
// Clearing any newly leaked [GoDb+0x300] on exit eliminates this permanently on both client and server.
static DWORD __fastcall Hook_CloneGoImpl(void* godb, void* edx, void* req,
                                         DWORD bForce, DWORD bIsContent, DWORD bDontNotify) {
    BYTE pre300 = 0;
    bool validGodb = godb && Readable(godb, 0x310);
    if (validGodb) {
        ReadVal(godb, 0x300, &pre300);
    }
    DWORD ret = g_origCloneGoImpl(godb, edx, req, bForce, bIsContent, bDontNotify);
    if (validGodb) {
        BYTE post300 = 0;
        ReadVal(godb, 0x300, &post300);
        if (doorfix::ShouldClearLeakedFullXferFlag(pre300, post300)) {
            *(BYTE*)((BYTE*)godb + 0x300) = 0;
            if (++g_cloneLeakFixCount <= 30) {
                Log("DoorFix: cleared leaked GoDb+0x300 after failed CloneGoImpl (bForce=%u ret=0x%08lX)",
                    (unsigned)(bForce & 0xFFu), ret);
            }
        }
    }
    return ret;
}

// ---- Part 1f: Client-side CloneGo_Client duplicate-GOID guard (0x0054AF1F) ----
// Mirror of Hook_CreateGoClient for cloned/sub-cloned Gos (items, potions, weapons, spells,
// spawned monsters). When a duplicate RCCloneGo arrives for an already-live/pending GOID on the
// client, stock CloneGo_Client -> AllocGoid leaks bucket refcount + [GoDb+0x110], leaks [GoDb+0x300]=1
// (when bForce=1, which SyncClientGo 0x0055217E sets for EVERY sub-clone!), and calls CleanupFailedCreate
// (0x005510B8) -> FreeGoid on the LIVE clone Go. Short-circuiting here prevents all three bugs at the source.
static BYTE __fastcall Hook_CloneGoClient(void* godb, void* edx, DWORD goid, DWORD seed, void* req,
                                          const char* tplName, DWORD bForce, void* xferBuf, DWORD xferSz,
                                          DWORD a8, DWORD a9) {
    void* existGo = GoByGoid(goid);
    if (existGo && Readable(existGo, 0x9C)) {
        BYTE f96 = 0;
        ReadVal(existGo, 0x96, &f96);
        if ((f96 & 0x08u) == 0 && xferBuf && xferSz) {
            GoXferSyncData(existGo, xferBuf, xferSz);
        }
        if (godb && Readable(godb, 0x334)) {
            DWORD scratchBase = 0;
            if (ReadVal(godb, 0x32C, &scratchBase)) {
                *(DWORD*)((BYTE*)godb + 0x330) = scratchBase;
            }
            DWORD lastOff = IsPrimaryThread() ? 0x2F0u : 0x2F4u;
            *(DWORD*)((BYTE*)godb + lastOff) = goid;
        }
        if (++g_cliCloneDupLogCount <= 30) {
            Log("DoorFix[CLI]: skipped duplicate CloneGo_Client for live goid=0x%08lX tpl=%s bF=%u f96=0x%02X xferSz=%lu",
                goid, GoTemplateName(existGo), (unsigned)(bForce & 0xFFu), (unsigned)f96, xferSz);
        }
        return 1;
    }

    BYTE  pre300 = 0, post300 = 0;
    DWORD pre308 = 0, post308 = 0;
    if (godb && Readable(godb, 0x310)) {
        ReadVal(godb, 0x300, &pre300);
        ReadVal(godb, 0x308, &pre308);
        if (doorfix::ShouldClearStuckFullXferAtIdle(pre308, pre300)) {
            *(BYTE*)((BYTE*)godb + 0x300) = 0;
            if (++g_stuckFlagFixCount <= 30) {
                Log("DoorFix[CLI]: cleared stuck GoDb+0x300=%u at depth=0 before CloneGo_Client goid=0x%08lX",
                    (unsigned)pre300, goid);
            }
            pre300 = 0;
        }
    }

    BYTE ret = g_origCloneGoCli(godb, edx, goid, seed, req, tplName, bForce, xferBuf, xferSz, a8, a9);
    if (!ret && ++g_cliCloneFailLogCnt <= 50) {
        if (godb && Readable(godb, 0x310)) {
            ReadVal(godb, 0x300, &post300);
            ReadVal(godb, 0x308, &post308);
        }
        const char* safeTpl = (tplName && Readable(tplName, 1)) ? tplName : "";
        Log("DoorFix[CLI]: CloneGo_Client FAILED goid=0x%08lX tpl=%s bF=%u f300=%u/%u d308=%lu/%lu xferSz=%lu existGo=%p",
            goid, safeTpl, (unsigned)(bForce & 0xFFu), (unsigned)pre300, (unsigned)post300,
            pre308, post308, xferSz, GoByGoid(goid));
    }
    return ret;
}

// ---- Part 2: Door GoDb snapshot (lock-safe, covers both committed GlobalGoDb and pending maps) ----
// GoDb+0xBC is the flat GlobalGoDb bucket table:
//   +0x10 (GoDb+0xCC): begin ptr
//   +0x14 (GoDb+0xD0): end ptr
//   +0x34 (GoDb+0xF0): read/write lock (read-lock via [0x7AC398], read-unlock via [0x7AC394])
//   stride = 0x1C; [entry+0x00] = primary Go*, [entry+0x10] = magic (nonzero if occupied).
// Pending Gos awaiting CommitPendingGos (0x00551274) have primary Go* == NULL in GlobalGoDb and live
// in two MSVC6 std::map<DWORD, Go*> trees at GoDb+0x2B0 and GoDb+0x2C0 guarded by GoDb+0x2D0.
// Including pending doors prevents false "missing" reports while a freshly streamed door awaits commit.
static void SnapshotLoadedDoorGoids(void* godb, std::vector<DWORD>& outGoids) {
    outGoids.clear();
    if (!godb || !Readable(godb, 0x100)) return;

    auto readLock   = (void (__thiscall*)(void*))(*(void**)0x007AC398);
    auto readUnlock = (void (__thiscall*)(void*))(*(void**)0x007AC394);
    if (!readLock || !readUnlock) return;

    auto maybeAddDoor = [&](void* go) {
        if (!go || !Readable(go, 0x9C)) return;
        DWORD goid = *(const DWORD*)((const BYTE*)go + 0x4C);
        if (!goid || goid == 0xFFFFFFFFu || (goid & 0xC0000000u) != 0) return;
        DWORD scid = *(const DWORD*)((const BYTE*)go + 0x50);
        if (!scid || scid == 0xFFFFFFFFu) return;
        const char* tpl = GoTemplateName(go);
        if (!doorfix::IsDoorTemplateName(tpl)) return;
        for (DWORD existing : outGoids) {
            if (existing == goid) return;
        }
        outGoids.push_back(goid);
    };

    void* rwLock = (BYTE*)godb + 0xF0;
    readLock(rwLock);
    BYTE* begin = nullptr;
    BYTE* end   = nullptr;
    bool ok = ReadVal(godb, 0xCC, &begin) && ReadVal(godb, 0xD0, &end);
    if (ok && begin && end > begin) {
        size_t bytes = (size_t)(end - begin);
        if (bytes % 0x1Cu == 0 && bytes <= 0x1Cu * 65536u && Readable(begin, bytes)) {
            for (BYTE* cur = begin; cur < end; cur += 0x1Cu) {
                DWORD magic = *(const DWORD*)(cur + 0x10);
                if (!magic) continue;
                void* go = *(void* const*)(cur + 0x00);
                maybeAddDoor(go);
            }
        }
    }
    readUnlock(rwLock);

    // Second pass (separate lock, never nested): walk GoDb+0x2B0 and GoDb+0x2C0 pending maps.
    if (Readable(godb, 0x2F0)) {
        void* pendLock = (BYTE*)godb + 0x2D0;
        readLock(pendLock);
        static const size_t kPendingMapOffsets[2] = { 0x2B0u, 0x2C0u };
        for (size_t mapOff : kPendingMapOffsets) {
            BYTE* sentinel = nullptr;
            DWORD mapSize  = 0;
            if (!ReadVal(godb, mapOff + 0x04, &sentinel) || !ReadVal(godb, mapOff + 0x0C, &mapSize)) continue;
            if (mapSize == 0 || !sentinel || !Readable(sentinel, 0x14)) continue;
            BYTE* node = *(BYTE* const*)(sentinel + 0x00);  // leftmost node (begin())
            int guard = 0;
            while (node && node != sentinel && ++guard <= 8192) {
                if (!Readable(node, 0x14)) break;
                void* go = *(void* const*)(node + 0x10);
                maybeAddDoor(go);
                RbTreeNext(&node);
            }
        }
        readUnlock(pendLock);
    }
}

// ---- Part 3: Client/Server DoorSync protocol & self-healing ----

struct QueuedReport {
    DWORD dpnid;
    doorfix::DoorSyncReport report;
};

static CRITICAL_SECTION g_lock;
static bool             g_lockInit = false;
static void*            g_dp8Client = nullptr;

static std::vector<QueuedReport> g_pendingReports;
static std::vector<DWORD>        g_destroyedPlayers;
static std::vector<DWORD>        g_failedHealGoids;
static doorfix::DoorMissingTracker g_tracker;

static DWORD g_lastClientScanMs = 0;
static DWORD g_lastClientSendMs = 0;
static DWORD g_lastHeroNode     = 0;
static DWORD g_nodeChangedAtMs  = 0;
static DWORD g_lastPruneMs      = 0;

static void SendDoorReportToServer(const doorfix::DoorSyncReport& rep) {
    void* dpClient = g_dp8Client;
    if (!dpClient || !Readable(dpClient, 4)) return;
    void** vtbl = *(void***)dpClient;
    if (!vtbl || !Readable(vtbl, 9 * sizeof(void*))) return;

    struct DpnBufferDesc {
        DWORD size;
        BYTE* data;
    };
    size_t sz = doorfix::SerializedReportSize(rep.count);
    DpnBufferDesc desc = { (DWORD)sz, (BYTE*)&rep };
    DWORD asyncHandle = 0;
    typedef HRESULT(WINAPI* SendFn)(void*, const DpnBufferDesc*, DWORD, DWORD, void*, DWORD*, DWORD);
    ((SendFn)vtbl[8])(dpClient, &desc, 1, 0, nullptr, &asyncHandle, 0x08 /*DPNSEND_GUARANTEED*/);
}

static bool IsInGameWorldState() {
    void* ws = *(void**)0x007ACF44;
    if (!ws || !Readable(ws, 0x24)) return false;
    int cur = *(const int*)((const BYTE*)ws + 0x20);
    return cur == eng::WS_MP_INGAME || cur == eng::WS_MP_INGAME_JIP;
}

static bool HasFailedHeal(DWORD goid) {
    for (DWORD g : g_failedHealGoids) {
        if (g == goid) return true;
    }
    return false;
}

static void ClientDoorTick() {
    if (!IsInGameWorldState()) {
        g_lastHeroNode = 0;
        g_nodeChangedAtMs = 0;
        g_failedHealGoids.clear();
        return;
    }
    DWORD now = GetTickCount();
    if (now - g_lastClientScanMs < 400u) return;
    g_lastClientScanMs = now;

    void* godb = *(void**)0x007ACF4C;
    void* hero = ScreenHero();
    if (!godb || !hero) return;

    DWORD heroNode = 0;
    ReadGoNode(hero, &heroNode);
    if (heroNode != g_lastHeroNode) {
        g_lastHeroNode = heroNode;
        g_nodeChangedAtMs = now;
    }

    std::vector<DWORD> doorGoids;
    SnapshotLoadedDoorGoids(godb, doorGoids);

    doorfix::DoorSyncReport rep{};
    rep.magic    = doorfix::kDoorSyncMagic;
    rep.version  = doorfix::kDoorSyncVersion;
    rep.heroNode = heroNode;

    for (DWORD goid : doorGoids) {
        void* go = GoByGoid(goid);
        if (!go || !Readable(go, 0x9C)) continue;

        BYTE f96 = 0;
        ReadVal(go, 0x96, &f96);
        if (f96 & 0x08u) continue;  // marked for deletion

        // Report every live/pending door present in client GoDb so server knows we have it.
        if (rep.count < doorfix::kMaxReportedDoors) {
            rep.doorGoids[rep.count++] = goid;
        } else {
            rep.flags |= doorfix::kFlagTruncated;
        }

        // Client-side P2 self-heal: committed door in our frustum with purged nema (lod >= 2)
        // or invisible while still usable -> reconstitute immediately so it renders & is clickable.
        if ((f96 & 0x80u) != 0) continue;  // still in pending creation map
        DWORD fm = 0;
        if (!ReadVal(go, 0x68, &fm) || fm == 0) continue;

        DWORD nodeGuid = 0;
        if (!ReadGoNode(go, &nodeGuid) || !nodeGuid) continue;
        void* snode = GetLoadedNode(nodeGuid);
        if (!snode || !Readable(snode, 0x10C)) continue;
        BYTE loaded = 0;
        if (!ReadVal(snode, 0x100, &loaded) || !loaded) continue;

        void* asp = nullptr;
        if (!ReadVal(go, 0x04, &asp) || !asp || !Readable(asp, 0x48)) continue;
        DWORD a3c = 0;
        void* nema = nullptr;
        ReadVal(asp, 0x3C, &a3c);
        ReadVal(asp, 0x44, &nema);

        DWORD scid = 0;
        ReadVal(go, 0x50, &scid);

        bool isVisible = (a3c & 0x0001u) != 0;
        bool isUsable  = (a3c & 0x0800u) != 0;

        if (nema && Readable(nema, 0x7C)) {
            int lod = 0;
            ReadVal(nema, 0x78, &lod);
            if (lod >= 2 && !HasFailedHeal(goid)) {
                bool ok = NemaReconstitute(nema, 0, 1);
                if (isUsable) NemaResetPose(nema);
                int newLod = 0;
                ReadVal(nema, 0x78, &newLod);
                if (newLod >= 2 && g_failedHealGoids.size() < 64) {
                    g_failedHealGoids.push_back(goid);
                }
                Log("DoorFix[CLI]: healed purged door scid=0x%08lX goid=0x%08lX tpl=%s node=0x%08lX (lod %d -> %d, ok=%d)",
                    scid, goid, GoTemplateName(go), nodeGuid, lod, newLod, ok ? 1 : 0);
            }
        }

        if (isUsable && !isVisible) {
            SetIsVisible(asp, true);
            Log("DoorFix[CLI]: restored visibility on usable door scid=0x%08lX goid=0x%08lX tpl=%s",
                scid, goid, GoTemplateName(go));
        }
    }

    bool duePeriodic   = (now - g_lastClientSendMs) >= 1500u;
    bool dueNodeChange = g_nodeChangedAtMs != 0 &&
                         (now - g_nodeChangedAtMs) >= 400u &&
                         (now - g_lastClientSendMs) >= 600u;
    if (duePeriodic || dueNodeChange) {
        g_lastClientSendMs = now;
        if (dueNodeChange) g_nodeChangedAtMs = 0;
        SendDoorReportToServer(rep);
    }
}

static void ServerDoorTick() {
    if (!g_lockInit || !IsInGameWorldState()) return;

    std::vector<QueuedReport> batch;
    std::vector<DWORD> gone;
    EnterCriticalSection(&g_lock);
    batch.swap(g_pendingReports);
    gone.swap(g_destroyedPlayers);
    LeaveCriticalSection(&g_lock);

    for (DWORD dpnid : gone) {
        g_tracker.ClearPlayer(dpnid);
    }

    DWORD now = GetTickCount();
    if (now - g_lastPruneMs >= 10000u) {
        g_lastPruneMs = now;
        g_tracker.PruneStale(now);
    }

    if (batch.empty()) return;

    void* godb = *(void**)0x007ACF4C;
    if (!godb) return;

    std::vector<DWORD> doorGoids;
    SnapshotLoadedDoorGoids(godb, doorGoids);
    if (doorGoids.empty()) return;

    for (const auto& item : batch) {
        if ((item.report.flags & doorfix::kFlagTruncated) != 0) continue;

        void* player = FindPlayerByMachineId(item.dpnid);
        if (!player || !Readable(player, 0x20)) continue;
        if (!IsPlayerValid(player) || !IsPlayerHuman(player)) continue;
        DWORD ctrl = 0;
        if (!ReadVal(player, 0x14, &ctrl) || ctrl == 2) continue;  // skip ServerHost
        DWORD plFrustum = 0;
        DWORD plId      = 0;
        ReadVal(player, 0x18, &plFrustum);
        ReadVal(player, 0x0C, &plId);
        if (!plFrustum) continue;

        for (DWORD goid : doorGoids) {
            void* go = GoByGoid(goid);
            if (!go || !Readable(go, 0x9C)) continue;

            BYTE f96 = 0;
            ReadVal(go, 0x96, &f96);
            if ((f96 & 0x88u) != 0) continue;  // pending or marked for deletion

            DWORD fm = 0;
            if (!ReadVal(go, 0x68, &fm) || (fm & plFrustum) == 0) continue;

            void* asp = nullptr;
            if (!ReadVal(go, 0x04, &asp) || !asp || !Readable(asp, 0x48)) continue;
            DWORD a3c = 0;
            ReadVal(asp, 0x3C, &a3c);

            bool doesBlock    = (a3c & 0x0040u) != 0;
            bool isSelectable = (a3c & 0x0200u) != 0;
            bool isUsable     = (a3c & 0x0800u) != 0;
            if (!doesBlock) continue;

            DWORD scid = 0;
            ReadVal(go, 0x50, &scid);
            const char* tpl = GoTemplateName(go);

            // Server P3 self-heal: oneshot gear door already in Done$ (ScidBit 1 == true,
            // non-selectable & non-usable) must never block pathing.
            if (doorfix::IsGearDoorTemplateName(tpl) && !isSelectable && !isUsable && GetScidBit(godb, scid, 1)) {
                SetDoesBlockPath(asp, false);
                Log("DoorFix[SRV]: cleared stale blocking flag on Done$ door scid=0x%08lX goid=0x%08lX tpl=%s",
                    scid, goid, tpl);
                continue;
            }

            if (doorfix::ReportContainsGoid(item.report, goid)) {
                g_tracker.MarkPresent(item.dpnid, goid);
                continue;
            }

            if (g_tracker.RecordMissing(item.dpnid, goid, item.report.heroNode, now)) {
                DWORD nodeGuid = 0;
                ReadGoNode(go, &nodeGuid);
                DWORD regId = 0;
                BYTE  f97   = 0;
                ReadVal(go, 0x64, &regId);
                ReadVal(go, 0x97, &f97);
                DWORD bFullSync = (f97 >> 3) & 1u;
                Log("DoorFix[SRV]: RESYNC missing door scid=0x%08lX goid=0x%08lX tpl=%s reg=0x%lX node=0x%08lX fm=0x%lX a3c=0x%04lX f96=0x%02X f97=0x%02X -> dpnid=0x%08lX plId=0x%lX heroNode=0x%08lX",
                    scid, goid, tpl, regId, nodeGuid, fm, a3c, (unsigned)f96, (unsigned)f97, item.dpnid, plId, item.report.heroNode);
                SyncGoToClient(godb, player, go, bFullSync);
            }
        }
    }
}

}  // namespace

void DoorFixInstall() {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_lock);
        g_lockInit = true;
    }
    if (!g_xferLockInit) {
        InitializeCriticalSection(&g_xferLock);
        g_xferLockInit = true;
    }
    static const BYTE kExpectCloneGoImpl[6] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14};
    InstallDetour(0x0054A7BA, kExpectCloneGoImpl, 6, (void*)&Hook_CloneGoImpl, (void**)&g_origCloneGoImpl, "GoDb::CloneGoImpl");

    if (Cfg().role == Role::Server) {
        static const BYTE kExpectSCreate[9]    = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
        static const BYTE kExpectSClone[9]     = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB4, 0x00, 0x00, 0x00};
        static const BYTE kExpectCreateXfer[8] = {0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C, 0x24, 0x0C};
        static const BYTE kExpectCloneXfer[9]  = {0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x7C, 0x24, 0x10};
        static const BYTE kExpectPackQueue[6]  = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C};
        InstallDetour(0x00549A0E, kExpectSCreate,    9, (void*)&Hook_SCreateGo,            (void**)&g_origSCreateGo,     "GoDb::SCreateGo");
        InstallDetour(0x0054A9E6, kExpectSClone,     9, (void*)&Hook_SCloneGo,             (void**)&g_origSCloneGo,      "GoDb::SCloneGo");
        InstallDetour(0x005451B1, kExpectCreateXfer, 8, (void*)&Hook_GoCreateReq_Xfer,     (void**)&g_origCreateReqXfer, "GoCreateReq::Xfer");
        InstallDetour(0x005459C9, kExpectCloneXfer,  9, (void*)&Hook_GoCloneReq_Xfer,      (void**)&g_origCloneReqXfer,  "GoCloneReq::Xfer");
        InstallDetour(0x00594D71, kExpectPackQueue,  6, (void*)&Hook_NetFuBiSend_PackQueue,(void**)&g_origPackQueue,     "NetFuBiSend::PackQueue");
    } else if (Cfg().role == Role::Client) {
        static const BYTE kExpectCGC[6] = {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56};
        static const BYTE kExpectClGC[6] = {0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56};
        InstallDetour(0x00549E47, kExpectCGC,  6, (void*)&Hook_CreateGoClient, (void**)&g_origCreateGoCli, "GoDb::CreateGo_Client");
        InstallDetour(0x0054AF1F, kExpectClGC, 6, (void*)&Hook_CloneGoClient,  (void**)&g_origCloneGoCli,  "GoDb::CloneGo_Client");
    }
    Log("DoorFix: installed (role=%s)", Cfg().role == Role::Server ? "server" : "client");
}

void DoorFixTick() {
    if (Cfg().role == Role::Server) {
        ServerDoorTick();
    } else if (Cfg().role == Role::Client) {
        ClientDoorTick();
    }
}

void DoorFix_SetDirectPlay(void* dpClient, void* dpServer) {
    if (dpClient) g_dp8Client = dpClient;
    (void)dpServer;
}

bool DoorFix_OnReceivePacket(bool isServer, unsigned long senderDpnid, const unsigned char* data, unsigned long size) {
    doorfix::DoorSyncReport rep{};
    if (!doorfix::ValidateReportPacket(data, size, &rep)) return false;
    if (!isServer || !g_lockInit) return true;  // consume packet even if received out of role

    EnterCriticalSection(&g_lock);
    // Coalesce: keep only the latest report per dpnid so queue stays tiny under load.
    bool replaced = false;
    for (auto& q : g_pendingReports) {
        if (q.dpnid == senderDpnid) {
            q.report = rep;
            replaced = true;
            break;
        }
    }
    if (!replaced && g_pendingReports.size() < 64) {
        g_pendingReports.push_back({senderDpnid, rep});
    }
    LeaveCriticalSection(&g_lock);
    return true;
}

void DoorFix_OnPlayerDestroyed(unsigned long dpnid) {
    if (!g_lockInit || !dpnid) return;
    EnterCriticalSection(&g_lock);
    if (g_destroyedPlayers.size() < 64) {
        g_destroyedPlayers.push_back(dpnid);
    }
    LeaveCriticalSection(&g_lock);
}
