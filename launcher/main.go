// SiegeLauncher — friend-side launcher: login, download online save, start Dungeon Siege with SiegeHost
// auto-join, upload the save while playing and after exit. Lives in the game folder next to DSLOA.exe.
// Docs: 04-server/SiegeLauncher/README.md
package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"time"
)

const version = "0.9.6"

type Settings struct {
	PortalURL   string `json:"portal_url"` // e.g. http://my-server.example.com:8440
	Account     string `json:"account"`
	Token       string `json:"token"`
	ExtraArgs   string `json:"extra_args"` // extra DSLOA.exe arguments, e.g. "fullscreen=true"
	Width       int    `json:"width"`
	Height      int    `json:"height"`
	Fullscreen  bool   `json:"fullscreen"`   // old (<= 0.6.0), ignored: see Display
	Display     string `json:"display"`      // "fullscreen" (default), "window" (Width x Height), or 0.7.5 "game" = pass nothing, let DSLOA decide
	Resolution  string `json:"resolution"`   // 0.7.2: "" / "auto" = the desktop resolution in full screen; otherwise "WxH", e.g. "2560x1080"
	SyncMinutes int    `json:"sync_minutes"` // upload interval while playing
	ServerID    string `json:"server_id"`    // last chosen game server (region), e.g. "na" / "eu"
}

var (
	gameDir   string
	logFile   *os.File
	settings  Settings
	client    = &http.Client{Timeout: 60 * time.Second}
	serverArg string
	stdin     = bufio.NewReader(os.Stdin) // one shared reader: separate readers swallow each other's buffered input
)

func main() {
	exe, _ := os.Executable()
	gameDir = filepath.Dir(exe)
	for i := 1; i < len(os.Args)-1; i++ {
		switch os.Args[i] {
		case "--dir":
			gameDir = os.Args[i+1]
		case "--server":
			serverArg = os.Args[i+1]
		}
	}
	_ = os.MkdirAll(filepath.Join(gameDir, "SiegeHostLogs"), 0o755)
	logFile, _ = os.OpenFile(filepath.Join(gameDir, "SiegeHostLogs", "launcher.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	say("==== SiegeLauncher %s ====", version)
	if err := run(); err != nil {
		say("")
		say("ERROR: %v", err)
		say("(details in SiegeHostLogs\\launcher.log — send that file to the server admin)")
		pause()
		os.Exit(1)
	}
}

func say(f string, a ...any) {
	msg := fmt.Sprintf(f, a...)
	fmt.Println(msg)
	if logFile != nil {
		fmt.Fprintf(logFile, "%s %s\n", time.Now().Format("2006-01-02 15:04:05"), msg)
	}
}

func logf(f string, a ...any) {
	if logFile != nil {
		fmt.Fprintf(logFile, "%s %s\n", time.Now().Format("2006-01-02 15:04:05"), fmt.Sprintf(f, a...))
	}
}

func pause() {
	fmt.Print("\nPress Enter to close...")
	stdin.ReadString('\n')
}

func settingsPath() string { return filepath.Join(gameDir, "launcher.json") }

// The portal this launcher belongs to. Only used when launcher.json is missing or unreadable, so
// that a player whose settings file is damaged is not stranded at a dead end with nobody to ask.
const defaultPortalURL = "http://72.60.164.124:8440"

// 0.8.7. launcher.json turned into 330 bytes of zeros after the game was force-closed, and
// the launcher answered with "launcher.json is invalid" and a "send that file to the server admin"
// that no ordinary player can act on. A launcher that can be bricked by its own settings file is not
// shippable: anything wrong with that file is now repaired here and the launch continues.
//
// A file of NUL bytes is what Windows leaves behind when a file is extended and the data never
// reaches the disk, which a hard kill or a power cut during a save will do. saveSettings below no
// longer writes in a way that can produce one.
func loadSettings() error {
	settings = Settings{Width: 1280, Height: 720, SyncMinutes: 2}
	b, err := os.ReadFile(settingsPath())
	switch {
	case err != nil:
		logf("launcher.json is missing, writing a fresh one.")
		return recoverSettings(nil)
	case isBlank(b):
		logf("launcher.json is empty or damaged (%d bytes, no readable content), writing a fresh one.", len(b))
		return recoverSettings(b)
	}
	if err := json.Unmarshal(b, &settings); err != nil {
		logf("launcher.json could not be read (%v), writing a fresh one.", err)
		return recoverSettings(b)
	}
	if settings.PortalURL == "" {
		logf("launcher.json has no portal_url, filling in the default.")
		settings.PortalURL = defaultPortalURL
		saveSettings()
	}
	settings.PortalURL = strings.TrimRight(settings.PortalURL, "/")
	if settings.SyncMinutes <= 0 {
		settings.SyncMinutes = 2
	}
	// Anyone left on a display mode an older launcher offered is moved to full screen once, quietly.
	if strings.EqualFold(settings.Display, "window") || strings.EqualFold(settings.Display, "game") {
		was := settings.Display
		settings.Display = "fullscreen"
		saveSettings()
		logf("display: %q is no longer offered, this install has been moved to full screen.", was)
	}
	return nil
}

// isBlank reports whether the file holds nothing a settings file could ever hold: no bytes at all,
// or nothing but NULs and whitespace.
func isBlank(b []byte) bool {
	for _, c := range b {
		if c != 0 && c != ' ' && c != '\t' && c != '\r' && c != '\n' {
			return false
		}
	}
	return true
}

// recoverSettings keeps the damaged file for anyone who wants to look at it later, then writes a
// working one. The player logs in again, which is the whole cost of this happening.
func recoverSettings(bad []byte) error {
	if len(bad) > 0 {
		kept := settingsPath() + ".bad"
		if err := os.WriteFile(kept, bad, 0o644); err == nil {
			logf("the old launcher.json was kept as %s", filepath.Base(kept))
		}
	}
	settings = Settings{
		PortalURL:   defaultPortalURL,
		Width:       1280,
		Height:      720,
		SyncMinutes: 2,
		Display:     "fullscreen",
	}
	saveSettings()
	b, err := os.ReadFile(settingsPath())
	if err != nil || isBlank(b) {
		return fmt.Errorf("could not write a new launcher.json in %s. Check the folder is not read-only.", gameDir)
	}
	say("Your launcher settings were damaged and have been reset. You will be asked to sign in again.")
	logf("settings reset. The player will be asked to sign in again.")
	return nil
}

// saveSettings writes through a temporary file in the same folder and renames it into place, so a
// crash, a kill or a power cut can never leave a half-written or zero-filled launcher.json behind.
// The old behaviour truncated the real file first, which could leave the file empty.
func saveSettings() {
	b, err := json.MarshalIndent(settings, "", "  ")
	if err != nil {
		return
	}
	tmp := settingsPath() + ".tmp"
	f, err := os.OpenFile(tmp, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return
	}
	if _, err = f.Write(b); err == nil {
		err = f.Sync()
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		_ = os.Remove(tmp)
		return
	}
	if err := os.Rename(tmp, settingsPath()); err != nil {
		_ = os.Remove(tmp)
	}
}

func run() error {
	if _, err := os.Stat(filepath.Join(gameDir, "DSLOA.exe")); err != nil {
		return fmt.Errorf("DSLOA.exe not found. Extract the zip INTO your Dungeon Siege folder (next to DSLOA.exe). Looked in: %s", gameDir)
	}
	if err := loadSettings(); err != nil {
		return err
	}

	// 0. Auto-update (launcher, SiegeHost, required game mods) from the central server
	if err := autoUpdate(); err != nil {
		return err
	}

	// 1. Server status + region choice
	var status struct {
		ServerName  string            `json:"server_name"`
		GameAddress string            `json:"game_address"`
		GamePort    int               `json:"game_port"`
		MaxPlayers  int               `json:"max_players"`
		Invite      bool              `json:"invite_required"`
		Game        map[string]string `json:"game"`
		Servers     []struct {
			ID          string            `json:"id"`
			Name        string            `json:"name"`
			Region      string            `json:"region"`
			GameAddress string            `json:"game_address"`
			GamePort    int               `json:"game_port"`
			MaxPlayers  int               `json:"max_players"`
			Online      bool              `json:"online"`
			Players     string            `json:"players"`
			Game        map[string]string `json:"game"`
			OnDemand    bool              `json:"on_demand"`
			Instance    string            `json:"instance_state"`
			SendFreq    int               `json:"send_frequency"`
			NeedCode    bool              `json:"join_code_required"` // 0.9.3: ask for a code before starting the session
			VetLevel    int               `json:"veteran_level"`      // 0.9.3: the level a veteran world asks for on this shard
			HostChar    string            `json:"host_character"`     // 0.9.4: this shard's idle host character, so it can be left out of the party panel
		} `json:"servers"`
	}
	if err := getJSON("/api/status", "", &status); err != nil {
		return fmt.Errorf("cannot reach the server at %s: %v", settings.PortalURL, err)
	}
	onDemand, onDemandOnline := false, false
	sendFreq := 0
	needCode := false
	veteranLevel := 0
	hostChar := ""
	if len(status.Servers) > 0 {
		pick := -1
		want := serverArg
		if want == "" && len(status.Servers) == 1 {
			want = status.Servers[0].ID
		}
		for i, sv := range status.Servers {
			if strings.EqualFold(sv.ID, want) {
				pick = i
			}
		}
		if pick < 0 {
			fmt.Println()
			fmt.Println("Choose a server:")
			width := 0
			for _, sv := range status.Servers {
				if len(sv.Name) > width {
					width = len(sv.Name)
				}
			}
			for i, sv := range status.Servers {
				state := "Offline"
				if sv.OnDemand && !sv.Online {
					state = "Sleeping - starts in about a minute"
				}
				if sv.Online {
					players := sv.Players
					if players == "" {
						players = "0"
					}
					if sv.MaxPlayers > 0 {
						players = fmt.Sprintf("%s/%d", players, sv.MaxPlayers)
					}
					state = "Online - " + players + " players"
				}
				last := ""
				if strings.EqualFold(sv.ID, settings.ServerID) {
					last = "   <- last time"
				}
				fmt.Printf("  %d) %-*s  [%s]%s\n", i+1, width, sv.Name, state, last)
			}
			in := stdin
			for pick < 0 {
				fmt.Print("Number (Enter = last time): ")
				line, _ := in.ReadString('\n')
				line = strings.TrimSpace(line)
				if line == "" {
					for i, sv := range status.Servers {
						if strings.EqualFold(sv.ID, settings.ServerID) {
							pick = i
						}
					}
					continue
				}
				var n int
				if _, err := fmt.Sscan(line, &n); err == nil && n >= 1 && n <= len(status.Servers) {
					pick = n - 1
				}
			}
		}
		sv := status.Servers[pick]
		sendFreq = sv.SendFreq
		needCode, veteranLevel, hostChar = sv.NeedCode, sv.VetLevel, sv.HostChar
		onDemand, onDemandOnline = sv.OnDemand, sv.Online
		settings.ServerID = sv.ID
		saveSettings()
		status.ServerName, status.GameAddress, status.Game, status.GamePort = sv.Name, sv.GameAddress, sv.Game, sv.GamePort
		if sv.MaxPlayers > 0 {
			status.MaxPlayers = sv.MaxPlayers
		}
	}
	say("Server: %s  (game %s, online=%s, players=%s/%d)", status.ServerName, status.GameAddress, status.Game["online"],
		orDash(status.Game["players"]), status.MaxPlayers)
	if status.Game["online"] == "false" && !onDemand {
		say("WARNING: the game server is not in-game right now (it may be restarting). Trying anyway.")
	}

	// 2. DirectPlay
	if err := ensureDirectPlay(); err != nil {
		return err
	}

	// 3. Login (token cached; password never stored)
	if err := login(status.Invite); err != nil {
		return err
	}
	if err := accountMenu(status.Invite); err != nil {
		return err
	}

	// 3b. On-demand server (EU on AWS): ask the central server to start it and wait until it is in game.
	if onDemand && !onDemandOnline {
		addr, port, maxp, game, err := wakeAndWait(settings.ServerID)
		if err != nil {
			return err
		}
		status.GameAddress, status.GamePort, status.Game = addr, port, game
		if maxp > 0 {
			status.MaxPlayers = maxp
		}
	}

	// 4. Session lease + download save
	host, _ := os.Hostname()
	var sess struct {
		Session string `json:"session"`
		Lease   int    `json:"lease_seconds"`
		SaveRev int    `json:"save_rev"`
		Ticket  string `json:"ticket"` // 0.6.0 join ticket: the game server only lets in players who have one
	}
	// 0.9.3: some shards are not open to everyone and ask for a code first. A wrong code costs nothing,
	// because the portal refuses before it touches the lease, so it is worth retrying rather than making
	// the player start the launcher again.
	joinCode := ""
	for attempt := 0; ; attempt++ {
		if needCode {
			fmt.Printf("Join code for %s: ", status.ServerName)
			line, _ := stdin.ReadString('\n')
			joinCode = strings.TrimSpace(line)
			if joinCode == "" {
				return fmt.Errorf("%s needs a join code", status.ServerName)
			}
		}
		err := postJSON("/api/session/start", settings.Token,
			map[string]any{"Machine": host, "Server": settings.ServerID, "Code": joinCode}, &sess)
		if err == nil {
			break
		}
		var ae *apiError
		if needCode && errors.As(err, &ae) && ae.Code == 403 && attempt < 2 {
			say("%s", ae.Msg)
			continue
		}
		return err
	}
	_ = os.MkdirAll(filepath.Join(gameDir, "MMOProfile", settings.Account), 0o755) // DSLOA needs user_path to exist
	profile := spacelessPath(filepath.Join(gameDir, "MMOProfile", settings.Account), settings.Account)
	if strings.Contains(profile, " ") {
		return fmt.Errorf("could not find a profile folder without spaces (the game can't use it): %s", profile)
	}
	lastServerRev = sess.SaveRev
	if err := syncProfile(profile, sess.Session, sess.SaveRev); err != nil {
		endSession(sess.Session)
		return err
	}
	seedDisplayConfig(profile)

	// 5. Configure SiegeHost as client with auto-join
	gameAddr := status.GameAddress
	if isLocalPortal() { // playing on the server PC itself: routers often can't loop back to their own public IP
		gameAddr = "127.0.0.1"
	}
	ip, err := resolveIPv4(gameAddr)
	if err != nil {
		endSession(sess.Session)
		return fmt.Errorf("cannot resolve game address %q: %v", status.GameAddress, err)
	}
	// search the host's fixed port directly; a reply routed via dpnsvr (6073) comes from another port and home NATs drop it.
	joinPort := status.GamePort
	if joinPort == 0 {
		joinPort = 2350
	} else if joinPort < 0 {
		joinPort = 0
	}
	iniBase := fmt.Sprintf("[SiegeHost]\r\nEnabled=1\r\nRole=client\r\nMaxPlayers=%d\r\nLogDirectPlay=1\r\n[AutoJoin]\r\nAddress=%s\r\nPort=%d\r\nPlayerName=%s\r\nRetries=20\r\n",
		clamp(status.MaxPlayers, 0, 30), ip, joinPort, settings.Account)
	// 0.7.0: the server decides the engine multiplayer send rate; host and client must match.
	if sendFreq > 0 {
		iniBase += fmt.Sprintf("[Net]\r\nSendFrequency=%d\r\n", clamp(sendFreq, 1, 60))
	}
	// 0.9.3: the level a veteran world asks for is decided by the shard, not the map, and the check that
	// refuses a character runs on this PC, so SiegeHost here needs the same number the shard uses.
	if veteranLevel > 0 {
		iniBase += fmt.Sprintf("[World]\r\nVeteranRequiredLevel=%d\r\n", clamp(veteranLevel, 1, 200))
	}
	// 0.9.4: a shard's idle host character is kept out of the party panel by name, and that filter runs
	// here rather than on the server, so this PC has to be told which name. Without it SiegeHost falls
	// back to "ServerHost" and any shard hosting under another name shows its host to everyone.
	// AutoHost itself never arms on a client, so the section is only read, never acted on.
	if hostChar != "" && !strings.ContainsAny(hostChar, "\r\n") {
		iniBase += fmt.Sprintf("[AutoHost]\r\nHostCharacter=%s\r\n", hostChar)
	}
	writeIni := func(ticket string) error {
		return os.WriteFile(filepath.Join(gameDir, "SiegeHost.ini"), []byte(iniBase+"[JoinGuard]\r\nTicket="+ticket+"\r\n"), 0o644)
	}
	if err := writeIni(sess.Ticket); err != nil {
		endSession(sess.Session)
		return fmt.Errorf("cannot write SiegeHost.ini (is the game folder read-only?): %v", err)
	}
	refusedFile := filepath.Join(gameDir, "SiegeHostLogs", "join_refused.txt")
	_ = os.Remove(refusedFile)
	exe, _ := os.Executable()
	if msg, err := installSiegeHost(gameDir, filepath.Join(filepath.Dir(exe), "SiegeHost-Mss32.dll")); err != nil {
		endSession(sess.Session)
		return fmt.Errorf("could not install SiegeHost: %v", err)
	} else {
		say("SiegeHost: %s", msg)
	}

	// 6. Launch
	dargs, dw, dh := displayArgs()
	if dw > 0 && dh > 0 {
		syncIniResolution(profile, dw, dh) // the ini and the command line must agree
	}
	args := append([]string{"nointro=true", "zonematch=true", "multi=true", "user_path=" + profile}, dargs...)
	if settings.ExtraArgs != "" {
		args = append(args, strings.Fields(settings.ExtraArgs)...)
	}
	say("Starting Dungeon Siege (auto-connecting to %s as %s)...", ip, settings.Account)
	cmd := exec.Command(filepath.Join(gameDir, "DSLOA.exe"), args...)
	cmd.Dir = gameDir
	started := time.Now()
	if err := cmd.Start(); err != nil {
		endSession(sess.Session)
		return fmt.Errorf("could not start DSLOA.exe: %v", err)
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()

	say("Playing. Keep this window open: your character is backed up online every %d minutes and when the game closes.", settings.SyncMinutes)
	saveDir := filepath.Join(profile, "Save")
	lastHash := hashDir(saveDir)
	renew := time.NewTicker(time.Duration(max(30, sess.Lease/3)) * time.Second)
	sync := time.NewTicker(time.Duration(settings.SyncMinutes) * time.Minute)
	defer renew.Stop()
	defer sync.Stop()
	for {
		select {
		case <-renew.C:
			var rn struct {
				Ticket  string `json:"ticket"`
				Refused string `json:"refused"`
			}
			if err := postJSON("/api/session/renew?session="+sess.Session, settings.Token, nil, &rn); err != nil {
				say("WARNING: session renew failed: %v", err)
			} else {
				if rn.Ticket != "" && rn.Ticket != sess.Ticket {
					sess.Ticket = rn.Ticket // fresh join pass (it expires), so rejoining from the game menu keeps working
					_ = writeIni(sess.Ticket)
				}
				if rn.Refused != "" {
					showRefused(rn.Refused)
				}
			}
		case <-sync.C:
			if h := hashDir(saveDir); h != lastHash {
				if err := uploadSave(profile, sess.Session); err != nil {
					say("WARNING: online backup failed (will retry): %v", err)
				} else {
					lastHash = h
				}
			}
		case err := <-done:
			say("Game closed (%v). Uploading your character...", errOrOK(err))
			if failedToStart(err, time.Since(started)) {
				say("")
				say("Dungeon Siege stopped with its own error dialog moments after starting.")
				say("That is nearly always the resolution, not your character or the server.")
				say("Press A for account options and try 4) Screen resolution - pick a smaller one,")
				say("such as 1024x768.")
				say("")
			}
			_ = writeIni("") // the join pass is only for this launcher session; DSLOA.exe started directly later gets no pass
			if b, rerr := os.ReadFile(refusedFile); rerr == nil && len(strings.TrimSpace(string(b))) > 0 {
				showRefused(strings.TrimSpace(string(b)))
				_ = os.Remove(refusedFile)
			}
			time.Sleep(2 * time.Second) // let the game finish flushing files
			var upErr error
			for i := 0; i < 5; i++ {
				if upErr = uploadSave(profile, sess.Session); upErr == nil {
					break
				}
				say("  upload attempt %d failed: %v", i+1, upErr)
				time.Sleep(5 * time.Second)
			}
			var rn struct {
				Refused string `json:"refused"`
			}
			if postJSON("/api/session/renew?session="+sess.Session, settings.Token, nil, &rn) == nil && rn.Refused != "" {
				showRefused(rn.Refused)
			}
			endSession(sess.Session)
			if upErr != nil {
				// The next run must not download over this character. The marker is what stops it.
				savePending(settings.Account, lastServerRev, upErr.Error())
				say("")
				say("Your character could NOT be saved online, so it is being kept on this PC.")
				say("Start the launcher again when you can and it will be uploaded before anything else.")
				say("Until then, do not delete anything in MMOProfile.")
				say("")
				return fmt.Errorf("your save could NOT be uploaded (it is still safe on this PC in %s): %v", profile, upErr)
			}
			clearPending()
			say("All saved online. See you next time!")
			time.Sleep(3 * time.Second)
			return nil
		}
	}
}

// 0.8.8: window mode is gone. Every report of Dungeon Siege failing to start or drawing a corrupted
// screen has come from running it in a window; nobody has had trouble in full screen. The setting is
// no longer offered, and a saved "window" is migrated to full screen in loadSettings. "game" (let
// Dungeon Siege decide) stays, because that is the escape hatch when the game will not start at all.
func windowMode() bool { return false }

// gameDecides (0.7.5): pass no display arguments at all and let Dungeon Siege use whatever is in
// its own DungeonSiege.ini — i.e. start exactly as it does when the player launches it themselves.
// The escape hatch for machines where our forced display mode does not work.
// 0.8.10: the "let the game choose" option is gone too. 0.8.9 had already stopped it producing a
// window, but a second display option earning its keep on a server where full screen is the only
// supported mode was never a strong case. The resolution is the one thing still worth changing, and
// it is the lever that actually helps when Dungeon Siege will not start. A saved "game" is migrated
// to full screen in loadSettings, the same way "window" is.
func gameDecides() bool { return false }

// seedDisplayConfig gives a brand-new MMO profile the display settings the player already has
// working, by copying them out of their existing DungeonSiege.ini.
//
// Why (0.7.4): the launcher starts the game with user_path=MMOProfile\<account>, and Dungeon Siege
// keeps its D3D device settings — driver_description, width, height, bpp — in DungeonSiege.ini
// *inside* that profile. A fresh profile has none, so the game picks a device on its own. On a
// machine where the working setup needs something specific (16-bit, a particular driver, a wrapper
// like dgVoodoo or DDrawCompat) that guess fails and the player gets "D3D Initialization Failure:
// Dungeon Siege was unable to properly create a 3D graphics device" — while the same game started
// outside the launcher, on their normal profile, runs fine. Profile loading verification.
//
// Only the keys above the first [section] are copied: those are the device settings. The
// [multiplayer] block is deliberately left alone, because the source file may point at matchmaking
// servers we do not use and we do not want the game contacting them.
//
// Does nothing when the profile already has a DungeonSiege.ini (after the first successful run that
// file is the player's own MMO display config, and theirs wins), and nothing when no source exists.
func seedDisplayConfig(profile string) {
	dst := filepath.Join(profile, "DungeonSiege.ini")
	if _, err := os.Stat(dst); err == nil {
		return // the player already has one here - never overwrite it
	}
	var candidates []string
	candidates = append(candidates, filepath.Join(gameDir, "DungeonSiege.ini"))
	if home, err := os.UserHomeDir(); err == nil {
		candidates = append(candidates,
			filepath.Join(home, "Documents", "Dungeon Siege", "DungeonSiege.ini"),
			filepath.Join(home, "My Documents", "Dungeon Siege", "DungeonSiege.ini"))
	}
	for _, src := range candidates {
		if src == dst {
			continue
		}
		b, err := os.ReadFile(src)
		if err != nil {
			continue
		}
		head, keys := deviceSettings(string(b))
		if keys == 0 {
			continue // a file with no device keys teaches us nothing
		}
		if err := os.WriteFile(dst, []byte(head), 0o644); err != nil {
			logf("display: could not seed %s: %v", dst, err)
			return
		}
		say("Copied your existing display settings into this profile (%d setting(s) from %s).", keys, src)
		return
	}
	logf("display: no existing DungeonSiege.ini found to seed from; the game will choose its own device")
}

// Keys deliberately NOT seeded from another profile (0.7.5, ): the launcher decides the
// resolution and passes it on DSLOA.exe's command line, so a width/height copied out of some other
// profile can only ever disagree with what we are about to pass. When it does, the game renders at
// the ini's size and presents at the command line's, and the player sees the top-left corner of a
// too-large frame. x/y are a saved window position, equally none of our business.
var notSeeded = map[string]bool{"width": true, "height": true, "x": true, "y": true}

// deviceSettings returns the lines of a DungeonSiege.ini above the first [section] — the display
// device block — minus the keys in notSeeded, plus how many key=value lines survived.
func deviceSettings(s string) (string, int) {
	var out []string
	keys := 0
	for _, line := range strings.Split(s, "\n") {
		t := strings.TrimSpace(strings.TrimSuffix(line, "\r"))
		if strings.HasPrefix(t, "[") {
			break // first section header ends the device block
		}
		if t == "" {
			continue
		}
		if k, _, ok := strings.Cut(t, "="); ok {
			if notSeeded[strings.ToLower(strings.TrimSpace(k))] {
				continue
			}
			keys++
		}
		out = append(out, t)
	}
	if keys == 0 {
		return "", 0
	}
	return strings.Join(out, "\r\n") + "\r\n", keys
}

// syncIniResolution writes w x h into the profile's DungeonSiege.ini so it agrees with what we are
// about to pass on the command line. Without this the two can drift apart. Called only
// when the launcher is actually setting a resolution; in "let the game decide" mode the ini is the
// only opinion there is and we must not touch it.
func syncIniResolution(profile string, w, h int) {
	p := filepath.Join(profile, "DungeonSiege.ini")
	b, err := os.ReadFile(p)
	if err != nil {
		return // nothing there yet; the game will write its own and it will match what we passed
	}
	var out []string
	seenW, seenH, done := false, false, false
	for _, line := range strings.Split(string(b), "\n") {
		t := strings.TrimSuffix(line, "\r")
		trimmed := strings.TrimSpace(t)
		if !done && strings.HasPrefix(trimmed, "[") {
			// end of the device block: add whichever key was missing before the first section
			if !seenW {
				out = append(out, fmt.Sprintf("width = %d", w))
			}
			if !seenH {
				out = append(out, fmt.Sprintf("height = %d", h))
			}
			done = true
		}
		if !done {
			if k, _, ok := strings.Cut(trimmed, "="); ok {
				switch strings.ToLower(strings.TrimSpace(k)) {
				case "width":
					out, seenW = append(out, fmt.Sprintf("width = %d", w)), true
					continue
				case "height":
					out, seenH = append(out, fmt.Sprintf("height = %d", h)), true
					continue
				}
			}
		}
		out = append(out, t)
	}
	if !done { // a file with no sections at all
		if !seenW {
			out = append(out, fmt.Sprintf("width = %d", w))
		}
		if !seenH {
			out = append(out, fmt.Sprintf("height = %d", h))
		}
	}
	if err := os.WriteFile(p, []byte(strings.Join(out, "\r\n")), 0o644); err != nil {
		logf("display: could not sync %s to %dx%d: %v", p, w, h, err)
		return
	}
	logf("display: profile DungeonSiege.ini synced to %dx%d", w, h)
}

// pickDesktopResolution chooses between the size a DPI-unaware program is told (virtual) and the
// panel's real size (physical). The virtual one wins whenever it is sane, because that is what
// Dungeon Siege will see; the physical one is only a fallback for when the call fails or returns
// something absurd. Pure and platform-free so it can be tested without Windows.
func pickDesktopResolution(vw, vh, pw, ph int) (int, int) {
	sane := func(w, h int) bool { return w >= 640 && h >= 480 && w <= 16384 && h <= 16384 }
	if sane(vw, vh) {
		return vw, vh
	}
	if sane(pw, ph) {
		return pw, ph
	}
	return 0, 0
}

// chosenResolution returns the player's [Net]-independent screen choice from settings.Resolution.
// ok is false for "" / "auto" / anything unparseable, which means "use the desktop resolution".
func chosenResolution() (w, h int, ok bool) {
	r := strings.ToLower(strings.TrimSpace(settings.Resolution))
	if r == "" || r == "auto" {
		return 0, 0, false
	}
	if _, err := fmt.Sscanf(r, "%dx%d", &w, &h); err != nil || w < 640 || h < 480 || w > 7680 || h > 4320 {
		logf("display: ignoring unusable resolution %q, using the desktop resolution", settings.Resolution)
		return 0, 0, false
	}
	return w, h, true
}

// displayArgs picks the resolution DSLOA.exe starts at.
//
// 0.7.2: the player's own choice (A -> 5) now wins. Before this, full screen ALWAYS forced the
// desktop resolution on every launch, so a player on a large monitor could not get a bigger HUD -
// whatever they set in the game's own Options menu was overwritten the next time they started.
// The engine only refuses a resolution larger than the desktop in WINDOWED mode ("You cannot run
// %S in a resolution (%dx%d) higher than your desktop"), so full screen at a lower resolution is
// fine and is exactly what makes the 2002 fixed-size interface readable on a big screen.
// displayArgs returns the display arguments for DSLOA.exe plus the resolution they set
// (0,0 when we are setting none, so the caller knows not to touch the profile ini).
func displayArgs() ([]string, int, int) {
	// 0.8.9: this used to pass no display arguments at all, which left the game to whatever its own
	// DungeonSiege.ini said - and an ini carried over from an older install can say fullscreen=false.
	// That put people back in the window mode 0.8.8 removed, through the one option that was supposed
	// to be the safe fallback. It now insists on full screen and only gives up the resolution, which
	// is the part a machine can actually refuse.
	if gameDecides() {
		logf("display: full screen, resolution left to the game (no width or height passed)")
		return []string{"fullscreen=true"}, 0, 0
	}
	w, h, chosen := chosenResolution()
	if windowMode() {
		if !chosen {
			w, h = settings.Width, settings.Height
		}
		if dw, dh := desktopResolution(); dw > 0 && dh > 0 && (w > dw || h > dh) {
			logf("display: %dx%d is larger than the desktop %dx%d - a window cannot be bigger, using the desktop size", w, h, dw, dh)
			w, h = dw, dh
		}
		logf("display: window %dx%d", w, h)
		return []string{"fullscreen=false", fmt.Sprintf("width=%d", w), fmt.Sprintf("height=%d", h)}, w, h
	}
	if !chosen {
		w, h = desktopResolution()
		if w < 800 || h < 600 {
			w, h = 1920, 1080
		}
		logf("display: full screen %dx%d (desktop)", w, h)
	} else {
		logf("display: full screen %dx%d (chosen)", w, h)
	}
	return []string{"fullscreen=true", fmt.Sprintf("width=%d", w), fmt.Sprintf("height=%d", h)}, w, h
}

// ---------- screen resolution (0.7.2) ----------

// Dungeon Siege draws its interface at a fixed pixel size, so the bigger the resolution the smaller
// the HUD looks. Running full screen at a LOWER resolution than the desktop is the standard cure and
// the engine allows it (only windowed mode is capped at the desktop size). Keeping the same shape as
// the monitor matters: picking 1920x1080 on a 21:9 screen gives black bars down the sides, while
// 2560x1080 stays full width.
var commonResolutions = [][2]int{
	{1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1600, 900}, {1680, 1050},
	{1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160},
}

func resolutionLabel() string {
	if w, h, ok := chosenResolution(); ok {
		return fmt.Sprintf("%dx%d", w, h)
	}
	if w, h := desktopResolution(); w > 0 && h > 0 {
		return fmt.Sprintf("automatic (your desktop, %dx%d)", w, h)
	}
	return "automatic (your desktop)"
}

// sameShape reports whether w:h is within ~2% of the desktop's aspect ratio.
func sameShape(w, h, dw, dh int) bool {
	if dw <= 0 || dh <= 0 {
		return false
	}
	a, b := float64(w)/float64(h), float64(dw)/float64(dh)
	d := a - b
	if d < 0 {
		d = -d
	}
	return d/b < 0.02
}

func chooseResolution(in *bufio.Reader) {
	dw, dh := desktopResolution()
	fmt.Println()
	fmt.Println("Screen resolution")
	if dw > 0 && dh > 0 {
		fmt.Printf("Your desktop is %dx%d. A lower resolution makes the game's interface bigger,\n", dw, dh)
		fmt.Println("which helps on large or ultrawide screens. Options with * are the same shape as")
		fmt.Println("your screen - a different shape gives black bars.")
	} else {
		fmt.Println("A lower resolution makes the game's interface bigger, which helps on large screens.")
	}
	fmt.Println()
	fmt.Println("  0) Automatic (use my desktop resolution)")
	var offered [][2]int
	for _, r := range commonResolutions {
		if dw > 0 && dh > 0 && (r[0] > dw || r[1] > dh) {
			continue // the engine will not go above the desktop in a window, and it is pointless full screen
		}
		offered = append(offered, r)
		star := " "
		if sameShape(r[0], r[1], dw, dh) {
			star = "*"
		}
		fmt.Printf("  %d)%s %dx%d\n", len(offered), star, r[0], r[1])
	}
	fmt.Println("  Or type a size yourself, e.g. 2560x1080")
	fmt.Println("  Press Enter to leave it as it is")
	fmt.Print("Choice: ")
	line, _ := in.ReadString('\n')
	line = strings.TrimSpace(line)
	if line == "" {
		return
	}
	if line == "0" {
		settings.Resolution = "auto"
		saveSettings()
		say("Resolution: automatic (your desktop).")
		return
	}
	var n int
	if _, err := fmt.Sscan(line, &n); err == nil && n >= 1 && n <= len(offered) {
		settings.Resolution = fmt.Sprintf("%dx%d", offered[n-1][0], offered[n-1][1])
		saveSettings()
		say("Resolution: %s. It applies the next time the game starts.", settings.Resolution)
		return
	}
	var w, h int
	if _, err := fmt.Sscanf(strings.ToLower(line), "%dx%d", &w, &h); err == nil && w >= 640 && h >= 480 && w <= 7680 && h <= 4320 {
		settings.Resolution = fmt.Sprintf("%dx%d", w, h)
		saveSettings()
		say("Resolution: %s. It applies the next time the game starts.", settings.Resolution)
		return
	}
	say("Did not understand %q - nothing changed.", line)
}

func showRefused(reason string) {
	fmt.Println()
	say("*** The game server did not let you in: %s", reason)
	fmt.Println()
}

func isLocalPortal() bool {
	u := strings.ToLower(settings.PortalURL)
	return strings.Contains(u, "://127.0.0.1") || strings.Contains(u, "://localhost")
}

func orDash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}

// dsFatalExit is Dungeon Siege's own critical-error exit code: the four bytes spell "Fatl".
// It is what the engine returns after showing a dialog such as "D3D Initialization Failure",
// so seeing it seconds after launch means the game never got as far as running.
const dsFatalExit = 0x4661746c

func failedToStart(err error, ran time.Duration) bool {
	var ee *exec.ExitError
	if !errors.As(err, &ee) {
		return false
	}
	return ee.ExitCode() == dsFatalExit && ran < 90*time.Second
}

func errOrOK(err error) string {
	if err == nil {
		return "ok"
	}
	return err.Error()
}

func clamp(v, lo, hi int) int { return min(max(v, lo), hi) }

// ---------- HTTP ----------

func doReq(method, path, token string, body io.Reader, ctype string) (*http.Response, error) {
	req, err := http.NewRequest(method, settings.PortalURL+path, body)
	if err != nil {
		return nil, err
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if ctype != "" {
		req.Header.Set("Content-Type", ctype)
	}
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode >= 400 {
		defer resp.Body.Close()
		var e struct{ Error string }
		b, _ := io.ReadAll(resp.Body)
		if json.Unmarshal(b, &e) == nil && e.Error != "" {
			return resp, &apiError{resp.StatusCode, e.Error}
		}
		return resp, &apiError{resp.StatusCode, strings.TrimSpace(string(b))}
	}
	return resp, nil
}

type apiError struct {
	Code int
	Msg  string
}

func (e *apiError) Error() string { return fmt.Sprintf("%s (HTTP %d)", e.Msg, e.Code) }

func getJSON(path, token string, out any) error {
	resp, err := doReq("GET", path, token, nil, "")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	return json.NewDecoder(resp.Body).Decode(out)
}

func postJSON(path, token string, in, out any) error {
	b, _ := json.Marshal(in)
	resp, err := doReq("POST", path, token, bytes.NewReader(b), "application/json")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if out != nil {
		return json.NewDecoder(resp.Body).Decode(out)
	}
	return nil
}

func endSession(session string) {
	_ = postJSON("/api/session/end?session="+session, settings.Token, nil, nil)
}

// ---------- login ----------

// minPasswordLen mirrors the portal's own rule (SiegePortal handleLogin: "password must be at
// least 6 characters"). Kept here only so the launcher can say it before a wasted round trip.
const minPasswordLen = 6

// newPassword asks for a password twice and returns it only when the two typings match.
// Used everywhere a password is *created* or *changed* (0.7.8). A typo when setting a password is
// the expensive kind: nobody finds out until the next login, and on a brand-new account it means
// being locked out of an account that was never successfully used. A typo when *logging in* just
// fails and can be retried, which is why login still asks once.
// An empty first entry cancels, matching the rest of the launcher's prompts.
func newPassword(in *bufio.Reader, prompt string) (string, bool) {
	for attempt := 0; attempt < 3; attempt++ {
		fmt.Print(prompt)
		p1 := readPassword(in)
		if p1 == "" {
			return "", false
		}
		// The portal enforces this too (handleLogin / handleChangePassword), but saying it here
		// costs no round trip and no second typing.
		if len(p1) < minPasswordLen {
			say("That is too short - at least %d characters, please.", minPasswordLen)
			continue
		}
		fmt.Print("Type the password again: ")
		p2 := readPassword(in)
		if p1 != p2 {
			say("The two passwords are different, try again.")
			continue
		}
		return p1, true
	}
	return "", false
}

// confirmCreatedPassword runs in one specific case: the player chose "Log in", the name turned out
// not to exist, and the portal created the account from that single password entry (POST /api/login
// is one endpoint for both, so the launcher cannot know in advance). The password has been typed
// once at that point, so it is confirmed here, and reset properly if the two differ.
func confirmCreatedPassword(in *bufio.Reader, typed string) {
	fmt.Print("Type the password once more, so a typo cannot lock you out: ")
	if readPassword(in) == typed {
		return
	}
	say("Those two are different, so the password is not what you meant. Setting it again:")
	p, ok := newPassword(in, "Password (at least 6 characters, shown as you type): ")
	if !ok {
		say("Leaving it as the first password you typed. You can change it any time from Account options.")
		return
	}
	if err := postJSON("/api/account/password", settings.Token, map[string]string{"NewPassword": p}, nil); err != nil {
		say("Could not set the password: %v", err)
		say("It is still the first password you typed. You can change it from Account options.")
		return
	}
	say("Password set. Use that one next time you log in.")
}

func login(inviteRequired bool) error {
	if settings.Token != "" {
		var probe struct{}
		// A cheap authenticated call: session renew with an empty id returns 409 (valid token) or 401 (invalid).
		err := postJSON("/api/session/renew?session=", settings.Token, nil, &probe)
		var ae *apiError
		if errors.As(err, &ae) && ae.Code == 409 {
			say("Logged in as %s.", settings.Account)
			return nil
		}
		say("Your login expired, please log in again.")
		settings.Token = ""
	}
	in := stdin
	for attempt := 0; attempt < 5; attempt++ {
		// 0.7.8: logging in and creating an account are asked for separately, so that a new
		// password can be typed twice. The portal still has a single endpoint for both.
		fmt.Println()
		fmt.Println("Log in, or create a new account.")
		fmt.Println("  1) Log in")
		fmt.Println("  2) Create a new account")
		fmt.Print("Choice (Enter = log in): ")
		c, _ := in.ReadString('\n')
		creating := strings.TrimSpace(c) == "2"

		fmt.Print("Account name: ")
		name, _ := in.ReadString('\n')
		name = strings.TrimSpace(name)
		if name == "" {
			say("Please type an account name.")
			continue
		}

		var pass string
		if creating {
			p, ok := newPassword(in, "Password (at least 6 characters, shown as you type): ")
			if !ok {
				say("Account not created.")
				continue
			}
			pass = p
		} else {
			fmt.Print("Password (shown as you type): ")
			pass = readPassword(in)
		}

		invite := ""
		if inviteRequired {
			fmt.Print("Invite code (only needed for new accounts): ")
			invite, _ = in.ReadString('\n')
			invite = strings.TrimSpace(invite)
		}
		var res struct {
			Token   string `json:"token"`
			Name    string `json:"name"`
			Created bool   `json:"created"`
		}
		err := postJSON("/api/login", "", map[string]string{"Name": name, "Password": pass, "Invite": invite}, &res)
		if err != nil {
			if creating {
				say("Could not create that account: %v", err)
				// 401 is the portal's answer when the name exists and the password is wrong -
				// from this side that means the name is taken. Any other error (name format,
				// invite code, registration closed) already says what it is.
				var ae *apiError
				if errors.As(err, &ae) && ae.Code == 401 {
					say("That name is already taken. Pick another one - or choose 1) Log in if it is yours.")
				}
			} else {
				say("Login failed: %v", err)
			}
			continue
		}
		settings.Account, settings.Token = res.Name, res.Token
		saveSettings()
		switch {
		case res.Created && creating:
			say("Account %s created. Remember your password!", res.Name)
		case res.Created && !creating:
			// The name was free, so the portal made a new account from a password typed once.
			say("There was no account called %s, so one has been created for you.", res.Name)
			confirmCreatedPassword(in, pass)
		case creating:
			say("An account called %s already existed and that is its password, so you are logged into it.", res.Name)
		default:
			say("Welcome back, %s.", res.Name)
		}
		return nil
	}
	return errors.New("too many failed login attempts")
}

// ---------- DirectPlay ----------

func ensureDirectPlay() error {
	if runtime.GOOS != "windows" {
		return nil
	}
	win := os.Getenv("WINDIR")
	if _, err := os.Stat(filepath.Join(win, "SysWOW64", "dpnet.dll")); err == nil {
		return nil
	}
	say("DirectPlay (needed for multiplayer) is not enabled. A Windows prompt will ask for permission to enable it...")
	ps := `Start-Process dism -ArgumentList '/online','/enable-feature','/featurename:DirectPlay','/all','/norestart' -Verb RunAs -Wait`
	if err := exec.Command("powershell", "-NoProfile", "-Command", ps).Run(); err != nil {
		return fmt.Errorf("could not enable DirectPlay: %v", err)
	}
	if _, err := os.Stat(filepath.Join(win, "SysWOW64", "dpnet.dll")); err != nil {
		return errors.New("DirectPlay still not enabled. Open 'Turn Windows features on or off' → Legacy Components → DirectPlay")
	}
	say("DirectPlay enabled.")
	return nil
}

func resolveIPv4(addr string) (string, error) {
	if ip := net.ParseIP(addr); ip != nil && ip.To4() != nil {
		return ip.String(), nil
	}
	ips, err := net.LookupIP(addr)
	if err != nil {
		return "", err
	}
	for _, ip := range ips {
		if v4 := ip.To4(); v4 != nil {
			return v4.String(), nil
		}
	}
	return "", errors.New("no IPv4 address")
}

// ---------- save sync ----------

// Synced items inside the per-account user_path. Logs and screenshots stay local.
var syncItems = []string{"Save", "prefs.gas", "Keys"}

func downloadSave(profile string, rev int) error {
	_ = os.MkdirAll(profile, 0o755)
	resp, err := doReq("GET", "/api/save", settings.Token, nil, "")
	if err != nil {
		return fmt.Errorf("could not download your online save: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode == http.StatusNoContent {
		say("No online save yet — you'll create your character in game.")
		return nil
	}
	data, err := io.ReadAll(resp.Body)
	if err != nil {
		return err
	}
	// Keep a local safety copy of whatever was there before overwriting.
	if hashDir(filepath.Join(profile, "Save")) != hashOfEmpty {
		bk := filepath.Join(gameDir, "MMOProfile", "_local_backups", settings.Account+"_"+time.Now().Format("20060102-150405")+".zip")
		_ = os.MkdirAll(filepath.Dir(bk), 0o755)
		if z, err := zipProfile(profile); err == nil {
			_ = os.WriteFile(bk, z, 0o644)
		}
	}
	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return fmt.Errorf("online save is corrupt: %v", err)
	}
	for _, it := range syncItems {
		_ = os.RemoveAll(filepath.Join(profile, it))
	}
	for _, f := range zr.File {
		clean := filepath.Clean(filepath.FromSlash(f.Name))
		if strings.HasPrefix(clean, "..") || filepath.IsAbs(clean) {
			continue
		}
		dst := filepath.Join(profile, clean)
		if f.FileInfo().IsDir() {
			_ = os.MkdirAll(dst, 0o755)
			continue
		}
		_ = os.MkdirAll(filepath.Dir(dst), 0o755)
		rc, err := f.Open()
		if err != nil {
			return err
		}
		b, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			return err
		}
		if err := os.WriteFile(dst, b, 0o644); err != nil {
			return err
		}
	}
	say("Online save downloaded (revision %d, %d files).", rev, len(zr.File))
	return nil
}

func zipProfile(profile string) ([]byte, error) {
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for _, it := range syncItems {
		root := filepath.Join(profile, it)
		_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
			if err != nil || d.IsDir() {
				return nil
			}
			rel, _ := filepath.Rel(profile, path)
			b, err := os.ReadFile(path)
			if err != nil {
				return nil // file locked mid-write; next sync picks it up
			}
			w, err := zw.Create(filepath.ToSlash(rel))
			if err != nil {
				return err
			}
			_, err = w.Write(b)
			return err
		})
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func uploadSave(profile, session string) error {
	z, err := zipProfile(profile)
	if err != nil {
		return err
	}
	resp, err := doReq("PUT", "/api/save?session="+session, settings.Token, bytes.NewReader(z), "application/zip")
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	var r struct {
		Rev       int  `json:"save_rev"`
		Unchanged bool `json:"unchanged"`
	}
	_ = json.NewDecoder(resp.Body).Decode(&r)
	if r.Rev > 0 {
		lastServerRev = r.Rev
	}
	if !r.Unchanged {
		say("  character backed up online (revision %d, %d KB)", r.Rev, len(z)/1024)
	}
	return nil
}

// lastServerRev is the online revision this run knows about: the one handed out at session start, moved on by
// every upload the server accepts. A marker written when the final uploads fail names this, so the next run
// can tell "nobody else has touched it" from "another PC has uploaded since".
var lastServerRev int

var hashOfEmpty = hashDir("")

func hashDir(dir string) string {
	h := sha256.New()
	var files []string
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			files = append(files, p)
		}
		return nil
	})
	sort.Strings(files)
	for _, f := range files {
		st, err := os.Stat(f)
		if err != nil {
			continue
		}
		fmt.Fprintf(h, "%s|%d|%d\n", f[len(dir):], st.Size(), st.ModTime().UnixNano())
	}
	return hex.EncodeToString(h.Sum(nil))
}

// ---------- auto-update ----------

// retiredFiles are game files we used to ship and no longer want anybody running. The manifest can
// only ever add or replace a file - it has no way to say "delete this one" - so a file dropped from
// the manifest simply stays on every machine that already has it. These are removed on every start.
//
// Resources/ikkyo_mpsave_beta_6.dsres: Ikkyo's Multiplayer Quest Save. It saved quest state in
// script and loaded it from the host, which is why quest progress used to be shared by everybody.
// SiegeHost 1.0.0 turns on the engine's own per-character quest journal instead, so the mod is not
// only redundant but works against it, and its replacement journal window carries the mod's name.
//
// Return to Arhok (.dsmap / .dsres and XPMap / XPRes): Raid event files that cause mod mismatch
// errors when players connect to the MMO server without them installed.
var retiredFiles = []string{
	"Resources/ikkyo_mpsave_beta_6.dsres",
	"Maps/Return to Arhok.dsmap",
	"Resources/Return to Arhok.dsres",
	"DSLOA/XPMap.dsmap",
	"DSLOA/XPRes.dsres",
	"Maps/XPMap.dsmap",
	"Resources/XPRes.dsres",
}

// isRetired compares on forward slashes so it behaves the same whichever way the path arrived and
// whichever platform this is built for. Manifest paths use forward slashes; filepath.Clean on
// Windows hands back backslashes.
func isRetired(clean string) bool {
	norm := func(p string) string {
		return strings.ToLower(path.Clean(strings.ReplaceAll(p, `\`, "/")))
	}
	for _, r := range retiredFiles {
		if norm(clean) == norm(r) {
			return true
		}
	}
	return false
}

// removeRetired deletes the files above from the game folder if they are there. It refuses anything
// that is not a plain relative path inside the game folder, and it never touches a directory. A file
// that cannot be deleted is reported and skipped rather than being treated as a fatal error: a
// player with a read-only folder should still get into the game.
func removeRetired() {
	for _, r := range retiredFiles {
		clean := filepath.Clean(filepath.FromSlash(r))
		if clean == "." || strings.HasPrefix(clean, "..") || filepath.IsAbs(clean) || strings.Contains(clean, ":") {
			continue
		}
		dst := filepath.Join(gameDir, clean)
		st, err := os.Stat(dst)
		if err != nil || st.IsDir() {
			continue
		}
		if err := os.Remove(dst); err != nil {
			say("WARNING: could not remove %s (%v). Close the game and try again.", r, err)
			continue
		}
		say("Removed %s - it is no longer part of Dungeon Siege MMO.", r)
	}
}

// autoUpdate downloads every file from the central server's manifest whose SHA-256 differs from the local copy.
// Paths are relative to the game folder. A new SiegeLauncher.exe replaces the running one and restarts it.
func autoUpdate() error {
	exe, _ := os.Executable()
	_ = os.Remove(exe + ".old") // left over from a previous self-update
	var man struct {
		Files []struct {
			Path   string `json:"path"`
			Size   int64  `json:"size"`
			SHA256 string `json:"sha256"`
		} `json:"files"`
		Retired []string `json:"retired"`
	}
	if err := getJSON("/api/manifest", "", &man); err != nil {
		var ae *apiError
		if errors.As(err, &ae) && ae.Code == 404 {
			return nil // older portal without updates
		}
		say("WARNING: update check failed (%v). Continuing with the files you have.", err)
		return nil
	}
	for _, r := range man.Retired {
		if !isRetired(r) {
			retiredFiles = append(retiredFiles, r)
		}
	}
	selfUpdated := false
	updated := 0
	for _, f := range man.Files {
		clean := filepath.Clean(filepath.FromSlash(f.Path))
		if clean == "." || strings.HasPrefix(clean, "..") || filepath.IsAbs(clean) || strings.Contains(clean, ":") {
			continue
		}
		if isRetired(clean) {
			continue // never re-download something we are about to delete
		}
		dst := filepath.Join(gameDir, clean)
		isSelf := strings.EqualFold(filepath.Base(clean), filepath.Base(exe)) && strings.EqualFold(filepath.Dir(clean), ".")
		if isSelf {
			dst = exe
		}
		if sum, err := fileSHA256(dst); err == nil && strings.EqualFold(sum, f.SHA256) {
			continue
		}
		if isSelf && os.Getenv("SIEGELAUNCHER_UPDATED") == "1" {
			say("WARNING: launcher update did not stick; continuing with version %s.", version)
			continue
		}
		say("Updating %s (%d KB)...", f.Path, f.Size/1024)
		data, err := download("/files/" + escapePath(f.Path))
		if err != nil {
			return fmt.Errorf("update download failed for %s: %v", f.Path, err)
		}
		sum := sha256.Sum256(data)
		if !strings.EqualFold(hex.EncodeToString(sum[:]), f.SHA256) {
			return fmt.Errorf("update for %s arrived damaged (checksum mismatch), try again", f.Path)
		}
		_ = os.MkdirAll(filepath.Dir(dst), 0o755)
		tmp := dst + ".new"
		if err := os.WriteFile(tmp, data, 0o755); err != nil {
			return fmt.Errorf("cannot write %s (is the game running or the folder read-only?): %v", dst, err)
		}
		if isSelf {
			_ = os.Remove(exe + ".old")
			if err := os.Rename(exe, exe+".old"); err != nil {
				return fmt.Errorf("cannot replace the launcher: %v", err)
			}
			selfUpdated = true
		} else {
			_ = os.Remove(dst)
		}
		if err := os.Rename(tmp, dst); err != nil {
			return fmt.Errorf("cannot install %s: %v", dst, err)
		}
		updated++
	}
	removeRetired()
	if updated > 0 {
		say("Updated %d file(s).", updated)
	}
	if selfUpdated {
		say("Launcher updated, restarting it...")
		cmd := exec.Command(exe, os.Args[1:]...)
		cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
		cmd.Env = append(os.Environ(), "SIEGELAUNCHER_UPDATED=1")
		if err := cmd.Run(); err != nil {
			var ee *exec.ExitError
			if errors.As(err, &ee) {
				os.Exit(ee.ExitCode())
			}
			return fmt.Errorf("could not restart the updated launcher: %v", err)
		}
		os.Exit(0)
	}
	return nil
}

func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func escapePath(p string) string {
	parts := strings.Split(p, "/")
	for i, s := range parts {
		parts[i] = url.PathEscape(s)
	}
	return strings.Join(parts, "/")
}

func download(path string) ([]byte, error) {
	resp, err := doReq("GET", path, "", nil, "")
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	return io.ReadAll(resp.Body)
}

// wakeAndWait starts a sleeping on-demand server and polls /api/status until it reports in game (max 8 min).
func wakeAndWait(id string) (string, int, int, map[string]string, error) {
	say("The %s server is asleep to save costs. Starting it now - this takes about a minute, keep this window open.", strings.ToUpper(id))
	var wake struct {
		State string `json:"state"`
	}
	lastWake := time.Now()
	if err := postJSON("/api/servers/wake?id="+url.QueryEscape(id), settings.Token, map[string]any{}, &wake); err != nil {
		return "", 0, 0, nil, fmt.Errorf("could not start the server: %v", err)
	}
	start := time.Now()
	lastLine := ""
	for time.Since(start) < 8*time.Minute {
		time.Sleep(10 * time.Second)
		if time.Since(lastWake) > 2*time.Minute { // keeps the server from being stopped while we wait
			_ = postJSON("/api/servers/wake?id="+url.QueryEscape(id), settings.Token, map[string]any{}, &wake)
			lastWake = time.Now()
		}
		var st struct {
			Servers []struct {
				ID          string            `json:"id"`
				GameAddress string            `json:"game_address"`
				GamePort    int               `json:"game_port"`
				MaxPlayers  int               `json:"max_players"`
				Online      bool              `json:"online"`
				Game        map[string]string `json:"game"`
				Instance    string            `json:"instance_state"`
			} `json:"servers"`
		}
		if err := getJSON("/api/status", "", &st); err != nil {
			continue
		}
		for _, sv := range st.Servers {
			if sv.ID != id {
				continue
			}
			if sv.Online {
				say("The %s server is up (%s) after %d s.", strings.ToUpper(id), sv.GameAddress, int(time.Since(start).Seconds()))
				return sv.GameAddress, sv.GamePort, sv.MaxPlayers, sv.Game, nil
			}
			phase := "machine " + orDash(sv.Instance)
			if sv.Instance == "running" {
				phase = "loading the world (" + orDash(sv.Game["phase"]) + ")"
			}
			line := fmt.Sprintf("  ... %d s: %s", int(time.Since(start).Seconds()), phase)
			if line != lastLine {
				fmt.Println(line)
				lastLine = line
			}
		}
	}
	return "", 0, 0, nil, fmt.Errorf("the %s server did not come up within 8 minutes. Try again, or pick another server", strings.ToUpper(id))
}

// ---------- account options (0.5.0) ----------

// accountMenu lets a logged-in player change password or account name, or log out. Being logged in on this PC is
// the proof of identity, so the old password is never asked.
func accountMenu(inviteRequired bool) error {
	in := stdin
	for {
		fmt.Print("Press Enter to play, or A for account options: ")
		line, _ := in.ReadString('\n')
		if !strings.EqualFold(strings.TrimSpace(line), "a") {
			return nil
		}
		fmt.Println()
		fmt.Printf("Account: %s\n", settings.Account)
		fmt.Println("  1) Change password")
		fmt.Println("  2) Change account name")
		fmt.Println("  3) Log out (use another account)")
		// 0.8.10: there is no display choice any more. The game always starts in full screen, so the
		// only thing left to change is the resolution.
		fmt.Printf("  4) Screen resolution: %s\n", resolutionLabel())
		fmt.Println("  Press Enter to go back")
		fmt.Print("Choice: ")
		c, _ := in.ReadString('\n')
		switch strings.TrimSpace(c) {
		case "1":
			changePassword(in)
		case "2":
			changeAccountName(in)
		case "4":
			chooseResolution(in)
		case "3":
			old := settings.Account
			settings.Token, settings.Account = "", ""
			saveSettings()
			say("Logged out of %s.", old)
			if err := login(inviteRequired); err != nil {
				return err
			}
		}
		fmt.Println()
	}
}

func changePassword(in *bufio.Reader) {
	// 0.7.8: the double entry lives in newPassword now, shared with account creation.
	p, ok := newPassword(in, "New password (at least 6 characters, shown as you type): ")
	if !ok {
		say("Password not changed.")
		return
	}
	if err := postJSON("/api/account/password", settings.Token, map[string]string{"NewPassword": p}, nil); err != nil {
		say("Password not changed: %v", err)
		return
	}
	say("Password changed. Use the new password next time you log in.")
}

func changeAccountName(in *bufio.Reader) {
	fmt.Print("New account name (3-20 letters, digits or _; Enter = cancel): ")
	name, _ := in.ReadString('\n')
	name = strings.TrimSpace(name)
	if name == "" {
		say("Account name not changed.")
		return
	}
	var res struct {
		Name  string `json:"name"`
		Token string `json:"token"`
	}
	if err := postJSON("/api/account/rename", settings.Token, map[string]string{"NewName": name}, &res); err != nil {
		say("Account name not changed: %v", err)
		return
	}
	old := settings.Account
	settings.Account, settings.Token = res.Name, res.Token
	saveSettings()
	// Move the local profile (character files, prefs) so the next download/upload uses the same folder.
	base := filepath.Join(gameDir, "MMOProfile")
	from, to := filepath.Join(base, old), filepath.Join(base, res.Name)
	if _, err := os.Stat(from); err == nil && !strings.EqualFold(from, to) {
		if _, err := os.Stat(to); err == nil {
			bk := filepath.Join(base, "_local_backups", res.Name+"_old_"+time.Now().Format("20060102-150405"))
			_ = os.MkdirAll(filepath.Dir(bk), 0o755)
			_ = os.Rename(to, bk)
		}
		if err := os.Rename(from, to); err != nil {
			say("Note: could not rename the local folder %s (%v); your character is safe online and downloads again.", from, err)
		}
	} else if err == nil {
		tmp := to + "_renaming"
		if os.Rename(from, tmp) == nil {
			_ = os.Rename(tmp, to)
		}
	}
	say("Account renamed from %s to %s. Your character and online saves moved with it; log in with the new name from now on.", old, res.Name)
}
