// snapshot.cpp — captures in-game screenshots for headless host monitoring and diagnostics.
//
// 0.4.1 used GDI PrintWindow/BitBlt: on real Windows the Direct3D (DDrawCompat) surface came out fully black
// and the desktop fallback could capture other apps. The engine screenshot reads the game's own
// back buffer, so it shows only the game.
//
// Files land in "<user_path>\Screen Shots\Dungeon Siege Screen - NNNN.bmp"; the log line records which label
// each numbered file belongs to.
#include "siegehost.h"
#include "engine.h"
#include <set>

namespace {
int g_count = 0;
std::set<std::string> g_seen;
}  // namespace

void Snapshot(const char* label) {
    if (g_count >= 40 || !g_seen.insert(label).second) return;  // one shot per distinct screen, max 40 (~1.4 MB each)
    void* app = eng::RapiAppModule_Singleton();
    if (!app) { Log("Snapshot skipped (no RapiAppModule): %s", label); return; }
    // Exactly what the "take_screenshot" key binding does (0x476B9B): this->vtbl[0xFC](0, 0, 0).
    auto vfn = (bool (__thiscall*)(void*, void*, void*, void*))(*(void***)app)[0xFC / 4];
    Log("Snapshot #%d requesting: %s", g_count + 1, label);
    bool ok = vfn(app, nullptr, nullptr, nullptr);
    ++g_count;
    Log("Snapshot #%d %s (game screenshot -> <user_path>\\Screen Shots, newest file): %s", g_count, ok ? "saved" : "FAILED", label);
}
