package main

import (
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

func winePath(p string) string { return p }
