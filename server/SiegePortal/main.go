// SiegePortal — accounts, online character saves and server status for the Dungeon Siege MMO project.
// Single static binary, stdlib only. Runs on the game-host PC (Windows) or on a Linux VPS.
// Docs: 04-server/SiegePortal/README.md
package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"
)

const version = "0.7.2"

// See SiegeServer-24-7: absent means 30 Hz, so a portal.json that update.sh preserved still tells
// launchers the new rate. Host and client must agree, so this comes from the server entry.
const defaultSendFrequency = 30

func sendHz(p *int) int {
	if p == nil {
		return defaultSendFrequency
	}
	if *p < 0 {
		return 0
	}
	if *p > 60 {
		return 60
	}
	return *p
}

type Config struct {
	Listen            string `json:"listen"`             // e.g. ":8440"
	DataDir           string `json:"data_dir"`           // accounts + saves
	ServerName        string `json:"server_name"`        // shown in launcher
	GameAddress       string `json:"game_address"`       // public IP/hostname friends join (DirectPlay)
	MaxPlayers        int    `json:"max_players"`        // informational
	AllowRegistration bool   `json:"allow_registration"` // first login creates the account
	InviteCode        string `json:"invite_code"`        // optional: required to register when non-empty
	HeartbeatFile     string `json:"heartbeat_file"`     // SiegeHostLogs\heartbeat.txt of the game host
	MaxSaveMB         int    `json:"max_save_mb"`
	KeepBackups       int    `json:"keep_backups"`
	LeaseSeconds      int    `json:"lease_seconds"` // how long a play session lock lives without renewal

	// Central-server mode (0.2.0): one portal for every region.
	Servers   []ServerEntry `json:"servers"`    // game servers (shards) shown in the launcher; empty = legacy single server
	UpdateDir string        `json:"update_dir"` // files the launcher keeps in sync (default <data_dir>/files)

	// Join guard (0.5.0): see joinguard.go
	JoinTicketMinutes int   `json:"join_ticket_minutes"`    // ticket lifetime; the launcher refreshes it while the game runs (default 20)
	JoinNewCharMaxXP  int64 `json:"join_new_char_max_xp"`   // most experience an unknown (new) character may have (default 500)
	JoinToleranceXP   int64 `json:"join_tolerance_xp"`      // allowed above the record (last seconds before a crash) (default 1000)
	JoinTolerancePct  int64 `json:"join_tolerance_percent"` // plus this percentage of the record (default 5)
}

// ServerEntry is one game server (shard), e.g. NA on Hostinger or EU on AWS. The shard's SiegeServer pushes its
// heartbeat to /api/shard/heartbeat with ShardKey; HeartbeatFile can be used instead when the game runs on this box.
type ServerEntry struct {
	ID            string    `json:"id"`           // "na", "eu"
	Name          string    `json:"name"`         // "North America (Hostinger)"
	Region        string    `json:"region"`       // "NA", "EU"
	GameAddress   string    `json:"game_address"` // public IPv4/hostname clients join; "auto" = what the shard reports
	GamePort      int       `json:"game_port"`      // fixed DirectPlay host port (0 = 2350 default)
	SendFrequency *int      `json:"send_frequency,omitempty"` // engine send rate in Hz the launcher writes into SiegeHost.ini. Absent = 30; 0 = engine default
	MaxPlayers    int       `json:"max_players"`
	ShardKey      string    `json:"shard_key"`           // shared secret for heartbeat pushes
	HeartbeatFile string    `json:"heartbeat_file"`      // optional local heartbeat.txt
	OnDemand      *OnDemand `json:"on_demand,omitempty"` // 0.3.0: cloud instance started on demand, stopped when empty
	JoinCode      string    `json:"join_code,omitempty"`     // 0.7.0: players must type this before a ticket is issued. Never sent to a client.
	VeteranLevel  int       `json:"veteran_level,omitempty"` // 0.7.0: the level a veteran world asks for on this shard. 0 = whatever the map says.
	HostCharacter string    `json:"host_character,omitempty"` // 0.7.1: this shard's idle host character. Clients need the name to leave it out of the party panel; they default to "ServerHost" and so show it by name on any shard that uses another.
}

type shardState struct {
	fields  map[string]string
	address string
	seen    time.Time
}

type Account struct {
	Name      string    `json:"name"`
	Salt      string    `json:"salt"`
	Hash      string    `json:"hash"`
	Created   time.Time `json:"created"`
	LastLogin time.Time `json:"last_login"`
	Banned    bool      `json:"banned"`
	SaveRev   int       `json:"save_rev"`
	SaveSHA   string    `json:"save_sha"`
	SaveTime  time.Time `json:"save_time"`
	// 0.5.0 join guard: experience record per character (key = charKey(name)); CharsInit = existing save scanned
	Chars     map[string]*CharRec `json:"chars,omitempty"`
	CharsInit bool                `json:"chars_init,omitempty"`
}

type lease struct {
	Session string
	Machine string
	Server  string // server id the player is joining (on-demand idle detection)
	Expires time.Time
	Started time.Time
}

type Portal struct {
	cfg      Config
	mu       sync.Mutex
	accounts map[string]*Account
	leases   map[string]*lease
	secret   []byte
	shards   map[string]*shardState
	od       map[string]*odState

	refusals   map[string]refusal // account (lower) -> last refusal/kick reported by a game server
	charsSaved time.Time
	charsDirty bool

	leasesDirty bool // 0.7.2: an expiry moved; leasesFlushLoop writes it. Starts and ends write immediately.
}

var nameRe = regexp.MustCompile(`^[A-Za-z0-9_]{3,20}$`)

func main() {
	cfgPath := flag.String("config", "portal.json", "config file (created with defaults if missing)")
	upsert := flag.String("upsert-server", "", "admin: add or replace a servers[] entry (JSON file with an \"id\") in the config, then exit")
	resetPw := flag.String("reset-password", "", "admin: give this account a temporary password (asks the running portal), then exit")
	allowChar := flag.String("allow-character", "", "admin: ACCOUNT/CHARACTER - accept this character once more (asks the running portal), then exit")
	showKey := flag.String("shard-key", "", "admin: print the shard_key of this server id from the config, then exit")
	flag.Parse()
	if *showKey != "" {
		raw, err := os.ReadFile(*cfgPath)
		must(err)
		var c Config
		must(json.Unmarshal(raw, &c))
		for _, s := range c.Servers {
			if s.ID == *showKey && s.ShardKey != "" {
				fmt.Println(s.ShardKey)
				return
			}
		}
		os.Exit(3)
	}
	if *allowChar != "" {
		must(adminCharacterCLI(*cfgPath, *allowChar))
		return
	}
	if *resetPw != "" {
		must(adminResetCLI(*cfgPath, *resetPw))
		return
	}
	if *upsert != "" {
		must(upsertServer(*cfgPath, *upsert))
		return
	}
	cfg := loadConfig(*cfgPath)
	p := &Portal{cfg: cfg, accounts: map[string]*Account{}, leases: map[string]*lease{}, shards: map[string]*shardState{}, refusals: map[string]refusal{}}
	must(os.MkdirAll(filepath.Join(cfg.DataDir, "saves"), 0o755))
	p.secret = p.loadSecret()
	p.loadAccounts()
	p.initAllChars()
	p.loadLeases() // before the listener opens, so a session that was live across the restart is honoured on its first request
	go p.charsFlushLoop()
	go p.leasesFlushLoop()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/status", p.handleStatus)
	mux.HandleFunc("POST /api/login", p.handleLogin)
	mux.HandleFunc("POST /api/account/password", p.auth(p.handleChangePassword))
	mux.HandleFunc("POST /api/account/rename", p.auth(p.handleRename))
	mux.HandleFunc("POST /api/admin/reset-password", p.handleAdminReset)
	mux.HandleFunc("POST /api/admin/character", p.handleAdminCharacter)
	mux.HandleFunc("POST /api/session/start", p.auth(p.handleSessionStart))
	mux.HandleFunc("POST /api/session/renew", p.auth(p.handleSessionRenew))
	mux.HandleFunc("POST /api/session/end", p.auth(p.handleSessionEnd))
	mux.HandleFunc("GET /api/save", p.auth(p.handleGetSave))
	mux.HandleFunc("PUT /api/save", p.auth(p.handlePutSave))
	mux.HandleFunc("GET /files/", p.handleFiles)
	mux.HandleFunc("GET /api/manifest", p.handleManifest)
	mux.HandleFunc("POST /api/shard/heartbeat", p.handleShardHeartbeat)
	mux.HandleFunc("GET /api/shard/stash", p.handleGetShardStash)
	mux.HandleFunc("PUT /api/shard/stash", p.handlePutShardStash)
	mux.HandleFunc("POST /api/servers/wake", p.auth(p.handleWake))
	p.startOnDemandLoops()

	if len(cfg.Servers) == 0 && (strings.EqualFold(cfg.GameAddress, "auto") || cfg.GameAddress == "") {
		go p.autoPublicIP()
	}
	for _, s := range cfg.Servers {
		log.Printf("server %q (%s) address=%s heartbeat=%s", s.ID, s.Name, s.GameAddress, map[bool]string{true: "push", false: "file"}[s.HeartbeatFile == ""])
	}
	log.Printf("SiegePortal %s listening on %s (data=%s, game=%s)", version, cfg.Listen, cfg.DataDir, cfg.GameAddress)
	srv := &http.Server{Addr: cfg.Listen, Handler: logRequests(mux), ReadHeaderTimeout: 15 * time.Second}
	log.Fatal(srv.ListenAndServe())
}

// autoPublicIP keeps game_address set to this machine's public IPv4 (home connections change IP).
func (p *Portal) autoPublicIP() {
	c := &http.Client{Timeout: 10 * time.Second}
	for {
		for _, u := range []string{"https://api.ipify.org", "https://checkip.amazonaws.com", "https://ifconfig.me/ip"} {
			resp, err := c.Get(u)
			if err != nil {
				continue
			}
			b, _ := io.ReadAll(io.LimitReader(resp.Body, 64))
			resp.Body.Close()
			ip := net.ParseIP(strings.TrimSpace(string(b)))
			if ip == nil || ip.To4() == nil {
				continue
			}
			p.mu.Lock()
			if p.cfg.GameAddress != ip.String() {
				log.Printf("public IP detected: %s (via %s)", ip, u)
				p.cfg.GameAddress = ip.String()
			}
			p.mu.Unlock()
			break
		}
		time.Sleep(10 * time.Minute)
	}
}

func loadConfig(path string) Config {
	cfg := Config{Listen: ":8440", DataDir: "portal-data", ServerName: "Dungeon Siege MMO", GameAddress: "",
		MaxPlayers: 30, AllowRegistration: true, HeartbeatFile: "", MaxSaveMB: 64, KeepBackups: 20, LeaseSeconds: 300,
		JoinTicketMinutes: 20, JoinNewCharMaxXP: 500, JoinToleranceXP: 1000, JoinTolerancePct: 5}
	b, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		out, _ := json.MarshalIndent(cfg, "", "  ")
		must(os.WriteFile(path, out, 0o644))
		log.Printf("wrote default config %s", path)
		return cfg
	}
	must(err)
	must(json.Unmarshal(b, &cfg))
	if cfg.UpdateDir == "" {
		cfg.UpdateDir = filepath.Join(cfg.DataDir, "files")
	}
	return cfg
}

func must(err error) {
	if err != nil {
		log.Fatal(err)
	}
}

// ---------- persistence ----------

func (p *Portal) accountsPath() string { return filepath.Join(p.cfg.DataDir, "accounts.json") }

func (p *Portal) loadAccounts() {
	b, err := os.ReadFile(p.accountsPath())
	if errors.Is(err, os.ErrNotExist) {
		return
	}
	must(err)
	var list []*Account
	must(json.Unmarshal(b, &list))
	for _, a := range list {
		p.accounts[strings.ToLower(a.Name)] = a
	}
	log.Printf("loaded %d accounts", len(list))
}

// saveAccountsLocked writes atomically. Caller holds p.mu.
func (p *Portal) saveAccountsLocked() error {
	list := make([]*Account, 0, len(p.accounts))
	for _, a := range p.accounts {
		list = append(list, a)
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Name < list[j].Name })
	b, _ := json.MarshalIndent(list, "", "  ")
	return writeAtomic(p.accountsPath(), b)
}

func writeAtomic(path string, data []byte) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func (p *Portal) loadSecret() []byte {
	path := filepath.Join(p.cfg.DataDir, "secret.key")
	if b, err := os.ReadFile(path); err == nil && len(b) >= 32 {
		return b
	}
	b := make([]byte, 32)
	_, _ = rand.Read(b)
	must(os.WriteFile(path, b, 0o600))
	return b
}

// ---------- auth ----------

func hashPassword(pass, salt string) string {
	k, _ := pbkdf2.Key(sha256.New, pass, []byte(salt), 200_000, 32)
	return hex.EncodeToString(k)
}

func (p *Portal) token(name string) string {
	payload := strings.ToLower(name) + "|" + fmt.Sprint(time.Now().Add(90*24*time.Hour).Unix())
	m := hmac.New(sha256.New, p.secret)
	m.Write([]byte(payload))
	return base64.RawURLEncoding.EncodeToString([]byte(payload)) + "." + base64.RawURLEncoding.EncodeToString(m.Sum(nil))
}

func (p *Portal) verify(tok string) (string, bool) {
	parts := strings.Split(tok, ".")
	if len(parts) != 2 {
		return "", false
	}
	payload, err1 := base64.RawURLEncoding.DecodeString(parts[0])
	sig, err2 := base64.RawURLEncoding.DecodeString(parts[1])
	if err1 != nil || err2 != nil {
		return "", false
	}
	m := hmac.New(sha256.New, p.secret)
	m.Write(payload)
	if !hmac.Equal(sig, m.Sum(nil)) {
		return "", false
	}
	f := strings.Split(string(payload), "|")
	var exp int64
	if len(f) != 2 {
		return "", false
	}
	fmt.Sscan(f[1], &exp)
	if time.Now().Unix() > exp {
		return "", false
	}
	return f[0], true
}

type ctxHandler func(w http.ResponseWriter, r *http.Request, acct *Account)

func (p *Portal) auth(h ctxHandler) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		name, ok := p.verify(tok)
		if !ok {
			jsonErr(w, http.StatusUnauthorized, "invalid or expired login, please log in again")
			return
		}
		p.mu.Lock()
		acct := p.accounts[name]
		p.mu.Unlock()
		// A token for a name that was freed by a rename and later registered again must not open the new account.
		if acct != nil && tokenIssued(tok).Before(acct.Created.Add(-time.Second)) {
			acct = nil
		}
		if acct == nil || acct.Banned {
			jsonErr(w, http.StatusForbidden, "account not found or banned")
			return
		}
		h(w, r, acct)
	}
}

// ---------- handlers ----------

func jsonOut(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func jsonErr(w http.ResponseWriter, code int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

func readHeartbeatFile(path string) map[string]string {
	out := map[string]string{"online": "unknown"}
	if path == "" {
		return out
	}
	st, err := os.Stat(path)
	if err != nil {
		out["online"] = "false"
		return out
	}
	b, _ := os.ReadFile(path)
	for _, line := range strings.Split(string(b), "\n") {
		if k, v, ok := strings.Cut(strings.TrimSpace(line), "="); ok {
			out[k] = v
		}
	}
	fresh := time.Since(st.ModTime()) < 60*time.Second
	out["online"] = fmt.Sprint(fresh && out["phase"] == "ingame")
	out["heartbeat_age_s"] = fmt.Sprint(int(time.Since(st.ModTime()).Seconds()))
	return out
}

func (p *Portal) readHeartbeat() map[string]string { return readHeartbeatFile(p.cfg.HeartbeatFile) }

// serverList returns every configured game server with live status. Caller must NOT hold p.mu.
func (p *Portal) serverList() []map[string]any {
	odStates := map[string]string{}
	for _, s := range p.cfg.Servers {
		if s.OnDemand != nil {
			odStates[s.ID], _ = p.onDemandInfo(s.ID)
		}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	var list []map[string]any
	for _, s := range p.cfg.Servers {
		var game map[string]string
		addr := s.GameAddress
		if s.HeartbeatFile != "" {
			game = readHeartbeatFile(s.HeartbeatFile)
		} else if st := p.shards[s.ID]; st != nil {
			game = map[string]string{}
			for k, v := range st.fields {
				game[k] = v
			}
			age := time.Since(st.seen)
			game["heartbeat_age_s"] = fmt.Sprint(int(age.Seconds()))
			game["online"] = fmt.Sprint(age < 60*time.Second && game["phase"] == "ingame")
			if strings.EqualFold(addr, "auto") || addr == "" {
				addr = st.address
			}
		} else {
			game = map[string]string{"online": "false", "phase": "no heartbeat yet"}
		}
		entry := map[string]any{"id": s.ID, "name": s.Name, "region": s.Region, "game_address": addr, "game_port": gp(s.GamePort),
			"max_players": s.MaxPlayers, "online": game["online"] == "true", "players": game["players"], "game": game,
			"send_frequency": sendHz(s.SendFrequency)}
		if s.JoinCode != "" { // whether one is needed, never what it is
			entry["join_code_required"] = true
		}
		if s.VeteranLevel > 0 {
			entry["veteran_level"] = s.VeteranLevel
		}
		if s.HostCharacter != "" {
			entry["host_character"] = s.HostCharacter
		}
		if s.OnDemand != nil {
			entry["on_demand"] = true
			entry["instance_state"] = odStates[s.ID]
			if odStates[s.ID] != "running" { // a stale heartbeat from before a stop must not look joinable
				entry["online"] = false
			}
		}
		list = append(list, entry)
	}
	return list
}

func (p *Portal) checkShardAuth(r *http.Request) (*ServerEntry, bool) {
	id, key := r.Header.Get("X-Shard-Id"), r.Header.Get("X-Shard-Key")
	for i := range p.cfg.Servers {
		if p.cfg.Servers[i].ID == id {
			entry := &p.cfg.Servers[i]
			if entry.ShardKey != "" && hmac.Equal([]byte(key), []byte(entry.ShardKey)) {
				return entry, true
			}
			return nil, false
		}
	}
	return nil, false
}

func sanitizeAccount(s string) string {
	var b strings.Builder
	for _, c := range strings.ToLower(s) {
		if (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' {
			b.WriteRune(c)
		}
	}
	return b.String()
}

// GET /api/shard/stash?account=<name> (X-Shard-Id, X-Shard-Key)
func (p *Portal) handleGetShardStash(w http.ResponseWriter, r *http.Request) {
	if _, ok := p.checkShardAuth(r); !ok {
		jsonErr(w, 403, "unknown shard or bad key")
		return
	}
	acct := sanitizeAccount(r.URL.Query().Get("account"))
	if acct == "" {
		jsonErr(w, 400, "missing account")
		return
	}
	path := filepath.Join(p.cfg.DataDir, "stash", acct+".gas")
	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			w.WriteHeader(http.StatusNotFound)
			return
		}
		jsonErr(w, 500, err.Error())
		return
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}

// PUT /api/shard/stash?account=<name> (X-Shard-Id, X-Shard-Key) body: gas file contents
func (p *Portal) handlePutShardStash(w http.ResponseWriter, r *http.Request) {
	if _, ok := p.checkShardAuth(r); !ok {
		jsonErr(w, 403, "unknown shard or bad key")
		return
	}
	acct := sanitizeAccount(r.URL.Query().Get("account"))
	if acct == "" {
		jsonErr(w, 400, "missing account")
		return
	}
	data, err := io.ReadAll(io.LimitReader(r.Body, 4<<20))
	if err != nil {
		jsonErr(w, 400, "could not read body")
		return
	}
	dir := filepath.Join(p.cfg.DataDir, "stash")
	_ = os.MkdirAll(dir, 0o755)
	tmp := filepath.Join(dir, fmt.Sprintf(".%s.%d.tmp", acct, time.Now().UnixNano()))
	if err := os.WriteFile(tmp, data, 0o644); err != nil {
		jsonErr(w, 500, "write error: "+err.Error())
		return
	}
	dest := filepath.Join(dir, acct+".gas")
	if err := os.Rename(tmp, dest); err != nil {
		jsonErr(w, 500, "rename error: "+err.Error())
		return
	}
	log.Printf("shard stash saved: %s (%d bytes)", acct, len(data))
	jsonOut(w, map[string]any{"ok": true})
}

// POST /api/shard/heartbeat  (X-Shard-Id, X-Shard-Key)  body: {"fields":{...heartbeat.txt...},"address":"1.2.3.4"}
func (p *Portal) handleShardHeartbeat(w http.ResponseWriter, r *http.Request) {
	entry, ok := p.checkShardAuth(r)
	if !ok {
		jsonErr(w, 403, "unknown shard or bad key")
		return
	}
	id := entry.ID
	var body struct {
		Fields  map[string]string `json:"fields"`
		Address string            `json:"address"`
	}
	if err := json.NewDecoder(io.LimitReader(r.Body, 16<<10)).Decode(&body); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	addr := strings.TrimSpace(body.Address)
	if addr == "" || strings.EqualFold(addr, "auto") {
		addr, _, _ = net.SplitHostPort(r.RemoteAddr)
	}
	p.mu.Lock()
	p.shardReportLocked(id, body.Fields)
	prev := p.shards[id]
	p.shards[id] = &shardState{fields: body.Fields, address: addr, seen: time.Now()}
	p.mu.Unlock()
	if prev == nil || prev.fields["phase"] != body.Fields["phase"] {
		log.Printf("shard %s: phase=%s players=%s address=%s", id, body.Fields["phase"], body.Fields["players"], addr)
	}
	jsonOut(w, map[string]any{"ok": true})
}

// GET /api/manifest — every file under update_dir with its SHA-256. The launcher downloads what differs from
// /files/<path> (paths are relative to the player's game folder, "/" separated).
func (p *Portal) handleManifest(w http.ResponseWriter, r *http.Request) {
	type entry struct {
		Path   string `json:"path"`
		Size   int64  `json:"size"`
		SHA256 string `json:"sha256"`
	}
	var files []entry
	root := p.cfg.UpdateDir
	_ = filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(root, path)
		rel = filepath.ToSlash(rel)
		if strings.HasPrefix(d.Name(), ".") {
			return nil
		}
		sum, size, err := p.fileHash(path)
		if err == nil {
			files = append(files, entry{Path: rel, Size: size, SHA256: sum})
		}
		return nil
	})
	sort.Slice(files, func(i, j int) bool { return files[i].Path < files[j].Path })
	jsonOut(w, map[string]any{"portal_version": version, "files": files})
}

var hashCache sync.Map // path -> [modtime, size, sha]

func (p *Portal) fileHash(path string) (string, int64, error) {
	st, err := os.Stat(path)
	if err != nil {
		return "", 0, err
	}
	key := fmt.Sprintf("%s|%d|%d", path, st.ModTime().UnixNano(), st.Size())
	if v, ok := hashCache.Load(key); ok {
		return v.(string), st.Size(), nil
	}
	f, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", 0, err
	}
	sum := hex.EncodeToString(h.Sum(nil))
	hashCache.Store(key, sum)
	return sum, st.Size(), nil
}

func (p *Portal) handleStatus(w http.ResponseWriter, r *http.Request) {
	servers := p.serverList()
	p.mu.Lock()
	addr := p.cfg.GameAddress
	maxPlayers := p.cfg.MaxPlayers
	p.mu.Unlock()
	game := p.readHeartbeat()
	if len(servers) > 0 { // legacy fields (launcher <= 0.2.0) mirror the first server
		addr, _ = servers[0]["game_address"].(string)
		game, _ = servers[0]["game"].(map[string]string)
		if mp, _ := servers[0]["max_players"].(int); mp > 0 {
			maxPlayers = mp
		}
	}
	jsonOut(w, map[string]any{
		"portal_version": version, "server_name": p.cfg.ServerName, "game_address": addr,
		"max_players": maxPlayers, "registration": p.cfg.AllowRegistration, "invite_required": p.cfg.InviteCode != "",
		"game": game, "servers": servers, "time": time.Now().UTC(),
	})
}

func (p *Portal) handleLogin(w http.ResponseWriter, r *http.Request) {
	var req struct{ Name, Password, Invite string }
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	if !nameRe.MatchString(req.Name) {
		jsonErr(w, 400, "account name must be 3-20 letters, digits or _")
		return
	}
	if len(req.Password) < 6 {
		jsonErr(w, 400, "password must be at least 6 characters")
		return
	}
	key := strings.ToLower(req.Name)
	p.mu.Lock()
	defer p.mu.Unlock()
	acct := p.accounts[key]
	created := false
	if acct == nil {
		if !p.cfg.AllowRegistration {
			jsonErr(w, 403, "registration is closed")
			return
		}
		if p.cfg.InviteCode != "" && req.Invite != p.cfg.InviteCode {
			jsonErr(w, 403, "invite code required")
			return
		}
		salt := make([]byte, 16)
		_, _ = rand.Read(salt)
		acct = &Account{Name: req.Name, Salt: hex.EncodeToString(salt), Created: time.Now().UTC(), CharsInit: true}
		acct.Hash = hashPassword(req.Password, acct.Salt)
		p.accounts[key] = acct
		created = true
		log.Printf("account created: %s from %s", req.Name, r.RemoteAddr)
	} else if !hmac.Equal([]byte(hashPassword(req.Password, acct.Salt)), []byte(acct.Hash)) {
		jsonErr(w, 401, "wrong password")
		return
	}
	if acct.Banned {
		jsonErr(w, 403, "account banned")
		return
	}
	acct.LastLogin = time.Now().UTC()
	if err := p.saveAccountsLocked(); err != nil {
		jsonErr(w, 500, "could not store account")
		return
	}
	jsonOut(w, map[string]any{"token": p.token(acct.Name), "name": acct.Name, "created": created,
		"save_rev": acct.SaveRev}) // p.mu held
}

// Session lease: stops two PCs from using (and overwriting) the same online save at the same time.
// joinCodeError says why this session may not start on this server, or "" if it may. The comparison
// ignores case and surrounding spaces: the code is something a player is told and types by hand.
// Servers without a code are unaffected, which is every server that does not set one.
func (p *Portal) joinCodeError(serverID, given string) string {
	for _, s := range p.cfg.Servers {
		if !strings.EqualFold(s.ID, serverID) || s.JoinCode == "" {
			continue
		}
		given = strings.TrimSpace(given)
		switch {
		case strings.EqualFold(given, s.JoinCode):
			return ""
		case given == "":
			return "this server needs a join code"
		default:
			return "that join code is not right for this server"
		}
	}
	return ""
}

func (p *Portal) handleSessionStart(w http.ResponseWriter, r *http.Request, acct *Account) {
	var req struct {
		Machine string
		Force   bool
		Server  string
		Code    string
	}
	_ = json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req)
	// Before the lease, not after: a wrong code must not cost the player the session they already had.
	if msg := p.joinCodeError(req.Server, req.Code); msg != "" {
		log.Printf("session start refused: %s on %s (%s)", acct.Name, req.Server, msg)
		jsonErr(w, 403, msg)
		return
	}
	key := strings.ToLower(acct.Name)
	p.mu.Lock()
	defer p.mu.Unlock()
	if l := p.leases[key]; l != nil && time.Now().Before(l.Expires) && l.Machine != req.Machine && !req.Force {
		jsonErr(w, 409, fmt.Sprintf("this account is already playing on another PC (%s). It frees up %d s after that game closes.", l.Machine, int(time.Until(l.Expires).Seconds())))
		return
	}
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	l := &lease{Session: hex.EncodeToString(b), Machine: req.Machine, Server: req.Server, Expires: time.Now().Add(time.Duration(p.cfg.LeaseSeconds) * time.Second), Started: time.Now()}
	p.leases[key] = l
	delete(p.refusals, key)
	p.flushLeasesLocked()
	ticket := p.ticketLocked(acct, req.Server)
	log.Printf("session start: %s on %s (server %q, join ticket %v)", acct.Name, req.Machine, req.Server, ticket != "")
	jsonOut(w, map[string]any{"session": l.Session, "lease_seconds": p.cfg.LeaseSeconds, "save_rev": acct.SaveRev, "save_sha": acct.SaveSHA, "ticket": ticket})
}

func (p *Portal) checkLease(acct *Account, session string) bool {
	l := p.leases[strings.ToLower(acct.Name)]
	return l != nil && l.Session == session && time.Now().Before(l.Expires)
}

func (p *Portal) handleSessionRenew(w http.ResponseWriter, r *http.Request, acct *Account) {
	session := r.URL.Query().Get("session")
	p.mu.Lock()
	defer p.mu.Unlock()
	if !p.checkLease(acct, session) {
		jsonErr(w, 409, "session expired or taken over by another PC")
		return
	}
	l := p.leases[strings.ToLower(acct.Name)]
	l.Expires = time.Now().Add(time.Duration(p.cfg.LeaseSeconds) * time.Second)
	p.leasesDirty = true
	jsonOut(w, map[string]any{"ok": true, "ticket": p.ticketLocked(acct, l.Server), "refused": p.refusalTextLocked(acct, l.Started)})
}

func (p *Portal) handleSessionEnd(w http.ResponseWriter, r *http.Request, acct *Account) {
	session := r.URL.Query().Get("session")
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.checkLease(acct, session) {
		delete(p.leases, strings.ToLower(acct.Name))
		p.flushLeasesLocked()
		log.Printf("session end: %s", acct.Name)
	}
	jsonOut(w, map[string]any{"ok": true})
}

func (p *Portal) saveDir(acct *Account) string {
	return filepath.Join(p.cfg.DataDir, "saves", strings.ToLower(acct.Name))
}

func (p *Portal) handleGetSave(w http.ResponseWriter, r *http.Request, acct *Account) {
	path := filepath.Join(p.saveDir(acct), "current.zip")
	f, err := os.Open(path)
	if errors.Is(err, os.ErrNotExist) {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if err != nil {
		jsonErr(w, 500, "read error")
		return
	}
	defer f.Close()
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("X-Save-Rev", fmt.Sprint(acct.SaveRev))
	w.Header().Set("X-Save-SHA256", acct.SaveSHA)
	_, _ = io.Copy(w, f)
}

func (p *Portal) handlePutSave(w http.ResponseWriter, r *http.Request, acct *Account) {
	session := r.URL.Query().Get("session")
	body, err := io.ReadAll(io.LimitReader(r.Body, int64(p.cfg.MaxSaveMB)<<20+1))
	if err != nil || len(body) == 0 {
		jsonErr(w, 400, "empty upload")
		return
	}
	if len(body) > p.cfg.MaxSaveMB<<20 {
		jsonErr(w, 413, "save too large")
		return
	}
	if !bytes.HasPrefix(body, []byte("PK")) {
		jsonErr(w, 400, "not a zip")
		return
	}
	sum := sha256.Sum256(body)
	sha := hex.EncodeToString(sum[:])

	p.mu.Lock()
	defer p.mu.Unlock()
	// 0.7.2: a save is refused only when someone else is demonstrably holding this account. "No lease" and
	// "someone else's lease" were one check until a restart wiped every lease and turned the first case into
	// silent data loss (RE-39). They are different situations and only the second one justifies a refusal.
	if l := p.leases[strings.ToLower(acct.Name)]; l == nil || !time.Now().Before(l.Expires) {
		log.Printf("save accepted without a live lease: %s (%d bytes) - no other session holds this account",
			acct.Name, len(body))
	} else if l.Session != session {
		jsonErr(w, 409, "this account is in a game on another PC; save NOT uploaded")
		return
	}
	if sha == acct.SaveSHA {
		jsonOut(w, map[string]any{"ok": true, "unchanged": true, "save_rev": acct.SaveRev})
		return
	}
	dir := p.saveDir(acct)
	if err := os.MkdirAll(filepath.Join(dir, "history"), 0o755); err != nil {
		jsonErr(w, 500, "storage error")
		return
	}
	cur := filepath.Join(dir, "current.zip")
	if _, err := os.Stat(cur); err == nil {
		hist := filepath.Join(dir, "history", fmt.Sprintf("rev%06d_%s.zip", acct.SaveRev, acct.SaveTime.Format("20060102-150405")))
		_ = os.Rename(cur, hist)
		p.pruneHistory(filepath.Join(dir, "history"))
	}
	if err := writeAtomic(cur, body); err != nil {
		jsonErr(w, 500, "storage error")
		return
	}
	acct.SaveRev++
	acct.SaveSHA = sha
	acct.SaveTime = time.Now().UTC()
	_ = p.saveAccountsLocked()
	// Only the session that owns the lease extends it. Accepting a save from a launcher with no lease must not
	// invent one, or a second PC starting up would find the account held by nobody in particular.
	if l := p.leases[strings.ToLower(acct.Name)]; l != nil && l.Session == session {
		l.Expires = time.Now().Add(time.Duration(p.cfg.LeaseSeconds) * time.Second)
		p.leasesDirty = true
	}
	log.Printf("save uploaded: %s rev=%d %d bytes", acct.Name, acct.SaveRev, len(body))
	jsonOut(w, map[string]any{"ok": true, "save_rev": acct.SaveRev})
}

func (p *Portal) pruneHistory(dir string) {
	entries, err := os.ReadDir(dir)
	if err != nil || len(entries) <= p.cfg.KeepBackups {
		return
	}
	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)
	for _, n := range names[:len(names)-p.cfg.KeepBackups] {
		_ = os.Remove(filepath.Join(dir, n))
	}
}

// /files/ serves launcher/client files from <data>/files (for future auto-update).
func (p *Portal) handleFiles(w http.ResponseWriter, r *http.Request) {
	root := p.cfg.UpdateDir
	http.StripPrefix("/files/", http.FileServer(http.Dir(root))).ServeHTTP(w, r)
}

func logRequests(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		h.ServeHTTP(w, r)
		host, _, _ := net.SplitHostPort(r.RemoteAddr)
		if r.URL.Path != "/api/session/renew" && r.URL.Path != "/api/shard/heartbeat" {
			log.Printf("%s %s %s (%s)", host, r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
		}
	})
}

func gp(p int) int {
	if p == 0 {
		return 2350
	}
	return p
}

// upsertServer edits portal.json in place: the servers[] entry with the same id is replaced, otherwise appended.
// Other settings are kept. A backup is written next to the config.
func upsertServer(cfgPath, entryPath string) error {
	raw, err := os.ReadFile(cfgPath)
	if err != nil {
		return err
	}
	var cfg map[string]any
	if err := json.Unmarshal(raw, &cfg); err != nil {
		return fmt.Errorf("%s: %v", cfgPath, err)
	}
	eb, err := os.ReadFile(entryPath)
	if err != nil {
		return err
	}
	var entry map[string]any
	if err := json.Unmarshal(eb, &entry); err != nil {
		return fmt.Errorf("%s: %v", entryPath, err)
	}
	id, _ := entry["id"].(string)
	if id == "" {
		return errors.New("entry has no id")
	}
	var check ServerEntry // validate field types against the real struct
	if err := json.Unmarshal(eb, &check); err != nil {
		return fmt.Errorf("entry does not match the server format: %v", err)
	}
	servers, _ := cfg["servers"].([]any)
	replaced := false
	for i, s := range servers {
		if m, ok := s.(map[string]any); ok && m["id"] == id {
			servers[i] = entry
			replaced = true
		}
	}
	if !replaced {
		servers = append(servers, entry)
	}
	cfg["servers"] = servers
	out, _ := json.MarshalIndent(cfg, "", "  ")
	if err := os.WriteFile(cfgPath+".bak-"+time.Now().Format("20060102-150405"), raw, 0o600); err != nil {
		return err
	}
	if err := writeAtomic(cfgPath, out); err != nil {
		return err
	}
	fmt.Printf("server %q %s in %s\n", id, map[bool]string{true: "replaced", false: "added"}[replaced], cfgPath)
	return nil
}

// tokenIssued returns when a (verified) token was issued: tokens expire 90 days after issue.
func tokenIssued(tok string) time.Time {
	parts := strings.Split(tok, ".")
	payload, _ := base64.RawURLEncoding.DecodeString(parts[0])
	f := strings.Split(string(payload), "|")
	var exp int64
	if len(f) == 2 {
		fmt.Sscan(f[1], &exp)
	}
	return time.Unix(exp, 0).Add(-90 * 24 * time.Hour)
}
