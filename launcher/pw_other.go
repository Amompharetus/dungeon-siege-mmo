//go:build !windows

package main

import (
	"bufio"
	"strings"
)

// readPassword — see the Windows version for why passwords are shown as they are typed (0.7.7).
func readPassword(in *bufio.Reader) string {
	s, _ := in.ReadString('\n')
	return strings.TrimSpace(s)
}
