// netrate.cpp — measure the game's main-loop rate and the engine's multiplayer send rate,
// and optionally raise that send rate.
//
// Reverse engineering (see 03-analysis/RE-05-mp-send-rate.md):
//   ?SetMPSendFrequency@TattooGame@@QAEXH@Z  0x00475C53  -> NetFuBiSend[0x40] = 1.0f / (float)freq
//   ?SetSendDelay@NetFuBiSend@@QAEXM@Z       0x00471D98  -> NetFuBiSend[0x40] = delay (seconds)
//   ?FUBI_GetClassSingleton@NetFuBiSend@@    0x004781E2  -> returns *(void**)0x007ACF68
// So the float at NetFuBiSend+0x40 is "seconds between network flushes". Measured in the lab
// the engine default is 0.200 s = 5 Hz, on the host and on every client. Live NA/EU
// play showed ~13.3 packets/s per client counting BOTH directions, i.e. ~6.6/s each way, which is
// that same 5 Hz plus DirectPlay's own acks and keep-alives. The rate was identical on a
// CPU-saturated host and an idle one, so it is an engine setting, not a symptom of load.
//
// Raising it is capped by the host's frame rate: the flush happens on the main pump, so a host
// running at N fps cannot send faster than N Hz however low the delay is set. That is why this file
// also reports fps and MPSleep — above ~15 Hz the frame rate, not this field, is the limit.
//
// We do not call SetMPSendFrequency: it is __thiscall and guarded on engine state. Writing the
// field directly is equivalent and reapplying it every AutoHost/AutoJoin tick also covers the case
// where the engine resets it (the "reapplied" counter in the log would show that happening).

#include "siegehost.h"

#include "engine.h"

#include <math.h>
#include <stdio.h>

namespace {

// The exe is not ASLR-relocated (image base 0x400000); every address in engine.h is absolute.
const DWORD kExpectedImageBase = 0x00400000;
void** const kNetPipeSlot = (void**)0x007ACF64;
void** const kNetFuBiSendSlot = (void**)0x007ACF68;
void** const kNetFuBiReceiveSlot = (void**)0x007ACF6C;
const size_t kSendDelayOffset = 0x40;
const size_t kRecvRetryDelayOffset = 0x60;
const size_t kNetPipeBackupThresholdOffset = 0x168;
const DWORD kTargetBackupThreshold = 0x20000;  // 128 KB (replaces 1 KB dial-up default at NetPipe+0x168)

// A send delay outside this range means the field is not what we think it is: never write then.
const float kMinSaneDelay = 0.001f;  // 1000 Hz
const float kMaxSaneDelay = 1.000f;  //    1 Hz

volatile LONG g_frames = 0;   // incremented from the main pump
DWORD g_windowStart = 0;
DWORD g_windowFrames = 0;
float g_fps = 0.0f;

bool g_baseChecked = false;
bool g_baseOk = false;
bool g_loggedDefault = false;
bool g_refusedOnce = false;
bool g_loggedRecvRetry = false;
bool g_loggedBackupThreshold = false;
float g_defaultDelay = 0.0f;
float g_curDelay = 0.0f;
long g_reapplied = 0;

// TattooGame+0x248 is the argument of a Sleep() in the game's main loop:
//   ?SetMPSleep@TattooGame@@QAEXH@Z  0x00401C3F  -> TattooGame[0x248] = ms
//   FUN_00477118                                 -> Sleep(TattooGame[0x248])
// Read-only unless [Net] MPSleep is set: lowering it raises the frame rate and the CPU cost.
const size_t kMpSleepOffset = 0x248;
int g_mpSleep = -1;

bool IsWritableField(const void* field) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(field, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & writable) != 0;
}

int* MpSleepPtr() {
    if (!g_baseOk) return nullptr;
    void* tg = eng::TattooGame_Singleton();
    if (!tg) return nullptr;
    char* field = (char*)tg + kMpSleepOffset;
    return IsWritableField(field) ? (int*)field : nullptr;
}

float* SendDelayPtr() {
    if (!g_baseChecked) {
        g_baseChecked = true;
        g_baseOk = ((DWORD)(ULONG_PTR)GetModuleHandleA(nullptr) == kExpectedImageBase);
        if (!g_baseOk)
            Log("NetRate: exe image base is %p, expected 0x%08lX - send-rate control disabled.",
                GetModuleHandleA(nullptr), kExpectedImageBase);
    }
    if (!g_baseOk) return nullptr;

    void* obj = *kNetFuBiSendSlot;  // null until the network singleton exists
    if (!obj) return nullptr;

    char* field = (char*)obj + kSendDelayOffset;
    return IsWritableField(field) ? (float*)field : nullptr;
}

float* RecvRetryDelayPtr() {
    if (!g_baseOk) return nullptr;
    void* obj = *kNetFuBiReceiveSlot;
    if (!obj) return nullptr;
    char* field = (char*)obj + kRecvRetryDelayOffset;
    return IsWritableField(field) ? (float*)field : nullptr;
}

DWORD* NetPipeBackupThresholdPtr() {
    if (!g_baseOk) return nullptr;
    void* obj = *kNetPipeSlot;
    if (!obj) return nullptr;
    char* field = (char*)obj + kNetPipeBackupThresholdOffset;
    return IsWritableField(field) ? (DWORD*)field : nullptr;
}

}  // namespace

// Called from the PeekMessageA hook on the main thread, once per frame. Must stay trivial.
void NR_PumpTick() { InterlockedIncrement(&g_frames); }

// Called from AutoHostTick / AutoJoinTick (every 250 ms).
void NR_Tick() {
    DWORD now = GetTickCount();
    if (!g_windowStart) {
        g_windowStart = now;
        g_windowFrames = (DWORD)g_frames;
    } else if (now - g_windowStart >= 1000) {
        DWORD frames = (DWORD)g_frames;
        g_fps = (float)(frames - g_windowFrames) * 1000.0f / (float)(now - g_windowStart);
        g_windowStart = now;
        g_windowFrames = frames;
    }

    float* p = SendDelayPtr();
    if (!p) return;

    float cur = *p;
    if (!(cur >= kMinSaneDelay && cur <= kMaxSaneDelay)) {
        if (!g_refusedOnce) {
            g_refusedOnce = true;
            Log("NetRate: send delay at NetFuBiSend+0x%02X reads %g, outside the sane range %g..%g - "
                "not touching it.", (unsigned)kSendDelayOffset, cur, kMinSaneDelay, kMaxSaneDelay);
        }
        return;
    }

    if (!g_loggedDefault) {
        g_loggedDefault = true;
        g_defaultDelay = cur;
        Log("NetRate: engine multiplayer send delay = %.5f s (%.2f Hz) [engine default]", cur, 1.0f / cur);
    }

    if (int* ms = MpSleepPtr()) {
        if (*ms >= 0 && *ms <= 1000) {
            if (g_mpSleep < 0) Log("NetRate: main-loop MPSleep = %d ms [engine default]", *ms);
            int wantSleep = Cfg().mpSleep;
            if (wantSleep >= 0 && *ms != wantSleep) {
                Log("NetRate: main-loop MPSleep %d -> %d ms", *ms, wantSleep);
                *ms = wantSleep;
            }
            g_mpSleep = *ms;
        }
    }

    int want = Cfg().sendFrequency;
    if (want == 30) want = 64;  // Upgrade 30 Hz default to 64 Hz to prevent 40-44 FPS frame-skipping aliasing
    if (want > 0) {
        float want_delay = 1.0f / (float)want;
        if (fabsf(cur - want_delay) > 1e-6f) {
            *p = want_delay;
            ++g_reapplied;
            // First few, then occasionally: a counter that keeps climbing means the engine is
            // resetting the field and we are fighting it.
            if (g_reapplied <= 3 || (g_reapplied % 400) == 0)
                Log("NetRate: send frequency set to %d Hz (delay %.5f s) [apply #%ld]", want, want_delay,
                    g_reapplied);
        }
        cur = *p;

        if (float* pr = RecvRetryDelayPtr()) {
            float curRetry = *pr;
            if (curRetry >= kMinSaneDelay && curRetry <= kMaxSaneDelay && fabsf(curRetry - want_delay) > 1e-6f) {
                *pr = want_delay;
                if (!g_loggedRecvRetry) {
                    g_loggedRecvRetry = true;
                    Log("NetRate: NetFuBiReceive retry delay %.5f s -> %.5f s (%.1f Hz)", curRetry, want_delay,
                        1.0f / want_delay);
                }
            }
        }
    }
    if (DWORD* pt = NetPipeBackupThresholdPtr()) {
        DWORD curThresh = *pt;
        if (curThresh >= 0x100 && curThresh < kTargetBackupThreshold) {
            *pt = kTargetBackupThreshold;
            if (!g_loggedBackupThreshold) {
                g_loggedBackupThreshold = true;
                Log("NetRate: NetPipe outbound backup threshold %lu -> %lu bytes", (unsigned long)curThresh,
                    (unsigned long)kTargetBackupThreshold);
            }
        }
    }
    g_curDelay = cur;
}

std::string NR_HeartbeatLines() {
    char buf[192];
    if (g_curDelay > 0.0f)
        snprintf(buf, sizeof buf, "fps=%.1f\nsend_hz=%.2f\nsend_delay_ms=%.2f\nsend_hz_default=%.2f\nmp_sleep_ms=%d\n",
                 g_fps, 1.0f / g_curDelay, g_curDelay * 1000.0f,
                 g_defaultDelay > 0.0f ? 1.0f / g_defaultDelay : 0.0f, g_mpSleep);
    else
        snprintf(buf, sizeof buf, "fps=%.1f\nmp_sleep_ms=%d\n", g_fps, g_mpSleep);
    return buf;
}
