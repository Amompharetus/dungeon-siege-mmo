package main

import (
	"archive/zip"
	"bytes"
	"compress/zlib"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/csv"
	"encoding/hex"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"log"
	"math"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

func getEnv(key, defaultVal string) string {
	if val := os.Getenv(key); val != "" {
		return val
	}
	return defaultVal
}

var (
	portalURL     = getEnv("PORTAL_URL", "http://127.0.0.1:8440")
	portalDir     = getEnv("PORTAL_DIR", filepath.Join("..", "ds1mmo_central"))
	webDir        = getEnv("WEB_DIR", ".")
	tankdumpBin   = getEnv("TANKDUMP_BIN", "/usr/local/bin/tankdump")
	listenAddr    = getEnv("LISTEN_ADDR", "127.0.0.1:8088")
	gmCookieName  = "ds1mmo_gm_session"
	awsCredsFile  = getEnv("AWS_CREDS_FILE", "aws.csv")
	awsEURegion   = getEnv("AWS_EU_REGION", "eu-central-1")
	awsEUInstance = getEnv("AWS_EU_INSTANCE", "")
	awsNARegion   = getEnv("AWS_NA_REGION", "us-east-1")
	awsNAInstance = getEnv("AWS_NA_INSTANCE", "")
)

var (
	gmUsersMu sync.RWMutex
	gmUsers   = map[string]bool{}
)

func loadGMUsers() {
	gmUsersMu.Lock()
	defer gmUsersMu.Unlock()
	gmUsers = make(map[string]bool)

	// 1. From environment variable (comma-separated list)
	if envList := os.Getenv("DS1MMO_GM_USERS"); envList != "" {
		for _, u := range strings.Split(envList, ",") {
			u = strings.ToLower(strings.TrimSpace(u))
			if u != "" {
				gmUsers[u] = true
			}
		}
	}

	// 2. From data/gm_users.json (if present)
	gmFile := filepath.Join(webDir, "data", "gm_users.json")
	if data, err := os.ReadFile(gmFile); err == nil {
		var list []string
		if err := json.Unmarshal(data, &list); err == nil {
			for _, u := range list {
				u = strings.ToLower(strings.TrimSpace(u))
				if u != "" {
					gmUsers[u] = true
				}
			}
		}
	}
}

func isAuthorizedGM(username string) bool {
	gmUsersMu.RLock()
	defer gmUsersMu.RUnlock()
	return gmUsers[strings.ToLower(strings.TrimSpace(username))]
}

type CachedChar struct {
	Name        string `json:"name"`
	Class       string `json:"class"`
	Race        string `json:"race"`
	Level       int    `json:"level"`
	RawLevel    int    `json:"raw_level"`
	HasPartyGas bool   `json:"has_party_gas"`
	SaveSHA     string `json:"save_sha"`
	EntrySig    string `json:"entry_sig"`
	StartLoc    string `json:"start_loc"`
	MapAreaId   string `json:"map_area_id"`
	SaveTime    int64  `json:"save_time"`
}

type PlayerInfo struct {
	AccountName   string `json:"account_name"`
	CharacterName string `json:"character_name"`
	Level         int    `json:"level"`
	XP            int64  `json:"xp"`
	Race          string `json:"race"`
	Class         string `json:"class"`
	Location      string `json:"location"`
	MapAreaId     string `json:"map_area_id"`
	Region        string `json:"region"`
	ServerID      string `json:"server_id"`
}

func formatTownName(s string) string {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "elddim":
		return "Elddim"
	case "crystwind":
		return "Crystwind"
	case "fallraen":
		return "Fallraen"
	case "meren":
		return "Meren"
	case "lang":
		return "Lang"
	case "quillrabe":
		return "Quillrabe"
	case "hiroth":
		return "Hiroth"
	case "grescal":
		return "Grescal"
	case "fortress", "neener_emporium", "neeners_emporium", "neener's emporium":
		return "Neener's Emporium"
	default:
		s = strings.TrimSpace(s)
		if len(s) > 0 {
			return strings.ToUpper(s[:1]) + strings.ToLower(s[1:])
		}
		return "Elddim"
	}
}

func regionToArea(region string) (string, string) {
	switch strings.ToLower(strings.TrimSpace(region)) {
	// Dornek's Stone Quarry & Sulfur Tunnels
	case "quarry":
		return "Dornek's Stone Quarry", "dorneks_stone_quarry"
	case "sr2":
		return "Sulfur Tunnels", "sulfur_tunnels"

	// Mt. Elspen
	case "sf_r1":
		return "Mt. Elspen Forest", "mt_elspen_forest"
	case "newbie_dun":
		return "Ancient Temple", "mt_elspen_forest"

	// Great Northern Forest & Hovart
	case "mega_forest", "mega_forest_r2", "mega_forest_r3":
		return "Great Northern Forest", "great_northern_forest"
	case "gd_r1":
		return "Hovart's Folly", "hovart_marsh"
	case "sd_r1", "crypts2sd", "abc_r1":
		return "Ancient Crypt", "iliarth_divide"

	// Elddim Region
	case "town_center", "elddim":
		return "Elddim", "elddim"
	case "farmland_and_chapel":
		return "Elddim Lowlands", "elddim_lowlands"
	case "orchard_cellar":
		return "Orchard Cellar", "elddim"
	case "path2mines":
		return "Elddim Glade", "elddim_glade"

	// Lang Region
	case "swamp_town", "lang":
		return "Lang", "lang"
	case "ds_r2", "swamp2gi":
		return "Lang Mire", "lang_mire"
	case "gi_a", "gi_a_r2":
		return "Goblin Warrens", "goblin_warrens"
	case "of_r1":
		return "Redwood Gap", "redwood_gap"
	case "wd_r1":
		return "Flooded Sanctuary", "redwood_gap"
	case "md_r1", "md_r2":
		return "Pit of Despair", "redwood_gap"

	// Iliarth / Crystwind
	case "river_forest", "river_forest_r2":
		return "Iliarth Valley", "iliarth_valley"
	case "path2crypts":
		return "Iliarth Divide", "iliarth_divide"
	case "river_canyon":
		return "Iliarth Canyon", "iliarth_canyon"
	case "grs_dry_hard_r1":
		return "Iliarth Falls", "iliarth_falls"
	case "mine_town", "crystwind":
		return "Crystwind", "crystwind"
	case "mines", "mines3":
		return "Crystwind Mines", "crystwind_mines"
	case "mines2":
		return "Crystwind Old Mines", "crystwind_old_mines"
	case "fd_r4":
		return "Fury Den", "fury_den"

	// Meren Coast & Cloud Forest
	case "ocean", "meren":
		return "Meren", "meren"
	case "sr_r5", "sno_r1":
		return "Meren Coast", "meren_coast"
	case "dark_forest", "dark_forest_r2", "df2ds":
		return "Cloud Forest", "cloud_forest"

	// Fallraen & Frozen Reaches
	case "snow_town", "fallraen":
		return "Fallraen", "fallraen"
	case "path2ice", "path2ice_r2":
		return "Mt. Utrae Forest", "mt_utrae_forest"
	case "sf_r2":
		return "Fallraen Forest", "fallraen_forest"
	case "ice_cave", "ice_cave_2":
		return "Glacial Caverns", "glacial_caverns"
	case "id_r2":
		return "Crystal Caverns", "mt_elspen_forest"

	// Quillrabe Canyons & Forgotten Mesas
	case "des_r1", "quillrabe":
		return "Quillrabe", "quillrabe"
	case "des_r2", "grs2des":
		return "Quillrabe Canyons", "quillrabe_canyons"
	case "gp_r1", "gp_r2":
		return "Forgotten Mesas", "quillrabe_canyons"

	// Hiroth Region
	case "castle_town", "hiroth":
		return "Hiroth", "hiroth"
	case "castle":
		return "Castle Hiroth", "castle_hiroth"
	case "castle_dun":
		return "Hiroth Mausoleum", "hiroth_mausoleum"
	case "dragon", "dragon_r2", "dl_r1":
		return "Drake Nest", "drake_nest"

	// Desert & Grescal
	case "desert":
		return "Mesa Desert", "mesa_desert"
	case "oasis", "grescal":
		return "Grescal", "grescal"
	case "oasis_cave":
		return "Grescal Oasis", "grescal"
	case "dunes", "dunes2", "eg_seck_dungeon", "endless_dunes", "the endless dunes", "endless dunes":
		return "Endless Desert", "endless_desert"
	case "dunes2hades", "hades":
		return "Volcanic Caverns", "volcanic_caverns"
	case "dunes2island", "island":
		return "Eastern Island", "eastern_island"

	// Central Hub
	case "new_hub", "hub_quest_dun", "hub":
		return "Utraean Basilicus", "utraean_basilicus"

	// Legends of Aranna Expansion (accessible via Grescal Displacer)
	case "fortress", "neener_emporium", "neeners_emporium", "neener's emporium":
		return "Neener's Emporium", "neeners_emporium"
	case "ramparts", "the ramparts":
		return "The Ramparts", "ramparts"
	case "a8_r1_ramparts", "stronghold gate", "the stronghold gate":
		return "Stronghold Gate", "stronghold_gate"
	case "a8_r1b_secretcave", "z_a8_r1b_to_a9_r2a", "stronghold tunnel", "the stronghold tunnel":
		return "Stronghold Tunnel", "stronghold_tunnel"
	case "stronghold", "zaurask stronghold", "the zaurask stronghold", "a8_r2_fortress", "the stronghold":
		return "Zaurask Stronghold", "zaurask_stronghold"
	case "a8_r2b_fortress", "inner stronghold", "the inner stronghold":
		return "Inner Stronghold", "inner_stronghold"
	case "a8_r3_tunnels", "stronghold caverns", "the stronghold caverns":
		return "Stronghold Caverns", "stronghold_caverns"
	case "a9_r1_mesa", "great clock wastelands", "the great clock wastelands":
		return "Great Clock Wastelands", "clock_wastelands"
	case "a9_r2_cave", "tunnels of the great clock", "the tunnels of the great clock":
		return "Tunnels of the Great Clock", "clock_tunnels"
	case "clock", "lands of the great clock", "the lands of the great clock":
		return "Lands of the Great Clock", "clock_wastelands"
	case "a9_r2a_clockdungeon", "halls of the great clock", "the halls of the great clock":
		return "Halls of the Great Clock", "clock_halls"
	case "a9_r3_clockroom", "great clock", "the great clock":
		return "The Great Clock", "great_clock"

	default:
		clean := strings.TrimSpace(region)
		if clean == "" {
			return "Elddim", "elddim"
		}
		parts := strings.Split(clean, "_")
		for i, p := range parts {
			if len(p) > 0 {
				parts[i] = strings.ToUpper(p[:1]) + strings.ToLower(p[1:])
			}
		}
		return strings.Join(parts, " "), strings.ToLower(clean)
	}
}



type ServerStatus struct {
	ID           string       `json:"id"`
	Name         string       `json:"name"`
	Region       string       `json:"region"`
	Online       bool         `json:"online"`
	PlayersCount int          `json:"players_count"`
	MaxPlayers   int          `json:"max_players"`
	Address      string       `json:"address"`
	Port         int          `json:"port"`
	FPS          string       `json:"fps"`
	Uptime       string       `json:"uptime"`
	Players      []PlayerInfo `json:"players"`
}

type LadderCharacter struct {
	AccountName   string `json:"account_name"`
	CharacterName string `json:"character_name"`
	Level         int    `json:"level"`
	Race          string `json:"race"`
	Class         string `json:"class"`
	XP            int64  `json:"xp"`
	LastSeen      string `json:"last_seen,omitempty"`
	LastSeenTime  int64  `json:"last_seen_time,omitempty"`
	IsOnline      bool   `json:"is_online"`
	OnlineShard   string `json:"online_shard,omitempty"`
}

type UserCharacter struct {
	Name  string `json:"name"`
	Race  string `json:"race"`
	Class string `json:"class"`
	Level int    `json:"level"`
}

type WebSession struct {
	Token     string    `json:"token"`
	Username  string    `json:"username"`
	IsGM      bool      `json:"is_gm"`
	ExpiresAt time.Time `json:"expires_at"`
}

type RealmStatus struct {
	ServerName  string            `json:"server_name"`
	TotalOnline int               `json:"total_online"`
	MapName     string            `json:"map_name"`
	TickRate    string            `json:"tick_rate"`
	NA          ServerStatus      `json:"na"`
	EU          ServerStatus      `json:"eu"`
	Ladder      []LadderCharacter `json:"ladder"`
	Updated     time.Time         `json:"updated"`
}

type Account struct {
	Name      string              `json:"name"`
	SaveSHA   string              `json:"save_sha"`
	SaveTime  time.Time           `json:"save_time"`
	LastLogin time.Time           `json:"last_login"`
	Chars     map[string]*CharRec `json:"chars"`
}

type CharRec struct {
	Name    string    `json:"name"`
	XP      int64     `json:"xp"`
	Updated time.Time `json:"updated"`
}

type StoredAccount struct {
	Name string `json:"name"`
	Salt string `json:"salt"`
	Hash string `json:"hash"`
}

// Shared, because http.Transport pools connections and must not be rebuilt per request.
var (
	awsHTTP = &http.Client{Timeout: 8 * time.Second}

	portalHTTP = &http.Client{
		Timeout: 3 * time.Second,
		Transport: &http.Transport{
			MaxIdleConns:        16,
			MaxIdleConnsPerHost: 8,
			IdleConnTimeout:     30 * time.Second,
		},
	}
)

// One rebuild at a time, shared by everyone waiting. The underlying data moves at heartbeat speed, so a few
// seconds of staleness is invisible, while rebuilding per request is not: it opens every account's save.
type cachedBuild[T any] struct {
	mu    sync.Mutex
	val   T
	at    time.Time
	ttl   time.Duration
	build func() T
}

func (c *cachedBuild[T]) get() T {
	c.mu.Lock()
	defer c.mu.Unlock()
	if time.Since(c.at) < c.ttl {
		return c.val
	}
	c.val, c.at = c.build(), time.Now()
	return c.val
}

var (
	// Unified 3-second snapshot shared by Realms, World Map, and Ladder so levels, classes,
	// ranks, XP, locations, and online badges stay 100% synchronized in real time.
	realmCache = &cachedBuild[RealmStatus]{ttl: 3 * time.Second, build: buildRealmStatus}
)

func getRealmStatus() RealmStatus {
	return realmCache.get()
}

func getLadderCharacters() []LadderCharacter {
	return getRealmStatus().Ladder
}

var (
	cacheMu      sync.RWMutex
	charCache    = make(map[string]CachedChar)
	gmSessionsMu sync.RWMutex
	gmSessions   = make(map[string]string) // token -> username
	sessionsMu   sync.RWMutex
	sessions     = make(map[string]WebSession)
	sessionsFile = filepath.Join(webDir, "sessions.json")

	classRe      = regexp.MustCompile(`(?i)actor_class\s*=\s*"([^"]+)"`)
	uberLvlRe    = regexp.MustCompile(`(?i)uber_level\s*=\s*([0-9.]+)`)
	screenNmRe   = regexp.MustCompile(`(?i)screen_name\s*=\s*"([^"]+)"`)
	templateRe   = regexp.MustCompile(`(?i)template_name\s*=\s*([^;\r\n]+);`)
	startLocRe   = regexp.MustCompile(`(?i)multiplayer_world\s*=\s*"?([a-zA-Z0-9_' -]+)"?`)
	regionRe       = regexp.MustCompile(`(?i)region\s*=\s*([a-zA-Z0-9_]+)`)
	teleportNodeRe = regexp.MustCompile(`(?i)teleport\s*=\s*[^:;\r\n]*:[^:;\r\n]*:[^,]+,[^,]+,([^,]+),(0x[0-9a-fA-F]+)\s*;`)
	saveTimeRe     = regexp.MustCompile(`(?i)\bsave_time\s*=\s*([0-9]+)`)

	// Neener's Emporium shares region = a8_r2_fortress with Zaurask Stronghold;
	// distinguish them by the saved teleport node inside the faction foyer/annex.
	neenerEmporiumNodes = map[string]bool{
		"0x01584f8f": true, "0x099f9811": true, "0x17012ff4": true, "0x24601e22": true,
		"0x42d5c89e": true, "0x45ec90be": true, "0x59996a3e": true, "0x5d183cc1": true,
		"0x73018fe1": true, "0x87144d12": true, "0x8d4db2ac": true, "0x9c9f2da6": true,
		"0x9e8a2dcd": true, "0xa0c902d7": true, "0xb18e9c00": true, "0xb2f00001": true,
		"0xb2f00002": true, "0xb2f00003": true, "0xb2f00004": true, "0xb2f00005": true,
		"0xb2f00006": true, "0xb2f00007": true, "0xb2f00008": true, "0xb2f00009": true,
		"0xb2f0000a": true, "0xb2f0000b": true, "0xb2f0000c": true, "0xb2f0000d": true,
		"0xb2f0000e": true, "0xb2f0000f": true, "0xb2f00010": true, "0xb2f00011": true,
		"0xb2f00012": true, "0xb2f00013": true, "0xb2f00014": true, "0xb2f00015": true,
		"0xb2f00016": true, "0xb2f00017": true, "0xb2f00018": true, "0xb2f00019": true,
		"0xbc135dde": true, "0xc3f55452": true, "0xcc4374aa": true, "0xce480eca": true,
		"0xce8b49e4": true, "0xdff38506": true, "0xe385ac87": true, "0xe409a4db": true,
		"0xef39b9aa": true, "0xf9dd8d8e": true,
	}
)

func resolveFortressRegion(infoStr string) string {
	tm := teleportNodeRe.FindStringSubmatch(infoStr)
	if len(tm) > 2 {
		node := strings.ToLower(strings.TrimSpace(tm[2]))
		if neenerEmporiumNodes[node] {
			return "neener_emporium"
		}
		if node == "0x7e13bba2" {
			if lz, err := strconv.ParseFloat(strings.TrimSpace(tm[1]), 64); err == nil && lz > 0.13 {
				return "neener_emporium"
			}
		}
	}
	return "a8_r2_fortress"
}

var oasisEndlessDunesNodes = map[string]bool{
	"0xb0760001": true,
	"0xb0760002": true,
	"0x45251ca5": true,
	"0x0b564081": true,
	"0x199a544b": true,
	"0x4a87e83e": true,
	"0x6b3c29f9": true,
	"0x7b8430e1": true,
	"0x7dbea029": true,
	"0xa58b325b": true,
	"0xa5a825cd": true,
	"0xb74b6738": true,
	"0xc2b31546": true,
	"0xd3184fc9": true,
	"0xef4617cd": true,
	"0xf8f76bd9": true,
	"0x664556bd": true,
	"0x8247bce7": true,
	"0xaae7f3fa": true,
	"0xc255164c": true,
}

func resolveOasisRegion(infoStr string) string {
	tm := teleportNodeRe.FindStringSubmatch(infoStr)
	if len(tm) > 2 {
		node := strings.ToLower(strings.TrimSpace(tm[2]))
		if oasisEndlessDunesNodes[node] {
			return "dunes"
		}
	}
	return "oasis"
}

func templateToRace(tpl string) string {
	tpl = strings.ToLower(strings.TrimSpace(tpl))
	switch {
	case strings.Contains(tpl, "halfgiant") || strings.Contains(tpl, "half_giant"):
		return "Half-Giant"
	case strings.Contains(tpl, "dwarf"):
		return "Dwarf"
	case strings.Contains(tpl, "skeleton"):
		return "Skeleton"
	case strings.Contains(tpl, "utraean"):
		return "Utraean"
	case strings.Contains(tpl, "farmboy") || strings.Contains(tpl, "farmgirl") || strings.Contains(tpl, "human") || strings.HasPrefix(tpl, "pm_fb") || strings.HasPrefix(tpl, "pm_fg") || strings.Contains(tpl, "hero"):
		return "Human"
	default:
		if tpl == "" {
			return "Human"
		}
		parts := strings.Split(tpl, "_")
		for i, p := range parts {
			if len(p) > 0 {
				parts[i] = strings.ToUpper(p[:1]) + strings.ToLower(p[1:])
			}
		}
		return strings.Join(parts, " ")
	}
}

func loadSessions() {
	sessionsMu.Lock()
	defer sessionsMu.Unlock()
	data, err := os.ReadFile(sessionsFile)
	if err != nil {
		if !os.IsNotExist(err) {
			log.Printf("[Sessions] Error reading sessions file: %v", err)
		}
		return
	}
	var list []WebSession
	if err := json.Unmarshal(data, &list); err != nil {
		log.Printf("[Sessions] Error parsing sessions file: %v", err)
		return
	}
	now := time.Now()
	for _, s := range list {
		if s.ExpiresAt.After(now) {
			sessions[s.Token] = s
			if s.IsGM {
				gmSessionsMu.Lock()
				gmSessions[s.Token] = s.Username
				gmSessionsMu.Unlock()
			}
		}
	}
	log.Printf("[Sessions] Loaded %d active persistent sessions from disk", len(sessions))
}

func saveSessions() {
	sessionsMu.RLock()
	var list []WebSession
	now := time.Now()
	for _, s := range sessions {
		if s.ExpiresAt.After(now) {
			list = append(list, s)
		}
	}
	sessionsMu.RUnlock()
	data, err := json.MarshalIndent(list, "", "  ")
	if err == nil {
		if err := os.WriteFile(sessionsFile, data, 0644); err != nil {
			log.Printf("[Sessions] Error saving sessions to disk: %v", err)
		}
	}
}

func getSession(r *http.Request) (WebSession, bool) {
	token := ""
	if c, err := r.Cookie("ds1mmo_session"); err == nil && c.Value != "" {
		token = c.Value
	} else if c, err := r.Cookie(gmCookieName); err == nil && c.Value != "" {
		token = c.Value
	}
	if token == "" {
		return WebSession{}, false
	}
	sessionsMu.RLock()
	defer sessionsMu.RUnlock()
	s, ok := sessions[token]
	if !ok || s.ExpiresAt.Before(time.Now()) {
		return WebSession{}, false
	}
	return s, true
}

func createSession(w http.ResponseWriter, username string) (string, WebSession) {
	token := generateToken(32)
	isGM := isAuthorizedGM(username)
	sess := WebSession{
		Token:     token,
		Username:  username,
		IsGM:      isGM,
		ExpiresAt: time.Now().Add(30 * 24 * time.Hour), // 30-day persistent session
	}
	sessionsMu.Lock()
	sessions[token] = sess
	sessionsMu.Unlock()

	if isGM {
		gmSessionsMu.Lock()
		gmSessions[token] = username
		gmSessionsMu.Unlock()
	}

	saveSessions()

	http.SetCookie(w, &http.Cookie{
		Name:     "ds1mmo_session",
		Value:    token,
		Path:     "/",
		MaxAge:   86400 * 30,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
	})
	http.SetCookie(w, &http.Cookie{
		Name:     gmCookieName,
		Value:    token,
		Path:     "/",
		MaxAge:   86400 * 30,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
	})
	return token, sess
}

func removeSession(w http.ResponseWriter, r *http.Request) {
	token := ""
	if c, err := r.Cookie("ds1mmo_session"); err == nil && c.Value != "" {
		token = c.Value
	} else if c, err := r.Cookie(gmCookieName); err == nil && c.Value != "" {
		token = c.Value
	}
	if token != "" {
		sessionsMu.Lock()
		delete(sessions, token)
		sessionsMu.Unlock()

		gmSessionsMu.Lock()
		delete(gmSessions, token)
		gmSessionsMu.Unlock()

		saveSessions()
	}

	http.SetCookie(w, &http.Cookie{
		Name:     "ds1mmo_session",
		Value:    "",
		Path:     "/",
		MaxAge:   -1,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
	})
	http.SetCookie(w, &http.Cookie{
		Name:     gmCookieName,
		Value:    "",
		Path:     "/",
		MaxAge:   -1,
		HttpOnly: true,
		SameSite: http.SameSiteLaxMode,
	})
}

func generateToken(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

func getGMSession(r *http.Request) (string, bool) {
	sess, ok := getSession(r)
	if !ok || !sess.IsGM {
		return "", false
	}
	return sess.Username, true
}

func pbkdf2SHA256(password, salt []byte, iter, keyLen int) []byte {
	prf := hmac.New(sha256.New, password)
	hashLen := prf.Size()
	numBlocks := (keyLen + hashLen - 1) / hashLen
	var buf [4]byte
	dk := make([]byte, 0, numBlocks*hashLen)
	u := make([]byte, hashLen)
	t := make([]byte, hashLen)

	for block := 1; block <= numBlocks; block++ {
		prf.Reset()
		prf.Write(salt)
		buf[0] = byte(block >> 24)
		buf[1] = byte(block >> 16)
		buf[2] = byte(block >> 8)
		buf[3] = byte(block)
		prf.Write(buf[:])
		copy(u, prf.Sum(nil))
		copy(t, u)

		for i := 2; i <= iter; i++ {
			prf.Reset()
			prf.Write(u)
			copy(u, prf.Sum(nil))
			for j := 0; j < hashLen; j++ {
				t[j] ^= u[j]
			}
		}
		dk = append(dk, t...)
	}
	return dk[:keyLen]
}

func verifyAccountPassword(username, password string) (bool, string, error) {
	accountsPath := filepath.Join(portalDir, "portal-data", "accounts.json")
	data, err := os.ReadFile(accountsPath)
	if err != nil {
		return false, "", fmt.Errorf("read accounts file: %w", err)
	}

	var accounts []StoredAccount
	if err := json.Unmarshal(data, &accounts); err != nil {
		return false, "", fmt.Errorf("parse accounts file: %w", err)
	}

	for _, a := range accounts {
		if strings.EqualFold(a.Name, username) {
			calcHash := hex.EncodeToString(pbkdf2SHA256([]byte(password), []byte(a.Salt), 200_000, 32))
			if hmac.Equal([]byte(strings.ToLower(calcHash)), []byte(strings.ToLower(a.Hash))) {
				return true, a.Name, nil
			}
			return false, a.Name, nil
		}
	}
	return false, "", nil
}

type awsCreds struct{ id, secret string }

func loadAWSCreds(path string) (awsCreds, error) {
	f, err := os.Open(path)
	if err != nil {
		return awsCreds{}, err
	}
	defer f.Close()
	rows, err := csv.NewReader(f).ReadAll()
	if err != nil {
		return awsCreds{}, err
	}
	for _, r := range rows {
		if len(r) < 2 {
			continue
		}
		id := strings.TrimSpace(strings.TrimPrefix(r[0], "\ufeff"))
		if strings.HasPrefix(id, "AKIA") || strings.HasPrefix(id, "ASIA") {
			return awsCreds{id: id, secret: strings.TrimSpace(r[1])}, nil
		}
	}
	return awsCreds{}, errors.New("no access key row found")
}

func awsEscape(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		if (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' {
			b.WriteByte(c)
		} else {
			fmt.Fprintf(&b, "%%%02X", c)
		}
	}
	return b.String()
}

func hmacSHA(key []byte, data string) []byte {
	h := hmac.New(sha256.New, key)
	h.Write([]byte(data))
	return h.Sum(nil)
}

func signV4(creds awsCreds, region, service, host, path string, params map[string]string, t time.Time) (query, authz, amzDate string) {
	dateStr := t.UTC().Format("20060102")
	amzDate = t.UTC().Format("20060102T150405Z")

	keys := make([]string, 0, len(params))
	for k := range params {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var qb strings.Builder
	for i, k := range keys {
		if i > 0 {
			qb.WriteByte('&')
		}
		qb.WriteString(awsEscape(k))
		qb.WriteByte('=')
		qb.WriteString(awsEscape(params[k]))
	}
	query = qb.String()

	emptyPayloadHash := sha256.Sum256(nil)
	canonicalReq := "GET\n" + path + "\n" + query + "\nhost:" + host + "\nx-amz-date:" + amzDate + "\n\nhost;x-amz-date\n" + hex.EncodeToString(emptyPayloadHash[:])
	reqHash := sha256.Sum256([]byte(canonicalReq))

	scope := dateStr + "/" + region + "/" + service + "/aws4_request"
	stringToSign := "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + hex.EncodeToString(reqHash[:])

	kDate := hmacSHA([]byte("AWS4"+creds.secret), dateStr)
	kRegion := hmacSHA(kDate, region)
	kService := hmacSHA(kRegion, service)
	kSigning := hmacSHA(kService, "aws4_request")
	signature := hex.EncodeToString(hmacSHA(kSigning, stringToSign))

	authz = fmt.Sprintf("AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-date, Signature=%s", creds.id, scope, signature)
	return
}

type portalOnDemandConfig struct {
	InstanceID      string `json:"instance_id"`
	Region          string `json:"region"`
	CredentialsFile string `json:"credentials_file"`
}

type portalServerEntry struct {
	ID       string               `json:"id"`
	OnDemand portalOnDemandConfig `json:"on_demand"`
}

type portalConfigFile struct {
	Servers []portalServerEntry `json:"servers"`
}

func getShardAWSConfig(shardID string) (region, instanceID, credsFile string) {
	// 1. Check environment variable override
	if shardID == "na" && awsNAInstance != "" {
		return awsNARegion, awsNAInstance, awsCredsFile
	}
	if shardID == "eu" && awsEUInstance != "" {
		return awsEURegion, awsEUInstance, awsCredsFile
	}

	// 2. Read dynamically from portal.json
	cfgPath := filepath.Join(portalDir, "portal.json")
	if data, err := os.ReadFile(cfgPath); err == nil {
		var pCfg portalConfigFile
		if err := json.Unmarshal(data, &pCfg); err == nil {
			for _, s := range pCfg.Servers {
				if strings.EqualFold(s.ID, shardID) {
					reg := s.OnDemand.Region
					if reg == "" {
						if shardID == "na" {
							reg = awsNARegion
						} else {
							reg = awsEURegion
						}
					}
					creds := s.OnDemand.CredentialsFile
					if creds == "" {
						creds = awsCredsFile
					}
					return reg, s.OnDemand.InstanceID, creds
				}
			}
		}
	}

	if shardID == "na" {
		return awsNARegion, awsNAInstance, awsCredsFile
	}
	return awsEURegion, awsEUInstance, awsCredsFile
}

func callEC2(region, action, credsPath string, extraParams map[string]string) ([]byte, int, error) {
	if credsPath == "" {
		credsPath = awsCredsFile
	}
	creds, err := loadAWSCreds(credsPath)
	if err != nil {
		return nil, 0, fmt.Errorf("load credentials: %w", err)
	}
	endpoint := fmt.Sprintf("https://ec2.%s.amazonaws.com/", region)
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, 0, err
	}
	params := map[string]string{
		"Action":  action,
		"Version": "2016-11-15",
	}
	for k, v := range extraParams {
		params[k] = v
	}
	query, authz, amzDate := signV4(creds, region, "ec2", u.Host, u.Path, params, time.Now())
	req, err := http.NewRequest("GET", u.Scheme+"://"+u.Host+u.Path+"?"+query, nil)
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("Authorization", authz)

	resp, err := awsHTTP.Do(req)
	if err != nil {
		return nil, 0, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	return body, resp.StatusCode, err
}

func getEC2InstanceState(region, instanceId, credsPath string) string {
	if instanceId == "" {
		return "unknown"
	}
	body, code, err := callEC2(region, "DescribeInstances", credsPath, map[string]string{
		"InstanceId.1": instanceId,
	})
	if err != nil || code != 200 {
		return "unknown"
	}
	var r struct {
		Instances []struct {
			ID    string `xml:"instanceId"`
			State string `xml:"instanceState>name"`
		} `xml:"reservationSet>item>instancesSet>item"`
	}
	if err := xml.Unmarshal(body, &r); err != nil || len(r.Instances) == 0 {
		return "unknown"
	}
	return r.Instances[0].State
}

func executeEC2Action(region, instanceId, action, credsPath string) error {
	if instanceId == "" {
		return errors.New("instance id not configured")
	}
	var ec2Action string
	switch action {
	case "start":
		ec2Action = "StartInstances"
	case "restart":
		st := getEC2InstanceState(region, instanceId, credsPath)
		if st == "stopped" {
			ec2Action = "StartInstances"
		} else {
			ec2Action = "RebootInstances"
		}
	case "stop":
		ec2Action = "StopInstances"
	default:
		return fmt.Errorf("invalid action: %s", action)
	}
	body, code, err := callEC2(region, ec2Action, credsPath, map[string]string{
		"InstanceId.1": instanceId,
	})
	if err != nil {
		return fmt.Errorf("ec2 %s: %w", ec2Action, err)
	}
	if code != 200 {
		return fmt.Errorf("ec2 %s returned HTTP %d: %s", ec2Action, code, string(body))
	}
	return nil
}

func getNAServerState() string {
	region, id, creds := getShardAWSConfig("na")
	return getEC2InstanceState(region, id, creds)
}

func getEUServerState() string {
	region, id, creds := getShardAWSConfig("eu")
	return getEC2InstanceState(region, id, creds)
}

func executeNAAction(action string) error {
	region, id, creds := getShardAWSConfig("na")
	return executeEC2Action(region, id, action, creds)
}

func executeEUAction(action string) error {
	region, id, creds := getShardAWSConfig("eu")
	return executeEC2Action(region, id, action, creds)
}

func main() {
	loadGMUsers()
	loadSessions()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/status", handleAPIStatus)
	mux.HandleFunc("GET /api/ladder", handleAPILadder)
	mux.HandleFunc("GET /api/character", handleAPICharacter)
	mux.HandleFunc("GET /api/character/portrait", handleAPICharacterPortrait)
	mux.HandleFunc("GET /api/wiki/items", handleAPIWikiItems)
	mux.HandleFunc("GET /api/auth/status", handleAuthStatus)
	mux.HandleFunc("POST /api/auth/login", handleAuthLogin)
	mux.HandleFunc("POST /api/auth/logout", handleAuthLogout)
	mux.HandleFunc("GET /api/gm/status", handleGMStatus)
	mux.HandleFunc("POST /api/gm/login", handleGMLogin)
	mux.HandleFunc("POST /api/gm/logout", handleGMLogout)
	mux.HandleFunc("GET /api/gm/servers", handleGMServers)
	mux.HandleFunc("POST /api/gm/servers/action", handleGMServerAction)

	mux.HandleFunc("GET /static/", http.StripPrefix("/static/", http.FileServer(http.Dir(filepath.Join(webDir, "static")))).ServeHTTP)
	mux.HandleFunc("GET /logo.png", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, filepath.Join(webDir, "static", "logo.png"))
	})
	mux.HandleFunc("GET /favicon.png", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, filepath.Join(webDir, "static", "favicon.png"))
	})
	mux.HandleFunc("GET /home", handleIndex)
	mux.HandleFunc("GET /realms", handleIndex)
	mux.HandleFunc("GET /world", handleIndex)
	mux.HandleFunc("GET /ladder", handleIndex)
	mux.HandleFunc("GET /character/", handleIndex)
	mux.HandleFunc("GET /wiki", handleIndex)
	mux.HandleFunc("GET /gm-panel", handleIndex)
	mux.HandleFunc("GET /gm", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/gm-panel", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /map", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/world", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /world-map", func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "/world", http.StatusMovedPermanently)
	})
	mux.HandleFunc("GET /", handleIndex)

	log.Printf("Dungeon Siege MMO Web Dashboard starting on %s...", listenAddr)
	srv := &http.Server{
		Addr:         listenAddr,
		Handler:      mux,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 60 * time.Second,
	}
	if err := srv.ListenAndServe(); err != nil {
		log.Fatalf("Server error: %v", err)
	}
}

func buildRealmStatus() RealmStatus {
	res := RealmStatus{
		ServerName: "Dungeon Siege MMO",
		MapName:    "Utraean Peninsula",
		TickRate:   "30 Hz",
		Updated:    time.Now().UTC(),
		NA: ServerStatus{
			ID: "na", Name: "North America", Region: "NA", MaxPlayers: 30, Players: []PlayerInfo{},
		},
		EU: ServerStatus{
			ID: "eu", Name: "Central Europe", Region: "EU", MaxPlayers: 30, Players: []PlayerInfo{},
		},
	}

	// Maps to store live game server heartbeat data (Authoritative Ground Truth)
	verifiedPlayers := make(map[string]string)    // lower(account) -> location
	heartbeatServers := make(map[string]string)   // lower(account) -> server id ("na" or "eu")
	heartbeatHeroes := make(map[string]string)    // lower(account) -> hero name decoded
	heartbeatXP := make(map[string]int64)         // lower(account) -> xp
	canonicalAcctNames := make(map[string]string) // lower(account) -> raw account name

	serverTown := make(map[string]string)
	serverJoinGuard := make(map[string]bool)

	// 1. Fetch live portal status
	resp, err := portalHTTP.Get(portalURL + "/api/status?internal=1")
	if err == nil {
		defer resp.Body.Close()
		var portalResp struct {
			ServerName string `json:"server_name"`
			Servers    []struct {
				ID          string            `json:"id"`
				Name        string            `json:"name"`
				Region      string            `json:"region"`
				Online      bool              `json:"online"`
				Players     string            `json:"players"`
				MaxPlayers  int               `json:"max_players"`
				GameAddress string            `json:"game_address"`
				GamePort    int               `json:"game_port"`
				Game        map[string]string `json:"game"`
			} `json:"servers"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&portalResp); err == nil {
			if portalResp.ServerName != "" {
				res.ServerName = portalResp.ServerName
			}
			for _, s := range portalResp.Servers {
				sID := strings.ToLower(s.ID)
				town := formatTownName(s.Game["start_location"])
				serverTown[sID] = town
				if strings.EqualFold(s.Game["joinguard"], "on") {
					serverJoinGuard[sID] = true
				}
				for k, v := range s.Game {
					if strings.HasPrefix(k, "pc") {
						parts := strings.Split(v, "|")
						if len(parts) > 0 && parts[0] != "" {
							rawAcct := strings.TrimSpace(parts[0])
							acctLower := strings.ToLower(rawAcct)
							verifiedPlayers[acctLower] = town
							heartbeatServers[acctLower] = sID
							canonicalAcctNames[acctLower] = rawAcct

							if len(parts) > 1 && parts[1] != "" {
								if dec, err := hex.DecodeString(parts[1]); err == nil && len(dec) > 0 {
									heartbeatHeroes[acctLower] = string(dec)
								}
							}
							if len(parts) > 2 {
								if xpVal, err := strconv.ParseInt(parts[2], 10, 64); err == nil {
									heartbeatXP[acctLower] = xpVal
								}
							}
						}
					}
				}
				pCount, _ := strconv.Atoi(s.Players)
				st := ServerStatus{
					ID:           s.ID,
					Name:         s.Name,
					Region:       s.Region,
					Online:       s.Online,
					PlayersCount: pCount,
					MaxPlayers:   s.MaxPlayers,
					Address:      s.GameAddress,
					Port:         s.GamePort,
					FPS:          s.Game["fps"],
					Players:      []PlayerInfo{},
				}
				if uptimeS, err := strconv.ParseInt(s.Game["uptime_s"], 10, 64); err == nil && uptimeS > 0 {
					d := time.Duration(uptimeS) * time.Second
					st.Uptime = fmt.Sprintf("%dh %dm", int(d.Hours()), int(d.Minutes())%60)
				}
				if sID == "na" {
					res.NA = st
				} else if sID == "eu" {
					res.EU = st
				}
			}
		}
	}

	// Also inspect local shard heartbeat files directly IF configured and server is online
	hbDir := os.Getenv("SHARD_LOG_DIR")
	var hbPaths []struct {
		path string
		id   string
	}
	if hbDir != "" {
		hbPaths = []struct {
			path string
			id   string
		}{
			{path: filepath.Join(hbDir, "ds1mmo_na_server", "game", "SiegeHostLogs", "heartbeat.txt"), id: "na"},
			{path: filepath.Join(hbDir, "ds1mmo_eu_server", "game", "SiegeHostLogs", "heartbeat.txt"), id: "eu"},
		}
	}
	for _, hb := range hbPaths {
		srvOnline := false
		if hb.id == "na" && res.NA.Online && res.NA.PlayersCount > 0 {
			srvOnline = true
		} else if hb.id == "eu" && res.EU.Online && res.EU.PlayersCount > 0 {
			srvOnline = true
		}
		if !srvOnline {
			continue
		}
		st, err := os.Stat(hb.path)
		if err != nil || time.Since(st.ModTime()) > 30*time.Second {
			continue
		}
		if b, err := os.ReadFile(hb.path); err == nil {
			content := string(b)
			if strings.Contains(content, "\nplayers=0\n") || strings.HasPrefix(content, "players=0\n") {
				continue
			}
			town := "Elddim"
			lines := strings.Split(content, "\n")
			for _, l := range lines {
				l = strings.TrimSpace(l)
				if strings.HasPrefix(l, "start_location=") {
					town = formatTownName(strings.TrimPrefix(l, "start_location="))
				} else if strings.EqualFold(l, "joinguard=on") {
					serverJoinGuard[hb.id] = true
				}
			}
			for _, l := range lines {
				l = strings.TrimSpace(l)
				if strings.HasPrefix(l, "pc") && strings.Contains(l, "=") {
					val := strings.TrimSpace(strings.SplitN(l, "=", 2)[1])
					parts := strings.Split(val, "|")
					if len(parts) > 0 && parts[0] != "" {
						rawAcct := strings.TrimSpace(parts[0])
						acctLower := strings.ToLower(rawAcct)
						verifiedPlayers[acctLower] = town
						heartbeatServers[acctLower] = hb.id
						canonicalAcctNames[acctLower] = rawAcct

						if len(parts) > 1 && parts[1] != "" {
							if dec, err := hex.DecodeString(parts[1]); err == nil && len(dec) > 0 {
								heartbeatHeroes[acctLower] = string(dec)
							}
						}
						if len(parts) > 2 {
							if xpVal, err := strconv.ParseInt(parts[2], 10, 64); err == nil {
								heartbeatXP[acctLower] = xpVal
							}
						}
					}
				}
			}
		}
	}

	// 2. Parse active sessions from portal.log
	activeSessions, sessionRawNames, sessionStartTimes, hasUploadedInSession := parseActiveSessions()

	// Build map of active servers and their player counts
	serverActive := map[string]bool{
		"na": res.NA.Online && res.NA.PlayersCount > 0,
		"eu": res.EU.Online && res.EU.PlayersCount > 0,
	}
	serverCap := map[string]int{
		"na": res.NA.PlayersCount,
		"eu": res.EU.PlayersCount,
	}

	// SiegeHost keeps departed players in pcN for 60s so their final XP reaches SiegePortal.
	// If pcN has more entries than the shard's live players count, filter out accounts whose
	// launcher session has already ended so they do not linger as ghosts on Realms/Map/Ladder.
	hbByServer := make(map[string][]string)
	for acctLower, srvID := range heartbeatServers {
		srvLower := strings.ToLower(srvID)
		if serverActive[srvLower] {
			hbByServer[srvLower] = append(hbByServer[srvLower], acctLower)
		}
	}

	finalOnlineSessions := make(map[string]string) // acctLower -> srvID
	serverVerifiedCount := make(map[string]int)

	for srvLower, accts := range hbByServer {
		if len(accts) > serverCap[srvLower] {
			var activeOnly []string
			for _, acctLower := range accts {
				if _, stillInSession := activeSessions[acctLower]; stillInSession {
					activeOnly = append(activeOnly, acctLower)
				}
			}
			if len(activeOnly) > 0 {
				accts = activeOnly
			}
		}
		for _, acctLower := range accts {
			finalOnlineSessions[acctLower] = srvLower
			serverVerifiedCount[srvLower]++
			if _, ok := sessionRawNames[acctLower]; !ok {
				sessionRawNames[acctLower] = canonicalAcctNames[acctLower]
			}
		}
	}

	// Match players connected to the shard who are not yet in pcN (e.g. sitting in Staging Screen
	// or during JoinGuard's 10s verification window) using active portal sessions ordered newest-first.
	for srvID, isActive := range serverActive {
		if !isActive {
			continue
		}
		needed := serverCap[srvID] - serverVerifiedCount[srvID]
		if needed <= 0 {
			continue
		}
		type candidate struct {
			acctLower string
			start     time.Time
		}
		var candidates []candidate
		for acctLower, s := range activeSessions {
			if strings.ToLower(s) == srvID {
				if _, already := finalOnlineSessions[acctLower]; !already {
					candidates = append(candidates, candidate{acctLower: acctLower, start: sessionStartTimes[acctLower]})
				}
			}
		}
		sort.Slice(candidates, func(i, j int) bool {
			return candidates[i].start.After(candidates[j].start)
		})
		for i := 0; i < len(candidates) && i < needed; i++ {
			c := candidates[i]
			finalOnlineSessions[c.acctLower] = srvID
			// Only force in-game status when JoinGuard is off (no pcN lines expected).
			// When JoinGuard is on, players without pcN or an in-session save remain in Staging Screen.
			if !serverJoinGuard[srvID] {
				hasUploadedInSession[c.acctLower] = true
			}
		}
	}

	accounts := loadAccounts()
	onlineActiveChar := make(map[string]string)

	for acctLower, srvID := range finalOnlineSessions {
		acct := accounts[acctLower]

		canonicalName := acctLower
		if n, ok := sessionRawNames[acctLower]; ok && n != "" {
			canonicalName = n
		}
		if acct != nil && acct.Name != "" {
			canonicalName = acct.Name
		}

		saveSHA := ""
		if acct != nil {
			saveSHA = acct.SaveSHA
		}

		liveXP := heartbeatXP[acctLower]
		activeChar, activeRec, charDetails := selectActiveCharacter(canonicalName, acct, saveSHA, heartbeatHeroes[acctLower], liveXP)
		onlineActiveChar[acctLower] = activeChar

		effectiveXP := liveXP
		if activeRec != nil && activeRec.XP > effectiveXP {
			effectiveXP = activeRec.XP
		}

		location := "Staging Screen"
		mapAreaId := "staging"
		isIngame := false

		// 1. Shard heartbeat check: verified in game server heartbeat (pcN lines)
		if _, ok := verifiedPlayers[acctLower]; ok {
			isIngame = true
		}

		// 2. In-session save upload: player has actively uploaded a save after session start
		if hasUploadedInSession[acctLower] {
			isIngame = true
		}

		// 3. Account / character update check: save_time or char updated after session start
		startTime := sessionStartTimes[acctLower]
		if !startTime.IsZero() && acct != nil {
			if acct.SaveTime.After(startTime) {
				isIngame = true
			}
			for _, ch := range acct.Chars {
				if ch.Updated.After(startTime) {
					isIngame = true
					break
				}
			}
		}

		if isIngame {
			if charDetails.StartLoc != "" {
				location = charDetails.StartLoc
				mapAreaId = charDetails.MapAreaId
			} else {
				locFromPrefs := extractStartLocation(canonicalName)
				if locFromPrefs != "" {
					location, mapAreaId = regionToArea(locFromPrefs)
				} else if t, ok := serverTown[srvID]; ok && t != "" {
					location, mapAreaId = regionToArea(t)
				} else {
					location = "Elddim"
					mapAreaId = "elddim"
				}
			}
			if mapAreaId == "" {
				_, mapAreaId = regionToArea(location)
			}
		}

		race := charDetails.Race
		if race == "" {
			race = "Human"
		}
		pInfo := PlayerInfo{
			AccountName:   canonicalName,
			CharacterName: charDetails.Name,
			Level:         charDetails.Level,
			XP:            effectiveXP,
			Race:          race,
			Class:         charDetails.Class,
			Location:      location,
			MapAreaId:     mapAreaId,
			Region:        strings.ToUpper(srvID),
			ServerID:      srvID,
		}

		if srvID == "na" {
			res.NA.Players = append(res.NA.Players, pInfo)
		} else if srvID == "eu" {
			res.EU.Players = append(res.EU.Players, pInfo)
		}
	}

	// Update player count from actual connected players
	if !res.NA.Online {
		res.NA.PlayersCount = 0
		res.NA.Players = []PlayerInfo{}
	} else if len(res.NA.Players) > res.NA.PlayersCount {
		res.NA.PlayersCount = len(res.NA.Players)
	}
	if !res.EU.Online {
		res.EU.PlayersCount = 0
		res.EU.Players = []PlayerInfo{}
	} else if len(res.EU.Players) > res.EU.PlayersCount {
		res.EU.PlayersCount = len(res.EU.Players)
	}
	res.TotalOnline = res.NA.PlayersCount + res.EU.PlayersCount

	// Always rank players from higher level to lower; tie-breaker: higher XP, then character name alphabetical
	sortPlayers := func(players []PlayerInfo) {
		sort.Slice(players, func(i, j int) bool {
			if players[i].Level != players[j].Level {
				return players[i].Level > players[j].Level
			}
			if players[i].XP != players[j].XP {
				return players[i].XP > players[j].XP
			}
			return strings.ToLower(players[i].CharacterName) < strings.ToLower(players[j].CharacterName)
		})
	}
	sortPlayers(res.NA.Players)
	sortPlayers(res.EU.Players)

	// Build Ladder from the exact same live snapshot (same accounts, online map, heartbeat heroes & live XP)
	res.Ladder = buildLadderFromSnapshot(accounts, finalOnlineSessions, heartbeatHeroes, heartbeatXP, onlineActiveChar)

	return res
}

var (
	portalLogMu      sync.Mutex
	portalLogPath    string
	portalLogSize    int64
	portalLogModTime time.Time
	portalLogSess    map[string]string
	portalLogRaw     map[string]string
	portalLogStarts  map[string]time.Time
	portalLogUploads map[string]bool
)

func parseActiveSessions() (map[string]string, map[string]string, map[string]time.Time, map[string]bool) {
	sessions := make(map[string]string) // lower(account) -> server (na/eu)
	rawNames := make(map[string]string) // lower(account) -> original casing
	sessionStartTimes := make(map[string]time.Time)
	hasUploadedInSession := make(map[string]bool)

	logPath := filepath.Join(portalDir, "logs", "portal.log")
	stat, err := os.Stat(logPath)
	if err != nil {
		return sessions, rawNames, sessionStartTimes, hasUploadedInSession
	}

	portalLogMu.Lock()
	if portalLogPath == logPath && portalLogSize == stat.Size() && portalLogModTime.Equal(stat.ModTime()) && portalLogSess != nil {
		now := time.Now().UTC()
		for k, v := range portalLogSess {
			if t := portalLogStarts[k]; now.Sub(t) <= 12*time.Hour {
				sessions[k] = v
				rawNames[k] = portalLogRaw[k]
				sessionStartTimes[k] = t
				hasUploadedInSession[k] = portalLogUploads[k]
			}
		}
		portalLogMu.Unlock()
		return sessions, rawNames, sessionStartTimes, hasUploadedInSession
	}
	portalLogMu.Unlock()

	f, err := os.Open(logPath)
	if err != nil {
		return sessions, rawNames, sessionStartTimes, hasUploadedInSession
	}
	defer f.Close()

	offset := int64(0)
	if stat.Size() > 10*1024*1024 {
		offset = stat.Size() - 10*1024*1024
	}
	_, _ = f.Seek(offset, io.SeekStart)

	b, _ := io.ReadAll(f)
	lines := strings.Split(string(b), "\n")

	parseLogTime := func(line string) time.Time {
		var year, month, day, hour, min, sec int
		if len(line) >= 19 && line[4] == '/' && line[7] == '/' && line[10] == ' ' {
			fmt.Sscanf(line[:19], "%04d/%02d/%02d %02d:%02d:%02d", &year, &month, &day, &hour, &min, &sec)
		}
		return time.Date(year, time.Month(month), day, hour, min, sec, 0, time.UTC)
	}

	for _, line := range lines {
		if idx := strings.Index(line, "session start: "); idx >= 0 {
			rem := line[idx+len("session start: "):]
			parts := strings.SplitN(rem, " on ", 2)
			if len(parts) == 2 {
				acct := strings.TrimSpace(parts[0])
				acctLower := strings.ToLower(acct)
				srv := "na"
				if strings.Contains(parts[1], `server "eu"`) {
					srv = "eu"
				}
				sessions[acctLower] = srv
				rawNames[acctLower] = acct
				sessionStartTimes[acctLower] = parseLogTime(line)
				hasUploadedInSession[acctLower] = false
			}
		} else if idx := strings.Index(line, "session end: "); idx >= 0 {
			acct := strings.TrimSpace(line[idx+len("session end: "):])
			acctLower := strings.ToLower(acct)
			delete(sessions, acctLower)
			delete(rawNames, acctLower)
			delete(sessionStartTimes, acctLower)
			delete(hasUploadedInSession, acctLower)
		} else if idx := strings.Index(line, "save uploaded: "); idx >= 0 {
			rem := line[idx+len("save uploaded: "):]
			parts := strings.SplitN(rem, " ", 2)
			if len(parts) > 0 {
				acct := strings.TrimSpace(parts[0])
				acctLower := strings.ToLower(acct)
				if _, ok := sessions[acctLower]; ok {
					hasUploadedInSession[acctLower] = true
				}
			}
		}
	}

	now := time.Now().UTC()
	for acctLower, t := range sessionStartTimes {
		if now.Sub(t) > 12*time.Hour {
			delete(sessions, acctLower)
			delete(rawNames, acctLower)
			delete(sessionStartTimes, acctLower)
			delete(hasUploadedInSession, acctLower)
		}
	}

	// Cache snapshot for unchanged portal.log
	cSess := make(map[string]string, len(sessions))
	cRaw := make(map[string]string, len(rawNames))
	cStarts := make(map[string]time.Time, len(sessionStartTimes))
	cUps := make(map[string]bool, len(hasUploadedInSession))
	for k, v := range sessions {
		cSess[k] = v
		cRaw[k] = rawNames[k]
		cStarts[k] = sessionStartTimes[k]
		cUps[k] = hasUploadedInSession[k]
	}
	portalLogMu.Lock()
	portalLogPath = logPath
	portalLogSize = stat.Size()
	portalLogModTime = stat.ModTime()
	portalLogSess = cSess
	portalLogRaw = cRaw
	portalLogStarts = cStarts
	portalLogUploads = cUps
	portalLogMu.Unlock()

	return sessions, rawNames, sessionStartTimes, hasUploadedInSession
}

func loadAccounts() map[string]*Account {
	out := make(map[string]*Account)
	acctPath := filepath.Join(portalDir, "portal-data", "accounts.json")
	b, err := os.ReadFile(acctPath)
	if err != nil {
		return out
	}
	var list []*Account
	if err := json.Unmarshal(b, &list); err == nil {
		for _, a := range list {
			out[strings.ToLower(a.Name)] = a
		}
	}
	return out
}

func extractStartLocation(account string) string {
	details := resolveCharacterDetails(account, "", "", nil)
	if details.StartLoc != "" {
		return details.StartLoc
	}
	return "Elddim"
}

var kExpTable = []float64{
	0, 70, 650, 1450, 2350,
	3350, 4650, 6650, 9450, 12950,
	17100, 22000, 27800, 34300, 41300,
	49191, 58327, 68882, 81054, 95071,
	111190, 129706, 150954, 175317, 203567,
	236338, 274352, 318449, 369600, 428936,
	497766, 577609, 670226, 777662, 902288,
	1046855, 1214551, 1409080, 1634732, 1896489,
	2200128, 2552348, 2960924, 3434872, 3984651,
	4622395, 5362179, 6220327, 7215779, 8370504,
	9709985, 11263782, 13066188, 15156978, 17582294,
	20395661, 23659167, 27444833, 31836207, 36930200,
	42839232, 49780637, 58283859, 68700306, 81460453,
	97091634, 116239830, 139696370, 168430631, 203630102,
	246749453, 299570658, 364276635, 443541456, 540640862,
	659587634, 805297430, 983791930, 1202447693, 1470301002,
	1798421306, 2200368678, 2692754209, 3295926485, 4034812522,
	4939947918, 6048738778, 7407007581, 9070886865, 11109138988,
	13605997839, 16664649931, 20411498744, 25001388540, 30624003540,
	37511706915, 45949143550, 56285003427, 68946431776, 84456681504,
	103456737421, 126731805919, 155243764829, 190170914494, 232956672834,
	285369226800, 349574605408, 428226194203, 524574390477, 642600930913,
	787183442946, 964297020188, 1181261152309, 1447042214156, 1772624014920,
	2171461720855, 2660037910626, 3258543743095, 3991713387870, 4889846202719,
	5990058900910, 7337819456193, 8988826136414, 11011309319686, 13488851219194,
	16523840046091, 20241701359039, 24796081467402, 30375197100145, 37209613750256,
	45581774146643, 55837670632215, 68401143827042, 83791398490705, 102644460453692,
	125739461358351, 154030837466559, 188687773199113, 231142519471492, 283149583655156,
	346858237280144, 424901337970755, 520504136316754, 637617564290602, 781081513558566,
	956824851411821, 1172110440282060, 1435835286648100, 1758898223446500, 2154650321024540,
	2639446640557650,
}

func maxLevelForXP(xp int64) int {
	if xp <= 0 {
		return 0
	}
	lo := 0
	for i := 0; i < len(kExpTable); i++ {
		if float64(xp) >= kExpTable[i] {
			lo = i
		} else {
			break
		}
	}
	return lo
}

func titleWords(s string) string {
	fields := strings.Fields(strings.TrimSpace(s))
	for i, w := range fields {
		if len(w) > 0 {
			fields[i] = strings.ToUpper(w[:1]) + strings.ToLower(w[1:])
		}
	}
	return strings.Join(fields, " ")
}

func isDspartyZipEntry(name string) (string, bool) {
	norm := strings.ReplaceAll(name, "\\", "/")
	lower := strings.ToLower(norm)
	if !strings.HasPrefix(lower, "save/") || !strings.HasSuffix(lower, ".dsparty") || strings.HasSuffix(lower, ".bak") {
		return "", false
	}
	return norm[len("Save/") : len(norm)-len(".dsparty")], true
}

// extractTankFiles reads specific files (lowercase keys in want) out of a DSigTank (.dsparty) byte slice.
// Pure Go reader (raw fmt=0 and stream/chunked zlib fmt=1): avoids spawning external tankdump subprocesses.
func extractTankFiles(data []byte, want map[string]bool) map[string][]byte {
	out := make(map[string][]byte)
	if len(data) < 64 || string(data[:8]) != "DSigTank" {
		return out
	}
	fs := binary.LittleEndian.Uint32(data[16:20])
	dataOff := binary.LittleEndian.Uint32(data[24:28])
	if int(fs)+4 > len(data) || int(dataOff) > len(data) {
		return out
	}
	fc := binary.LittleEndian.Uint32(data[fs : fs+4])
	if fc > 4096 || int(fs)+4+int(fc)*4 > len(data) {
		return out
	}
	for i := uint32(0); i < fc; i++ {
		relOff := binary.LittleEndian.Uint32(data[fs+4+i*4 : fs+8+i*4])
		b := int(fs) + int(relOff)
		if b < 0 || b+30 > len(data) {
			continue
		}
		size := binary.LittleEndian.Uint32(data[b+4 : b+8])
		doff := binary.LittleEndian.Uint32(data[b+8 : b+12])
		fmtType := binary.LittleEndian.Uint16(data[b+24 : b+26])
		nameLen := int(binary.LittleEndian.Uint16(data[b+28 : b+30]))
		if b+30+nameLen > len(data) {
			continue
		}
		name := strings.ToLower(string(data[b+30 : b+30+nameLen]))
		if !want[name] {
			continue
		}
		after := b + 28 + ((2 + nameLen + 1 + 3) &^ 3)
		base := int(dataOff) + int(doff)
		if base < 0 || base > len(data) || size > 4*1024*1024 {
			continue
		}
		if fmtType == 0 {
			if base+int(size) <= len(data) {
				cp := make([]byte, size)
				copy(cp, data[base:base+int(size)])
				out[name] = cp
			}
			continue
		}
		if fmtType != 1 || after+8 > len(data) {
			continue
		}
		csize := binary.LittleEndian.Uint32(data[after : after+4])
		chunk := binary.LittleEndian.Uint32(data[after+4 : after+8])
		if chunk == 0 {
			if base+int(csize) <= len(data) {
				if zr, err := zlib.NewReader(bytes.NewReader(data[base : base+int(csize)])); err == nil {
					if dec, err := io.ReadAll(io.LimitReader(zr, 4*1024*1024)); err == nil {
						out[name] = dec
					}
					_ = zr.Close()
				}
			}
			continue
		}
		nch := (size + chunk - 1) / chunk
		if nch > 1024 || after+8+int(nch)*16 > len(data) {
			continue
		}
		var buf bytes.Buffer
		buf.Grow(int(size))
		ok := true
		hp := after + 8
		for c := uint32(0); c < nch; c++ {
			ucs := binary.LittleEndian.Uint32(data[hp : hp+4])
			cs := binary.LittleEndian.Uint32(data[hp+4 : hp+8])
			extra := binary.LittleEndian.Uint32(data[hp+8 : hp+12])
			coff := binary.LittleEndian.Uint32(data[hp+12 : hp+16])
			hp += 16
			segStart := base + int(coff)
			segEnd := segStart + int(cs)
			if segStart < 0 || segEnd > len(data) || segEnd+int(extra) > len(data) {
				ok = false
				break
			}
			if ucs == cs {
				buf.Write(data[segStart:segEnd])
			} else {
				zr, err := zlib.NewReader(bytes.NewReader(data[segStart:segEnd]))
				if err != nil {
					ok = false
					break
				}
				_, err = io.Copy(&buf, io.LimitReader(zr, 4*1024*1024))
				_ = zr.Close()
				if err != nil {
					ok = false
					break
				}
				if extra > 0 {
					buf.Write(data[segEnd : segEnd+int(extra)])
				}
			}
		}
		if ok {
			out[name] = buf.Bytes()
		}
	}
	return out
}

func applyAntiCheatLevel(rawLevel int, hasPartyGas bool, effectiveXP int64) int {
	if !hasPartyGas {
		return maxLevelForXP(effectiveXP)
	}
	lvl := rawLevel
	if lvl < 0 {
		lvl = 0
	}
	if effectiveXP > 0 {
		maxLvl := maxLevelForXP(effectiveXP) + 2
		if lvl > maxLvl {
			lvl = maxLevelForXP(effectiveXP)
		}
	} else {
		if lvl > 1 {
			lvl = 0
		}
	}
	return lvl
}

func applyAntiCheatClass(cls string, level int) string {
	cls = strings.TrimSpace(cls)
	if cls == "" {
		return "Adventurer"
	}
	if level < 140 && strings.Contains(strings.ToLower(cls), "siegemaster") {
		return "Adventurer"
	}
	return cls
}

func effectiveXPForChar(rec *CharRec, liveXP int64) int64 {
	var xp int64
	if rec != nil && rec.XP > 0 {
		xp = rec.XP
	}
	if liveXP > xp {
		xp = liveXP
	}
	return xp
}

func readPrefsLocFromZip(r *zip.ReadCloser) (string, string) {
	if r == nil {
		return "", ""
	}
	for _, f := range r.File {
		if strings.EqualFold(filepath.Base(f.Name), "prefs.gas") {
			if rc, err := f.Open(); err == nil {
				b, _ := io.ReadAll(io.LimitReader(rc, 256*1024))
				rc.Close()
				if m := startLocRe.FindStringSubmatch(string(b)); len(m) > 1 {
					return regionToArea(m[1])
				}
			}
		}
	}
	return "", ""
}

func parseGasIntoChar(partyBytes, infoBytes []byte, res *CachedChar) {
	if len(partyBytes) > 0 {
		gasStr := string(partyBytes)
		res.HasPartyGas = true
		if m := templateRe.FindStringSubmatch(gasStr); len(m) > 1 {
			res.Race = templateToRace(m[1])
		}
		if m := classRe.FindStringSubmatch(gasStr); len(m) > 1 {
			if cls := titleWords(m[1]); cls != "" {
				res.Class = cls
			}
		}
		if m := uberLvlRe.FindStringSubmatch(gasStr); len(m) > 1 {
			if fl, err := strconv.ParseFloat(m[1], 64); err == nil {
				rawLvl := int(math.Floor(fl))
				if rawLvl < 0 {
					rawLvl = 0
				}
				res.RawLevel = rawLvl
			}
		}
		if m := screenNmRe.FindStringSubmatch(gasStr); len(m) > 1 && strings.TrimSpace(m[1]) != "" {
			res.Name = strings.TrimSpace(m[1])
		}
	}
	if len(infoBytes) > 0 {
		infoStr := string(infoBytes)
		if m := regionRe.FindStringSubmatch(infoStr); len(m) > 1 {
			reg := strings.TrimSpace(m[1])
			if reg != "" {
				if strings.EqualFold(reg, "a8_r2_fortress") {
					reg = resolveFortressRegion(infoStr)
				} else if strings.EqualFold(reg, "oasis") {
					reg = resolveOasisRegion(infoStr)
				}
				loc, mId := regionToArea(reg)
				res.StartLoc = loc
				res.MapAreaId = mId
			}
		}
		if m := saveTimeRe.FindStringSubmatch(infoStr); len(m) > 1 {
			if stVal, err := strconv.ParseInt(strings.TrimSpace(m[1]), 10, 64); err == nil && stVal > 0 {
				res.SaveTime = stVal
			}
		}
	}
}

func extractTankFilesFallback(raw []byte, cleanName string) ([]byte, []byte) {
	if tankdumpBin == "" || len(raw) == 0 {
		return nil, nil
	}
	safeName := normalizeCharName(cleanName)
	if safeName == "" {
		safeName = "char"
	}
	tmpDSP := fmt.Sprintf("/tmp/probe_%d_%s.dsparty", time.Now().UnixNano(), safeName)
	if err := os.WriteFile(tmpDSP, raw, 0600); err != nil {
		return nil, nil
	}
	tmpDir := fmt.Sprintf("/tmp/probe_out_%d_%s", time.Now().UnixNano(), safeName)
	_ = os.MkdirAll(tmpDir, 0755)
	defer func() {
		_ = os.Remove(tmpDSP)
		_ = os.RemoveAll(tmpDir)
	}()

	var partyBytes, infoBytes []byte
	ctxParty, cancelParty := context.WithTimeout(context.Background(), 5*time.Second)
	cmdParty := exec.CommandContext(ctxParty, tankdumpBin, tmpDSP, "party.gas", "-e")
	cmdParty.Dir = tmpDir
	_, _ = cmdParty.CombinedOutput()
	cancelParty()
	partyBytes, _ = os.ReadFile(filepath.Join(tmpDir, "party.gas"))

	ctxInfo, cancelInfo := context.WithTimeout(context.Background(), 5*time.Second)
	cmdInfo := exec.CommandContext(ctxInfo, tankdumpBin, tmpDSP, "info.gas", "-e")
	cmdInfo.Dir = tmpDir
	_, _ = cmdInfo.CombinedOutput()
	cancelInfo()
	infoBytes, _ = os.ReadFile(filepath.Join(tmpDir, "info.gas"))

	return partyBytes, infoBytes
}

// resolveZipEntryRaw parses & caches the RAW (unclamped) save fields for a single .dsparty zip entry,
// keyed by its CRC32 + UncompressedSize + prefs fallback so unchanged alts never re-parse and stale XP
// can never poison charCache.
func resolveZipEntryRaw(account, cleanName, saveSHA string, targetFile *zip.File, prefsLoc, prefsMapID string) CachedChar {
	cacheKey := fmt.Sprintf("%s:%s", strings.ToLower(account), strings.ToLower(cleanName))
	entrySig := fmt.Sprintf("%08x:%d|%s|%s", targetFile.CRC32, targetFile.UncompressedSize64, prefsLoc, prefsMapID)

	cacheMu.RLock()
	cached, found := charCache[cacheKey]
	cacheMu.RUnlock()
	if found && cached.EntrySig == entrySig && cached.StartLoc != "" && cached.MapAreaId != "" && cached.Race != "" {
		return cached
	}

	res := CachedChar{
		Name:      cleanName,
		Class:     "Adventurer",
		Race:      "Human",
		RawLevel:  0,
		SaveSHA:   saveSHA,
		EntrySig:  entrySig,
		StartLoc:  "",
		MapAreaId: "",
	}

	if rc, err := targetFile.Open(); err == nil {
		raw, readErr := io.ReadAll(io.LimitReader(rc, 8*1024*1024))
		rc.Close()
		if readErr == nil && len(raw) > 0 {
			files := extractTankFiles(raw, map[string]bool{
				"party.gas": true,
				"info.gas":  true,
			})
			partyBytes := files["party.gas"]
			infoBytes := files["info.gas"]
			if len(partyBytes) == 0 {
				fbParty, fbInfo := extractTankFilesFallback(raw, cleanName)
				if len(fbParty) > 0 {
					partyBytes = fbParty
				}
				if len(infoBytes) == 0 && len(fbInfo) > 0 {
					infoBytes = fbInfo
				}
			}
			parseGasIntoChar(partyBytes, infoBytes, &res)
		}
	}

	if res.StartLoc == "" {
		if prefsLoc != "" {
			res.StartLoc = prefsLoc
			res.MapAreaId = prefsMapID
		} else {
			res.StartLoc = "Elddim"
			res.MapAreaId = "elddim"
		}
	}

	cacheMu.Lock()
	charCache[cacheKey] = res
	cacheMu.Unlock()
	return res
}

func finalizeCharacterDetails(raw CachedChar, rec *CharRec, liveXP int64) CachedChar {
	effXP := effectiveXPForChar(rec, liveXP)
	out := raw
	out.Level = applyAntiCheatLevel(raw.RawLevel, raw.HasPartyGas, effXP)
	out.Class = applyAntiCheatClass(raw.Class, out.Level)
	return out
}

type cachedZipEntry struct {
	CleanName string
	Raw       CachedChar
	ModTime   time.Time
}

type cachedAccountZip struct {
	ZipPath     string
	Size        int64
	ModTime     time.Time
	SaveSHA     string
	PrefsLoc    string
	PrefsMapID  string
	Entries     []cachedZipEntry
}

var (
	accountZipMu    sync.RWMutex
	accountZipCache = make(map[string]cachedAccountZip)
)

func loadAccountZip(canonicalAcctName, saveSHA string) (cachedAccountZip, bool) {
	acctLower := strings.ToLower(strings.TrimSpace(canonicalAcctName))
	zipPath := filepath.Join(portalDir, "portal-data", "saves", acctLower, "current.zip")
	st, err := os.Stat(zipPath)
	if err != nil {
		return cachedAccountZip{}, false
	}

	accountZipMu.RLock()
	cached, ok := accountZipCache[acctLower]
	accountZipMu.RUnlock()
	if ok && cached.ZipPath == zipPath && cached.Size == st.Size() && cached.ModTime.Equal(st.ModTime()) && (saveSHA == "" || cached.SaveSHA == saveSHA) {
		return cached, true
	}

	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return cachedAccountZip{}, false
	}
	defer r.Close()

	prefsLoc, prefsMapID := readPrefsLocFromZip(r)
	var entries []cachedZipEntry
	for _, f := range r.File {
		cleanName, ok := isDspartyZipEntry(f.Name)
		if !ok {
			continue
		}
		raw := resolveZipEntryRaw(canonicalAcctName, cleanName, saveSHA, f, prefsLoc, prefsMapID)
		entries = append(entries, cachedZipEntry{
			CleanName: cleanName,
			Raw:       raw,
			ModTime:   f.ModTime(),
		})
	}

	res := cachedAccountZip{
		ZipPath:    zipPath,
		Size:       st.Size(),
		ModTime:    st.ModTime(),
		SaveSHA:    saveSHA,
		PrefsLoc:   prefsLoc,
		PrefsMapID: prefsMapID,
		Entries:    entries,
	}
	accountZipMu.Lock()
	accountZipCache[acctLower] = res
	accountZipMu.Unlock()
	return res, true
}

func resolveCharacterDetails(account, charName, saveSHA string, rec *CharRec, liveXPs ...int64) CachedChar {
	var liveXP int64
	if len(liveXPs) > 0 && liveXPs[0] > 0 {
		liveXP = liveXPs[0]
	}

	if az, ok := loadAccountZip(account, saveSHA); ok {
		normChar := normalizeCharName(charName)
		normAcct := normalizeCharName(account)

		for _, e := range az.Entries {
			normClean := normalizeCharName(e.CleanName)
			if charName != "" {
				if strings.EqualFold(e.CleanName, charName) || (normChar != "" && normClean == normChar) {
					return finalizeCharacterDetails(e.Raw, rec, liveXP)
				}
			} else if strings.EqualFold(e.CleanName, account) || (normAcct != "" && normClean == normAcct) {
				return finalizeCharacterDetails(e.Raw, rec, liveXP)
			}
		}

		// Fallback: if hero screen_name differs from .dsparty filename (punctuation/renames), match Raw.Name
		if charName != "" {
			for _, e := range az.Entries {
				if strings.EqualFold(e.Raw.Name, charName) || (normChar != "" && normalizeCharName(e.Raw.Name) == normChar) {
					return finalizeCharacterDetails(e.Raw, rec, liveXP)
				}
			}
		}

		if charName == "" && len(az.Entries) > 0 {
			return finalizeCharacterDetails(az.Entries[0].Raw, rec, liveXP)
		}

		loc, mId := az.PrefsLoc, az.PrefsMapID
		if loc == "" {
			loc, mId = "Elddim", "elddim"
		}
		return finalizeCharacterDetails(CachedChar{
			Name:        charName,
			Class:       "Adventurer",
			Race:        "Human",
			HasPartyGas: false,
			SaveSHA:     saveSHA,
			StartLoc:    loc,
			MapAreaId:   mId,
		}, rec, liveXP)
	}

	return finalizeCharacterDetails(CachedChar{
		Name:        charName,
		Class:       "Adventurer",
		Race:        "Human",
		HasPartyGas: false,
		SaveSHA:     saveSHA,
		StartLoc:    "Elddim",
		MapAreaId:   "elddim",
	}, rec, liveXP)
}

func formatRelativeTime(t time.Time) string {
	if t.IsZero() || t.Year() < 2020 {
		return "Unknown"
	}
	diff := time.Since(t)
	if diff < 0 || diff < time.Minute {
		return "Just now"
	}
	if diff < time.Hour {
		mins := int(diff.Minutes())
		if mins <= 1 {
			return "1 minute ago"
		}
		return fmt.Sprintf("%d minutes ago", mins)
	}
	if diff < 24*time.Hour {
		hours := int(diff.Hours())
		if hours <= 1 {
			return "1 hour ago"
		}
		return fmt.Sprintf("%d hours ago", hours)
	}
	if diff < 48*time.Hour {
		return "Yesterday"
	}
	days := int(diff.Hours() / 24)
	if days < 7 {
		if days <= 1 {
			return "1 day ago"
		}
		return fmt.Sprintf("%d days ago", days)
	}
	if days < 30 {
		weeks := days / 7
		if weeks <= 1 {
			return "1 week ago"
		}
		return fmt.Sprintf("%d weeks ago", weeks)
	}
	if days < 365 {
		months := days / 30
		if months <= 1 {
			return "1 month ago"
		}
		return fmt.Sprintf("%d months ago", months)
	}
	years := days / 365
	if years <= 1 {
		return "1 year ago"
	}
	return fmt.Sprintf("%d years ago", years)
}

func normalizeCharName(s string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(s) {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') {
			b.WriteRune(r)
		}
	}
	return b.String()
}

func matchCharRec(chars map[string]*CharRec, cleanName string) *CharRec {
	if chars == nil {
		return nil
	}
	for k, c := range chars {
		if strings.EqualFold(k, cleanName) || strings.EqualFold(c.Name, cleanName) {
			return c
		}
	}
	cleanNorm := normalizeCharName(cleanName)
	if cleanNorm == "" {
		return nil
	}
	for k, c := range chars {
		if normalizeCharName(k) == cleanNorm || normalizeCharName(c.Name) == cleanNorm {
			return c
		}
	}
	return nil
}

func selectActiveCharacter(canonicalAcctName string, acct *Account, saveSHA string, heroHint string, liveXP int64) (string, *CharRec, CachedChar) {
	var newestChar *CharRec
	if acct != nil {
		for _, ch := range acct.Chars {
			if newestChar == nil || ch.Updated.After(newestChar.Updated) {
				newestChar = ch
			}
		}
	}

	if heroHint != "" {
		var rec *CharRec
		if acct != nil {
			rec = matchCharRec(acct.Chars, heroHint)
		}
		// Never substitute a different character's CharRec when heroHint is known:
		// a brand-new Level 0 alt must not inherit an older alt's XP before accounts.json flushes.
		details := resolveCharacterDetails(canonicalAcctName, heroHint, saveSHA, rec, liveXP)
		return heroHint, rec, details
	}

	// If a character was updated on the game server more recently than the last save upload, it is actively in-game now.
	if acct != nil && !acct.SaveTime.IsZero() && newestChar != nil && newestChar.Name != "" && newestChar.Updated.After(acct.SaveTime) {
		details := resolveCharacterDetails(canonicalAcctName, newestChar.Name, saveSHA, newestChar, liveXP)
		return newestChar.Name, newestChar, details
	}

	// Otherwise, inspect the account's .dsparty saves in current.zip and pick the character most recently saved by the game (info.gas save_time).
	if az, ok := loadAccountZip(canonicalAcctName, saveSHA); ok {
		var bestName string
		var bestRec *CharRec
		var bestDetails CachedChar
		hasBest := false

		for _, e := range az.Entries {
			var rec *CharRec
			if acct != nil && acct.Chars != nil {
				rec = matchCharRec(acct.Chars, e.CleanName)
				if rec == nil && e.Raw.Name != "" {
					rec = matchCharRec(acct.Chars, e.Raw.Name)
				}
				if len(acct.Chars) > 0 && rec == nil {
					continue
				}
			}
			details := finalizeCharacterDetails(e.Raw, rec, liveXP)
			if !hasBest {
				bestName, bestRec, bestDetails, hasBest = e.CleanName, rec, details, true
				continue
			}
			if details.SaveTime > bestDetails.SaveTime {
				bestName, bestRec, bestDetails = e.CleanName, rec, details
			} else if details.SaveTime == bestDetails.SaveTime && rec != nil && (bestRec == nil || rec.Updated.After(bestRec.Updated)) {
				bestName, bestRec, bestDetails = e.CleanName, rec, details
			}
		}
		if hasBest {
			return bestName, bestRec, bestDetails
		}
	}

	charName := canonicalAcctName
	if newestChar != nil && newestChar.Name != "" {
		charName = newestChar.Name
	}
	details := resolveCharacterDetails(canonicalAcctName, charName, saveSHA, newestChar, liveXP)
	return charName, newestChar, details
}

func buildLadderFromSnapshot(
	accounts map[string]*Account,
	onlineSessions map[string]string,
	heartbeatHeroes map[string]string,
	heartbeatXP map[string]int64,
	onlineActiveChar map[string]string,
) []LadderCharacter {
	var list []LadderCharacter
	nowUnix := time.Now().Unix()

	for acctLower, acct := range accounts {
		canonicalAcctName := acct.Name
		if canonicalAcctName == "" {
			canonicalAcctName = acctLower
		}

		srvID, acctOnline := onlineSessions[acctLower]
		onlineShard := ""
		if acctOnline {
			onlineShard = strings.ToUpper(srvID)
		}
		hbHero := heartbeatHeroes[acctLower]
		hbHeroNorm := normalizeCharName(hbHero)
		activeName := onlineActiveChar[acctLower]
		activeNorm := normalizeCharName(activeName)
		liveXP := heartbeatXP[acctLower]

		isThisCharOnline := func(displayName, cleanName string) bool {
			if !acctOnline {
				return false
			}
			dNorm := normalizeCharName(displayName)
			cNorm := normalizeCharName(cleanName)
			if hbHero != "" {
				return strings.EqualFold(hbHero, displayName) ||
					strings.EqualFold(hbHero, cleanName) ||
					(hbHeroNorm != "" && (hbHeroNorm == dNorm || hbHeroNorm == cNorm))
			}
			if activeName != "" {
				return strings.EqualFold(activeName, displayName) ||
					strings.EqualFold(activeName, cleanName) ||
					(activeNorm != "" && (activeNorm == dNorm || activeNorm == cNorm))
			}
			return false
		}

		emittedRecs := make(map[*CharRec]bool)
		emittedOnlineHero := false

		if az, ok := loadAccountZip(canonicalAcctName, acct.SaveSHA); ok {
			type zipCharEntry struct {
				modTime   time.Time
				cleanName string
				charRec   *CharRec
				raw       CachedChar
			}
			var entries []zipCharEntry
			var maxSaveTime int64

			for _, ze := range az.Entries {
				var charRec *CharRec
				if acct.Chars != nil {
					charRec = matchCharRec(acct.Chars, ze.CleanName)
					if charRec == nil && ze.Raw.Name != "" {
						charRec = matchCharRec(acct.Chars, ze.Raw.Name)
					}
				}
				// Only MMO characters with an active server record (or currently verified online via pcN)
				// can appear on the ladder — prevents local-only singleplayer saves from polluting it.
				if charRec == nil && !(acctOnline && hbHero != "" && isThisCharOnline(ze.Raw.Name, ze.CleanName)) {
					continue
				}
				if ze.Raw.SaveTime > maxSaveTime {
					maxSaveTime = ze.Raw.SaveTime
				}
				entries = append(entries, zipCharEntry{
					modTime:   ze.ModTime,
					cleanName: ze.CleanName,
					charRec:   charRec,
					raw:       ze.Raw,
				})
			}

			dspartyCount := len(entries)
			for _, e := range entries {
				if e.charRec != nil {
					emittedRecs[e.charRec] = true
				}
				isOnline := isThisCharOnline(e.raw.Name, e.cleanName)
				var charLiveXP int64
				if isOnline {
					emittedOnlineHero = true
					charLiveXP = liveXP
				}

				charDetails := finalizeCharacterDetails(e.raw, e.charRec, charLiveXP)
				xp := effectiveXPForChar(e.charRec, charLiveXP)

				race := charDetails.Race
				if race == "" {
					race = "Human"
				}

				var lastTime time.Time
				if e.charRec != nil && !e.charRec.Updated.IsZero() && e.charRec.Updated.Year() >= 2020 {
					lastTime = e.charRec.Updated
				}
				// Only apply zip entry ModTime to the most recently saved .dsparty in current.zip,
				// since unpacking current.zip on session start resets mtimes of inactive characters.
				if dspartyCount <= 1 || (maxSaveTime > 0 && charDetails.SaveTime == maxSaveTime) {
					mod := e.modTime
					if !mod.IsZero() && mod.Year() >= 2020 && mod.After(lastTime) {
						lastTime = mod
					}
				}

				lastSeenStr := formatRelativeTime(lastTime)
				var lastSeenUnix int64
				if !lastTime.IsZero() && lastTime.Year() >= 2020 {
					lastSeenUnix = lastTime.Unix()
				}

				if isOnline {
					if onlineShard != "" {
						lastSeenStr = "Online (" + onlineShard + ")"
					} else {
						lastSeenStr = "Online Now"
					}
					lastSeenUnix = nowUnix
				}

				list = append(list, LadderCharacter{
					AccountName:   canonicalAcctName,
					CharacterName: charDetails.Name,
					Level:         charDetails.Level,
					Race:          race,
					Class:         charDetails.Class,
					XP:            xp,
					LastSeen:      lastSeenStr,
					LastSeenTime:  lastSeenUnix,
					IsOnline:      isOnline,
					OnlineShard:   onlineShard,
				})
			}
		}

		// If a brand-new verified character is currently online but hasn't uploaded to current.zip yet,
		// include them on the ladder immediately with live XP & level so Ladder matches Realms in real time.
		if acctOnline && !emittedOnlineHero && hbHero != "" {
			var charRec *CharRec
			if acct.Chars != nil {
				charRec = matchCharRec(acct.Chars, hbHero)
			}
			if charRec == nil || !emittedRecs[charRec] {
				xp := effectiveXPForChar(charRec, liveXP)
				lastSeenStr := "Online Now"
				if onlineShard != "" {
					lastSeenStr = "Online (" + onlineShard + ")"
				}
				list = append(list, LadderCharacter{
					AccountName:   canonicalAcctName,
					CharacterName: hbHero,
					Level:         maxLevelForXP(xp),
					Race:          "Human",
					Class:         "Adventurer",
					XP:            xp,
					LastSeen:      lastSeenStr,
					LastSeenTime:  nowUnix,
					IsOnline:      true,
					OnlineShard:   onlineShard,
				})
			}
		}
	}

	// Always rank from higher level to lower; tie-breaker: higher XP, then character name alphabetical
	sort.Slice(list, func(i, j int) bool {
		if list[i].Level != list[j].Level {
			return list[i].Level > list[j].Level
		}
		if list[i].XP != list[j].XP {
			return list[i].XP > list[j].XP
		}
		return strings.ToLower(list[i].CharacterName) < strings.ToLower(list[j].CharacterName)
	})

	return list
}

func handleAPIStatus(w http.ResponseWriter, r *http.Request) {
	status := getRealmStatus()
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	_, authed := getSession(r)
	if !authed {
		// Hide active player rosters and ladder if not logged in
		status.NA.Players = []PlayerInfo{}
		status.EU.Players = []PlayerInfo{}
		status.Ladder = []LadderCharacter{}
	}

	_ = json.NewEncoder(w).Encode(status)
}

func handleAPILadder(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	_, authed := getSession(r)
	if !authed {
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": "Sign in with your MMO account to view character ladder",
		})
		return
	}

	ladder := getLadderCharacters()
	_ = json.NewEncoder(w).Encode(ladder)
}

func handleAPIWikiItems(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	_, authed := getSession(r)
	if !authed {
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": "Sign in with your MMO account to view MMO Wiki",
		})
		return
	}

	wikiPath := filepath.Join(webDir, "data", "wiki_items.json")
	if _, err := os.Stat(wikiPath); os.IsNotExist(err) {
		wikiPath = filepath.Join(webDir, "wiki_items.json")
	}
	http.ServeFile(w, r, wikiPath)
}

func handleGMLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"status":"error","message":"Method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"status":"error","message":"Invalid JSON payload"}`, http.StatusBadRequest)
		return
	}
	req.Username = strings.TrimSpace(req.Username)
	req.Password = strings.TrimSpace(req.Password)
	if req.Username == "" || req.Password == "" {
		http.Error(w, `{"status":"error","message":"Username and password required"}`, http.StatusBadRequest)
		return
	}

	valid, canonicalName, err := verifyAccountPassword(req.Username, req.Password)
	if err != nil {
		log.Printf("[GM Auth] Error verifying password: %v", err)
		http.Error(w, `{"status":"error","message":"Authentication service error"}`, http.StatusInternalServerError)
		return
	}
	if !valid {
		http.Error(w, `{"status":"error","message":"Invalid username or password"}`, http.StatusUnauthorized)
		return
	}
	if !isAuthorizedGM(canonicalName) {
		http.Error(w, `{"status":"error","message":"Access denied: Game Master privileges required"}`, http.StatusForbidden)
		return
	}

	token, sess := createSession(w, canonicalName)

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"status":   "ok",
		"token":    token,
		"username": sess.Username,
	})
}

func handleGMStatus(w http.ResponseWriter, r *http.Request) {
	sess, authed := getSession(r)
	isGM := authed && sess.IsGM
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"authenticated": isGM,
		"username":      sess.Username,
	})
}

func handleGMLogout(w http.ResponseWriter, r *http.Request) {
	removeSession(w, r)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok"})
}

func getAccountCharacters(accountName string) []UserCharacter {
	var chars []UserCharacter
	acctLower := strings.ToLower(strings.TrimSpace(accountName))
	accounts := loadAccounts()
	acct := accounts[acctLower]

	saveSHA := ""
	if acct != nil {
		saveSHA = acct.SaveSHA
	}

	az, hasZip := loadAccountZip(accountName, saveSHA)
	if hasZip {
		for _, e := range az.Entries {
			var charRec *CharRec
			if acct != nil && acct.Chars != nil {
				charRec = matchCharRec(acct.Chars, e.CleanName)
				if charRec == nil && e.Raw.Name != "" {
					charRec = matchCharRec(acct.Chars, e.Raw.Name)
				}
			}
			details := finalizeCharacterDetails(e.Raw, charRec, 0)
			race := details.Race
			if race == "" {
				race = "Human"
			}
			cls := details.Class
			if cls == "" {
				cls = "Adventurer"
			}
			lvl := details.Level
			if lvl < 0 {
				lvl = 0
			}
			name := details.Name
			if name == "" {
				name = e.CleanName
			}
			chars = append(chars, UserCharacter{
				Name:  name,
				Race:  race,
				Class: cls,
				Level: lvl,
			})
		}
	}

	if len(chars) == 0 && acct != nil && acct.Chars != nil && !hasZip {
		for _, c := range acct.Chars {
			lvl := maxLevelForXP(c.XP)
			chars = append(chars, UserCharacter{
				Name:  c.Name,
				Race:  "Human",
				Class: "Adventurer",
				Level: lvl,
			})
		}
	}

	sort.Slice(chars, func(i, j int) bool {
		if chars[i].Level != chars[j].Level {
			return chars[i].Level > chars[j].Level
		}
		return strings.ToLower(chars[i].Name) < strings.ToLower(chars[j].Name)
	})

	return chars
}

func handleAuthStatus(w http.ResponseWriter, r *http.Request) {
	sess, authed := getSession(r)
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	if !authed {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"authenticated":     false,
			"username":          "",
			"is_gm":             false,
			"can_browse_armory": false,
			"characters":        []UserCharacter{},
		})
		return
	}

	chars := getAccountCharacters(sess.Username)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"authenticated":     true,
		"username":          sess.Username,
		"is_gm":             sess.IsGM,
		"can_browse_armory": true,
		"characters":        chars,
	})
}

func handleAuthLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"status":"error","message":"Method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"status":"error","message":"Invalid JSON payload"}`, http.StatusBadRequest)
		return
	}
	req.Username = strings.TrimSpace(req.Username)
	req.Password = strings.TrimSpace(req.Password)
	if req.Username == "" || req.Password == "" {
		http.Error(w, `{"status":"error","message":"Username and password required"}`, http.StatusBadRequest)
		return
	}

	valid, canonicalName, err := verifyAccountPassword(req.Username, req.Password)
	if err != nil {
		log.Printf("[Auth] Error verifying password: %v", err)
		http.Error(w, `{"status":"error","message":"Authentication service error"}`, http.StatusInternalServerError)
		return
	}
	if !valid {
		http.Error(w, `{"status":"error","message":"Invalid username or password"}`, http.StatusUnauthorized)
		return
	}

	token, sess := createSession(w, canonicalName)
	chars := getAccountCharacters(canonicalName)

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"status":            "ok",
		"token":             token,
		"username":          sess.Username,
		"is_gm":             sess.IsGM,
		"can_browse_armory": true,
		"characters":        chars,
	})
}

func handleAuthLogout(w http.ResponseWriter, r *http.Request) {
	removeSession(w, r)
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"status": "ok"})
}

func handleGMServers(w http.ResponseWriter, r *http.Request) {
	_, authed := getGMSession(r)
	if !authed {
		http.Error(w, `{"status":"error","message":"Unauthorized"}`, http.StatusUnauthorized)
		return
	}
	naState := getNAServerState()
	euState := getEUServerState()
	realmStatus := getRealmStatus()

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"na": map[string]any{
			"id":            "na",
			"name":          "North America",
			"state":         naState,
			"players_count": realmStatus.NA.PlayersCount,
			"max_players":   realmStatus.NA.MaxPlayers,
			"uptime":        realmStatus.NA.Uptime,
			"fps":           realmStatus.NA.FPS,
			"tick_rate":     realmStatus.TickRate,
		},
		"eu": map[string]any{
			"id":            "eu",
			"name":          "Central Europe",
			"state":         euState,
			"players_count": realmStatus.EU.PlayersCount,
			"max_players":   realmStatus.EU.MaxPlayers,
			"uptime":        realmStatus.EU.Uptime,
			"fps":           realmStatus.EU.FPS,
			"tick_rate":     realmStatus.TickRate,
		},
	})
}

func handleGMServerAction(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, `{"status":"error","message":"Method not allowed"}`, http.StatusMethodNotAllowed)
		return
	}
	_, authed := getGMSession(r)
	if !authed {
		http.Error(w, `{"status":"error","message":"Unauthorized"}`, http.StatusUnauthorized)
		return
	}
	var req struct {
		Server string `json:"server"`
		Action string `json:"action"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"status":"error","message":"Invalid JSON"}`, http.StatusBadRequest)
		return
	}
	req.Server = strings.ToLower(strings.TrimSpace(req.Server))
	req.Action = strings.ToLower(strings.TrimSpace(req.Action))

	var err error
	if req.Server == "na" {
		err = executeNAAction(req.Action)
	} else if req.Server == "eu" {
		err = executeEUAction(req.Action)
	} else {
		http.Error(w, `{"status":"error","message":"Invalid server specified"}`, http.StatusBadRequest)
		return
	}

	if err != nil {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusInternalServerError)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": err.Error(),
		})
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"status":  "ok",
		"message": fmt.Sprintf("%s action '%s' triggered successfully", strings.ToUpper(req.Server), req.Action),
	})
}

func handleIndex(w http.ResponseWriter, r *http.Request) {
	isCharRoute := strings.HasPrefix(r.URL.Path, "/character/") && len(strings.Split(strings.Trim(r.URL.Path, "/"), "/")) == 3
	if !isCharRoute && r.URL.Path != "/" && r.URL.Path != "/home" && r.URL.Path != "/realms" && r.URL.Path != "/world" && r.URL.Path != "/map" && r.URL.Path != "/world-map" && r.URL.Path != "/ladder" && r.URL.Path != "/gm" && r.URL.Path != "/gm-panel" && r.URL.Path != "/wiki" {
		http.NotFound(w, r)
		return
	}
	http.ServeFile(w, r, filepath.Join(webDir, "index.html"))
}
