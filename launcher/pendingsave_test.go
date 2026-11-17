package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The decision table. Getting this wrong either loses a character or silently keeps a stale one, so every
// branch is named rather than inferred.
func TestPendingDecision(t *testing.T) {
	cases := []struct {
		name    string
		marker  *pendingSave
		account string
		rev     int
		want    pendingAction
	}{
		{"no marker: download as usual", nil, "Ashley", 1080, pendingNone},
		{"empty marker is ignored", &pendingSave{}, "Ashley", 1080, pendingNone},
		{"online save untouched since we failed: upload ours",
			&pendingSave{Account: "Ashley", ServerRev: 1080}, "Ashley", 1080, pendingUpload},
		{"account name casing must not matter",
			&pendingSave{Account: "ashley", ServerRev: 1080}, "Ashley", 1080, pendingUpload},
		{"another PC uploaded since: the two have diverged",
			&pendingSave{Account: "Ashley", ServerRev: 1080}, "Ashley", 1083, pendingConflict},
		{"a different player on the same PC must not act on it",
			&pendingSave{Account: "Ashley", ServerRev: 1080}, "Rowan", 42, pendingNone},
		{"rev 0 on both sides still counts as untouched",
			&pendingSave{Account: "New", ServerRev: 0}, "New", 0, pendingUpload},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := pendingDecision(c.marker, c.account, c.rev); got != c.want {
				t.Fatalf("got %v want %v", got, c.want)
			}
		})
	}
}

// A marker left by one run has to be found by the next one, which is a different process.
func TestPendingSurvivesTheRun(t *testing.T) {
	gameDir = t.TempDir()

	if loadPending() != nil {
		t.Fatal("a fresh install must have no marker")
	}

	savePending("Ashley", 1080, "409 session expired or taken over by another PC")

	p := loadPending()
	if p == nil {
		t.Fatal("the marker was not written")
	}
	if p.Account != "Ashley" || p.ServerRev != 1080 {
		t.Fatalf("marker came back wrong: %+v", p)
	}
	if p.When.IsZero() {
		t.Fatal("the marker must record when it happened")
	}
	if pendingDecision(p, "Ashley", 1080) != pendingUpload {
		t.Fatal("the round-tripped marker must still say upload")
	}

	clearPending()
	if loadPending() != nil {
		t.Fatal("clearPending left the marker behind; the next run would refuse to download forever")
	}
}

// A corrupt or half-written marker must not stop the player from playing.
func TestUnreadableMarkerIsIgnored(t *testing.T) {
	gameDir = t.TempDir()
	if err := os.MkdirAll(backupsDir(), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(pendingPath(), []byte("{not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	if p := loadPending(); p != nil {
		t.Fatalf("a corrupt marker must be ignored, got %+v", p)
	}
	if pendingDecision(loadPending(), "Ashley", 1080) != pendingNone {
		t.Fatal("a corrupt marker must fall back to the normal download")
	}
}

// The conflict path copies the character aside before anything overwrites it. If that copy cannot be made the
// caller refuses to continue, so the copy actually appearing on disk is the thing worth testing.
func TestKeepLocalCopyWritesSomething(t *testing.T) {
	gameDir = t.TempDir()
	profile := filepath.Join(gameDir, "MMOProfile", "Ashley")
	if err := os.MkdirAll(filepath.Join(profile, "Save"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(profile, "Save", "QueenAshley.dsparty"), []byte("party data"), 0o644); err != nil {
		t.Fatal(err)
	}

	path, err := keepLocalCopy(profile, "unsaved", "Ashley")
	if err != nil {
		t.Fatalf("copy failed: %v", err)
	}
	st, err := os.Stat(path)
	if err != nil {
		t.Fatalf("the copy is not on disk: %v", err)
	}
	if st.Size() == 0 {
		t.Fatal("the copy is empty")
	}
	if filepath.Dir(path) != backupsDir() {
		t.Fatalf("the copy must go to _local_backups, went to %s", filepath.Dir(path))
	}
}

// An empty profile has nothing to protect, and must report that rather than writing an empty zip the player
// would later mistake for their character.
func TestKeepLocalCopyRefusesAnEmptyProfile(t *testing.T) {
	gameDir = t.TempDir()
	profile := filepath.Join(gameDir, "MMOProfile", "Nobody")
	if err := os.MkdirAll(profile, 0o755); err != nil {
		t.Fatal(err)
	}
	if _, err := keepLocalCopy(profile, "unsaved", "Nobody"); err == nil {
		t.Fatal("an empty profile must not produce a copy that looks like a character")
	}
}

func TestMigrateLegacyRoot(t *testing.T) {
	root := t.TempDir()
	// No SiegeMMO folder -> no-op
	if _, _, ok := migrateLegacyRoot(root); ok {
		t.Fatal("expected false when SiegeMMO does not exist")
	}
	// Create SiegeMMO/player1/Save/hero.dsparty
	oldSub := filepath.Join(root, "SiegeMMO", "player1", "Save")
	if err := os.MkdirAll(oldSub, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(oldSub, "hero.dsparty"), []byte("save"), 0o644); err != nil {
		t.Fatal(err)
	}
	oldDir, newDir, ok := migrateLegacyRoot(root)
	if !ok {
		t.Fatal("expected migrateLegacyRoot to succeed")
	}
	if filepath.Base(oldDir) != "SiegeMMO" || filepath.Base(newDir) != "Dungeon Siege MMO" {
		t.Fatalf("unexpected paths: %s -> %s", oldDir, newDir)
	}
	if _, err := os.Stat(oldDir); !os.IsNotExist(err) {
		t.Fatalf("expected oldDir to be gone, stat err=%v", err)
	}
	if b, err := os.ReadFile(filepath.Join(newDir, "player1", "Save", "hero.dsparty")); err != nil || string(b) != "save" {
		t.Fatalf("migrated file missing or corrupt: %v %q", err, string(b))
	}
}
