#!/bin/sh
# Cross-compiles SiegeHost for 32-bit Windows with MinGW-w64.
#  build/Mss32.dll   - PRIMARY: proxy for the game-local Miles DLL (original renamed to mss32_orig.dll)
#  build/version.dll - legacy proxy (does NOT load when Windows applies app-compat shims to DSLOA.exe)
set -e
mkdir -p build
SRC="src/siegehost.cpp src/autohost.cpp src/autojoin.cpp src/snapshot.cpp src/joinguard.cpp src/netrate.cpp src/jipguard.cpp src/chatdiag.cpp src/chatfix.cpp src/partypanel.cpp src/portraitfix.cpp src/areascale.cpp src/shopstock.cpp src/invfix.cpp src/jipqueue.cpp src/questmode.cpp src/pvpzone.cpp"
FLAGS="-std=c++17 -O2 -Wall -shared -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup -lole32"
i686-w64-mingw32-g++ $FLAGS -DSIEGEHOST_MSS32 -o build/Mss32.dll $SRC src/mss32.def -lole32 -lgdi32 -liphlpapi
i686-w64-mingw32-g++ $FLAGS -o build/version.dll $SRC src/version.def -Wl,--kill-at -lole32 -lgdi32 -liphlpapi
i686-w64-mingw32-strip build/Mss32.dll build/version.dll
