# Running the Dungeon Siege 1 Game Server on Linux (Wine + DirectPlay)

Environment setup and notes for running `DSLOA.exe` under Linux:
- Wine 9.0+, 32-bit prefix (`WINEARCH=win32`), Xvfb / virtual framebuffer.
- Dedicated host via SiegeHost `AutoHost`.
- DirectPlay network stack configuration.

## DirectPlay under Wine
Wine's built-in `dpnet` implementation lacks complete `IDirectPlay8Server::Host` support. To run a dedicated host under Linux, native DirectPlay 8 runtime libraries from Windows are used.

## Configuration Steps
1. **DirectPlay Runtime Files:**
   Obtain the 32-bit DirectPlay files from a Windows installation with DirectPlay enabled (`C:\Windows\SysWOW64`):
   `dpnet.dll`, `dpnaddr.dll`, `dpnathlp.dll`, `dpnhpast.dll`, `dpnhupnp.dll`, `dpnlobby.dll`, `dpwsockx.dll`, `dplayx.dll`, `dpmodemx.dll`, and `dpnsvr.exe`.
   *(Note: Microsoft system files must adhere to standard licensing and should not be redistributed).*

2. **String Resources:**
   Modern Windows stores DirectPlay strings in separate `.mui` files. Under Wine, `dpnet.dll` requires localized string tables in order to initialize properly without errors.
   - `addstr.c` compiles with `i686-w64-mingw32-gcc -municode` into `addstr.exe`.
   - Running `addstr.exe <dll>` under Wine adds a placeholder `RT_STRING` table (IDs 0–319) using `UpdateResource`.
   - Apply to: `dpnet.dll`, `dpnathlp.dll`, `dpwsockx.dll`, `dplayx.dll`, `dpnhpast.dll`, and `dpnhupnp.dll`.

3. **Installation & Registration:**
   Copy the files to `$WINEPREFIX/drive_c/windows/system32/`, then register the components:
   ```bash
   wine regsvr32 /s dpnet.dll dpnathlp.dll dplayx.dll
   ```
   This registers the TCP/IP service provider CLSID (`{EBFE7BA0-...}`).

4. **DLL Overrides:**
   Configure Wine to use native libraries:
   ```bash
   export WINEDLLOVERRIDES="dpnsvr.exe=n;dpnet,dpnaddr,dpnathlp,dpnhpast,dpnhupnp,dpnlobby,dpwsockx,dplayx=n"
   ```

5. **Execution:**
   Launch the game server executable with `SiegeHost.ini` configured for `Role=server`.
