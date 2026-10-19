// pendingsave.go — 0.9.5: never download over a character that has not been saved online yet.
//
// If uploads are refused during a session, the launcher leaves a marker to prevent subsequent runs
// from downloading the online save and overwriting the newer local progress on disk.
//
// So when the uploads at the end of a session all fail, the launcher now leaves a marker. On the next run,
// if the online save is still the one we last knew about, the local character is the newer of the two and it
// is uploaded instead of being replaced. If the online save has moved on in the meantime - which means
// another PC uploaded - the two really have diverged, so the local copy is zipped aside before the download
// and the player is told exactly where it is.
//
// The marker is cleared only when the save is safely online, or when the divergence has been dealt with.
package main

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type pendingSave struct {
	Account   string    `json:"account"`
	ServerRev int       `json:"server_rev"` // the online revision we last knew about when the upload failed
	When      time.Time `json:"when"`
	Reason    string    `json:"reason"`
}

// pendingAction is what the next launcher run should do about an unsaved character.
type pendingAction int

const (
	pendingNone     pendingAction = iota // nothing outstanding: download the online save as usual
	pendingUpload                        // the local character is the newer one: upload it, do not download
	pendingConflict                      // another PC uploaded since: keep the local copy aside, then download
)

// pendingDecision is the whole rule, kept separate from the file and network handling so it can be tested.
//
// A marker left by a different account is not ours to act on and must survive: one player's unsaved
// character must not be discarded because somebody else logged in on the same PC.
func pendingDecision(p *pendingSave, account string, serverRev int) pendingAction {
	switch {
	case p == nil || p.Account == "":
		return pendingNone
	case !strings.EqualFold(p.Account, account):
		return pendingNone
	case p.ServerRev == serverRev:
		return pendingUpload
	default:
		return pendingConflict
	}
}

func backupsDir() string { return filepath.Join(gameDir, "MMOProfile", "_local_backups") }

func pendingPath() string { return filepath.Join(backupsDir(), "pending-save.json") }

func loadPending() *pendingSave {
	b, err := os.ReadFile(pendingPath())
	if err != nil {
		return nil
	}
	var p pendingSave
	if err := json.Unmarshal(b, &p); err != nil || p.Account == "" {
		return nil
	}
	return &p
}

func savePending(account string, serverRev int, reason string) {
	if err := os.MkdirAll(backupsDir(), 0o755); err != nil {
		return
	}
	b, err := json.MarshalIndent(pendingSave{
		Account: account, ServerRev: serverRev, When: time.Now().UTC(), Reason: reason}, "", "  ")
	if err != nil {
		return
	}
	_ = os.WriteFile(pendingPath(), b, 0o644)
}

func clearPending() { _ = os.Remove(pendingPath()) }

// keepLocalCopy zips the profile into _local_backups under the given prefix and returns the path it wrote.
func keepLocalCopy(profile, prefix, account string) (string, error) {
	if err := os.MkdirAll(backupsDir(), 0o755); err != nil {
		return "", err
	}
	z, err := zipProfile(profile)
	if err != nil {
		return "", err
	}
	// An empty profile still zips to a valid archive of about 22 bytes, so the length proves nothing. Count
	// the entries: a copy with no files in it would be mistaken later for a character that was saved.
	zr, err := zip.NewReader(bytes.NewReader(z), int64(len(z)))
	if err != nil {
		return "", err
	}
	if len(zr.File) == 0 {
		return "", errors.New("there are no character files in the profile to copy")
	}
	path := filepath.Join(backupsDir(), fmt.Sprintf("%s_%s_%s.zip", prefix, account, time.Now().Format("20060102-150405")))
	if err := os.WriteFile(path, z, 0o644); err != nil {
		return "", err
	}
	return path, nil
}

// syncProfile decides between uploading what is on this PC and downloading what is online, and is what the
// launcher calls in place of going straight to downloadSave.
func syncProfile(profile, session string, serverRev int) error {
	switch pendingDecision(loadPending(), settings.Account, serverRev) {

	case pendingUpload:
		say("")
		say("Your last session could not be saved online, so your character here is the newer one.")
		say("Uploading it now. Nothing on this PC will be overwritten.")
		if err := uploadSave(profile, session); err != nil {
			say("")
			say("It still could not be uploaded: %v", err)
			say("Your character on this PC has been left exactly as it is, and the game will start with it.")
			say("It will go online by itself the next time the server can be reached.")
			say("")
			return nil
		}
		clearPending()
		say("Saved online. You are up to date.")
		return nil

	case pendingConflict:
		copyPath, err := keepLocalCopy(profile, "unsaved", settings.Account)
		if err != nil {
			// Refusing to continue is the safe answer: the download would overwrite a character we could not
			// first copy, which is the exact loss this file exists to prevent.
			return fmt.Errorf("your character on this PC has not been saved online and a copy could not be made (%v), so nothing has been changed. Please ask for help before playing again", err)
		}
		clearPending()
		say("")
		say("Your character was played on another PC since this one last saved online, so the two differ.")
		say("The online one is being used, and this PC's copy has been kept here:")
		say("  %s", copyPath)
		say("")
		return downloadSave(profile, serverRev)

	default:
		return downloadSave(profile, serverRev)
	}
}
