//go:build windows

package main

import (
	"bufio"
	"strings"
)

// readPassword reads a password in plain view.
//
// Until 0.7.6 this cleared ENABLE_ECHO_INPUT so the console showed nothing while typing, which is
// the usual convention — but on a small friends' server it caused more trouble than it prevented:
// people could not tell whether their keystrokes were registering, mistyped passwords silently, and
// hit "the two passwords are different" on a screen that had given them no feedback at all.
//
// The trade-off is real and accepted: anyone looking at the screen, or any screenshot, screen share
// or stream, can read the password. It is typed on the player's own machine, and the prompts say so.
func readPassword(in *bufio.Reader) string {
	s, _ := in.ReadString('\n')
	return strings.TrimSpace(s)
}
