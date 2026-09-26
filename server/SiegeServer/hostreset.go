package main

// 0.7.1 - quests reset with the world (RE-12).
//
// The bundled quest-save mod (ikkyo_mpsave_beta_6, world/contentdb/components/quest_manager.skrit)
// copies the world's quest state into every human player's character every 60 s, and loads it back
// into a NEW world only from the HOST's character. Our host is ServerHost, so its save file collects
// everything anyone ever did on this server and replays it on every world reset, EU wake-up and crash
// restart: new characters join a world where the quests are already done, and the one-time quest
// rewards (the *_gave_reward flags) are never handed out again.
//
// Fix: before every game start, put back a clean ServerHost - the same character the game's own
// AutoHost flow creates (same name, template, skins, gold), with no [questbits]. Nothing else about
// the host changes, no game file or mod is touched, and players' own characters are not affected.

import (
	_ "embed"
	"os"
	"path/filepath"
	"strings"
)

// A ServerHost created by DSLOA 1.50 + AutoHost in the lab, never used in a game with quest
// progress: party.gas is byte-for-byte the live servers' ServerHost minus the [questbits] block.
//
//go:embed serverhost-clean.dsparty
var cleanHostParty []byte

const cleanHostName = "ServerHost"

func resetHostQuests(p *bool) bool {
	if p == nil {
		return true
	}
	return *p
}

// resetHostCharacter writes the clean ServerHost into <base>/server-profile/Save before a game start.
// Returns a short description for the log, or "" when it did nothing.
func resetHostCharacter(base string, cfg Config) string {
	if !resetHostQuests(cfg.ResetHostQuests) {
		return ""
	}
	if !strings.EqualFold(cfg.HostCharacter, cleanHostName) {
		return "host character is " + cfg.HostCharacter + ", not " + cleanHostName + ": quest reset skipped"
	}
	save := filepath.Join(base, "server-profile", "Save")
	if err := os.MkdirAll(save, 0o755); err != nil {
		return "quest reset failed: " + err.Error()
	}
	dst := filepath.Join(save, cleanHostName+".dsparty")
	cur, err := os.ReadFile(dst)
	if err == nil && string(cur) == string(cleanHostParty) {
		if b, err := os.ReadFile(dst + ".bak"); err != nil || string(b) == string(cleanHostParty) {
			return "" // already clean
		}
	}
	// Keep the first file we replace, once, so what it carried can still be inspected.
	keep := dst + ".before-quest-reset"
	if err == nil {
		if _, statErr := os.Stat(keep); os.IsNotExist(statErr) {
			_ = os.WriteFile(keep, cur, 0o644)
		}
	}
	for _, p := range []string{dst, dst + ".bak"} {
		tmp := p + ".tmp"
		if err := os.WriteFile(tmp, cleanHostParty, 0o644); err != nil {
			return "quest reset failed: " + err.Error()
		}
		if err := os.Rename(tmp, p); err != nil {
			_ = os.Remove(tmp)
			return "quest reset failed: " + err.Error()
		}
	}
	return "host character reset: ServerHost restored to a clean character (quests and one-time quest rewards start fresh with this world)"
}
