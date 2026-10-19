// SiegeServer — 24/7 controller for the Dungeon Siege MMO host PC.
// Keeps SiegePortal (accounts/saves) and the DSLOA.exe game host (SiegeHost Role=server) running,
// restarting them on crash, hang or exit. Docs: 04-server/SiegeServer-24-7/README.md
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

const version = "0.7.3"

// defaultSendFrequency is the engine multiplayer send rate we ship. The engine's own default is 5 Hz
// (200 ms between network flushes, measured in lab testing); 30 Hz is the lab-verified value.
// An absent "send_frequency" key means "use this", so a server whose config update.sh preserved
// still picks the fix up. An explicit 0 keeps the engine default.
const defaultSendFrequency = 30

// pickStartTown: absent means true, so a siegeserver.json that update.sh preserved still gets it.
// The engine keeps its own per-town required level and sends under-levelled players to the default.
func pickStartTown(p *bool) bool {
	if p == nil {
		return true
	}
	return *p
}

func sendHz(p *int) int {
	if p == nil {
		return defaultSendFrequency
	}
	return clamp(*p, 0, 60)
}

type Config struct {
	GameDir          string `json:"game_dir"`           // folder containing DSLOA.exe
	ServerName       string `json:"server_name"`        // lobby name
	MaxPlayers       int    `json:"max_players"`        // 1..30
	HostCharacter    string `json:"host_character"`     // the host's own (idle) character name
	PortalExe        string `json:"portal_exe"`         // SiegePortal.exe
	PortalConfig     string `json:"portal_config"`      // portal.json
	HangSeconds      int    `json:"hang_seconds"`       // restart if the in-game heartbeat is older than this
	StartupSeconds   int    `json:"startup_seconds"`    // max time to reach in-game before restart
	RestartDelaySecs int    `json:"restart_delay_secs"` // pause between restarts
	ExtraArgs        string `json:"extra_args"`
	DailyRestartAt   string `json:"daily_restart_at"`    // "HH:MM" local time, empty = never (ignored while reset_when_empty is on)
	AllowPausing     bool   `json:"allow_pausing"`       // false = players cannot pause the shared game (default)
	ResetWhenEmpty   bool   `json:"reset_when_empty"`    // restart the game (fresh world) once everyone has left. Default true
	EmptyGraceSecs   int    `json:"empty_grace_seconds"` // how long the server must stay empty after play before the reset (default 120)
	WineExe          string `json:"wine_exe"`            // Linux only: Wine binary used to run DSLOA.exe (default "wine")
	Map              string `json:"map"`                 // internal map name (default "multiplayer_world" = Utraean Peninsula; "-" = game default)
	World            string `json:"world"`               // regular/veteran/elite; empty = game default
	StartLocation    string `json:"start_location"`      // start group for every player (default "elddim"; "-" = map default)
	GamePort         int    `json:"game_port"`           // fixed DirectPlay host UDP port clients search directly (default 2350, -1 = dynamic)
	SendFrequency    *int   `json:"send_frequency,omitempty"` // engine multiplayer send rate in Hz. Absent = 30; 0 = leave the engine default (5)
	PickStartTown    *bool  `json:"pick_start_town,omitempty"` // players choose their start town in staging. Absent = true; false = everyone at start_location

	// Shard mode (0.4.0): report this game server to the central portal instead of (or as well as) running one.
	HubURL        string `json:"hub_url"`        // e.g. http://127.0.0.1:8440 or http://72.60.164.124:8440; empty = off
	ShardID       string `json:"shard_id"`       // must match an entry in the central portal's "servers"
	ShardKey      string `json:"shard_key"`      // shared secret from that entry
	PublicAddress string `json:"public_address"` // address clients join; "auto" = the hub uses the source IP it sees

	// Quest reset (0.7.1): restore a clean host character before every game start, so quests reset with the world. Absent = true.
	ResetHostQuests *bool `json:"reset_host_quests,omitempty"`

	// Join guard (0.6.0): only players with a launcher join ticket may join (needs shard_id + shard_key). Default on.
	JoinGuard *bool `json:"join_guard,omitempty"`

	// 0.7.2: the level a veteran world asks for on this shard, whatever the map says. 0 = leave it alone.
	// SiegeHost-server.ini is written from scratch before every game start, so a hand-edited copy does not
	// survive one restart; a shard that needs this key has to say so here.
	VeteranRequiredLevel int `json:"veteran_required_level,omitempty"`

	// 0.7.3: host without dpnsvr.exe. DirectPlay's discovery helper wants UDP 6073 and there is only one of
	// that port on a machine, so the first game server to start takes it and every later one fails its
	// Host() with DPNERR_DPNSVRNOTAVAILABLE (0x80158185) after a 13 second timeout. A box running more than
	// one shard needs this on. The host then listens on game_port itself, which is where the launcher sends
	// players anyway, so nothing is lost: dpnsvr only ever served the engine's own LAN browser.
	NoDpnsvr bool `json:"no_dpnsvr,omitempty"`
}

var logger *log.Logger

var processStart = time.Now()

func main() {
	cfgPath := flag.String("config", "siegeserver.json", "config file")
	flag.Parse()
	base, _ := os.Getwd()
	_ = os.MkdirAll("logs", 0o755)
	lf, _ := os.OpenFile(filepath.Join("logs", "siegeserver.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	logger = log.New(io.MultiWriter(os.Stdout, lf), "", log.LstdFlags)
	cfg := loadConfig(*cfgPath)
	logger.Printf("==== SiegeServer %s (dir %s) ====", version, base)
	if cfg.ShardKey != "" && cfg.ShardID != "" && (cfg.JoinGuard == nil || *cfg.JoinGuard) {
		logger.Printf("join guard: ON (only players who start through the launcher can join %q)", cfg.ShardID)
	} else {
		logger.Printf("join guard: OFF (join_guard=false or no shard_id/shard_key)")
	}
	if _, err := os.Stat(filepath.Join(cfg.GameDir, "DSLOA.exe")); err != nil {
		logger.Fatalf("DSLOA.exe not found in game_dir %q — edit %s", cfg.GameDir, *cfgPath)
	}
	exe, _ := os.Executable()
	if msg, err := installSiegeHost(cfg.GameDir, filepath.Join(filepath.Dir(exe), "SiegeHost-Mss32.dll")); err != nil {
		logger.Fatalf("SiegeHost install failed: %v", err)
	} else {
		logger.Printf("SiegeHost: %s", msg)
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	var wg sync.WaitGroup
	quit := make(chan struct{})
	if cfg.PortalExe != "" {
		wg.Add(1)
		go func() {
			defer wg.Done()
			superviseProcess("portal", quit, cfg, func() *exec.Cmd { return portalCmd(cfg) }, nil)
		}()
	} else {
		logger.Printf("portal_exe empty: game server only (shard %q -> %s)", cfg.ShardID, cfg.HubURL)
	}
	if cfg.HubURL != "" {
		go reportToHub(cfg, quit)
	}
	wg.Add(1)
	go func() {
		defer wg.Done()
		superviseProcess("game", quit, cfg, func() *exec.Cmd { return gameCmd(cfg, base) }, gameHealth(cfg))
	}()
	<-stop
	logger.Printf("stop signal — stopping server...")
	close(quit)
	wg.Wait()
	logger.Printf("stopped")
}

func loadConfig(path string) Config {
	cfg := Config{GameDir: `..\01-client\Dungeon Siege 1`, ServerName: "Dungeon Siege MMO", MaxPlayers: 30, GamePort: 2350, Map: "multiplayer_world", StartLocation: "elddim", ResetWhenEmpty: true, EmptyGraceSecs: 120,
		HostCharacter: "ServerHost", PortalExe: "SiegePortal.exe", PortalConfig: "portal.json",
		HangSeconds: 120, StartupSeconds: 360, RestartDelaySecs: 10}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		out, _ := json.MarshalIndent(cfg, "", "  ")
		_ = os.WriteFile(path, out, 0o644)
		fmt.Printf("Wrote default %s\n", path)
		return cfg
	}
	if err != nil {
		log.Fatal(err)
	}
	if err := json.Unmarshal(b, &cfg); err != nil {
		log.Fatalf("%s: %v", path, err)
	}
	cfg.GameDir, _ = filepath.Abs(cfg.GameDir)
	return cfg
}

func portalCmd(cfg Config) *exec.Cmd {
	exe, _ := filepath.Abs(cfg.PortalExe)
	cmd := exec.Command(exe, "-config", cfg.PortalConfig)
	f, _ := os.OpenFile(filepath.Join("logs", "portal.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	cmd.Stdout, cmd.Stderr = f, f
	return cmd
}

func gameCmd(cfg Config, base string) *exec.Cmd {
	// Re-install SiegeHost before every (re)start: dropping a new SiegeHost-Mss32.dll next to SiegeServer.exe
	// updates the running server at its next restart, without anyone touching the PC.
	exe, _ := os.Executable()
	if msg, err := installSiegeHost(cfg.GameDir, filepath.Join(filepath.Dir(exe), "SiegeHost-Mss32.dll")); err != nil {
		logger.Printf("[game] SiegeHost install: %v", err)
	} else if msg != "SiegeHost up to date" {
		logger.Printf("[game] SiegeHost: %s", msg)
	}
	ini := filepath.Join(base, "SiegeHost-server.ini")
	gamePort := cfg.GamePort
	if gamePort < 0 {
		gamePort = 0
	}
	opt := func(v string) string {
		if v == "-" {
			return ""
		}
		return v
	}
	// With NoDpnsvr the engine never starts dpnsvr.exe and listens on HostPort itself, so HostPort has to be
	// this shard's own game port rather than the 6073 it defaults to, which is the port being fought over.
	dpnsvr := ""
	if cfg.NoDpnsvr {
		dpnsvr = fmt.Sprintf("NoDpnsvr=1\r\nHostPort=%d\r\n", gamePort)
	}
	content := fmt.Sprintf("[SiegeHost]\r\nEnabled=1\r\nRole=server\r\nMaxPlayers=%d\r\nLogDirectPlay=1\r\nGamePort=%d\r\n"+dpnsvr+"[AutoHost]\r\nGameName=%s\r\nHostCharacter=%s\r\nStuckTimeoutSec=%d\r\nAllowPausing=%d\r\nMap=%s\r\nWorld=%s\r\nStartLocation=%s\r\n",
		clamp(cfg.MaxPlayers, 1, 30), gamePort, cfg.ServerName, cfg.HostCharacter, cfg.StartupSeconds, b2i(cfg.AllowPausing), opt(cfg.Map), opt(cfg.World), opt(cfg.StartLocation))
	content += fmt.Sprintf("AllowStartLocationSelection=%d\r\n", b2i(pickStartTown(cfg.PickStartTown)))
	if hz := sendHz(cfg.SendFrequency); hz > 0 {
		content += fmt.Sprintf("[Net]\r\nSendFrequency=%d\r\n", hz)
	}
	if cfg.VeteranRequiredLevel > 0 {
		content += fmt.Sprintf("[World]\r\nVeteranRequiredLevel=%d\r\n", clamp(cfg.VeteranRequiredLevel, 1, 200))
	}
	if cfg.ShardKey != "" && cfg.ShardID != "" && (cfg.JoinGuard == nil || *cfg.JoinGuard) {
		m := hmac.New(sha256.New, []byte(cfg.ShardKey))
		m.Write([]byte("ds1mmo-join-ticket-v1"))
		content += fmt.Sprintf("[JoinGuard]\r\nEnabled=1\r\nServerId=%s\r\nKey=%s\r\n", cfg.ShardID, hex.EncodeToString(m.Sum(nil)))
	} else {
		content += "[JoinGuard]\r\nEnabled=0\r\n"
	}
	_ = os.WriteFile(ini, []byte(content), 0o600)
	_ = os.Chmod(ini, 0o600) // holds the join key
	_ = os.Remove(heartbeatPath(cfg))
	_ = os.MkdirAll(filepath.Join(base, "server-profile"), 0o755) // DSLOA shows a fatal "critical path" box if user_path is missing
	if msg := resetHostCharacter(base, cfg); msg != "" {
		logger.Printf("[game] %s", msg)
	}
	profile := spacelessPath(filepath.Join(base, "server-profile"), "_server")
	iniArg := ini
	if runtime.GOOS != "windows" {
		// Linux (Hostinger): the game runs under Wine; Windows programs see the Linux filesystem as drive Z:.
		profile, iniArg = winePath(profile), winePath(ini)
	}
	args := []string{"nointro=true", "zonematch=true", "multi=true", "always_active=true", "nosound=true",
		"fullscreen=false", "width=800", "height=600", "user_path=" + profile}
	args = append(args, strings.Fields(cfg.ExtraArgs)...)
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command(filepath.Join(cfg.GameDir, "DSLOA.exe"), args...)
	} else {
		wine := cfg.WineExe
		if wine == "" {
			wine = "wine"
		}
		cmd = exec.Command(wine, append([]string{"DSLOA.exe"}, args...)...)
		f, _ := os.OpenFile(filepath.Join("logs", "wine.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
		cmd.Stdout, cmd.Stderr = f, f
	}
	cmd.Dir = cfg.GameDir
	cmd.Env = append(os.Environ(), "SIEGEHOST_INI="+iniArg)
	return cmd
}

func heartbeatPath(cfg Config) string {
	return filepath.Join(cfg.GameDir, "SiegeHostLogs", "heartbeat.txt")
}

// gameHealth returns a checker that reports a reason when the host must be restarted.
// Called every 10 s by superviseProcess; the closure keeps the "reset when empty" state for the current game run.
func gameHealth(cfg Config) func(started time.Time) string {
	var run time.Time // game start this state belongs to
	var played bool   // at least one player was on since this game started
	var emptySince time.Time
	grace := time.Duration(cfg.EmptyGraceSecs) * time.Second
	if grace <= 0 {
		grace = 120 * time.Second
	}
	if cfg.ResetWhenEmpty {
		logger.Printf("world reset: when empty for %s after someone played (daily restart %q ignored)", grace, cfg.DailyRestartAt)
	}
	return func(started time.Time) string {
		if !started.Equal(run) {
			run, played, emptySince = started, false, time.Time{}
		}
		hb := heartbeatPath(cfg)
		st, err := os.Stat(hb)
		if err != nil {
			if time.Since(started) > time.Duration(cfg.StartupSeconds+60)*time.Second {
				return "no heartbeat since start"
			}
			return ""
		}
		if st.ModTime().Before(started) {
			return ""
		}
		if age := time.Since(st.ModTime()); age > time.Duration(cfg.HangSeconds)*time.Second {
			return fmt.Sprintf("heartbeat stale for %s (game frozen)", age.Round(time.Second))
		}
		if cfg.ResetWhenEmpty {
			fields := readKV(hb)
			if fields["phase"] == "ingame" {
				players, _ := strconv.Atoi(fields["players"])
				switch {
				case players > 0:
					if !played {
						logger.Printf("world reset: player activity seen (%d online) — world will reset after everyone leaves", players)
					} else if !emptySince.IsZero() {
						logger.Printf("world reset: cancelled, %d player(s) back", players)
					}
					played, emptySince = true, time.Time{}
				case played && emptySince.IsZero():
					emptySince = time.Now()
					logger.Printf("world reset: server empty — resetting the world in %s unless someone joins", grace)
				case played && time.Since(emptySince) >= grace:
					return fmt.Sprintf("world reset (nobody online for %s after play)", grace)
				}
			}
			return ""
		}
		if cfg.DailyRestartAt != "" {
			now := time.Now()
			if now.Format("15:04") == cfg.DailyRestartAt && time.Since(started) > 2*time.Minute {
				return "scheduled daily restart"
			}
		}
		return ""
	}
}

func readKV(path string) map[string]string {
	out := map[string]string{}
	b, err := os.ReadFile(path)
	if err != nil {
		return out
	}
	for _, line := range strings.Split(string(b), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok {
			out[k] = v
		}
	}
	return out
}

func superviseProcess(name string, quit <-chan struct{}, cfg Config, mk func() *exec.Cmd, health func(time.Time) string) {
	restarts := 0
	for {
		cmd := mk()
		started := time.Now()
		if err := cmd.Start(); err != nil {
			logger.Printf("[%s] failed to start: %v", name, err)
		} else {
			logger.Printf("[%s] started pid %d (restart #%d)", name, cmd.Process.Pid, restarts)
			done := make(chan error, 1)
			go func() { done <- cmd.Wait() }()
			tick := time.NewTicker(10 * time.Second)
		wait:
			for {
				select {
				case err := <-done:
					logger.Printf("[%s] exited after %s: %v", name, time.Since(started).Round(time.Second), errText(err))
					break wait
				case <-quit:
					_ = cmd.Process.Kill()
					<-done
					tick.Stop()
					logger.Printf("[%s] stopped", name)
					return
				case <-tick.C:
					if health != nil {
						if reason := health(started); reason != "" {
							logger.Printf("[%s] RESTARTING: %s", name, reason)
							_ = cmd.Process.Kill()
						}
					}
				}
			}
			tick.Stop()
		}
		restarts++
		select {
		case <-quit:
			return
		case <-time.After(time.Duration(cfg.RestartDelaySecs) * time.Second):
		}
	}
}

func errText(err error) string {
	if err == nil {
		return "exit code 0"
	}
	return err.Error()
}

func clamp(v, lo, hi int) int { return min(max(v, lo), hi) }

// reportToHub pushes heartbeat.txt to the central portal every 10 s (POST /api/shard/heartbeat).
func reportToHub(cfg Config, quit <-chan struct{}) {
	client := &http.Client{Timeout: 8 * time.Second}
	url := strings.TrimRight(cfg.HubURL, "/") + "/api/shard/heartbeat"
	failing := false
	for {
		fields := map[string]string{"phase": "starting"}
		if st, err := os.Stat(heartbeatPath(cfg)); err == nil && st.ModTime().Before(processStart) {
			// left over from before this start (e.g. the machine was stopped): never report it as live
		} else if b, err := os.ReadFile(heartbeatPath(cfg)); err == nil {
			for _, line := range strings.Split(string(b), "\n") {
				if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok {
					fields[k] = v
				}
			}
			if st, err := os.Stat(heartbeatPath(cfg)); err == nil && time.Since(st.ModTime()) > 60*time.Second {
				fields["phase"] = "stale"
			}
		}
		fields["siegeserver_version"] = version
		body, _ := json.Marshal(map[string]any{"fields": fields, "address": cfg.PublicAddress})
		req, _ := http.NewRequest("POST", url, bytes.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.Header.Set("X-Shard-Id", cfg.ShardID)
		req.Header.Set("X-Shard-Key", cfg.ShardKey)
		resp, err := client.Do(req)
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode != 200 {
				err = fmt.Errorf("HTTP %d", resp.StatusCode)
			}
		}
		if err != nil && !failing {
			logger.Printf("[hub] heartbeat to %s failed: %v", url, err)
		} else if err == nil && failing {
			logger.Printf("[hub] heartbeat to %s OK again", url)
		}
		failing = err != nil
		select {
		case <-quit:
			return
		case <-time.After(10 * time.Second):
		}
	}
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}
