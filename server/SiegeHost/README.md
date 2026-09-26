# SiegeHost

Runtime extension for **Dungeon Siege: Legends of Aranna** (`DSLOA.exe` v1.50 community build, PE timestamp `0x3F8FFE7F`).

**Since 0.4.0 the primary build is `Mss32.dll`**, a proxy for the game's own Miles sound DLL. The original is renamed to `mss32_orig.dll` and every export is forwarded to it. SiegeServer and SiegeLauncher install it automatically. The `version.dll` build is kept as a legacy option, but Windows 11 ignores it when compat shims are applied to DSLOA.exe. Legacy description: it is built as `version.dll`. Windows loads it automatically when the file sits next to `DSLOA.exe`; the game imports VERSION.dll, and version.dll is not a KnownDLL, so the copy in the game folder wins. All real `version.dll` functions are forwarded to the system copy, so the game behaves normally.

Current version: **0.4.0** (2026-09-15)

## What it does (v0.1.0)
| Feature | Details | RE reference |
|---|---|---|
| Safety check | Patches only if PE timestamp + SizeOfImage match **and** every original byte sequence matches (all-or-nothing). Otherwise it logs and acts as a plain proxy. | RE-01 §6 |
| Player-slot patch | Raises `Server` player slots from 9 to `MaxPlayers+1` (5 sites) + 2 team-friendship loops | RE-01 §3.1–3.5 |
| Session max players | Forces `[session+0xD8]` = `MaxPlayers` | RE-01 §3.6 |
| DirectPlay cap | Overrides `dwMaxPlayers` in `IDirectPlay8Server::Host` / `SetApplicationDesc` | RE-00 §4 |
| DirectPlay log | Logs Initialize, Host, connect, create/destroy player and terminate events | – |
| Logs | `<game>\SiegeHostLogs\SiegeHost_<pid>.log` (one file per process) | – |

**Host and clients must all run SiegeHost with the same `MaxPlayers` when it is above 8** (see RE-01 §1: clients index their player table without bounds checks).

## Configuration — `SiegeHost.ini` (next to DSLOA.exe)
```ini
[SiegeHost]
Enabled=1          ; 0 = pure version.dll proxy, no patches, no hooks
MaxPlayers=0       ; 0 = vanilla (8). 9..30 = raise the human player cap
; Advanced (normally derived from MaxPlayers):
; MaxSlots=        ; Server slots incl. index 0 (default MaxPlayers+1, min 9, max 31)
; DPMaxPlayers=    ; DirectPlay dwMaxPlayers (default MaxPlayers+1)
PatchTeams=1       ; patch Server::RSJoinTeam friend loops
LogDirectPlay=1
```
If the ini is missing, defaults apply: vanilla behaviour plus logging.

## Build
Linux/WSL with MinGW-w64 (i686):
```sh
./build.sh          # -> build/Mss32.dll and build/version.dll
```

## Install / uninstall
Rename the game's original `Mss32.dll` in your Dungeon Siege install directory to `mss32_orig.dll`, then copy `build/Mss32.dll` and `SiegeHost.ini` into that folder. Delete or restore the original file to uninstall.

## Roles (0.3.0)
```ini
[SiegeHost]
Role=server        ; server | client | none   (server/client default MaxPlayers=30)
[AutoHost]         ; Role=server: host a game unattended (RE-02)
GameName=Dungeon Siege MMO
HostCharacter=ServerHost
StuckTimeoutSec=300 ; exit(3) if not in game after this -> watchdog restarts
[AutoJoin]         ; Role=client: Multiplayer -> Internet -> Connect
Address=1.2.3.4    ; IPv4 only (the game's IP box accepts digits and dots)
PlayerName=Player1
Retries=20
```
* `SIEGEHOST_INI=<path>` environment variable overrides the ini location (the watchdog uses it so the server and a normal client can share one game folder).
* Role=server writes `SiegeHostLogs\heartbeat.txt` every 5 s (phase, worldstate, players, uptime).
* Crashes are logged with registers and stack, plus `crash_<pid>.dmp`.

## Changelog
* **0.4.0** (2026-09-15): primary build is the Mss32.dll forwarder (fixes "doesn't load on Windows 11"). Verified with sound on.
* **0.3.0** (2026-09-15): Role=client AutoJoin; `SIEGEHOST_INI`; MaxPlayers default 30 for server/client roles. Verified menu flow.
* **0.2.0** (2026-09-15): Role=server AutoHost via PeekMessageA main-thread hook + UIShell::SendUIMessage; heartbeat; crash handler; exit codes 3/4.
* **0.1.0** (2026-09-15): first version. Proxy, identity check, slot/max-player patches, DirectPlay logging. Smoke-tested under Wine (loads, patches apply); not yet tested in a real multiplayer session.
