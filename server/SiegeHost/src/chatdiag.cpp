// LAB ONLY (SIEGEHOST_CHATDIAG=1): measures the chat_box line timer. Not installed otherwise.
#include "siegehost.h"
#include "engine.h"
#include <cstring>
namespace {
const DWORD kVtUpdate = 0x00730484, kVtDraw = 0x00730478;
typedef void(__fastcall* UpdFn)(void*, void*, double);
typedef void(__fastcall* DrawFn)(void*, void*);
UpdFn g_upd = nullptr; DrawFn g_draw = nullptr;
void* g_box = nullptr;
DWORD g_calls = 0, g_forced = 0, g_expired = 0, g_last = 0, g_frames = 0; double g_dt = 0;
int Lines(void* b) { int s = *(int*)((char*)b + 0x1B8), e = *(int*)((char*)b + 0x1BC); return s && e ? (e - s) / 0x14 : 0; }
float& Elapsed(void* b) { return *(float*)((char*)b + 0x1E4); }
DWORD g_any = 0;
void __fastcall Upd(void* self, void* edx, double dt) {
    ++g_any;
    if (*(float*)((char*)self + 0x1D8) > 0.0f) {
        if (g_box != self) { Log("CHATDIAG: chat_box %p (timeout %.2f)", self, *(float*)((char*)self + 0x1D8)); g_box = self; }
        ++g_calls; g_dt += dt;
        typedef int(__thiscall* GetPos)(void*);
        GetPos gp = (GetPos)(*(void***)self)[6];
        int p0 = gp(self); float e0 = Elapsed(self);
        g_upd(self, edx, dt);
        int p1 = gp(self);
        if (p1 != p0) { ++g_expired; Log("CHATDIAG: scroll %d -> %d (elapsed was %.2f, dt %.3f, lines %d, fadeoff %.2f)", p0, p1, e0, dt, Lines(self), *(float*)((char*)self + 0x224)); }
        return;
    }
    g_upd(self, edx, dt);
}
void __fastcall Draw(void* self, void* edx) {
    float before = Elapsed(self);
    g_draw(self, edx);
    float t = *(float*)((char*)self + 0x1D8);
    if (self == g_box && before < t && Elapsed(self) == t && Lines(self) > 0) ++g_forced;
}
}
void ChatDiagInstall() {
    char e[4];
    if (!GetEnvironmentVariableA("SIEGEHOST_CHATDIAG", e, sizeof e)) return;
    DWORD o;
    VirtualProtect((void*)kVtDraw, 0x10, PAGE_READWRITE, &o);
    g_upd = *(UpdFn*)kVtUpdate; *(void**)kVtUpdate = (void*)&Upd;
    g_draw = *(DrawFn*)kVtDraw; *(void**)kVtDraw = (void*)&Draw;
    VirtualProtect((void*)kVtDraw, 0x10, o, &o);
    Log("CHATDIAG: hooked chat_box update/draw (orig %p %p)", (void*)g_upd, (void*)g_draw);
}
void ChatDiagTick() {
    if (!g_upd) return;
    ++g_frames;
    DWORD now = GetTickCount();
    if (now - g_last < 1000) return;
    g_last = now;
    if (!g_box) {
        void* w = eng::FindWindow("console_box", nullptr);
        if (w) {
            void** vt = *(void***)w;
            Log("CHATDIAG: console_box=%p vtable=%p timeout=%.2f elapsed=%.2f lines=%d slots40..5C: %p %p %p %p %p %p %p %p",
                w, (void*)vt, *(float*)((char*)w + 0x1D8), Elapsed(w), Lines(w), vt[0x10], vt[0x11], vt[0x12], vt[0x13], vt[0x14], vt[0x15], vt[0x16], vt[0x17]);
        } else Log("CHATDIAG: console_box not found; any_update_calls=%lu", g_any);
    }
    g_any = 0;
    if (g_box)
        Log("CHATDIAG: 1s frames=%lu upd_calls=%lu dt_sum=%.3f elapsed=%.2f lines=%d forced_by_draw=%lu expired=%lu rect=%d,%d,%d,%d",
            g_frames, g_calls, g_dt, Elapsed(g_box), Lines(g_box), g_forced, g_expired,
            *(int*)((char*)g_box + 0x60), *(int*)((char*)g_box + 0x64), *(int*)((char*)g_box + 0x68), *(int*)((char*)g_box + 0x6C));
    g_frames = g_calls = 0; g_dt = 0; g_forced = 0; g_expired = 0;
}
