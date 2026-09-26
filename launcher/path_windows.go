package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"unsafe"
)

// spacelessPath returns a path without spaces for dir (which must exist): DSLOA.exe's command-line
// parser ignores quoted "user_path=..." arguments .
func spacelessPath(dir, account string) string {
	if !strings.Contains(dir, " ") {
		return dir
	}
	_ = os.MkdirAll(dir, 0o755)
	p, _ := syscall.UTF16PtrFromString(dir)
	buf := make([]uint16, 1024)
	get := syscall.NewLazyDLL("kernel32.dll").NewProc("GetShortPathNameW")
	if n, _, _ := get.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf))); n > 0 && int(n) < len(buf) {
		if s := syscall.UTF16ToString(buf[:n]); !strings.Contains(s, " ") {
			return s
		}
	}
	alt := filepath.Join(os.Getenv("SystemDrive")+`\`, "SiegeMMO", account)
	_ = os.MkdirAll(alt, 0o755)
	return alt
}

// physicalResolution returns the primary display's real pixel mode, ignoring DPI scaling:
// EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS) -> DEVMODEW.dmPelsWidth (+172) / dmPelsHeight (+176).
// This is what the launcher used until 0.7.6 — and handing it to the game is what broke players on
// scaled displays. Kept only so the two numbers can be logged side by side.
func physicalResolution() (int, int) {
	var dm [220]byte
	binary.LittleEndian.PutUint16(dm[68:], 220) // dmSize
	enum := syscall.NewLazyDLL("user32.dll").NewProc("EnumDisplaySettingsW")
	if r, _, _ := enum.Call(0, uintptr(0xFFFFFFFF), uintptr(unsafe.Pointer(&dm[0]))); r == 0 {
		return 0, 0
	}
	return int(binary.LittleEndian.Uint32(dm[172:])), int(binary.LittleEndian.Uint32(dm[176:]))
}

// virtualResolution returns the screen size as a program without DPI awareness is told it is —
// GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN). Windows virtualises these for such a process, so on a
// display scaled to 125% a 2560x1440 panel reports 2048x1152. This launcher declares no DPI
// awareness (Go binaries ship no manifest), and neither does DSLOA.exe, so this is the same number
// the game will see.
func virtualResolution() (int, int) {
	get := syscall.NewLazyDLL("user32.dll").NewProc("GetSystemMetrics")
	w, _, _ := get.Call(0) // SM_CXSCREEN
	h, _, _ := get.Call(1) // SM_CYSCREEN
	return int(w), int(h)
}

// desktopResolution returns the screen size to hand to Dungeon Siege.
//
// 0.7.6 : it must be the size the *game* sees, not the size the panel really is.
// Dungeon Siege is from 2002 and has no DPI awareness, so on a scaled display Windows reports a
// smaller screen to it than physically exists. Passing the physical size instead caused every
// display problem of 2026-09-17: at 125% scaling a forced 2560x1440 exceeded the 2048x1152 the game
// believed in and the game died with its own fatal error, while a forced 1920x1080 fitted but left
// the interface laid out for a screen larger than the frame, so the compass and toolbar fell outside
// the view. Proven by setting Windows scaling to 100%, where the two numbers coincide and the same
// forced resolution worked perfectly.
//
// Both numbers are logged whenever they differ, because this file's assumption about GetSystemMetrics
// is worth being able to check against a real machine rather than trusting.
func desktopResolution() (int, int) {
	vw, vh := virtualResolution()
	pw, ph := physicalResolution()
	if vw > 0 && pw > 0 && (vw != pw || vh != ph) {
		logf("display: panel is %dx%d, but the game is told %dx%d (Windows display scaling ~%d%%) - using %dx%d",
			pw, ph, vw, vh, pw*100/vw, vw, vh)
	}
	return pickDesktopResolution(vw, vh, pw, ph)
}
