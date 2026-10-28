// engine.h — typed bindings to DSLOA.exe (v1.50, timestamp 0x3F8FFE7F) engine functions.
// Addresses come from the FuBi export table (see 03-analysis/RE-02-ui-automation.md).
#pragma once
#include <windows.h>

namespace eng {

// eUIMessage values (table at 0x7A5188: msg_none=0 ... confirmed: UIButton sends 9 on press, rollover=0xB)
enum UIMsg : int {
    MSG_ONLBUTTONDOWN = 3,
    MSG_ONLBUTTONUP = 4,
    MSG_ONBUTTONPRESS = 9,
    MSG_ONCHECK = 0x0D,
    MSG_ONUNCHECK = 0x0E,
};

struct gpstring { char* p; };  // GPG ref-counted COW string (header at p-10). Only construct via ctor below.

using ThisFn0 = void* (__thiscall*)(void* self);
// gpstring(const char*, const allocator&)       0x0042266A  thiscall, ret 8
static const auto gpstring_ctor = (void* (__thiscall*)(gpstring*, const char*, void*))0x0042266A;
// gpstring::~gpstring()                         0x00402FD9  thiscall
static const auto gpstring_dtor = (void (__thiscall*)(gpstring*))0x00402FD9;

// Singletons (FUBI_GetClassSingleton exports, cdecl, no args)
static const auto UIShell_Singleton = (void* (__cdecl*)())0x0047BC0F;     // [0x7ACFE4]
static const auto WorldState_Singleton = (void* (__cdecl*)())0x00492558;  // [0x7ACF44]
static const auto Server_Singleton = (void* (__cdecl*)())0x0047273B;

// UIShell
// UIWindow* UIShell::FindUIWindow(const char* name, const char* interfaceName)
static const auto UIShell_FindUIWindow = (void* (__thiscall*)(void*, const char*, const char*))0x006EA5A4;
// bool UIShell::IsInterfaceVisible(const gpstring&)
static const auto UIShell_IsInterfaceVisible = (bool (__thiscall*)(void*, const gpstring*))0x006ECDE7;
// void UIShell::ShowInterface(const gpstring&)
static const auto UIShell_ShowInterface = (void (__thiscall*)(void*, const gpstring*))0x006E8C12;
// void UIShell::HideInterface(const gpstring&)
static const auto UIShell_HideInterface = (void (__thiscall*)(void*, const gpstring*))0x006E8C20;
// bool UIShell::SendUIMessage(eUIMessage, const char* window, const char* interface)
static const auto UIShell_SendUIMessage = (bool (__thiscall*)(void*, int, const char*, const char*))0x006EA737;

// bool UIWindow::GetVisible() const   ([this+0x108])
static const auto UIWindow_GetVisible = (bool (__thiscall*)(void*))0x0047B07A;
// UIEditBox::SetText(const gpstring&)  (converts to wide and sets)
static const auto UIEditBox_SetText = (void (__thiscall*)(void*, const gpstring*))0x0047F74E;
// UIEditBox::SetPromptIndex(int) @ 0x006FB649
static const auto UIEditBox_SetPromptIndex = (void (__thiscall*)(void*, int))0x006FB649;
// UIEditBox::GetPromptIndex() @ 0x00702E73
static const auto UIEditBox_GetPromptIndex = (int (__thiscall*)(void*))0x00702E73;
// UIEditBox::SetTextPosition(int start, int end) @ 0x0047F723
static const auto UIEditBox_SetTextPosition = (void (__thiscall*)(void*, int, int))0x0047F723;
// UICheckbox::SetCheckState(bool)
static const auto UICheckbox_SetCheckState = (void (__thiscall*)(void*, bool))0x0047B491;

// WorldMap (see 03-analysis RE-03). Singleton [0x7ACFB8].
static const auto WorldMap_Singleton = (void* (__cdecl*)())0x00473099;
static const auto WorldMap_GetMapName = (gpstring* (__thiscall*)(void*))0x00473095;      // this+4
static const auto WorldMap_GetMpWorldName = (gpstring* (__thiscall*)(void*))0x00470D16;  // this+0xC
// bool WorldMap::SSet(const char* map, const char* mpWorld, DWORD) — what the staging map list calls (0x4824B2):
// resets players' start groups when the map changes, then RCSet (broadcast to clients). ret 0xC
static const auto WorldMap_SSet = (bool (__thiscall*)(void*, const char*, const char*, DWORD))0x0058E186;
// start groups: vector [this+0x1C, this+0x20), element 0x3C bytes, +0x10 bool default, +0x14 gpstring name
// Player::RSSetStartingGroup(const gpstring&) — group name stored at Player+0x4C
static const auto Player_RSSetStartingGroup = (void* (__thiscall*)(void*, const gpstring*))0x0056BFE4;
// bool IsValid(Player*) cdecl
static const auto Player_IsValid = (bool (__cdecl*)(void*))0x00569054;
static const auto Server_RCSetAllowStartLocationSelection = (void* (__thiscall*)(void*, bool))0x005701FA;

// WorldState
static const auto WorldState_GetCurrentState = (int (__thiscall*)(void*))0x004E8A96;
// const char* ToString(eWorldState)  cdecl
static const auto WorldState_ToString = (const char* (__cdecl*)(int))0x0052243C;
// eWorldState values (string table at 0x78E650, index == enum value; verified against IsInGame at 0x52245E):
//   7 ws_load_map, 9 ws_loaded_map, 0xB ws_wait_for_begin, 0x23 ws_mp_staging_area_server,
//   0x24 ws_mp_staging_area_client, 0x25 ws_mp_character_select, 0x28 ws_mp_ingame, 0x29 ws_mp_ingame_jip
enum { WS_MP_INGAME = 0x28, WS_MP_INGAME_JIP = 0x29 };
// bool IsInFrontend(eWorldState) cdecl — true for 0x1F..0x26, i.e. every multiplayer menu/staging screen
static const auto IsInFrontend = (bool (__cdecl*)(int))0x00522486;

// Simulation clock: [0x007ACF50] is the world-time object, +0x10 the current game time in seconds
// (read at the top of GoDb::Update 0x00549188 and of the Go update pass 0x00565EFC). It advances only
// when the world actually simulates, which makes it the honest answer to "is the world frozen?".
inline double WorldTime() {
    DWORD p = *(DWORD*)0x007ACF50;
    return p ? *(double*)(p + 0x10) : 0.0;
}
// WorldTime::GetSimCount() — +0x1C, incremented once per simulation step by WorldTime::Update
// (0x0057FA96). A world time that stands still with a rising sim count means something else is
// eating the step; both standing still means the game is not simulating at all.
inline unsigned WorldSimCount() {
    DWORD p = *(DWORD*)0x007ACF50;
    return p ? *(unsigned*)(p + 0x1C) : 0;
}
// TattooGame pause counter (+0x80) and flags (+0x4C): the frame update at 0x00477118 takes its
// paused branch when the counter is positive and bit 3 of the flags is clear.
inline int GamePauseCount() { void* g = ((void* (__cdecl*)())0x004856D3)(); return g ? *(int*)((char*)g + 0x80) : -1; }
inline unsigned GameFlags() { void* g = ((void* (__cdecl*)())0x004856D3)(); return g ? *(unsigned*)((char*)g + 0x4C) : 0; }

// Server
// Server::RCSetAllowPausing(bool) — stores Server+0x28 and broadcasts to clients
static const auto Server_RCSetAllowPausing = (void (__thiscall*)(void*, bool))0x0057031B;
// AppModule singleton [0x7A99A0]; IsUserPaused = byte this+0x7C
static const auto AppModule_Singleton = (void* (__cdecl*)())0x004011B5;
inline bool GamePaused() { void* a = AppModule_Singleton(); return a && *((unsigned char*)a + 0x7C); }
// TattooGame singleton; 0x475AF8 = thiscall (bool pause): local player requests pause (what the pause button does)
static const auto TattooGame_Singleton = (void* (__cdecl*)())0x004856D3;
static const auto TattooGame_RequestPause = (void (__thiscall*)(void*, bool))0x00475AF8;
static const auto Server_RCSetAllowJIP = (void (__thiscall*)(void*, bool))0x00570465;
static const auto Server_RCSetMaxPlayers = (void* (__thiscall*)(void*, unsigned))0x0056FD9F;
static const auto Server_RCSetDropInvOption = (void (__thiscall*)(void*, int))0x005707A8;
// Player* Server::GetLocalHumanPlayer()
static const auto Server_GetLocalHumanPlayer = (void* (__thiscall*)(void*))0x0056EBF8;

// RapiAppModule (renderer app). ScreenShot(const char* name) — same path as the in-game PrintScreen key:
// writes "<user_path>\Screen Shots\Dungeon Siege Screen - NNNN.bmp" from the Direct3D back buffer. nullptr = default name.
static const auto RapiAppModule_Singleton = (void* (__cdecl*)())0x004012BE;  // [0x7A99A8]
static const auto RapiAppModule_ScreenShot = (bool (__thiscall*)(void*, const char*))0x00401208;

// Player string members used by the staging "I'm ready" check (UIMultiplayer handler at 0x484BB1):
//   +0x58 hero name (wide gpstring), +0x64 hero clone-source template (gpstring). Both empty -> "Please select a character first."
inline const char* GpC(void* obj, int off) {
    char* p = *(char**)((char*)obj + off);
    return p ? p : "";
}

inline int GpLen(void* obj, int off) {
    char* p = *(char**)((char*)obj + off);
    return p ? *(int*)(p - 10) : 0;
}

// Convenience RAII wrapper
struct GpStr {
    gpstring s{nullptr};
    char alloc = 0;
    explicit GpStr(const char* text) { gpstring_ctor(&s, text, &alloc); }
    ~GpStr() { gpstring_dtor(&s); }
};

inline void* UI() { return UIShell_Singleton(); }

inline bool InterfaceVisible(const char* name) {
    void* ui = UI();
    if (!ui) return false;
    GpStr g(name);
    return UIShell_IsInterfaceVisible(ui, &g.s);
}

inline void ShowInterface(const char* name) {
    if (void* ui = UI()) {
        GpStr g(name);
        UIShell_ShowInterface(ui, &g.s);
    }
}

inline void HideInterface(const char* name) {
    if (void* ui = UI()) {
        GpStr g(name);
        UIShell_HideInterface(ui, &g.s);
    }
}

inline void* FindWindow(const char* name, const char* iface) {
    void* ui = UI();
    return ui ? UIShell_FindUIWindow(ui, name, iface) : nullptr;
}

inline bool Press(const char* button, const char* iface) {
    void* ui = UI();
    return ui && UIShell_SendUIMessage(ui, MSG_ONBUTTONPRESS, button, iface);
}

inline bool Send(int msg, const char* window, const char* iface) {
    void* ui = UI();
    return ui && UIShell_SendUIMessage(ui, msg, window, iface);
}

inline bool WindowVisible(const char* name, const char* iface) {
    void* w = FindWindow(name, iface);
    return w && UIWindow_GetVisible(w);
}

inline bool SetEditText(const char* box, const char* iface, const char* text) {
    void* w = FindWindow(box, iface);
    if (!w) return false;
    GpStr g(text);
    UIEditBox_SetText(w, &g.s);
    return true;
}

inline bool Check(const char* box, const char* iface) {
    void* w = FindWindow(box, iface);
    if (!w) return false;
    UICheckbox_SetCheckState(w, true);
    void* ui = UI();
    return UIShell_SendUIMessage(ui, MSG_ONCHECK, box, iface);
}

inline void* LocalPlayer() {
    void* srv = Server_Singleton();
    return srv ? Server_GetLocalHumanPlayer(srv) : nullptr;
}

// true when the local (host) player has a hero chosen/created, i.e. the game will accept "Ready".
inline bool LocalHeroChosen() {
    void* pl = LocalPlayer();
    return pl && GpLen(pl, 0x58) > 0 && GpLen(pl, 0x64) > 0;
}

inline const char* CurrentWorldState() {
    void* ws = WorldState_Singleton();
    if (!ws) return "(no WorldState)";
    const char* s = WorldState_ToString(WorldState_GetCurrentState(ws));
    return s ? s : "(null)";
}

}  // namespace eng
