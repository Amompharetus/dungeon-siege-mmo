#!/bin/bash
set -e
cd "$(dirname "$0")"

# Compile Windows PE version info and icon if windres is available
if command -v i686-w64-mingw32-windres >/dev/null 2>&1; then
    i686-w64-mingw32-windres -F pe-i386 -i version.rc -O coff -o version_windows_386.syso
elif command -v windres >/dev/null 2>&1; then
    windres -F pe-i386 -i version.rc -O coff -o version_windows_386.syso
fi

export CGO_ENABLED=0
export GOOS=windows
export GOARCH=386
go build -trimpath -ldflags="-s -w" -o SiegeLauncher.exe .
ls -lh SiegeLauncher.exe
