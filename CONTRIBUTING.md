# Contributing to Dungeon Siege MMO

Thank you for your interest in contributing to the Dungeon Siege MMO project. Whether you are fixing bugs, improving server performance, designing quality-of-life mods, or enhancing web features, contributions are welcome.

---

## Getting Started

### Prerequisites

* **Go 1.24+**: For compiling `launcher/`, `server/SiegePortal/`, `server/SiegeServer/`, and `website/`.
* **MinGW-w64 (32-bit `i686-w64-mingw32-g++`)**: For compiling the C++ hook DLL (`server/SiegeHost/`).
* **Git**: For version control.

### Building Components

1. **Client Launcher:**
   ```sh
   cd launcher
   GOOS=windows GOARCH=386 go build -ldflags "-s -w" -o SiegeLauncher.exe .
   ```

2. **Server Watchdog & Central Portal:**
   ```sh
   cd server/SiegeServer && go build -o SiegeServer .
   cd ../SiegePortal && go build -o SiegePortal .
   ```

3. **SiegeHost Hook DLL (`Mss32.dll`):**
   ```sh
   cd server/SiegeHost
   ./build.sh
   ```

4. **Web Portal & Wiki:**
   ```sh
   cd website
   go build -o website .
   ```

---

## Submitting Pull Requests

1. **Fork & Branch:** Create a feature branch from `main` (e.g., `git checkout -b feature/my-new-feature`).
2. **Atomic Commits:** Keep commits focused, clean, and well-described.
3. **Format Code:**
   * Go code should adhere to standard Go formatting: `go fmt ./...`.
   * C++ code should follow existing naming and formatting conventions.
4. **Test Before Submitting:**
   * Verify that your changes compile without warnings.
   * If modifying spell scripts (`.skrit`) or templates (`.gas`), verify that they load cleanly in the engine without causing template errors or multiplayer desync.
5. **Open a PR:** Describe clearly what problem your PR solves, how you tested it, and reference any relevant issues.

---

## Game Modifications (Mods)

* Community mods should be added under the `mods/` directory with clear source scripts (`.skrit`, `.gas`), configurations, and documentation.
* Do not submit compiled `.dsres` or `.tank` binaries directly into git; provide the uncompressed source assets and documentation in `mods/README.md`.
* Ensure that any community mods you contribute are properly credited to their original authors.

---

## Reporting Issues

* **Bug Reports:** Open an issue on GitHub detailing the issue, your operating system, and reproduction steps.
* **Security & Exploits:** Please consult [SECURITY.md](SECURITY.md) to report vulnerabilities privately.
