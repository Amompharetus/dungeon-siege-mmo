package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func withGameDir(t *testing.T) string {
	t.Helper()
	d := t.TempDir()
	old := gameDir
	gameDir = d
	t.Cleanup(func() { gameDir = old })
	return d
}

// The exact shape of a corrupted file: 330 bytes of NUL, left by a hard kill mid-write.
func TestNulFileIsRepaired(t *testing.T) {
	d := withGameDir(t)
	if err := os.WriteFile(filepath.Join(d, "launcher.json"), make([]byte, 330), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := loadSettings(); err != nil {
		t.Fatalf("a damaged settings file must not stop the launcher: %v", err)
	}
	if settings.PortalURL != defaultPortalURL {
		t.Fatalf("portal_url = %q, want the default", settings.PortalURL)
	}
	b, err := os.ReadFile(filepath.Join(d, "launcher.json"))
	if err != nil {
		t.Fatal(err)
	}
	var got Settings
	if err := json.Unmarshal(b, &got); err != nil {
		t.Fatalf("the file left on disk is still not valid JSON: %v", err)
	}
	if _, err := os.Stat(filepath.Join(d, "launcher.json.bad")); err != nil {
		t.Fatalf("the damaged file should have been kept: %v", err)
	}
}

func TestEmptyAndMissingAndGarbage(t *testing.T) {
	for _, tc := range []struct {
		name  string
		write []byte // nil means write no file at all
	}{
		{"missing", nil},
		{"empty", []byte{}},
		{"whitespace and nuls", []byte("  \x00\n\x00 ")},
		{"truncated json", []byte(`{"portal_url": "http://x`)},
		{"not json at all", []byte("<html>portal is down</html>")},
		{"json but no portal", []byte(`{"width": 800}`)},
	} {
		t.Run(tc.name, func(t *testing.T) {
			d := withGameDir(t)
			if tc.write != nil {
				if err := os.WriteFile(filepath.Join(d, "launcher.json"), tc.write, 0o644); err != nil {
					t.Fatal(err)
				}
			}
			if err := loadSettings(); err != nil {
				t.Fatalf("must recover, got %v", err)
			}
			if settings.PortalURL == "" {
				t.Fatal("portal_url is still empty after recovery")
			}
			if settings.SyncMinutes <= 0 {
				t.Fatalf("sync_minutes = %d", settings.SyncMinutes)
			}
		})
	}
}

// A file that is fine must be left exactly as the player had it.
func TestGoodFileIsNotTouched(t *testing.T) {
	d := withGameDir(t)
	// "fullscreen", not "window": since 0.8.8 a saved window mode is migrated, which is a rewrite
	// on purpose. This test is about a file with nothing to migrate being left completely alone.
	good := `{"portal_url":"http://example.test:8440/","account":"Someone","width":1366,"height":768,"sync_minutes":5,"display":"fullscreen","server_id":"eu"}`
	p := filepath.Join(d, "launcher.json")
	if err := os.WriteFile(p, []byte(good), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := loadSettings(); err != nil {
		t.Fatal(err)
	}
	if settings.Account != "Someone" || settings.ServerID != "eu" || settings.SyncMinutes != 5 {
		t.Fatalf("settings were not kept: %+v", settings)
	}
	if settings.PortalURL != "http://example.test:8440" {
		t.Fatalf("portal_url = %q, the trailing slash should be trimmed and nothing else", settings.PortalURL)
	}
	b, _ := os.ReadFile(p)
	if string(b) != good {
		t.Fatal("a valid launcher.json was rewritten; it should have been left alone")
	}
	if _, err := os.Stat(p + ".bad"); err == nil {
		t.Fatal("a valid file must not be backed up as damaged")
	}
}

func TestSaveLeavesNoTempAndIsReadable(t *testing.T) {
	d := withGameDir(t)
	settings = Settings{PortalURL: "http://example.test:8440", Width: 1280, Height: 720, SyncMinutes: 2, ServerID: "na"}
	saveSettings()
	if _, err := os.Stat(filepath.Join(d, "launcher.json.tmp")); err == nil {
		t.Fatal("a temporary file was left behind")
	}
	b, err := os.ReadFile(filepath.Join(d, "launcher.json"))
	if err != nil {
		t.Fatal(err)
	}
	if isBlank(b) {
		t.Fatal("saved an empty file")
	}
	var got Settings
	if err := json.Unmarshal(b, &got); err != nil {
		t.Fatalf("saved file is not valid JSON: %v", err)
	}
	if got.ServerID != "na" {
		t.Fatalf("server_id = %q", got.ServerID)
	}
}

func TestIsBlank(t *testing.T) {
	blank := [][]byte{nil, {}, make([]byte, 330), []byte("\x00\x00"), []byte(" \r\n\t")}
	for _, b := range blank {
		if !isBlank(b) {
			t.Fatalf("%q should count as blank", b)
		}
	}
	for _, b := range [][]byte{[]byte("{}"), []byte("\x00{\x00"), []byte("x")} {
		if isBlank(b) {
			t.Fatalf("%q should not count as blank", b)
		}
	}
}

// Recovery must survive a folder it cannot write to, with a message a player can act on.
func TestReadOnlyFolderSaysSomethingUseful(t *testing.T) {
	d := withGameDir(t)
	if os.Geteuid() == 0 {
		t.Skip("running as root, permissions do not apply")
	}
	if err := os.WriteFile(filepath.Join(d, "launcher.json"), make([]byte, 16), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chmod(d, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(d, 0o755) })
	err := loadSettings()
	if err == nil {
		t.Skip("the filesystem allowed the write anyway")
	}
	if !strings.Contains(err.Error(), "read-only") {
		t.Fatalf("the error should tell the player what to check, got: %v", err)
	}
}
