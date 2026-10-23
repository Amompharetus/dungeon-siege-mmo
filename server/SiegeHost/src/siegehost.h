#pragma once
#include <windows.h>
#include <string>

#define SIEGEHOST_VERSION "1.5.12"

enum class Role { None, Server, Client };

struct Config {
    int enabled = 1;
    int snapshots = 0;       // 0 = disabled (default); 1 = save debug screenshots on screen transitions
    Role role = Role::None;
    int maxPlayers = 0;      // 0 = vanilla; N (9..30) = raise human cap (host AND clients must match)
    int maxSlots = 9;        // Server slots incl. index 0 (auto = MaxPlayers+1)
    int dpMaxPlayers = 0;    // DirectPlay dwMaxPlayers (auto = MaxPlayers+1)
    int patchTeams = 1;
    int logDirectPlay = 1;
    int noDpnsvr = 0;          // host without dpnsvr.exe, listening directly on HostPort (Wine/Linux servers)
    int hostPort = 6073;
    int gamePort = 0;          // server: fixed DirectPlay host port (0 = dynamic 2302-2400)
    int joinPort = 0;          // client: send the game search straight to this host port instead of 6073/dpnsvr
    // [Net] (0.7.0)
    int sendFrequency = 0;     // engine multiplayer send rate in Hz (0 = leave the engine default, which is 5)
    int mpSleep = -1;          // main-loop Sleep() in ms (-1 = leave the engine default; lower = more fps and more CPU)
    // [AutoHost] (Role=server)
    std::string gameName = "Dungeon Siege MMO";
    std::string hostCharacter = "ServerHost";
    int stuckTimeoutSec = 300;
    std::string map;            // internal map name, e.g. multiplayer_world (Utraean Peninsula); empty = game default
    std::string mpWorld;        // world level (regular/veteran/elite); empty = keep the game's current one
    int allowPausing = 0;       // 0 = players cannot pause the shared game (Server::RCSetAllowPausing)
    std::string startLocation;  // start group every player spawns at, e.g. elddim; empty = map default
    int areaScaling = 1;        // server: 1 = new monsters scale with the players in their area, not everyone online (0.7.7)
    int countServerHost = 0;    // server: 1 = ServerHost adds +1 to that count as the stock game did, 0 = real players only (0.7.8)
    // [Store] (0.8.2) - shop stock we add at runtime; server only, no content change
    int shopBackpacks = 4;              // how many backpacks every shop a player opens is topped up to (0 = off)
    int shopBackpackPrice = 1500;       // what the shop charges for one
    std::string shopBackpackTemplate = "dsx_backpack_empty";
    int jipQueue = 1;                   // server: 1 = whoever ticks ready joins without waiting for the others (0.8.6)
    int invFix = 1;                     // server: 1 = an item that will not fit goes to the player's backpack (0.8.4)
    int allowStartSelect = 0;   // 0.7.0: 1 = let each player pick their start town in staging (StartLocation is then only the default)
    std::string pvpLocations = "402,409,901";  // 1.5.6: world_locations.gas IDs with PvP enabled (Endless Dunes=402, Pit of Despair=409, Eastern Island=901)
    int dropInvNothing = 1;                    // 1.5.6: 1 = dio_nothing (keep all equipped, inventory, and gold on death)
    // [AutoJoin] (Role=client)
    std::string joinAddress;    // IPv4 of the game server (the edit box only accepts digits and dots)
    std::string playerName = "Player";
    int joinRetries = 20;
    int chatLineSeconds = 10;   // client: seconds a line stays in the in-game chat box (0 = leave the game value) (0.7.5)
    int chatHistoryOnOpen = 1;  // client: opening the chat shows the latest lines again (0.7.7)
    int showOtherPlayers = 1;   // client: 0 = the top-left panel shows only your own character (0.7.5; default 1 since 0.7.6)
    int showServerHost = 0;     // client: 1 = keep the server's ServerHost in the top-left panel (0.7.6)
    int nearbyOnly = 1;         // client: 1 = top-left panel shows only players in your area group (0.7.6)
    int portraitFix = 1;        // client: 1 = frame other players' portraits from the screen centre (0.7.6)
    int exitAfterGame = 1;      // client: close DSLOA.exe once the player has left the MMO game (0.6.1)
    // [World] (1.5.2) - one shard only: the level a veteran world asks for, whatever its map says.
    int veteranRequiredLevel = 0;   // 0 = leave every world exactly as the map set it
    // [JoinGuard] (0.6.0)
    std::string iniPath;        // the SiegeHost.ini that was loaded (the client re-reads Ticket= from it at every Connect)
    int jgEnabled = 1;          // server: refuse players without a valid launcher join ticket (needs Key + ServerId)
    std::string jgKey;          // server: hex HMAC key (siegeserver derives it from the shard key)
    std::string jgServerId;     // server: this server's id ("na", "eu")
};

const Config& Cfg();
void Log(const char* fmt, ...);
std::string LogDir();          // "<game>\SiegeHostLogs\"
long RemotePlayerCount();
[[noreturn]] void FlushAndExit(UINT code);
void AutoHostTick();
void AutoJoinTick();
void Snapshot(const char* label);
void JoinGuardServerTick();
void JG_Init();
bool JG_Enforced();
void JG_ClientConnectData(const void* data, DWORD size, const void** outData, DWORD* outSize);
void JG_ClientConnectComplete(PVOID msg);
HRESULT JG_ServerMessage(DWORD id, PVOID msg, HRESULT (WINAPI* callGame)(void*, DWORD, PVOID), void* gameCtx);
std::string JG_HeartbeatLines();
void JG_InstallJipGuard();              // 0.7.3: the staging-screen freeze fix (src/jipguard.h)
std::string JG_JipSelfCheck();
void JG_JipTick();                      // every frame, main thread, both roles
std::string JG_JipHeartbeatLines();
void JipQueueInstall();                 // 0.8.6: ready-first joining (src/jipqueue.cpp)
void JipQueueTick();
std::string JipQueueHeartbeatLines();
std::string WorldLevelHeartbeatLines();
void PvpZoneInstall();                  // 1.5.6: zone-based PvP + dio_nothing (src/pvpzone.cpp)
void PvpZoneTick();
std::string PvpZoneHeartbeatLines();
void InvFixInstall();                   // 0.8.4: pickups that do not fit go to the backpack (src/invfix.cpp)
void InvFixTick();
std::string InvFixHeartbeatLines();
void ShopStockInstall();                // 0.8.2: backpacks on every shop's MISC tab (src/shopstock.cpp)
void ShopStockTick();
void QuestModeInstall();             // 1.3.0: quests shared across the world (src/questmode.cpp)
void QuestModeTick();
std::string QuestModeHeartbeatLines();
void ShopStockClientTick();
void ShopStockClientInstall();                   // every frame, main thread, server only
std::string ShopStockHeartbeatLines();
void NR_PumpTick();
void NR_Tick();
std::string NR_HeartbeatLines();
void CompareSessionInfo(const BYTE* remoteAppReserved);  // join compatibility diagnostics
void ChatKeyFilter(LPMSG msg);  // 1.5.10: in-game chat Ctrl+Backspace, Ctrl+A filter
