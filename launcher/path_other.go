//go:build !windows

package main

func spacelessPath(dir, account string) string { return dir }

func desktopResolution() (int, int) { return 0, 0 }
