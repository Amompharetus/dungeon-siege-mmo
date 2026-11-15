package main

import (
	"archive/zip"
	"bytes"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// savePortalStub stands in for SiegePortal: it serves one online save and accepts uploads, recording what it got.
type savePortalStub struct {
	online     []byte // what GET /api/save returns
	uploaded   []byte // what the launcher PUT, if anything
	uploadFail bool   // refuse uploads, the way a 409 or an unreachable server does
	gets, puts int
}

func (f *savePortalStub) start(t *testing.T) *httptest.Server {
	t.Helper()
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == "GET" && r.URL.Path == "/api/save":
			f.gets++
			w.Header().Set("Content-Type", "application/zip")
			_, _ = w.Write(f.online)
		case r.Method == "PUT" && r.URL.Path == "/api/save":
			f.puts++
			if f.uploadFail {
				w.WriteHeader(http.StatusConflict)
				_, _ = w.Write([]byte(`{"error":"this account is in a game on another PC; save NOT uploaded"}`))
				return
			}
			b, _ := io.ReadAll(r.Body)
			f.uploaded = b
			_, _ = w.Write([]byte(`{"ok":true,"save_rev":99}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(srv.Close)
	return srv
}

// zipWith builds an online save holding one character file with the given contents.
func zipWith(t *testing.T, name, body string) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	w, err := zw.Create("Save/" + name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := w.Write([]byte(body)); err != nil {
		t.Fatal(err)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func newProfile(t *testing.T, body string) string {
	t.Helper()
	profile := filepath.Join(gameDir, "MMOProfile", "Ashley")
	if err := os.MkdirAll(filepath.Join(profile, "Save"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(profile, "Save", "QueenAshley.dsparty"), []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return profile
}

func localCharacter(t *testing.T, profile string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(profile, "Save", "QueenAshley.dsparty"))
	if err != nil {
		t.Fatalf("the character file is gone: %v", err)
	}
	return string(b)
}

// This is the incident, replayed. The player's two hours are on this PC, the online save is the older one,
// and the previous run left a marker because its uploads were refused. The launcher must not download.
func TestUnsavedCharacterIsUploadedNotOverwritten(t *testing.T) {
	gameDir = t.TempDir()
	profile := newProfile(t, "18400034 experience")

	f := &savePortalStub{online: zipWith(t, "QueenAshley.dsparty", "15630444 experience")}
	srv := f.start(t)
	settings.PortalURL, settings.Account, settings.Token = srv.URL, "Ashley", "tok"

	savePending("Ashley", 1080, "409 save NOT uploaded")

	if err := syncProfile(profile, "session-1", 1080); err != nil {
		t.Fatalf("sync failed: %v", err)
	}

	if got := localCharacter(t, profile); got != "18400034 experience" {
		t.Fatalf("the newer character was overwritten: %q", got)
	}
	if f.gets != 0 {
		t.Fatal("the online save must not even be downloaded while a save is outstanding")
	}
	if f.puts != 1 {
		t.Fatalf("expected exactly one upload, got %d", f.puts)
	}
	// The body is a zip, so the character has to be read out of it rather than matched as raw bytes.
	if got := characterInZip(t, f.uploaded); got != "18400034 experience" {
		t.Fatalf("the wrong character was uploaded: %q", got)
	}
	if loadPending() != nil {
		t.Fatal("the marker must be cleared once the character is safely online")
	}
}

// If the upload fails again, the launcher still must not overwrite, and must keep the marker for next time.
func TestStillUnreachableKeepsTheCharacterAndTheMarker(t *testing.T) {
	gameDir = t.TempDir()
	profile := newProfile(t, "18400034 experience")

	f := &savePortalStub{online: zipWith(t, "QueenAshley.dsparty", "15630444 experience"), uploadFail: true}
	srv := f.start(t)
	settings.PortalURL, settings.Account, settings.Token = srv.URL, "Ashley", "tok"

	savePending("Ashley", 1080, "409 save NOT uploaded")

	if err := syncProfile(profile, "session-1", 1080); err != nil {
		t.Fatalf("a failed upload must not stop the player from playing: %v", err)
	}
	if got := localCharacter(t, profile); got != "18400034 experience" {
		t.Fatalf("the character was overwritten after a failed upload: %q", got)
	}
	if f.gets != 0 {
		t.Fatal("it downloaded anyway")
	}
	if loadPending() == nil {
		t.Fatal("the marker must survive so the next run tries again")
	}
}

// A genuine divergence: another PC uploaded while this one had work outstanding. The online save wins, but
// this PC's character is kept where the player can get at it.
func TestDivergenceKeepsACopyBeforeDownloading(t *testing.T) {
	gameDir = t.TempDir()
	profile := newProfile(t, "18400034 experience")

	f := &savePortalStub{online: zipWith(t, "QueenAshley.dsparty", "from the other PC")}
	srv := f.start(t)
	settings.PortalURL, settings.Account, settings.Token = srv.URL, "Ashley", "tok"

	savePending("Ashley", 1080, "409 save NOT uploaded")

	// The server is now at 1083: three revisions arrived from somewhere else.
	if err := syncProfile(profile, "session-1", 1083); err != nil {
		t.Fatalf("sync failed: %v", err)
	}

	if got := localCharacter(t, profile); got != "from the other PC" {
		t.Fatalf("the online save should have been used, got %q", got)
	}
	matches, _ := filepath.Glob(filepath.Join(backupsDir(), "unsaved_Ashley_*.zip"))
	if len(matches) != 1 {
		t.Fatalf("this PC's character was discarded instead of kept: %v", matches)
	}
	if loadPending() != nil {
		t.Fatal("the marker must be cleared once the divergence has been dealt with")
	}
}

// With nothing outstanding the launcher behaves exactly as it always has.
func TestNoMarkerDownloadsAsBefore(t *testing.T) {
	gameDir = t.TempDir()
	profile := newProfile(t, "old local data")

	f := &savePortalStub{online: zipWith(t, "QueenAshley.dsparty", "the online save")}
	srv := f.start(t)
	settings.PortalURL, settings.Account, settings.Token = srv.URL, "Ashley", "tok"

	if err := syncProfile(profile, "session-1", 1080); err != nil {
		t.Fatalf("sync failed: %v", err)
	}
	if got := localCharacter(t, profile); got != "the online save" {
		t.Fatalf("the normal download did not happen: %q", got)
	}
	if f.puts != 0 {
		t.Fatal("nothing should have been uploaded")
	}
}

// characterInZip reads QueenAshley's file out of an uploaded profile zip.
func characterInZip(t *testing.T, z []byte) string {
	t.Helper()
	zr, err := zip.NewReader(bytes.NewReader(z), int64(len(z)))
	if err != nil {
		t.Fatalf("the upload is not a zip: %v", err)
	}
	for _, f := range zr.File {
		if filepath.Base(f.Name) == "QueenAshley.dsparty" {
			rc, err := f.Open()
			if err != nil {
				t.Fatal(err)
			}
			defer rc.Close()
			b, err := io.ReadAll(rc)
			if err != nil {
				t.Fatal(err)
			}
			return string(b)
		}
	}
	t.Fatalf("the upload contains no character file: %d entries", len(zr.File))
	return ""
}
