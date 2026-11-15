package main

import (
	"bufio"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

// Tests for the 0.7.8 password-confirmation work: a password that is being *created* or *changed*
// is typed twice; a password typed to *log in* is still typed once.

// ---------- helpers ----------

// feed replaces the launcher's shared stdin with a scripted set of lines and captures everything
// printed, so a test can assert both what the launcher asked and what it did.
func feed(t *testing.T, lines ...string) func() string {
	t.Helper()
	oldIn, oldOut := stdin, os.Stdout
	stdin = bufio.NewReader(strings.NewReader(strings.Join(lines, "\n") + "\n"))
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	done := make(chan string, 1)
	go func() { b, _ := io.ReadAll(r); done <- string(b) }()
	return func() string {
		_ = w.Close()
		os.Stdout, stdin = oldOut, oldIn
		return <-done
	}
}

type loginCall struct {
	Name, Password, Invite string
}

// fakePortal stands in for SiegePortal. exists is the set of accounts it already knows.
type fakePortal struct {
	exists    map[string]string // name -> password
	logins    []loginCall
	pwChanges []string
	srv       *httptest.Server
}

func newFakePortal(t *testing.T, exists map[string]string) *fakePortal {
	t.Helper()
	p := &fakePortal{exists: exists}
	if p.exists == nil {
		p.exists = map[string]string{}
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/api/login", func(w http.ResponseWriter, r *http.Request) {
		var in loginCall
		_ = json.NewDecoder(r.Body).Decode(&in)
		p.logins = append(p.logins, in)
		pw, known := p.exists[in.Name]
		switch {
		case known && pw == in.Password:
			_ = json.NewEncoder(w).Encode(map[string]any{"token": "tok", "name": in.Name, "created": false})
		case known:
			w.WriteHeader(http.StatusUnauthorized)
			_ = json.NewEncoder(w).Encode(map[string]string{"error": "wrong password"})
		default:
			p.exists[in.Name] = in.Password
			_ = json.NewEncoder(w).Encode(map[string]any{"token": "tok", "name": in.Name, "created": true})
		}
	})
	mux.HandleFunc("/api/account/password", func(w http.ResponseWriter, r *http.Request) {
		var in struct{ NewPassword string }
		_ = json.NewDecoder(r.Body).Decode(&in)
		p.pwChanges = append(p.pwChanges, in.NewPassword)
		_, _ = w.Write([]byte("{}"))
	})
	p.srv = httptest.NewServer(mux)
	t.Cleanup(p.srv.Close)

	old := settings
	settings = Settings{PortalURL: p.srv.URL}
	t.Cleanup(func() { settings = old })
	return p
}

// saveSettings writes launcher.json next to the executable; tests must not touch the real one.
func isolateSettings(t *testing.T) {
	t.Helper()
	old := gameDir
	gameDir = t.TempDir()
	t.Cleanup(func() { gameDir = old })
}

// ---------- newPassword ----------

func TestNewPasswordMatchFirstTry(t *testing.T) {
	stop := feed(t, "hunter22", "hunter22")
	p, ok := newPassword(stdin, "pw: ")
	out := stop()
	if !ok || p != "hunter22" {
		t.Fatalf("got %q %v, want hunter22 true", p, ok)
	}
	if !strings.Contains(out, "Type the password again:") {
		t.Fatalf("second prompt missing from output: %q", out)
	}
}

func TestNewPasswordMismatchThenMatch(t *testing.T) {
	stop := feed(t, "hunter22", "huntre22", "hunter22", "hunter22")
	p, ok := newPassword(stdin, "pw: ")
	out := stop()
	if !ok || p != "hunter22" {
		t.Fatalf("got %q %v, want hunter22 true", p, ok)
	}
	if !strings.Contains(out, "The two passwords are different") {
		t.Fatalf("player was not told why it repeated: %q", out)
	}
}

func TestNewPasswordEmptyCancels(t *testing.T) {
	stop := feed(t, "", "should not be read")
	p, ok := newPassword(stdin, "pw: ")
	out := stop()
	if ok || p != "" {
		t.Fatalf("got %q %v, want empty false", p, ok)
	}
	if strings.Contains(out, "Type the password again") {
		t.Fatal("cancelling must not ask for a second entry")
	}
}

func TestNewPasswordGivesUpAfterThree(t *testing.T) {
	// Long enough to pass the length check, so this exercises the mismatch path and not that one.
	stop := feed(t, "attempt1", "wrong111", "attempt2", "wrong222", "attempt3", "wrong333", "unused")
	_, ok := newPassword(stdin, "pw: ")
	out := stop()
	if ok {
		t.Fatal("three mismatched pairs should fail, not loop forever")
	}
	if strings.Count(out, "The two passwords are different") != 3 {
		t.Fatalf("expected three mismatch messages: %q", out)
	}
}

func TestNewPasswordHandlesCRLF(t *testing.T) {
	// Real Windows consoles deliver \r\n; readPassword trims it.
	oldIn := stdin
	stdin = bufio.NewReader(strings.NewReader("hunter22\r\nhunter22\r\n"))
	defer func() { stdin = oldIn }()
	oldOut := os.Stdout
	os.Stdout, _ = os.Open(os.DevNull)
	defer func() { os.Stdout = oldOut }()
	p, ok := newPassword(stdin, "pw: ")
	if !ok || p != "hunter22" {
		t.Fatalf("got %q %v, want hunter22 true", p, ok)
	}
}

// ---------- creating an account ----------

func TestCreateAsksTwice(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	stop := feed(t, "2", "player1", "hunter22", "hunter22")
	err := login(false)
	out := stop()
	if err != nil {
		t.Fatal(err)
	}
	if len(p.logins) != 1 || p.logins[0].Password != "hunter22" || p.logins[0].Name != "player1" {
		t.Fatalf("portal saw %+v", p.logins)
	}
	if settings.Account != "player1" || settings.Token != "tok" {
		t.Fatalf("settings not saved: %+v", settings)
	}
	if !strings.Contains(out, "Type the password again:") {
		t.Fatalf("creation did not confirm the password: %q", out)
	}
	if !strings.Contains(out, "Account player1 created") {
		t.Fatalf("no creation message: %q", out)
	}
}

func TestCreateMismatchRetries(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	stop := feed(t, "2", "player1", "hunter22", "typo", "hunter22", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	stop()
	// One login call only: the mismatch is caught before the portal is touched at all.
	if len(p.logins) != 1 || p.logins[0].Password != "hunter22" {
		t.Fatalf("portal saw %+v", p.logins)
	}
}

func TestCreateNameAlreadyTakenExplainsItself(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, map[string]string{"player1": "realpw"})
	// First attempt: taken name with the wrong password. Second: log in to it properly.
	stop := feed(t, "2", "player1", "hunter22", "hunter22", "1", "player1", "realpw")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if len(p.logins) != 2 {
		t.Fatalf("expected two portal calls, got %+v", p.logins)
	}
	if !strings.Contains(out, "Could not create that account") || !strings.Contains(out, "already taken") {
		t.Fatalf("player was not told the name is taken: %q", out)
	}
	if !strings.Contains(out, "Welcome back, player1") {
		t.Fatalf("second attempt did not log in: %q", out)
	}
}

func TestCreateWhenAccountAlreadyExistsWithThatPassword(t *testing.T) {
	isolateSettings(t)
	newFakePortal(t, map[string]string{"player1": "hunter22"})
	stop := feed(t, "2", "player1", "hunter22", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if !strings.Contains(out, "already existed") {
		t.Fatalf("misleading 'created' message for an existing account: %q", out)
	}
}

func TestNewPasswordRejectsShortLocally(t *testing.T) {
	stop := feed(t, "abc", "hunter22", "hunter22")
	p, ok := newPassword(stdin, "pw: ")
	out := stop()
	if !ok || p != "hunter22" {
		t.Fatalf("got %q %v, want hunter22 true", p, ok)
	}
	if !strings.Contains(out, "at least 6 characters, please") {
		t.Fatalf("player not told why it was rejected: %q", out)
	}
}

func TestCreateShortPasswordNeverReachesPortal(t *testing.T) {
	isolateSettings(t)
	fp := newFakePortal(t, nil)
	stop := feed(t, "2", "player1", "abc", "hunter22", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	stop()
	if len(fp.logins) != 1 || fp.logins[0].Password != "hunter22" {
		t.Fatalf("portal saw %+v", fp.logins)
	}
}

// ---------- logging in ----------

func TestLoginAsksOnce(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, map[string]string{"player1": "hunter22"})
	stop := feed(t, "1", "player1", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if strings.Contains(out, "Type the password again") {
		t.Fatal("logging in must not ask for the password twice")
	}
	if len(p.logins) != 1 || p.logins[0].Password != "hunter22" {
		t.Fatalf("portal saw %+v", p.logins)
	}
}

func TestEnterDefaultsToLogIn(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, map[string]string{"player1": "hunter22"})
	stop := feed(t, "", "player1", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	stop()
	if len(p.logins) != 1 || p.logins[0].Name != "player1" || p.logins[0].Password != "hunter22" {
		t.Fatalf("Enter did not read name and password in step: %+v", p.logins)
	}
}

func TestInviteStillReadInStep(t *testing.T) {
	// The invite prompt must consume the line after the password, not the password itself.
	isolateSettings(t)
	p := newFakePortal(t, nil)
	stop := feed(t, "2", "player1", "hunter22", "hunter22", "INVITE-1")
	if err := login(true); err != nil {
		t.Fatal(err)
	}
	stop()
	if len(p.logins) != 1 || p.logins[0].Invite != "INVITE-1" || p.logins[0].Password != "hunter22" {
		t.Fatalf("prompts fell out of step: %+v", p.logins)
	}
}

func TestEmptyNameRepromptsWithoutCallingPortal(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, map[string]string{"player1": "hunter22"})
	stop := feed(t, "1", "", "1", "player1", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	stop()
	if len(p.logins) != 1 {
		t.Fatalf("an empty name must not reach the portal: %+v", p.logins)
	}
}

// ---------- the crossed case: "Log in" to a name that did not exist ----------

func TestLoginThatCreatesConfirmsPassword(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	stop := feed(t, "1", "player1", "hunter22", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if !strings.Contains(out, "so one has been created for you") {
		t.Fatalf("player not told a new account was made: %q", out)
	}
	if len(p.pwChanges) != 0 {
		t.Fatalf("a matching confirmation must not reset the password: %+v", p.pwChanges)
	}
}

func TestLoginThatCreatesFixesATypo(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	// Typed "huntre22" by mistake, confirmed as "hunter22", then set it properly.
	stop := feed(t, "1", "player1", "huntre22", "hunter22", "hunter22", "hunter22")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if len(p.pwChanges) != 1 || p.pwChanges[0] != "hunter22" {
		t.Fatalf("password not corrected: %+v", p.pwChanges)
	}
	if !strings.Contains(out, "Password set.") {
		t.Fatalf("no confirmation that the password was set: %q", out)
	}
}

func TestLoginThatCreatesLeavesPasswordWhenCancelled(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	// Confirmation differs, then the player presses Enter instead of setting a new one.
	stop := feed(t, "1", "player1", "huntre22", "hunter22", "")
	if err := login(false); err != nil {
		t.Fatal(err)
	}
	out := stop()
	if len(p.pwChanges) != 0 {
		t.Fatalf("cancelling must change nothing: %+v", p.pwChanges)
	}
	if !strings.Contains(out, "Leaving it as the first password you typed") {
		t.Fatalf("player not told what their password is: %q", out)
	}
}

// ---------- changing a password ----------

func TestChangePasswordStillDoubleEntry(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	settings.Token = "tok"
	stop := feed(t, "newpass1", "newpass1")
	changePassword(stdin)
	out := stop()
	if len(p.pwChanges) != 1 || p.pwChanges[0] != "newpass1" {
		t.Fatalf("portal saw %+v", p.pwChanges)
	}
	if !strings.Contains(out, "Password changed.") {
		t.Fatalf("no confirmation: %q", out)
	}
}

func TestChangePasswordCancelsOnEmpty(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	settings.Token = "tok"
	stop := feed(t, "", "unused")
	changePassword(stdin)
	out := stop()
	if len(p.pwChanges) != 0 {
		t.Fatalf("cancelling changed the password: %+v", p.pwChanges)
	}
	if !strings.Contains(out, "Password not changed.") {
		t.Fatalf("no cancellation message: %q", out)
	}
}

func TestChangePasswordMismatchRetries(t *testing.T) {
	isolateSettings(t)
	p := newFakePortal(t, nil)
	settings.Token = "tok"
	stop := feed(t, "newpass1", "typo", "newpass1", "newpass1")
	changePassword(stdin)
	stop()
	if len(p.pwChanges) != 1 || p.pwChanges[0] != "newpass1" {
		t.Fatalf("portal saw %+v", p.pwChanges)
	}
}
