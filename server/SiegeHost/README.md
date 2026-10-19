# SiegeHost

Runtime extension DLL for **Dungeon Siege: Legends of Aranna** (`DSLOA.exe` v1.50 community build, PE timestamp `0x3F8FFE7F`).

Built as `Mss32.dll`, a proxy for the game's Miles sound system DLL. The original Miles binary is renamed to `mss32_orig.dll` and all exports forward directly to it. When `DSLOA.exe` starts, Windows loads `Mss32.dll` from the game directory, hooking engine functions in memory without modifying retail game executables or tank archives.

Current version: **1.5.9**

---

## Core Capabilities

| System | Role | Description |
|---|---|---|
| **Player Cap Extension** | Both | Raises the human multiplayer cap from 8 to 30 players. Patches slot tables, DirectPlay limits, and friendship arrays. |
| **JoinGuard** | Server | Verifies HMAC-SHA256 join tickets issued by the central portal. Kicks unauthorized direct connections and validates skill levels against experience progression curves. |
| **AutoHost** | Server | Fully automated, headless server hosting. Navigates game menus, configures map parameters, dismisses dialog prompts, and re-hosts when the world empties. |
| **AutoJoin** | Client | Automatically joins the target shard IP from the launcher without manual menu navigation. |
| **Join-in-Progress (JIP) Queue** | Both | Defers map-load freezes during late joins and manages a ready-first queue so players currently in the world are not interrupted. |
| **Zone-Based PvP** | Both | Enables hostile player-vs-player combat only within designated world zones (The Endless Dunes, The Pit of Despair, The Eastern Island). Blocks cross-player damage elsewhere. |
| **Death Drop Protection** | Server | Enforces `dio_nothing` inside PvP zones so players retain all equipped items, inventory, and gold on death. |
| **Veteran Level Override** | Both | Patches world level parsing in memory to permit characters meeting shard-specific level requirements (e.g. level 54+) into veteran tier maps. |
| **Shared Quest Sync** | Both | Synchronizes active quest state across all players in the world, resetting cleanly when a world cycle restarts. |
| **Inventory & Shop Fixes** | Server | Fixes inventory overflow so picked-up items properly route into backpacks; restocks merchant shelves once per world cycle; places the Fury Eye on the bandit boss. |
| **Area Scaling** | Server | Scales monster difficulty and density based on players present in the local area group rather than the global player count. |
| **UI & Chat Quality of Life** | Client | Adds nearby-only party panel filtering, corrects portrait centering, extends chat line duration, and rate-limits out-of-mana warning messages. |

---

## Configuration (`SiegeHost.ini`)

The configuration file sits in the game root next to `DSLOA.exe`:

```ini
[SiegeHost]
Enabled=1              ; 1 = active proxy hooks, 0 = pure pass-through
Role=server            ; server | client | none (defaults MaxPlayers=30)
MaxPlayers=30          ; 0 = vanilla (8); 9..30 = raised cap
PatchTeams=1           ; Synchronize team friendship arrays
LogDirectPlay=1        ; Log connection and networking events
Snapshots=0            ; 1 = save diagnostic screenshots on screen transitions

[AutoHost]             ; Role=server settings
GameName=Dungeon Siege MMO
HostCharacter=ServerHost
StuckTimeoutSec=300    ; Watchdog timeout before restarting unresponsive staging
AllowPausing=0         ; Disable multiplayer pausing
Map=multiplayer_world
World=regular          ; regular | veteran | elite
StartLocation=elddim
AllowStartLocationSelection=1
PvpLocations=402,409,901 ; Location IDs enabling PvP (Dunes=402, Pit=409, Island=901)
DropInvNothing=1       ; 1 = keep items and gold on death in PvP zones

[AutoJoin]             ; Role=client settings
Address=127.0.0.1      ; Shard IP to connect to
PlayerName=Player
Retries=20
ExitAfterGame=1        ; Automatically close DSLOA.exe upon exiting the session

[Net]
SendFrequency=30       ; DirectPlay flush rate in Hz (default 30, engine stock is 5)

[Chat]
LineSeconds=10         ; Duration chat lines remain visible on screen
HistoryOnOpen=1        ; Display recent chat lines when opening the input box

[UI]
ShowOtherPlayers=1     ; Show party member panels
NearbyOnly=1           ; Only display players within the same map area group
PortraitFix=1          ; Frame portraits properly at high display resolutions

[World]
VeteranRequiredLevel=0 ; Shard-specific veteran level override (0 = map default)

[JoinGuard]
Enabled=1              ; Enforce launcher authentication tickets
Key=                   ; Hex HMAC key shared with the server watchdog
ServerId=              ; Unique identifier for the server shard
```

---

## Build

Built on Linux or WSL using MinGW-w64 (32-bit `i686` target):

```sh
./build.sh
```

Outputs `build/Mss32.dll` and legacy `build/version.dll`.

---

## Installation

1. In your Dungeon Siege install directory, rename the original `Mss32.dll` to `mss32_orig.dll`.
2. Copy `build/Mss32.dll` into the game directory.
3. Place `SiegeHost.ini` alongside `DSLOA.exe`.

To uninstall, remove `Mss32.dll` and restore `mss32_orig.dll`.

---

## Changelog Summary

* **1.5.9**: Added zone-based PvP for Endless Dunes, Pit of Despair, and Eastern Island with `dio_nothing` death item protection. Added AutoHost dialog text hook and Yes/No auto-confirmation.
* **1.5.5**: Added 3-second cooldown on out-of-mana chat notices and corrected player leave message grammar.
* **1.5.2**: Added in-memory GAS parser override for veteran world required level (`VeteranRequiredLevel`).
* **1.5.0**: Added skill level validation against experience tables in JoinGuard.
* **1.3.0**: Shared quest synchronization across players; shop restocking once per world; placed Fury Eye in the bandit boss loot table.
* **1.0.2**: Fixed inventory backpack pickup overflow and corrected backpack counter recounts during character restore.
* **0.8.6**: Implemented ready-first Join-in-Progress (JIP) queuing.
* **0.7.6**: Added area-based monster scaling, nearby player party filtering, and portrait framing fixes.
* **0.6.0**: Added JoinGuard HMAC ticket validation.
* **0.4.0**: Switched primary build to `Mss32.dll` Miles audio forwarder.
* **0.1.0**: Initial proxy DLL, memory patch safety checks, and 30-player cap expansion.
