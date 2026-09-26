//go:build !windows

package main

import "strings"

func spacelessPath(dir, account string) string { return dir }

// winePath maps a Linux path to the Z: drive Wine exposes (no spaces allowed in user_path).
func winePath(p string) string { return "Z:" + strings.ReplaceAll(p, "/", "\\") }
