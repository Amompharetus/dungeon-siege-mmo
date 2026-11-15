package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Anyone an older launcher left in window mode must come back in full screen, once, without being
// asked, and the change must be on disk so it sticks.
func TestSavedWindowModeIsMigrated(t *testing.T) {
	d := withGameDir(t)
	p := filepath.Join(d, "launcher.json")
	if err := os.WriteFile(p, []byte(`{"portal_url":"http://example.test:8440","display":"window","width":1024,"height":768,"sync_minutes":2}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := loadSettings(); err != nil {
		t.Fatal(err)
	}
	if !strings.EqualFold(settings.Display, "fullscreen") {
		t.Fatalf("display = %q, want fullscreen", settings.Display)
	}
	b, _ := os.ReadFile(p)
	var got Settings
	if err := json.Unmarshal(b, &got); err != nil {
		t.Fatal(err)
	}
	if !strings.EqualFold(got.Display, "fullscreen") {
		t.Fatalf("on disk display = %q, the migration did not stick", got.Display)
	}
}

// Case should not matter, and neither should the other settings surviving.
func TestWindowMigrationKeepsEverythingElse(t *testing.T) {
	d := withGameDir(t)
	p := filepath.Join(d, "launcher.json")
	if err := os.WriteFile(p, []byte(`{"portal_url":"http://example.test:8440","account":"Someone","display":"WINDOW","width":1366,"height":768,"sync_minutes":5,"server_id":"eu","resolution":"1024x768"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := loadSettings(); err != nil {
		t.Fatal(err)
	}
	if !strings.EqualFold(settings.Display, "fullscreen") {
		t.Fatalf("display = %q", settings.Display)
	}
	if settings.Account != "Someone" || settings.ServerID != "eu" || settings.SyncMinutes != 5 || settings.Resolution != "1024x768" {
		t.Fatalf("other settings were lost: %+v", settings)
	}
}

// "game" is the escape hatch for a machine where the game will not start; it must survive.
// Retired in 0.8.10: "game" is now migrated away rather than kept. See TestSavedGameModeIsMigrated.
func skippedTestGameDecidesIsKept(t *testing.T) {
	d := withGameDir(t)
	if err := os.WriteFile(filepath.Join(d, "launcher.json"), []byte(`{"portal_url":"http://example.test:8440","display":"game"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := loadSettings(); err != nil {
		t.Fatal(err)
	}
	if !gameDecides() {
		t.Fatalf("display = %q, 'game' should be left alone", settings.Display)
	}
}

// Nothing anywhere may report window mode any more, whatever is in the settings.
func TestWindowModeIsNeverReported(t *testing.T) {
	for _, v := range []string{"window", "WINDOW", "Window", "fullscreen", "game", ""} {
		settings = Settings{Display: v}
		if windowMode() {
			t.Fatalf("windowMode() is true for display=%q", v)
		}
	}
}

// 0.8.9. "Let the game choose" used to pass no display arguments at all, so a DungeonSiege.ini
// carrying fullscreen=false from an older install put the player back in a window - through the very
// option that was meant to be the safe fallback. It must now insist on full screen and give up only
// the resolution.
// Retired in 0.8.10: the option it covered no longer exists, and
// TestEveryDisplayValueGivesFullScreenWithAResolution is the stronger check that replaced it.
func skippedTestGameDecidesStillForcesFullScreen(t *testing.T) {
	withGameDir(t)
	settings = Settings{Display: "game", Width: 1024, Height: 768}
	args, w, h := displayArgs()
	if len(args) != 1 || args[0] != "fullscreen=true" {
		t.Fatalf("args = %v, want exactly [fullscreen=true]", args)
	}
	if w != 0 || h != 0 {
		t.Fatalf("w,h = %d,%d, want 0,0 - the resolution is the game's to pick", w, h)
	}
}

// Whatever the settings say, nothing may ever ask for a window.
func TestNoDisplayModeCanProduceAWindow(t *testing.T) {
	withGameDir(t)
	for _, d := range []string{"game", "GAME", "fullscreen", "window", "WINDOW", "", "nonsense"} {
		settings = Settings{Display: d, Width: 1024, Height: 768}
		args, _, _ := displayArgs()
		for _, a := range args {
			if a == "fullscreen=false" {
				t.Fatalf("display=%q produced %v, which starts the game in a window", d, args)
			}
		}
		seen := false
		for _, a := range args {
			if a == "fullscreen=true" {
				seen = true
			}
		}
		if !seen {
			t.Fatalf("display=%q produced %v, which does not insist on full screen", d, args)
		}
	}
}

// Full screen still passes a resolution; that is the difference between the two options.
func TestFullScreenStillPassesAResolution(t *testing.T) {
	withGameDir(t)
	settings = Settings{Display: "fullscreen", Width: 1024, Height: 768, Resolution: "1280x720"}
	args, w, h := displayArgs()
	if w != 1280 || h != 720 {
		t.Fatalf("w,h = %d,%d, want 1280,720", w, h)
	}
	if len(args) != 3 {
		t.Fatalf("args = %v, want fullscreen plus width and height", args)
	}
}

// 0.8.10. The display choice is gone entirely; full screen is the only mode. A saved "game", like a
// saved "window", must be migrated so nobody is left on an option that no longer exists.
func TestSavedGameModeIsMigrated(t *testing.T) {
	for _, v := range []string{"game", "GAME", "Game"} {
		d := withGameDir(t)
		p := filepath.Join(d, "launcher.json")
		if err := os.WriteFile(p, []byte(`{"portal_url":"http://example.test:8440","account":"Someone","display":"`+v+`","sync_minutes":3,"server_id":"na"}`), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := loadSettings(); err != nil {
			t.Fatal(err)
		}
		if !strings.EqualFold(settings.Display, "fullscreen") {
			t.Fatalf("display=%q became %q, want fullscreen", v, settings.Display)
		}
		if settings.Account != "Someone" || settings.ServerID != "na" || settings.SyncMinutes != 3 {
			t.Fatalf("other settings were lost: %+v", settings)
		}
		b, _ := os.ReadFile(p)
		var got Settings
		if err := json.Unmarshal(b, &got); err != nil {
			t.Fatal(err)
		}
		if !strings.EqualFold(got.Display, "fullscreen") {
			t.Fatalf("on disk display = %q, the migration did not stick", got.Display)
		}
	}
}

// Whatever is in the settings, the game is launched full screen with a real resolution.
func TestEveryDisplayValueGivesFullScreenWithAResolution(t *testing.T) {
	withGameDir(t)
	for _, d := range []string{"game", "GAME", "window", "WINDOW", "fullscreen", "", "nonsense"} {
		settings = Settings{Display: d, Width: 1024, Height: 768, Resolution: "1280x720"}
		args, w, h := displayArgs()
		if w != 1280 || h != 720 {
			t.Fatalf("display=%q gave w,h = %d,%d", d, w, h)
		}
		full := false
		for _, a := range args {
			if a == "fullscreen=false" {
				t.Fatalf("display=%q produced %v, which starts the game in a window", d, args)
			}
			if a == "fullscreen=true" {
				full = true
			}
		}
		if !full || len(args) != 3 {
			t.Fatalf("display=%q produced %v, want full screen plus width and height", d, args)
		}
	}
}

func TestGameDecidesIsRetired(t *testing.T) {
	for _, v := range []string{"game", "GAME", "fullscreen", ""} {
		settings = Settings{Display: v}
		if gameDecides() {
			t.Fatalf("gameDecides() is true for display=%q", v)
		}
	}
}
