# Dungeon Siege MMO Server Stack

Dedicated server infrastructure and client launcher for Dungeon Siege: Legends of Aranna (v1.50).

## Repository Structure

- `launcher/`: Client launcher (`SiegeLauncher`). Handles account login, character save sync, display settings, and auto-joining the game server.
- `server/SiegeHost/`: C++ hook DLL (`Mss32.dll`) loaded by `DSLOA.exe`. Enables 30-player cap, headless hosting (`AutoHost`), auto-join, mob scaling, and heartbeat.
- `server/SiegePortal/`: Central authentication service (Go). Manages accounts, cloud saves, sessions, and status API.
- `server/SiegeServer/`: Watchdog daemon (Go). Keeps `DSLOA.exe` and `SiegePortal` running, auto-restarts on crash/hang, and resets world when empty.
- `server/wine-directplay/`: Linux Wine DirectPlay setup files and test tools.
- `website/`: Status dashboard and web portal (Go).
- `mods/`: Sources, configurations, and documentation for client and server mods distributed through the launcher manifest.

## Build Instructions

### SiegeHost (C++)
```sh
cd server/SiegeHost
./build.sh
```

### Go Components
```sh
# Launcher
cd launcher && go build -o SiegeLauncher.exe .

# Server watchdog
cd server/SiegeServer && go build -o SiegeServer.exe .

# Central portal
cd server/SiegePortal && go build -o SiegePortal.exe .

# Website
cd website && go build -o website .
```

## Ports

- `UDP 6073`: DirectPlay discovery
- `UDP 2300-2400`: DirectPlay game data
- `TCP 8440`: SiegePortal API
- `TCP 8088`: Website
