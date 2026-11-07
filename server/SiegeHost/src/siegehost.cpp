// SiegeHost — runtime extension DLL for Dungeon Siege: Legends of Aranna (DSLOA.exe v1.50 community build)
// Loaded automatically as a version.dll proxy placed next to DSLOA.exe.
// See 04-server/SiegeHost/README.md and 03-analysis/RE-01-player-cap-and-ids.md

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include "siegehost.h"
#include "characterabilities.h"

// ----------------------------------------------------------------------------------------------
// version.dll forwarding (real exports resolved from the system directory)
// ----------------------------------------------------------------------------------------------
extern "C" {
void* g_real[17] = {0};
}
static const char* kVersionExports[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW"};

#define STUB(i) __asm__(".globl _vstub_" #i "\n_vstub_" #i ": jmp *(_g_real+" #i "*4)\n");
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8)
STUB(9) STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16)

static HMODULE g_realVersion = nullptr;
static HMODULE g_self = nullptr;

static bool LoadRealVersion() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit process
    if (!n || n > MAX_PATH - 20) return false;
    wcscat(path, L"\\version.dll");
    g_realVersion = LoadLibraryW(path);
    if (!g_realVersion) return false;
    for (int i = 0; i < 17; ++i) g_real[i] = (void*)GetProcAddress(g_realVersion, kVersionExports[i]);
    return true;
}

// ----------------------------------------------------------------------------------------------
// Logging
// ----------------------------------------------------------------------------------------------
static CRITICAL_SECTION g_logLock;
static FILE* g_log = nullptr;
static std::string g_dir;  // directory of DSLOA.exe, with trailing backslash
std::string LogDir() { return g_dir + "SiegeHostLogs\\"; }

void ChatDiagInstall();
void ChatFixInstall();
void PartyPanelInstall();
void PortraitFixInstall();
void PortraitDiagInstall();
void PanelHostFilterInstall();
void PanelFilterTick();
void ChatHistoryTick();
void LabMoveLocTick();
void AreaScaleInstall();
void WorldLevelInstall();
void AreaScaleTick();
void AreaScaleClientDiagInstall();
void PartyPanelDiagTick();
void PartyPanelTick();
void ChatFixTick();
void ChatDiagTick();
void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [pid %lu] ", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ----------------------------------------------------------------------------------------------
// Configuration (SiegeHost.ini next to the exe)
// ----------------------------------------------------------------------------------------------
static Config g_cfg;
const Config& Cfg() { return g_cfg; }

static std::string IniString(const char* sec, const char* key, const char* def, const std::string& ini) {
    char buf[512];
    GetPrivateProfileStringA(sec, key, def, buf, sizeof(buf), ini.c_str());
    return buf;
}

static void LoadConfig() {
    std::string ini = g_dir + "SiegeHost.ini";
    char envIni[MAX_PATH];
    if (GetEnvironmentVariableA("SIEGEHOST_INI", envIni, MAX_PATH) > 0) ini = envIni;  // lets server + client share one game folder
    Log("Config file: %s", ini.c_str());
    const char* s = "SiegeHost";
    g_cfg.enabled = GetPrivateProfileIntA(s, "Enabled", 1, ini.c_str());
    g_cfg.snapshots = GetPrivateProfileIntA(s, "Snapshots", 0, ini.c_str());
    char envSnap[16];
    if (GetEnvironmentVariableA("SIEGEHOST_SNAPSHOTS", envSnap, sizeof(envSnap)) > 0) {
        g_cfg.snapshots = atoi(envSnap);
    }
    std::string role = IniString(s, "Role", "none", ini);
    g_cfg.role = _stricmp(role.c_str(), "server") == 0 ? Role::Server : _stricmp(role.c_str(), "client") == 0 ? Role::Client : Role::None;
    int defMax = g_cfg.role == Role::None ? 0 : 30;  // server/client default to the maximum
    g_cfg.maxPlayers = GetPrivateProfileIntA(s, "MaxPlayers", defMax, ini.c_str());
    if (g_cfg.maxPlayers < 0) g_cfg.maxPlayers = 0;
    if (g_cfg.maxPlayers > 30) g_cfg.maxPlayers = 30;
    int autoSlots = g_cfg.maxPlayers > 0 ? g_cfg.maxPlayers + 1 : 9;
    g_cfg.maxSlots = GetPrivateProfileIntA(s, "MaxSlots", autoSlots, ini.c_str());
    g_cfg.dpMaxPlayers = GetPrivateProfileIntA(s, "DPMaxPlayers", g_cfg.maxPlayers > 0 ? g_cfg.maxPlayers + 1 : 0, ini.c_str());
    g_cfg.patchTeams = GetPrivateProfileIntA(s, "PatchTeams", 1, ini.c_str());
    g_cfg.logDirectPlay = GetPrivateProfileIntA(s, "LogDirectPlay", 1, ini.c_str());
    g_cfg.noDpnsvr = GetPrivateProfileIntA(s, "NoDpnsvr", 0, ini.c_str());
    g_cfg.hostPort = GetPrivateProfileIntA(s, "HostPort", 6073, ini.c_str());
    g_cfg.gamePort = GetPrivateProfileIntA(s, "GamePort", 0, ini.c_str());
    g_cfg.joinPort = GetPrivateProfileIntA("AutoJoin", "Port", 0, ini.c_str());
    g_cfg.sendFrequency = GetPrivateProfileIntA("Net", "SendFrequency", 0, ini.c_str());
    g_cfg.chatLineSeconds = GetPrivateProfileIntA("Chat", "LineSeconds", 10, ini.c_str());
    g_cfg.showOtherPlayers = GetPrivateProfileIntA("UI", "ShowOtherPlayers", 1, ini.c_str());
    g_cfg.showServerHost = GetPrivateProfileIntA("UI", "ShowServerHost", 0, ini.c_str());
    g_cfg.chatHistoryOnOpen = GetPrivateProfileIntA("Chat", "HistoryOnOpen", 1, ini.c_str());
    g_cfg.nearbyOnly = GetPrivateProfileIntA("UI", "NearbyOnly", 1, ini.c_str());
    g_cfg.portraitFix = GetPrivateProfileIntA("UI", "PortraitFix", 1, ini.c_str());
    g_cfg.abilityBarOption1 = GetPrivateProfileIntA("UI", "AbilityBarOption1", 1, ini.c_str());
    g_cfg.abilityBarOption2 = GetPrivateProfileIntA("UI", "AbilityBarOption2", 0, ini.c_str());
    g_cfg.mpSleep = GetPrivateProfileIntA("Net", "MPSleep", -1, ini.c_str());
    if (g_cfg.maxSlots < 9) g_cfg.maxSlots = 9;
    if (g_cfg.maxSlots > 31) g_cfg.maxSlots = 31;  // PlayerId = 1<<index in a DWORD; keep bit 31 unused
    const char* a = "AutoHost";
    g_cfg.gameName = IniString(a, "GameName", "Dungeon Siege MMO", ini);
    g_cfg.hostCharacter = IniString(a, "HostCharacter", "ServerHost", ini);
    g_cfg.stuckTimeoutSec = GetPrivateProfileIntA(a, "StuckTimeoutSec", 300, ini.c_str());
    g_cfg.map = IniString(a, "Map", "", ini);
    g_cfg.mpWorld = IniString(a, "World", "", ini);
    g_cfg.startLocation = IniString(a, "StartLocation", "", ini);
    g_cfg.allowPausing = GetPrivateProfileIntA(a, "AllowPausing", 0, ini.c_str());
    g_cfg.allowStartSelect = GetPrivateProfileIntA(a, "AllowStartLocationSelection", 0, ini.c_str());
    g_cfg.areaScaling = GetPrivateProfileIntA(a, "AreaScaling", 1, ini.c_str());
    g_cfg.countServerHost = GetPrivateProfileIntA(a, "CountServerHost", 0, ini.c_str());
    const char* st = "Store";
    g_cfg.shopBackpacks = GetPrivateProfileIntA(st, "Backpacks", 4, ini.c_str());
    g_cfg.shopBackpackPrice = GetPrivateProfileIntA(st, "BackpackPrice", 1500, ini.c_str());
    g_cfg.shopBackpackTemplate = IniString(st, "BackpackTemplate", "dsx_backpack_empty", ini);
    g_cfg.invFix = GetPrivateProfileIntA(st, "BackpackPickup", 1, ini.c_str());
    g_cfg.jipQueue = GetPrivateProfileIntA(a, "ReadyFirst", 1, ini.c_str());
    g_cfg.pvpLocations = IniString(a, "PvpLocations", "402,409,901", ini);
    g_cfg.dropInvNothing = GetPrivateProfileIntA(a, "DropInvNothing", 1, ini.c_str());
    const char* j = "AutoJoin";
    g_cfg.joinAddress = IniString(j, "Address", "", ini);
    g_cfg.playerName = IniString(j, "PlayerName", "Player", ini);
    g_cfg.joinRetries = GetPrivateProfileIntA(j, "Retries", 20, ini.c_str());
    g_cfg.exitAfterGame = GetPrivateProfileIntA(j, "ExitAfterGame", 1, ini.c_str());
    g_cfg.veteranRequiredLevel = GetPrivateProfileIntA("World", "VeteranRequiredLevel", 0, ini.c_str());
    g_cfg.iniPath = ini;
    g_cfg.jgEnabled = GetPrivateProfileIntA("JoinGuard", "Enabled", 1, ini.c_str());
    g_cfg.jgKey = IniString("JoinGuard", "Key", "", ini);
    g_cfg.jgServerId = IniString("JoinGuard", "ServerId", "", ini);
    g_cfg.portalHubUrl   = IniString("Portal", "HubUrl", "", ini);
    g_cfg.portalShardId  = IniString("Portal", "ShardId", "", ini);
    g_cfg.portalShardKey = IniString("Portal", "ShardKey", "", ini);
}

// ----------------------------------------------------------------------------------------------
// Target identification
// ----------------------------------------------------------------------------------------------
static const DWORD kExpectedTimestamp = 0x3F8FFE7F;  // DSLOA.exe 2003-10-17 14:36:47 UTC
static const DWORD kExpectedImageSize = 0x006AE000;

static bool IdentifyTarget(HMODULE exe) {
    auto dos = (IMAGE_DOS_HEADER*)exe;
    auto nt = (IMAGE_NT_HEADERS32*)((BYTE*)exe + dos->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp, sz = nt->OptionalHeader.SizeOfImage;
    Log("Target exe: timestamp=0x%08lX sizeOfImage=0x%08lX base=%p", ts, sz, exe);
    return ts == kExpectedTimestamp && sz == kExpectedImageSize;
}

// ----------------------------------------------------------------------------------------------
// Byte patches (all verified before any is written; all-or-nothing)
// ----------------------------------------------------------------------------------------------
struct Patch {
    const char* name;
    DWORD va;
    std::vector<BYTE> expect;
    std::vector<BYTE> replace;
};

static bool WriteCode(DWORD va, const BYTE* data, size_t n) {
    DWORD old;
    if (!VirtualProtect((LPVOID)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)va, data, n);
    VirtualProtect((LPVOID)va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)va, n);
    return true;
}

static bool ApplyPatches(std::vector<Patch>& patches) {
    for (auto& p : patches) {
        if (memcmp((void*)p.va, p.expect.data(), p.expect.size()) != 0) {
            std::string got;
            char b[4];
            for (size_t i = 0; i < p.expect.size(); ++i) { sprintf(b, "%02X", ((BYTE*)p.va)[i]); got += b; }
            Log("PATCH VERIFY FAILED: %s at 0x%08lX (found %s). No patches applied.", p.name, p.va, got.c_str());
            return false;
        }
    }
    for (auto& p : patches) {
        if (!WriteCode(p.va, p.replace.data(), p.replace.size())) {
            Log("PATCH WRITE FAILED: %s at 0x%08lX (error %lu)", p.name, p.va, GetLastError());
            return false;
        }
        Log("Patched %-40s at 0x%08lX", p.name, p.va);
    }
    return true;
}

static void BuildSlotPatches(std::vector<Patch>& out) {
    int n = g_cfg.maxSlots;       // number of slots (indices 0..n-1)
    BYTE last = (BYTE)(n - 1);    // last valid index
    BYTE last4 = (BYTE)((n - 1) * 4);
    // RE-01 §3: Server slot-count sites (vanilla 9 slots)
    out.push_back({"Server::Server  vector resize(9)", 0x0056DFC5, {0x6A, 0x09}, {0x6A, (BYTE)n}});
    out.push_back({"Server::Server  create loop (<=8)", 0x0056E004, {0x83, 0x7D, 0x08, 0x08}, {0x83, 0x7D, 0x08, last}});
    out.push_back({"Server::~Server destroy loop (<=0x20)", 0x0056E1C6, {0x83, 0xFB, 0x20}, {0x83, 0xFB, last4}});
    out.push_back({"Server::FindFreeSlot loop (<=0x20)", 0x0056E239, {0x83, 0xFF, 0x20}, {0x83, 0xFF, last4}});
    out.push_back({"Server::Reset recreate loop (<=8)", 0x0056E9DB, {0x83, 0xFE, 0x08}, {0x83, 0xFE, last}});
    if (g_cfg.patchTeams) {
        out.push_back({"Server::RSJoinTeam friend loop A", 0x0056F9BB, {0x83, 0x7D, 0xE4, 0x08}, {0x83, 0x7D, 0xE4, last}});
        out.push_back({"Server::RSJoinTeam friend loop B", 0x0056FA44, {0x83, 0x7D, 0xF8, 0x08}, {0x83, 0x7D, 0xF8, last}});
    }
}

static void BuildMaxPlayersPatch(std::vector<Patch>& out) {
    // RE-01 §5: NetPipe session setter (0x56FED5) stores max players at [session+0xD8].
    // Called by Server::RCSetMaxPlayers, NetPipe host and the client when it receives the app desc.
    // mov eax,[esp+8]; mov ecx,esi  ->  push N; pop eax; mov ecx,esi; nop
    BYTE n = (BYTE)g_cfg.maxPlayers;
    out.push_back({"NetPipe::SetMaxPlayers force value", 0x0056FEDE, {0x8B, 0x44, 0x24, 0x08, 0x8B, 0xCE},
                   {0x6A, n, 0x58, 0x8B, 0xCE, 0x90}});
}

// ----------------------------------------------------------------------------------------------
// Staging screen: disable the "Import Character" button so clicking it does nothing (not hidden)
// ----------------------------------------------------------------------------------------------
static void StagingImportDisableInstall() {
    // In DSLOA.exe v1.50, UIMultiplayer::ReceiveNotify (0x004803EF) routes both
    // staging_import_character and staging_import_dsx_character to 0x00485351.
    // Original code at 0x00485351: 68 8C C6 77 00 (push 0x77C68C)
    // Function epilogue at 0x00485583: 5F 5E 5B C9 C2 08 00 (pop edi; pop esi; pop ebx; leave; ret 8)
    // Patching with a relative jmp (0xE9 0x0000022D) safely exits the notify handler,
    // so the button plays its click animation/sound normally but performs no import.
    const DWORD target = 0x00485351;
    const DWORD dest   = 0x00485583;
    const BYTE expect[5] = { 0x68, 0x8C, 0xC6, 0x77, 0x00 };
    BYTE patch[5] = { 0xE9, 0, 0, 0, 0 };
    DWORD rel = dest - (target + 5);
    memcpy(patch + 1, &rel, 4);

    if (memcmp((void*)target, expect, 5) == 0) {
        WriteCode(target, patch, 5);
        Log("StagingImport: disabled import button at 0x%08lX (jmp 0x%08lX). Button remains visible and clickable, but does nothing.", target, dest);
    } else {
        Log("StagingImport: unexpected bytes at 0x%08lX, patch skipped.", target);
    }
}

// ----------------------------------------------------------------------------------------------
// DirectPlay 8 instrumentation (IAT hook on ole32!CoCreateInstance -> vtable hooks)
// ----------------------------------------------------------------------------------------------
static const GUID CLSID_DP8Server = {0xDA825E1B, 0x6830, 0x43D7, {0x83, 0x5D, 0x0B, 0x5A, 0xD8, 0x29, 0x56, 0xA2}};
static const GUID CLSID_DP8Client = {0x743F1DC6, 0x5ABA, 0x429F, {0x8B, 0xDF, 0xC5, 0x4D, 0x03, 0x25, 0x3D, 0xC2}};

typedef HRESULT(WINAPI* PFNDPNMESSAGEHANDLER)(PVOID, DWORD, PVOID);
struct DpnAppDesc {  // DPN_APPLICATION_DESC (x86)
    DWORD dwSize, dwFlags;
    GUID guidInstance, guidApplication;
    DWORD dwMaxPlayers, dwCurrentPlayers;
    WCHAR* pwszSessionName;
    WCHAR* pwszPassword;
    PVOID pvReservedData;
    DWORD dwReservedDataSize;
    PVOID pvApplicationReservedData;
    DWORD dwApplicationReservedDataSize;
};

struct HandlerCtx {
    const char* role;
    PVOID userCtx;
    PFNDPNMESSAGEHANDLER original;
};

static LONG g_remotePlayers = 0;
long RemotePlayerCount() { return g_remotePlayers > 0 ? g_remotePlayers - 1 : 0; }  // minus the host itself

// NetPipe::Connect refuses a session unless session+0xC4 ("compatible") is set. 0x71FECF computes it by comparing
// the host's appReserved blob (X = blob+4) with this client's own blob, built by 0x71FE7E (thiscall, fills 0x4B+ bytes):
//   X+5 dword (protocol tag "m110"), X+0x2E bits 0/1 (NetPipe+0x182 compress, +0x183 turtle), X+9 byte,
//   X+0x1A dword ([0x7A99A0] checksum), X+0x1E 16 bytes (0x451C96 hash), X+0xA 16 bytes (NetPipe+2 version).
void CompareSessionInfo(const BYTE* remote) {
    BYTE local[0x100] = {0};
    auto build = (void* (__thiscall*)(BYTE*))0x0071FE7E;
    build(local);
    const BYTE* R = remote + 4;
    const BYTE* L = local + 4;
    char hex[200] = "";
    for (int i = 0; i < 0x4B; ++i) sprintf(hex + i * 2, "%02X", local[i]);
    Log("JoinCheck: local appRes=%s", hex);
    Log("JoinCheck: tag(+5) %s | flags(+2E) local=0x%02X host=0x%02X %s | byte(+9) %s | checksum(+1A) local=%08lX host=%08lX %s | hash(+1E) %s | version(+A) %s",
        memcmp(L + 5, R + 5, 4) ? "DIFF" : "ok", L[0x2E], R[0x2E], ((L[0x2E] ^ R[0x2E]) & 3) ? "DIFF" : "ok", L[9] != R[9] ? "DIFF" : "ok",
        *(DWORD*)(L + 0x1A), *(DWORD*)(R + 0x1A), memcmp(L + 0x1A, R + 0x1A, 4) ? "DIFF" : "ok", memcmp(L + 0x1E, R + 0x1E, 16) ? "DIFF" : "ok",
        memcmp(L + 0xA, R + 0xA, 16) ? "DIFF" : "ok");
}

static const char* DpnMsgName(DWORD id) {
    switch (id) {
        case 0xFFFF0003: return "ASYNC_OP_COMPLETE";
        case 0xFFFF0004: return "CLIENT_INFO";
        case 0xFFFF0005: return "CONNECT_COMPLETE";
        case 0xFFFF0007: return "CREATE_PLAYER";
        case 0xFFFF0009: return "DESTROY_PLAYER";
        case 0xFFFF000A: return "ENUM_HOSTS_QUERY";
        case 0xFFFF000B: return "ENUM_HOSTS_RESPONSE";
        case 0xFFFF000E: return "INDICATE_CONNECT";
        case 0xFFFF000F: return "INDICATED_CONNECT_ABORTED";
        case 0xFFFF0011: return "RECEIVE";
        case 0xFFFF0014: return "SEND_COMPLETE";
        case 0xFFFF0016: return "TERMINATE_SESSION";
        default: return nullptr;
    }
}

static HRESULT WINAPI HookedHandler(PVOID pvCtx, DWORD id, PVOID msg) {
    auto* ctx = (HandlerCtx*)pvCtx;
    if (id == 0xFFFF0011 && msg) {
        DWORD* m = (DWORD*)msg;
        if (CharacterAbilities_OnReceivePacket(ctx->role[0] == 's', m[1], (const BYTE*)m[3], m[4])) {
            return S_OK;
        }
        if (SharedStash_OnReceivePacket(ctx->role[0] == 's', m[1], (const BYTE*)m[3], m[4])) {
            return S_OK;
        }
    }
    HRESULT hr = ctx->role[0] == 's' ? JG_ServerMessage(id, msg, ctx->original, ctx->userCtx)
                                     : ctx->original(ctx->userCtx, id, msg);
    if (id == 0xFFFF0005 && ctx->role[0] == 'c') JG_ClientConnectComplete(msg);
    if (g_cfg.logDirectPlay && (id == 0xFFFF000A || id == 0xFFFF000B)) {  // Diagnostics: enum query/response details
        static LONG nQ = 0, nR = 0;
        LONG n = InterlockedIncrement(id == 0xFFFF000A ? &nQ : &nR);
        if (n <= 3 || n % 20 == 0) {
            DWORD* m = (DWORD*)msg;
            char sender[600] = "?", device[600] = "?";
            // IDirectPlay8Address vtable slot 9 (0x24) is GetURLW (wide, size in WCHARs), slot 10 is GetURLA.
            // 0.4.4 called slot 9 with a char buffer and a byte count -> stack overflow in the DirectPlay thread.
            auto url = [](void* addr, char* out, DWORD cap) {
                strcpy(out, "(null)");
                if (!addr) return;
                WCHAR w[512] = L"";
                DWORD sz = 511;  // characters
                auto fn = (HRESULT(WINAPI*)(void*, WCHAR*, DWORD*))(*(void***)addr)[9];
                HRESULT r = fn(addr, w, &sz);
                if (FAILED(r)) { sprintf(out, "(GetURLW hr=0x%08lX need=%lu)", (DWORD)r, sz); return; }
                w[511] = 0;
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap - 1, nullptr, nullptr);
                out[cap - 1] = 0;
            };
            url((void*)m[1], sender, sizeof sender);
            url((void*)m[2], device, sizeof device);
            if (id == 0xFFFF000A) {
                Log("DP8[%s] ENUM_HOSTS_QUERY #%ld from %s via %s -> hr=0x%08lX", ctx->role, n, sender, device, hr);
            } else {
                auto* d = (DpnAppDesc*)m[3];
                char name[128] = "";
                if (d && d->pwszSessionName) WideCharToMultiByte(CP_UTF8, 0, d->pwszSessionName, -1, name, sizeof(name) - 1, nullptr, nullptr);
                char hex[160] = "";
                if (d && d->pvApplicationReservedData)
                    for (DWORD i = 0; i < d->dwApplicationReservedDataSize && i < 75; ++i)
                        sprintf(hex + i * 2, "%02X", ((BYTE*)d->pvApplicationReservedData)[i]);
                Log("DP8[%s] ENUM_HOSTS_RESPONSE #%ld from %s via %s: session='%s' flags=0x%lX max=%lu cur=%lu pwd=%d latency=%lums appRes=%s -> hr=0x%08lX",
                    ctx->role, n, sender, device, name, d ? d->dwFlags : 0, d ? d->dwMaxPlayers : 0, d ? d->dwCurrentPlayers : 0,
                    d && d->pwszPassword ? 1 : 0, m[7], hex, hr);
                if (d && d->pvApplicationReservedData && d->dwApplicationReservedDataSize >= 0x4B) CompareSessionInfo((BYTE*)d->pvApplicationReservedData);
            }
        }
    }
    if (g_cfg.logDirectPlay && id != 0xFFFF0011 && id != 0xFFFF0014 && id != 0xFFFF000A && id != 0xFFFF000B) {
        const char* nm = DpnMsgName(id);
        if (id == 0xFFFF0007 || id == 0xFFFF0009) {
            DWORD dpnid = ((DWORD*)msg)[1];
            if (id == 0xFFFF0007) InterlockedIncrement(&g_remotePlayers); else InterlockedDecrement(&g_remotePlayers);
            Log("DP8[%s] %s dpnid=0x%08lX -> hr=0x%08lX (players incl. host: %ld)", ctx->role, nm, dpnid, hr, g_remotePlayers);
        } else if (id == 0xFFFF0005) {
            HRESULT result = (HRESULT)((DWORD*)msg)[3];  // DPNMSG_CONNECT_COMPLETE.hResultCode
            Log("DP8[%s] CONNECT_COMPLETE result=0x%08lX", ctx->role, (DWORD)result);
        } else if (nm) {
            Log("DP8[%s] %s -> hr=0x%08lX", ctx->role, nm, hr);
        } else {
            Log("DP8[%s] msg 0x%08lX -> hr=0x%08lX", ctx->role, id, hr);
        }
    }
    return hr;
}

typedef HRESULT(WINAPI* InitializeFn)(void* self, PVOID ctx, PFNDPNMESSAGEHANDLER pfn, DWORD flags);
typedef HRESULT(WINAPI* SetAppDescFn)(void* self, const DpnAppDesc* desc, DWORD flags);
typedef HRESULT(WINAPI* HostFn)(void* self, const DpnAppDesc* desc, void** dev, DWORD cDev, void* sec, void* cred, PVOID playerCtx, DWORD flags);

static InitializeFn g_origServerInit = nullptr, g_origClientInit = nullptr;
static SetAppDescFn g_origSetAppDesc = nullptr;
static HostFn g_origHost = nullptr;

static HRESULT InitCommon(InitializeFn orig, const char* role, void* self, PVOID ctx, PFNDPNMESSAGEHANDLER pfn, DWORD flags) {
    auto* hc = new HandlerCtx{role, ctx, pfn};  // intentionally leaked: lives as long as the DP8 object
    Log("DP8[%s] Initialize (flags=0x%08lX) — message handler wrapped", role, flags);
    return orig(self, hc, HookedHandler, flags);
}
static HRESULT WINAPI HookServerInit(void* self, PVOID ctx, PFNDPNMESSAGEHANDLER pfn, DWORD flags) {
    CharacterAbilities_SetDirectPlay(nullptr, self);
    SharedStash_SetDirectPlay(nullptr, self);
    return InitCommon(g_origServerInit, "server", self, ctx, pfn, flags);
}
static HRESULT WINAPI HookClientInit(void* self, PVOID ctx, PFNDPNMESSAGEHANDLER pfn, DWORD flags) {
    CharacterAbilities_SetDirectPlay(self, nullptr);
    SharedStash_SetDirectPlay(self, nullptr);
    return InitCommon(g_origClientInit, "client", self, ctx, pfn, flags);
}

static void LogDesc(const char* what, const DpnAppDesc* d) {
    char name[128] = "";
    if (d->pwszSessionName) WideCharToMultiByte(CP_UTF8, 0, d->pwszSessionName, -1, name, sizeof(name) - 1, nullptr, nullptr);
    char hex[160] = "";
    if (d->pvApplicationReservedData)
        for (DWORD i = 0; i < d->dwApplicationReservedDataSize && i < 75; ++i) sprintf(hex + i * 2, "%02X", ((BYTE*)d->pvApplicationReservedData)[i]);
    Log("DP8[server] %s: flags=0x%08lX maxPlayers=%lu session='%s' appReserved=%lu bytes %s (compat flags +0x32=0x%02X)", what, d->dwFlags,
        d->dwMaxPlayers, name, d->dwApplicationReservedDataSize, hex,
        d->pvApplicationReservedData && d->dwApplicationReservedDataSize > 0x32 ? ((BYTE*)d->pvApplicationReservedData)[0x32] : 0);
}

static void AddrUrl(void* addr, char* out, size_t cap);
static HRESULT WINAPI HookHost(void* self, const DpnAppDesc* desc, void** dev, DWORD cDev, void* sec, void* cred, PVOID pc, DWORD flags) {
    LogDesc("Host (game requested)", desc);
    for (DWORD i = 0; dev && i < cDev; ++i) {
        char u[600];
        AddrUrl(dev[i], u, sizeof u);
        Log("DP8[server] Host device[%lu] = %s (hostFlags=0x%lX)", i, u, flags);
    }
    if (!g_cfg.noDpnsvr && g_cfg.gamePort > 0 && dev) {
        // pin the host port. dpnsvr (6073) forwards a client's search to the host, but the host answers from its
        // own port; home NAT routers drop that answer because the client only sent to :6073. Clients search this fixed
        // port directly ([AutoJoin] Port) so query and answer use the same address pair.
        for (DWORD i = 0; i < cDev; ++i) {
            if (!dev[i]) continue;
            DWORD port = (DWORD)g_cfg.gamePort;
            auto add = (HRESULT(WINAPI*)(void*, const WCHAR*, const void*, DWORD, DWORD))(*(void***)dev[i])[18];  // AddComponent
            HRESULT r = add(dev[i], L"port", &port, sizeof port, 2 /*DPNA_DATATYPE_DWORD*/);
            Log("DP8[server] GamePort: device %lu port=%lu -> hr=0x%08lX", i, port, (DWORD)r);
        }
    }
    if (g_cfg.noDpnsvr && dev) {
        // DPNSESSION_NODPNSVR: no dpnsvr.exe (Wine cannot run it: Host fails with DPNERR_DPNSVRNOTAVAILABLE). Clients
        // enumerate port 6073 by default, so the host must listen on that port itself.
        for (DWORD i = 0; i < cDev; ++i) {
            if (!dev[i]) continue;
            DWORD port = (DWORD)g_cfg.hostPort;
            auto add = (HRESULT(WINAPI*)(void*, const WCHAR*, const void*, DWORD, DWORD))(*(void***)dev[i])[18];  // AddComponent
            HRESULT r = add(dev[i], L"port", &port, sizeof port, 2 /*DPNA_DATATYPE_DWORD*/);
            Log("DP8[server] NoDpnsvr: device %lu port=%lu -> hr=0x%08lX", i, port, (DWORD)r);
        }
    }
    if ((g_cfg.dpMaxPlayers > 0 || g_cfg.noDpnsvr) && desc) {
        DpnAppDesc copy = *desc;
        if (g_cfg.dpMaxPlayers > 0) copy.dwMaxPlayers = (DWORD)g_cfg.dpMaxPlayers;
        if (g_cfg.noDpnsvr) copy.dwFlags |= 0x40;  // DPNSESSION_NODPNSVR
        LogDesc("Host (SiegeHost override)", &copy);
        HRESULT hr = g_origHost(self, &copy, dev, cDev, sec, cred, pc, flags);
        Log("DP8[server] Host -> hr=0x%08lX", hr);
        return hr;
    }
    HRESULT hr = g_origHost(self, desc, dev, cDev, sec, cred, pc, flags);
    Log("DP8[server] Host -> hr=0x%08lX", hr);
    return hr;
}

static HRESULT WINAPI HookSetAppDesc(void* self, const DpnAppDesc* desc, DWORD flags) {
    if (g_cfg.dpMaxPlayers > 0 && desc) {
        DpnAppDesc copy = *desc;
        copy.dwMaxPlayers = (DWORD)g_cfg.dpMaxPlayers;
        return g_origSetAppDesc(self, &copy, flags);
    }
    return g_origSetAppDesc(self, desc, flags);
}

// ---- Join diagnostics ------------------------------------------------------------------------------------
// IDirectPlay8Client::Connect (vtable slot 7).
typedef HRESULT(WINAPI* ConnectFn)(void* self, const DpnAppDesc* desc, void* host, void* dev, void* sec, void* cred,
                                   const void* data, DWORD dataSize, void* asyncCtx, DWORD* hAsync, DWORD flags);
static ConnectFn g_origConnect = nullptr;
static void AddrUrl(void* addr, char* out, size_t cap) {
    strcpy(out, "(null)");
    if (!addr) return;
    WCHAR w[512] = L"";
    DWORD sz = 511;
    auto fn = (HRESULT(WINAPI*)(void*, WCHAR*, DWORD*))(*(void***)addr)[9];  // GetURLW
    if (FAILED(fn(addr, w, &sz))) { strcpy(out, "(GetURLW failed)"); return; }
    w[511] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap - 1, nullptr, nullptr);
    out[cap - 1] = 0;
}
// IDirectPlay8Client::EnumHosts (vtable slot 5).
typedef HRESULT(WINAPI* EnumHostsFn)(void* self, DpnAppDesc* desc, void* host, void* dev, void* data, DWORD dataSize, DWORD count,
                                     DWORD retry, DWORD timeout, void* ctx, DWORD* hAsync, DWORD flags);
static EnumHostsFn g_origEnumHosts = nullptr;
static HRESULT WINAPI HookEnumHosts(void* self, DpnAppDesc* desc, void* host, void* dev, void* data, DWORD dataSize, DWORD count,
                                    DWORD retry, DWORD timeout, void* ctx, DWORD* hAsync, DWORD flags) {
    char before[600];
    AddrUrl(host, before, sizeof before);
    if (host && g_cfg.joinPort > 0 && strstr(before, "hostname=") && !strstr(before, "hostname=127.") && !strstr(before, ";port=")) {
        DWORD port = (DWORD)g_cfg.joinPort;
        auto add = (HRESULT(WINAPI*)(void*, const WCHAR*, const void*, DWORD, DWORD))(*(void***)host)[18];  // AddComponent
        HRESULT r = add(host, L"port", &port, sizeof port, 2);
        char after[600];
        AddrUrl(host, after, sizeof after);
        Log("DP8[client] EnumHosts: search port %lu added (hr=0x%08lX): %s", port, (DWORD)r, after);
    } else {
        Log("DP8[client] EnumHosts %s", before);
    }
    HRESULT hr = g_origEnumHosts(self, desc, host, dev, data, dataSize, count, retry, timeout, ctx, hAsync, flags);
    Log("DP8[client] EnumHosts -> hr=0x%08lX", (DWORD)hr);
    return hr;
}

static HRESULT WINAPI HookConnect(void* self, const DpnAppDesc* desc, void* host, void* dev, void* sec, void* cred,
                                  const void* data, DWORD dataSize, void* asyncCtx, DWORD* hAsync, DWORD flags) {
    char h[600], d[600];
    AddrUrl(host, h, sizeof h);
    AddrUrl(dev, d, sizeof d);
    CharacterAbilities_NotifyConnect(h);
    Log("DP8[client] Connect(host=%s, device=%s, userData=%lu bytes, flags=0x%lX, max=%lu) ...", h, d, dataSize, flags,
        desc ? desc->dwMaxPlayers : 0);
    const void* sendData = data;
    DWORD sendSize = dataSize;
    if (g_cfg.role == Role::Client) JG_ClientConnectData(data, dataSize, &sendData, &sendSize);
    HRESULT hr = g_origConnect(self, desc, host, dev, sec, cred, sendData, sendSize, asyncCtx, hAsync, flags);
    Log("DP8[client] Connect -> hr=0x%08lX", (DWORD)hr);
    return hr;
}

// NetPipe::Connect(session, password) at 0x71CB00 (thiscall, ret 8): returns 0 on success, else a join-failure reason
// that UIMultiplayer (0x488F93) turns into "Could not join the game." etc. Wrapped at its two UI call sites.
static int __fastcall WrapNetPipeConnect(void* np, void*, void* session, void* password) {
    auto orig = (int(__thiscall*)(void*, void*, void*))0x0071CB00;
    int r = orig(np, session, password);
    Log("NetPipe::Connect(session=%p) -> %d%s", session, r, r == 0 ? " (started)" : " (join failed; reason code)");
    return r;
}
static void PatchCall(DWORD site, void* target) {
    BYTE code[5] = {0xE8};
    if (*(BYTE*)site != 0xE8) { Log("PatchCall: unexpected byte at %08lX", site); return; }
    LONG rel = (LONG)((DWORD)target - (site + 5));
    memcpy(code + 1, &rel, 4);
    WriteCode(site, code, 5);
}

static void* SwapVtbl(void* obj, int index, void* hook) {
    void** vtbl = *(void***)obj;
    void* orig = vtbl[index];
    if (orig == hook) return nullptr;  // already hooked (vtable shared across instances)
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_READWRITE, &old);
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return orig;
}

typedef HRESULT(WINAPI* CoCreateInstanceFn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
static CoCreateInstanceFn g_origCoCreate = nullptr;

static HRESULT WINAPI HookCoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* ppv) {
    HRESULT hr = g_origCoCreate(clsid, outer, ctx, iid, ppv);
    if (FAILED(hr) || !ppv || !*ppv) return hr;
    if (IsEqualGUID(clsid, CLSID_DP8Server)) {
        Log("CoCreateInstance(DirectPlay8Server) -> %p", *ppv);
        if (void* o = SwapVtbl(*ppv, 3, (void*)HookServerInit)) g_origServerInit = (InitializeFn)o;
        if (void* o = SwapVtbl(*ppv, 12, (void*)HookSetAppDesc)) g_origSetAppDesc = (SetAppDescFn)o;
        if (void* o = SwapVtbl(*ppv, 13, (void*)HookHost)) g_origHost = (HostFn)o;
    } else if (IsEqualGUID(clsid, CLSID_DP8Client)) {
        Log("CoCreateInstance(DirectPlay8Client) -> %p", *ppv);
        if (void* o = SwapVtbl(*ppv, 3, (void*)HookClientInit)) g_origClientInit = (InitializeFn)o;
        if (void* o = SwapVtbl(*ppv, 7, (void*)HookConnect)) g_origConnect = (ConnectFn)o;
        if (void* o = SwapVtbl(*ppv, 5, (void*)HookEnumHosts)) g_origEnumHosts = (EnumHostsFn)o;
    }
    return hr;
}

static bool HookIAT(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    auto dos = (IMAGE_DOS_HEADER*)mod;
    auto nt = (IMAGE_NT_HEADERS32*)((BYTE*)mod + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((char*)mod + imp->Name, dll) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA32*)((BYTE*)mod + imp->FirstThunk);
        auto orig_thunk = (IMAGE_THUNK_DATA32*)((BYTE*)mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig_thunk->u1.AddressOfData; ++thunk, ++orig_thunk) {
            if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + orig_thunk->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, 4, PAGE_READWRITE, &old);
            *orig = (void*)thunk->u1.Function;
            thunk->u1.Function = (DWORD)hook;
            VirtualProtect(&thunk->u1.Function, 4, old, &old);
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------------------------
// Main-thread pump (USER32!PeekMessageA is called every frame by the game loop)
// ----------------------------------------------------------------------------------------------
typedef BOOL(WINAPI* PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageFn g_origPeek = nullptr;
static DWORD g_mainThread = 0;

#ifndef GWLP_WNDPROC
#define GWLP_WNDPROC (-4)
#endif
#ifndef SetWindowLongPtrA
#define SetWindowLongPtrA SetWindowLongA
#endif

static WNDPROC g_origWndProc = nullptr;
HWND g_gameHwnd = nullptr;
static HKL g_lastSafeHkl = nullptr;
static volatile HKL g_pendingCjkHkl = nullptr;
static HHOOK g_getMsgHook = nullptr;
static HHOOK g_callWndHook = nullptr;

typedef HIMC (WINAPI *PFN_ImmAssociateContext)(HWND, HIMC);
typedef HIMC (WINAPI *PFN_ImmGetContext)(HWND);
typedef BOOL (WINAPI *PFN_ImmReleaseContext)(HWND, HIMC);
typedef BOOL (WINAPI *PFN_ImmIsIME)(HKL);

static PFN_ImmAssociateContext g_pfnImmAssociateContext = nullptr;
static PFN_ImmGetContext g_pfnImmGetContext = nullptr;
static PFN_ImmReleaseContext g_pfnImmReleaseContext = nullptr;
static PFN_ImmIsIME g_pfnImmIsIME = nullptr;

static void InitImmFunctions() {
    static bool s_init = false;
    if (s_init) return;
    s_init = true;
    HMODULE hImm = GetModuleHandleA("imm32.dll");
    if (!hImm) hImm = LoadLibraryA("imm32.dll");
    if (hImm) {
        g_pfnImmAssociateContext = (PFN_ImmAssociateContext)GetProcAddress(hImm, "ImmAssociateContext");
        g_pfnImmGetContext = (PFN_ImmGetContext)GetProcAddress(hImm, "ImmGetContext");
        g_pfnImmReleaseContext = (PFN_ImmReleaseContext)GetProcAddress(hImm, "ImmReleaseContext");
        g_pfnImmIsIME = (PFN_ImmIsIME)GetProcAddress(hImm, "ImmIsIME");
    }
}

static bool IsCjkOrImeLayout(HKL hkl) {
    if (!hkl) return false;
    WORD langId = LOWORD((DWORD_PTR)hkl);
    WORD prim = (WORD)(langId & 0x03FF);
    // LANG_CHINESE (0x04), LANG_JAPANESE (0x11), LANG_KOREAN (0x12)
    if (prim == 0x11 || prim == 0x04 || prim == 0x12) return true;
    // Legacy IME HKL high-word marker (0xE000..0xEFFF)
    WORD hi = HIWORD((DWORD_PTR)hkl);
    if ((hi & 0xF000) == 0xE000) return true;
    return false;
}

static HKL FindNextSafeKeyboardLayout(HKL blockedHkl) {
    HKL list[32];
    int n = GetKeyboardLayoutList(32, list);
    if (n > 0) {
        int startIdx = 0;
        for (int i = 0; i < n; ++i) {
            if (list[i] == blockedHkl || LOWORD((DWORD_PTR)list[i]) == LOWORD((DWORD_PTR)blockedHkl)) {
                startIdx = i;
                break;
            }
        }
        // Pass 1: find the next non-CJK layout that differs from g_lastSafeHkl so cycling rus -> en -> jp steps to rus
        if (g_lastSafeHkl) {
            for (int step = 1; step <= n; ++step) {
                HKL cand = list[(startIdx + step) % n];
                if (!IsCjkOrImeLayout(cand) && LOWORD((DWORD_PTR)cand) != LOWORD((DWORD_PTR)g_lastSafeHkl)) {
                    return cand;
                }
            }
        }
        // Pass 2: any non-CJK layout in the user's installed list
        for (int step = 1; step <= n; ++step) {
            HKL cand = list[(startIdx + step) % n];
            if (!IsCjkOrImeLayout(cand)) {
                return cand;
            }
        }
    }
    if (g_lastSafeHkl && !IsCjkOrImeLayout(g_lastSafeHkl)) return g_lastSafeHkl;
    return LoadKeyboardLayoutA("00000409", KLF_ACTIVATE);
}

#ifndef KLF_SETFORPROCESS
#define KLF_SETFORPROCESS 0x00000100
#endif

static HKL RedirectCjkKeyboardLayout(HKL blockedHkl) {
    HKL safeHkl = FindNextSafeKeyboardLayout(blockedHkl);
    if (safeHkl && safeHkl != blockedHkl) {
        g_lastSafeHkl = safeHkl;
        ActivateKeyboardLayout(safeHkl, 0);
        Log("Skipped CJK/IME keyboard layout %p -> activated safe layout %p.", blockedHkl, safeHkl);
    }
    return safeHkl ? safeHkl : ((g_lastSafeHkl && !IsCjkOrImeLayout(g_lastSafeHkl)) ? g_lastSafeHkl : (HKL)0x04090409);
}

static void DetachWindowImeContext(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    InitImmFunctions();
    if (g_pfnImmAssociateContext) {
        g_pfnImmAssociateContext(hwnd, NULL);
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        MSG* m = (MSG*)lParam;
        if (m->message == WM_INPUTLANGCHANGEREQUEST) {
            HKL targetHkl = (HKL)m->lParam;
            if (IsCjkOrImeLayout(targetHkl)) {
                HKL safeHkl = FindNextSafeKeyboardLayout(targetHkl);
                g_lastSafeHkl = safeHkl;
                m->lParam = (LPARAM)safeHkl;
            } else if (targetHkl) {
                g_lastSafeHkl = targetHkl;
            }
        } else if (m->message == WM_INPUTLANGCHANGE) {
            HKL newHkl = (HKL)m->lParam;
            if (m->hwnd) DetachWindowImeContext(m->hwnd);
            if (IsCjkOrImeLayout(newHkl)) {
                g_pendingCjkHkl = newHkl;
            } else if (newHkl) {
                g_lastSafeHkl = newHkl;
            }
            m->message = WM_NULL;
            m->wParam = 0;
            m->lParam = 0;
            return 0;
        } else if ((m->message >= 0x0280 && m->message <= 0x029F) ||
                   (m->message >= 0x010D && m->message <= 0x010F) ||
                   ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) && m->wParam == VK_PROCESSKEY)) {
            if (m->hwnd) DetachWindowImeContext(m->hwnd);
            m->message = WM_NULL;
            m->wParam = 0;
            m->lParam = 0;
            return 0;
        }
    }
    return CallNextHookEx(g_getMsgHook, code, wParam, lParam);
}

static LRESULT CALLBACK CallWndHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code >= 0 && lParam) {
        CWPSTRUCT* cwp = (CWPSTRUCT*)lParam;
        if (cwp->message == WM_INPUTLANGCHANGEREQUEST) {
            HKL targetHkl = (HKL)cwp->lParam;
            if (IsCjkOrImeLayout(targetHkl)) {
                HKL safeHkl = FindNextSafeKeyboardLayout(targetHkl);
                g_lastSafeHkl = safeHkl;
                cwp->lParam = (LPARAM)safeHkl;
            } else if (targetHkl) {
                g_lastSafeHkl = targetHkl;
            }
        } else if (cwp->message == WM_INPUTLANGCHANGE) {
            if (cwp->hwnd) DetachWindowImeContext(cwp->hwnd);
            HKL newHkl = (HKL)cwp->lParam;
            if (IsCjkOrImeLayout(newHkl)) {
                g_pendingCjkHkl = newHkl;
            } else if (newHkl) {
                g_lastSafeHkl = newHkl;
            }
        }
    }
    return CallNextHookEx(g_callWndHook, code, wParam, lParam);
}

static void EnsureThreadMessageHooksInstalled(bool forceFront) {
    if (!g_mainThread || GetCurrentThreadId() != g_mainThread) return;
    if (forceFront) {
        if (g_getMsgHook) { UnhookWindowsHookEx(g_getMsgHook); g_getMsgHook = nullptr; }
        if (g_callWndHook) { UnhookWindowsHookEx(g_callWndHook); g_callWndHook = nullptr; }
    }
    if (!g_getMsgHook) {
        g_getMsgHook = SetWindowsHookExA(WH_GETMESSAGE, GetMsgHookProc, nullptr, g_mainThread);
    }
    if (!g_callWndHook) {
        g_callWndHook = SetWindowsHookExA(WH_CALLWNDPROC, CallWndHookProc, nullptr, g_mainThread);
    }
}

static void EnsureGameWindowSubclassed(HWND candidateHwnd) {
    EnsureThreadMessageHooksInstalled(false);
    // Also check AppModule singleton (*0x007ACFC8 + 0x70) for DSLOA's main window handle
    HWND appHwnd = nullptr;
    uintptr_t appMod = *(uintptr_t*)0x007ACFC8;
    if (appMod && !IsBadReadPtr((const void*)(appMod + 0x70), sizeof(HWND))) {
        appHwnd = *(HWND*)(appMod + 0x70);
    }
    HWND targets[2] = { appHwnd, candidateHwnd ? GetAncestor(candidateHwnd, GA_ROOT) : nullptr };
    for (int i = 0; i < 2; ++i) {
        HWND root = targets[i];
        if (!root || !IsWindow(root)) continue;
        // Only subclass DSLOA's own main window (appHwnd, or a window belonging to this thread)
        if (appHwnd && root != appHwnd) continue;
        if (GetWindowThreadProcessId(root, nullptr) != g_mainThread) continue;
        WNDPROC curProc = (WNDPROC)GetWindowLongPtrA(root, GWLP_WNDPROC);
        // 1.5.16: ddraw/shims may already have subclassed the window, so accept any proc but ours.
        if (curProc && curProc != (WNDPROC)HookedWndProc && !g_gameHwnd) {
            g_gameHwnd = root;
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(root, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            DetachWindowImeContext(root);
            EnsureThreadMessageHooksInstalled(true); // re-seat at head of WH_GETMESSAGE chain ahead of msctf
            Log("Subclassed DSLOA main HWND %p (orig WndProc=%p): IME & CJK layout shield active.", root, g_origWndProc);
            break;
        }
    }
}


// Intercept AppModule::PrivateWndProc (0x00414FE4) called from StaticWndProc (0x00414FD1) so even if
// DirectDraw re-subclasses GWLP_WNDPROC after us, IME/CJK messages never reach DSLOA or DefWindowProcA.
typedef LRESULT (__attribute__((thiscall)) *PrivateWndProcFn)(void* self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static PrivateWndProcFn g_origPrivateWndProc = (PrivateWndProcFn)0x00414FE4;

static LRESULT __attribute__((thiscall)) HookPrivateWndProc(void* self, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUTLANGCHANGEREQUEST) {
        HKL targetHkl = (HKL)lParam;
        if (IsCjkOrImeLayout(targetHkl)) {
            HKL safeHkl = FindNextSafeKeyboardLayout(targetHkl);
            g_lastSafeHkl = safeHkl;
            return DefWindowProcA(hwnd, msg, wParam, (LPARAM)safeHkl);
        }
        if (targetHkl) g_lastSafeHkl = targetHkl;
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_INPUTLANGCHANGE) {
        DetachWindowImeContext(hwnd);
        HKL newHkl = (HKL)lParam;
        if (IsCjkOrImeLayout(newHkl)) {
            g_pendingCjkHkl = newHkl;
            return 0;
        }
        if (newHkl) g_lastSafeHkl = newHkl;
        return 0;
    }
    if ((msg >= 0x0280 && msg <= 0x029F) || (msg >= 0x010D && msg <= 0x010F)) {
        DetachWindowImeContext(hwnd);
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_PROCESSKEY) {
        return 0;
    }
    return g_origPrivateWndProc(self, hwnd, msg, wParam, lParam);
}

// 1.5.17: user32!GetKeyboardLayout(0) calls back into msctf.dll (TSF) whenever a TSF text service such as the
// Windows 10/11 Japanese IME is installed. msctf's per-thread init (0x3A05F) stores its SYSTHREAD pointer only AFTER
// its constructor returns, and that constructor itself calls GetKeyboardLayout(0), so the first call made while the
// window is being created (imm32!ImmSetActiveContext in WM_NCCREATE) recurses until the stack overflows (0xC0000005
// in msctf.dll). With idThread != 0 user32 returns the kernel HKL directly and never enters msctf, and for the
// current thread the result is identical, so always pass the real thread id.
// 1.5.20: NEVER mask the returned HKL here — msctf.dll+0x2C3C1 calls GetKeyboardLayout(0) after ActivateKeyboardLayout
// and compares it against the target HKL (0x2C3CD: cmp edx, ecx); returning a different HKL causes infinite recursion
// in msctf.dll (0x2C3DA -> 0xB3E64 -> 0x3A3FC -> 0x2C260) ending in STATUS_STACK_OVERFLOW (0xC00000FD).
typedef HKL (WINAPI *GetKeyboardLayoutFn)(DWORD);
static GetKeyboardLayoutFn g_origGetKeyboardLayout = nullptr;

static HKL WINAPI HookGetKeyboardLayout(DWORD idThread) {
    return g_origGetKeyboardLayout(idThread ? idThread : GetCurrentThreadId());
}

static void InstallGetKeyboardLayoutHook() {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    BYTE* fn = u32 ? (BYTE*)GetProcAddress(u32, "GetKeyboardLayout") : nullptr;
    static const BYTE prologue[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    if (!fn || memcmp(fn, prologue, 5) != 0) { Log("GetKeyboardLayout hook: unexpected prologue, NOT installed."); return; }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { Log("GetKeyboardLayout hook: VirtualAlloc failed."); return; }
    memcpy(tramp, fn, 5);
    tramp[5] = 0xE9;
    *(DWORD*)(tramp + 6) = (DWORD)((fn + 5) - (tramp + 10));
    g_origGetKeyboardLayout = (GetKeyboardLayoutFn)tramp;
    DWORD old;
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) { Log("GetKeyboardLayout hook: VirtualProtect failed."); return; }
    fn[0] = 0xE9;
    *(DWORD*)(fn + 1) = (DWORD)((BYTE*)HookGetKeyboardLayout - (fn + 5));
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    Log("Hooked user32!GetKeyboardLayout (TSF recursion guard).");
}

static void ImeDisableInstall() {
    // Completely stub UIShell::InitIme (0x006ED98F) and ImeUi_Initialize (0x00638B50) so DSLOA never
    // calls ImmDisableTextFrameService(0xffffffff) (0x00638D48), which crashes Windows 10/11's modern
    // TSF-only Japanese IME (imjptip.dll / msctf.dll) when the user cycles keyboard layouts.
    const BYTE patchRet[1] = { 0xC3 };                       // ret
    const BYTE patchMovAl1Ret[3] = { 0xB0, 0x01, 0xC3 };     // mov al, 1; ret
    const BYTE patchXorAlRet[3] = { 0x32, 0xC0, 0xC3 };      // xor al, al; ret

    WriteCode(0x006ED98F, patchRet, sizeof(patchRet));             // UIShell::InitIme
    WriteCode(0x00638B50, patchMovAl1Ret, sizeof(patchMovAl1Ret)); // ImeUi_Initialize
    WriteCode(0x00638B0E, patchRet, sizeof(patchRet));             // ImeUi_EnableIme
    // ImeUi_ProcessMessage: caller does not initialise *bHandled (5th arg), so clear it before returning 0.
    const BYTE patchProcessMsg[10] = { 0x8B, 0x44, 0x24, 0x14, 0xC6, 0x00, 0x00, 0x33, 0xC0, 0xC3 };
    WriteCode(0x00638251, patchProcessMsg, sizeof(patchProcessMsg)); // ImeUi_ProcessMessage
    WriteCode(0x006392F6, patchXorAlRet, sizeof(patchXorAlRet));   // ImeUi_IgnoreHotKey
    WriteCode(0x006389D5, patchRet, sizeof(patchRet));             // ImeUi_SetState
    WriteCode(0x0063793D, patchRet, sizeof(patchRet));             // ImeUi_Render
    WriteCode(0x0063956A, patchRet, sizeof(patchRet));             // ImeUi_CheckToggleState
    WriteCode(0x00639BDC, patchRet, sizeof(patchRet));             // ImeUi TSF CoUninitialize helper

    // Hook StaticWndProc -> PrivateWndProc call at 0x00414FD1
    PatchCall(0x00414FD1, (void*)HookPrivateWndProc);
    Log("ImeDisableInstall: DSLOA internal ImeUi disabled and StaticWndProc hooked.");
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUTLANGCHANGEREQUEST) {
        HKL targetHkl = (HKL)lParam;
        if (IsCjkOrImeLayout(targetHkl)) {
            HKL safeHkl = FindNextSafeKeyboardLayout(targetHkl);
            g_lastSafeHkl = safeHkl;
            return DefWindowProcA(hwnd, msg, wParam, (LPARAM)safeHkl);
        }
        if (targetHkl) g_lastSafeHkl = targetHkl;
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_INPUTLANGCHANGE) {
        DetachWindowImeContext(hwnd);
        HKL newHkl = (HKL)lParam;
        if (IsCjkOrImeLayout(newHkl)) {
            g_pendingCjkHkl = newHkl;
            return 0;
        }
        if (newHkl) g_lastSafeHkl = newHkl;
        return 0;
    }
    // Shield against IME (e.g. Japanese, Chinese, Korean) messages causing DirectX fullscreen disruptions
    if ((msg >= 0x0280 && msg <= 0x029F) || (msg >= 0x010D && msg <= 0x010F)) {
        DetachWindowImeContext(hwnd);
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_PROCESSKEY) {
        return 0;
    }
    if (msg == WM_DESTROY && hwnd == g_gameHwnd) {
        WNDPROC orig = g_origWndProc;
        g_origWndProc = nullptr;
        g_gameHwnd = nullptr;
        return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
    }
    return g_origWndProc ? CallWindowProcA(g_origWndProc, hwnd, msg, wParam, lParam)
                         : DefWindowProcA(hwnd, msg, wParam, lParam);
}

static BOOL WINAPI HookPeekMessageA(LPMSG msg, HWND wnd, UINT fmin, UINT fmax, UINT remove) {
    if (!g_mainThread) {
        g_mainThread = GetCurrentThreadId();
        EnsureThreadMessageHooksInstalled(false);
        Log("Main loop detected on thread %lu (PeekMessageA).", g_mainThread);
    }
    if (GetCurrentThreadId() == g_mainThread) {
        EnsureGameWindowSubclassed(wnd);
        HKL pending = g_pendingCjkHkl;
        if (pending) {
            g_pendingCjkHkl = nullptr;
            RedirectCjkKeyboardLayout(pending);
        }
        NR_PumpTick();
        JG_JipTick();  // both roles: the join freeze is a per-machine thing (src/jipguard.h)
        CharacterAbilities_Tick();
        SharedStash_Tick();
        if (g_cfg.role == Role::Server) { AutoHostTick(); LabMoveLocTick(); AreaScaleTick(); ShopStockTick(); InvFixTick(); QuestModeTick(); JipQueueTick(); PvpZoneTick(); }
        else if (g_cfg.role == Role::Client) { AutoJoinTick(); InvFixTick(); QuestModeTick(); ShopStockClientTick(); ChatFixTick(); ChatHistoryTick(); PartyPanelTick(); PanelFilterTick(); ChatDiagTick(); PartyPanelDiagTick(); }
    }
    BOOL res = g_origPeek(msg, wnd, fmin, fmax, remove);
    if (GetCurrentThreadId() == g_mainThread && g_gameHwnd) {
        HKL curHkl = g_origGetKeyboardLayout ? g_origGetKeyboardLayout(g_mainThread) : GetKeyboardLayout(g_mainThread);
        if (g_pendingCjkHkl || IsCjkOrImeLayout(curHkl)) {
            HKL blocked = g_pendingCjkHkl ? g_pendingCjkHkl : curHkl;
            g_pendingCjkHkl = nullptr;
            RedirectCjkKeyboardLayout(blocked);
        }
    }
    if (res && msg) {
        EnsureGameWindowSubclassed(msg->hwnd);

        if (msg->message == WM_INPUTLANGCHANGEREQUEST) {
            HKL targetHkl = (HKL)msg->lParam;
            if (IsCjkOrImeLayout(targetHkl)) {
                HKL safeHkl = FindNextSafeKeyboardLayout(targetHkl);
                g_lastSafeHkl = safeHkl;
                msg->lParam = (LPARAM)safeHkl;
            } else if (targetHkl) {
                g_lastSafeHkl = targetHkl;
            }
        } else if (msg->message == WM_INPUTLANGCHANGE) {
            HKL newHkl = (HKL)msg->lParam;
            if (msg->hwnd) DetachWindowImeContext(msg->hwnd);
            if (IsCjkOrImeLayout(newHkl)) {
                g_pendingCjkHkl = newHkl;
            } else if (newHkl) {
                g_lastSafeHkl = newHkl;
            }
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
        } else if ((msg->message >= 0x0280 && msg->message <= 0x029F) ||
                   (msg->message >= 0x010D && msg->message <= 0x010F) ||
                   ((msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN) && msg->wParam == VK_PROCESSKEY)) {
            msg->message = WM_NULL;
            msg->wParam = 0;
            msg->lParam = 0;
        }

        if (remove & PM_REMOVE) {
            if (CharacterAbilities_FilterMouse(msg)) {
                // Click handled by Character Abilities
            } else if (SharedStash_FilterMouse(msg)) {
                // Click handled by Shared Stash
            } else if (CharacterAbilities_FilterKey(msg)) {
                // Key consumed by Character Abilities
            } else if (SharedStash_FilterKey(msg)) {
                // Handled /stash or /sharedstash
            } else if (g_cfg.role != Role::Server) {
                ChatKeyFilter(msg);
            }
        }
    }
    return res;
}

// ----------------------------------------------------------------------------------------------
// Crash reporting: log + minidump into SiegeHostLogs
// ----------------------------------------------------------------------------------------------
typedef void (WINAPI *ExitProcessFn)(UINT);
static ExitProcessFn g_origExitProcess = nullptr;

static void WINAPI HookExitProcess(UINT code) {
    Log("!!!! HookExitProcess: ExitProcess called with code=%u from %p !!!!", code, __builtin_return_address(0));
    void* stack[32];
    USHORT captured = CaptureStackBackTrace(0, 32, stack, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        char mod[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)stack[i], &hm))
            GetModuleFileNameA(hm, mod, MAX_PATH);
        Log("    [%u] %p (%s + 0x%lX)", (unsigned)i, stack[i], mod, hm ? (DWORD)((BYTE*)stack[i] - (BYTE*)hm) : 0);
    }
    DWORD* sp;
    __asm__("movl %%esp, %0" : "=r"(sp));
    for (int i = 0; i < 32 && !IsBadReadPtr(sp + i, 4); ++i) {
        Log("    [esp+%02X] %08lX", i * 4, sp[i]);
    }
    if (g_log) fflush(g_log);
    if (g_origExitProcess) g_origExitProcess(code);
    else ExitProcess(code);
}

[[noreturn]] void FlushAndExit(UINT code) {
    Log("Process exiting with code %u", code);
    if (g_log) fflush(g_log);
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(1000);
}

static EXCEPTION_POINTERS* g_crashEp = nullptr;
static BYTE g_crashStack[65536] __attribute__((aligned(16)));

static void __attribute__((noinline)) CrashFilterWorker() {
    EXCEPTION_POINTERS* ep = g_crashEp;
    auto* er = ep->ExceptionRecord;
    auto* cx = ep->ContextRecord;
    static char mod[MAX_PATH];
    strcpy(mod, "?");
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &hm))
        GetModuleFileNameA(hm, mod, MAX_PATH);
    Log("!!! CRASH code=0x%08lX addr=%p module=%s (+0x%lX) info0=%lu info1=0x%08lX", er->ExceptionCode, er->ExceptionAddress, mod,
        hm ? (DWORD)((BYTE*)er->ExceptionAddress - (BYTE*)hm) : 0,
        er->NumberParameters > 0 ? (DWORD)er->ExceptionInformation[0] : 0,
        er->NumberParameters > 1 ? (DWORD)er->ExceptionInformation[1] : 0);
    Log("    eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX", cx->Eax, cx->Ebx, cx->Ecx,
        cx->Edx, cx->Esi, cx->Edi, cx->Ebp, cx->Esp);
    DWORD* fp = (DWORD*)cx->Ebp;
    for (int f = 0; f < 40 && fp && !IsBadReadPtr(fp, 8); ++f) {
        DWORD ret = fp[1];
        HMODULE fm = nullptr;
        static char fmod[MAX_PATH];
        fmod[0] = '\0';
        if (ret >= 0x00400000 &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)ret, &fm)) {
            GetModuleFileNameA(fm, fmod, MAX_PATH);
            const char* base = strrchr(fmod, '\\');
            base = base ? base + 1 : fmod;
            Log("    [frame %02d] ebp=%p ret=%08lX (%s + 0x%lX)", f, fp, ret, base, (DWORD)(ret - (DWORD)fm));
        } else {
            Log("    [frame %02d] ebp=%p ret=%08lX", f, fp, ret);
        }
        DWORD* nextFp = (DWORD*)fp[0];
        if (nextFp <= fp || (DWORD)nextFp - (DWORD)fp > 0x100000) break;
        fp = nextFp;
    }
    DWORD* sp = (DWORD*)cx->Esp;
    for (int i = 0; i < 64 && !IsBadReadPtr(sp + i, 4); ++i) {
        DWORD val = sp[i];
        HMODULE sm = nullptr;
        static char smod[MAX_PATH];
        smod[0] = '\0';
        if (val >= 0x00400000 &&
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)val, &sm)) {
            GetModuleFileNameA(sm, smod, MAX_PATH);
            const char* base = strrchr(smod, '\\');
            base = base ? base + 1 : smod;
            Log("    [esp+%02X] %08lX (%s + 0x%lX)", i * 4, val, base, (DWORD)(val - (DWORD)sm));
        } else {
            Log("    [esp+%02X] %08lX", i * 4, val);
        }
    }
    if (HMODULE dbg = LoadLibraryA("dbghelp.dll")) {
        typedef BOOL(WINAPI * MDW)(HANDLE, DWORD, HANDLE, int, PVOID, PVOID, PVOID);
        struct MINIDUMP_EXCEPTION_INFORMATION_ { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
        if (auto fn = (MDW)GetProcAddress(dbg, "MiniDumpWriteDump")) {
            static char path[64];
            sprintf(path, "crash_%lu.dmp", GetCurrentProcessId());
            HANDLE f = CreateFileA((LogDir() + path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION_ mei{GetCurrentThreadId(), ep, FALSE};
                fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0 /*MiniDumpNormal*/, &mei, nullptr, nullptr);
                CloseHandle(f);
                Log("    minidump written: %s", path);
            }
        }
    }
    if (g_log) fflush(g_log);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG s_crashLogged = 0;
    if (InterlockedCompareExchange(&s_crashLogged, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_crashEp = ep;
    DWORD oldEsp;
    DWORD newEsp = (DWORD)(g_crashStack + sizeof(g_crashStack) - 64);
    __asm__ volatile(
        "movl %%esp, %0\n\t"
        "movl %1, %%esp\n\t"
        "call %P2\n\t"
        "movl %0, %%esp\n\t"
        : "=&r"(oldEsp)
        : "r"(newEsp), "i"(CrashFilterWorker)
        : "memory", "cc", "eax", "ecx", "edx"
    );
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        CrashFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// DSLOA.exe wraps WinMain in an SEH __except filter at 0x00414F5C that passes bCallPreviousFilter=0
// to 0x0041C731, bypassing SetUnhandledExceptionFilter and exiting with code 1. Hook 0x00414F5C
// (7 bytes: 57 FF 15 54 91 72 00) so CrashFilter always logs registers, frames, and minidump first.
extern "C" int __stdcall Orig_DsloaSehFilter(EXCEPTION_POINTERS* ep);
__asm__(
    ".text\n"
    ".globl _Orig_DsloaSehFilter\n"
    ".globl _Orig_DsloaSehFilter@4\n"
    "_Orig_DsloaSehFilter:\n"
    "_Orig_DsloaSehFilter@4:\n"
    "  pushl %edi\n"
    "  call *0x00729154\n"
    "  movl $0x00414F63, %edx\n"
    "  jmp *%edx\n"
);

extern "C" int __stdcall Hook_DsloaSehFilter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if ((code & 0x80000000U) != 0 || code == 0x4661746CU /*'Fatl'*/) {
            CrashFilter(ep);
        }
    }
    return Orig_DsloaSehFilter(ep);
}

static void DsloaSehFilterInstall() {
    const DWORD kTarget = 0x00414F5C;
    const BYTE kExpect[7] = { 0x57, 0xFF, 0x15, 0x54, 0x91, 0x72, 0x00 };
    if (memcmp((const void*)kTarget, kExpect, 7) == 0) {
        BYTE patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        DWORD rel = (DWORD)(void*)&Hook_DsloaSehFilter - (kTarget + 5);
        memcpy(patch + 1, &rel, 4);
        if (WriteCode(kTarget, patch, 7)) {
            Log("Hooked DSLOA top-level SEH filter at 0x%08lX (crash minidump + stack trace).", kTarget);
        }
    } else {
        Log("WARNING: unexpected bytes at DSLOA SEH filter 0x%08lX.", kTarget);
    }
}

// ----------------------------------------------------------------------------------------------
// Startup
// ----------------------------------------------------------------------------------------------
static void Startup() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    g_dir = exePath;
    g_dir = g_dir.substr(0, g_dir.find_last_of("\\/") + 1);

    InitializeCriticalSection(&g_logLock);
    char logName[64];
    // One log per process so multiple instances (multi=true) don't interleave.
    sprintf(logName, "SiegeHost_%lu.log", GetCurrentProcessId());
    CreateDirectoryA((g_dir + "SiegeHostLogs").c_str(), nullptr);
    g_log = fopen((g_dir + "SiegeHostLogs\\" + logName).c_str(), "a");

    Log("==== SiegeHost %s starting in %s ====", SIEGEHOST_VERSION, exePath);
    Log("Command line: %s", GetCommandLineA());
#ifdef SIEGEHOST_MSS32
    Log("Loaded as Mss32.dll proxy (forwarding to mss32_orig.dll)");
#else
    Log("Loaded as version.dll proxy");
#endif
    LoadConfig();
    Log("Config: Enabled=%d Role=%s MaxPlayers=%d MaxSlots=%d DPMaxPlayers=%d PatchTeams=%d LogDirectPlay=%d Snapshots=%d",
        g_cfg.enabled, g_cfg.role == Role::Server ? "server" : g_cfg.role == Role::Client ? "client" : "none", g_cfg.maxPlayers,
        g_cfg.maxSlots, g_cfg.dpMaxPlayers, g_cfg.patchTeams, g_cfg.logDirectPlay, g_cfg.snapshots);
    SetUnhandledExceptionFilter(CrashFilter);
    AddVectoredExceptionHandler(1, VectoredCrashHandler);
    InstallGetKeyboardLayoutHook();
    JG_Init();

    // Disable Windows IME text services early so East Asian layout switching cannot disrupt DirectX fullscreen
    // 1.5.16: no Imm* calls inside DllMain (loader lock); the main thread does it in HookPeekMessageA.

    if (!g_cfg.enabled) { Log("Disabled by config. Acting as plain version.dll proxy."); return; }

    HMODULE exe = GetModuleHandleA(nullptr);
    if (!IdentifyTarget(exe)) { Log("Unknown executable build. Patches NOT applied (safety)."); return; }

    DsloaSehFilterInstall();
    ImeDisableInstall();

    std::vector<Patch> patches;
    if (g_cfg.maxSlots != 9) BuildSlotPatches(patches);
    else Log("MaxSlots=9 (vanilla): slot patches skipped.");
    if (g_cfg.maxPlayers > 0) BuildMaxPlayersPatch(patches);
    if (!patches.empty() && ApplyPatches(patches))
        Log("Patches applied: slots=%d maxPlayers=%d.", g_cfg.maxSlots, g_cfg.maxPlayers);

    // 0.7.3: stop a player sitting in the character-select screen from freezing everyone else's world.
    // Both roles: the freeze is a per-machine branch, so the game server AND every player needs it.
    JG_InstallJipGuard();
    // 1.5.2: both roles. The level a world demands is refused on the player's own machine as well as
    // asked for by the host, and the key that drives this is absent everywhere except the one shard.
    WorldLevelInstall();
    // 1.5.6: both roles. Zone-based PvP (Endless Dunes, Pit of Despair, Eastern Island) + dio_nothing.
    PvpZoneInstall();
    CharacterAbilities_Init();
    SharedStash_Init();
    StagingImportDisableInstall();
    if (g_cfg.role == Role::Server) { AreaScaleInstall(); ShopStockInstall(); InvFixInstall(); QuestModeInstall(); JipQueueInstall(); }
    else { InvFixInstall(); QuestModeInstall(); ShopStockClientInstall(); }   // 0.9.0: the backpack cap has to come off on a player's own machine too
    if (g_cfg.role == Role::Client) { ChatFixInstall(); PartyPanelInstall(); PortraitFixInstall(); if (g_cfg.showOtherPlayers) PanelHostFilterInstall(); PortraitDiagInstall(); ChatDiagInstall(); AreaScaleClientDiagInstall(); }
    else if (g_cfg.role == Role::None) { ChatFixGameMsgCooldownInstall(); }

    if (g_cfg.role == Role::Client) {
        PatchCall(0x00488D01, (void*)WrapNetPipeConnect);
        PatchCall(0x004885C4, (void*)WrapNetPipeConnect);
        Log("Wrapped NetPipe::Connect call sites (join diagnostics).");
    }
    if (HookIAT(exe, "kernel32.dll", "ExitProcess", (void*)HookExitProcess, (void**)&g_origExitProcess))
        Log("Hooked kernel32!ExitProcess (shutdown diagnostics).");
    else
        Log("WARNING: could not hook ExitProcess.");

    if (HookIAT(exe, "ole32.dll", "CoCreateInstance", (void*)HookCoCreateInstance, (void**)&g_origCoCreate))
        Log("Hooked ole32!CoCreateInstance (DirectPlay instrumentation).");
    else
        Log("WARNING: could not hook CoCreateInstance.");

    if (HookIAT(exe, "user32.dll", "PeekMessageA", (void*)HookPeekMessageA, (void**)&g_origPeek))
        Log("Hooked user32!PeekMessageA (main-thread pump%s).", g_cfg.role == Role::Server ? ", AutoHost enabled" : "");
    else
        Log("WARNING: could not hook PeekMessageA (AutoHost unavailable).");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
#ifndef SIEGEHOST_MSS32
        if (!LoadRealVersion()) return FALSE;
#else
        // Mss32 proxy: every Miles export is a PE forwarder to mss32_orig.dll (see src/mss32.def), nothing to load here.
#endif
        Startup();
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("==== SiegeHost unloading ====");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}

