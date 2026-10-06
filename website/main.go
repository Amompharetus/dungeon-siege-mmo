package main

import (
	"archive/zip"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
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

const (
	portalURL    = "http://127.0.0.1:8440"
	portalDir    = "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_central"
	webDir       = "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_web"
	tankdumpBin  = "/usr/local/bin/tankdump"
	listenAddr   = "127.0.0.1:8088"
	gmCookieName = "ds1mmo_gm_session"
	awsCredsFile = "/etc/ds1mmo/aws-eu.csv"
	awsRegion    = "eu-central-1"
	awsInstance  = "i-00000000000000001"
)

var gmUsers = map[string]bool{
	"admin": true,
	"gm1":      true,
	"gm2":       true,
}

func isAuthorizedGM(username string) bool {
	return gmUsers[strings.ToLower(strings.TrimSpace(username))]
}

type CachedChar struct {
	Name      string `json:"name"`
	Class     string `json:"class"`
	Race      string `json:"race"`
	Level     int    `json:"level"`
	SaveSHA   string `json:"save_sha"`
	StartLoc  string `json:"start_loc"`
	MapAreaId string `json:"map_area_id"`
}

type PlayerInfo struct {
	AccountName   string `json:"account_name"`
	CharacterName string `json:"character_name"`
	Level         int    `json:"level"`
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
	case "dunes", "dunes2", "eg_seck_dungeon":
		return "Endless Desert", "endless_desert"
	case "dunes2hades", "hades":
		return "Volcanic Caverns", "volcanic_caverns"
	case "dunes2island", "island":
		return "Eastern Island", "eastern_island"

	// Central Hub
	case "new_hub", "hub_quest_dun", "hub":
		return "Utraean Basilicus", "utraean_basilicus"

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
	realmCache  = &cachedBuild[RealmStatus]{ttl: 5 * time.Second, build: buildRealmStatus}
	ladderCache = &cachedBuild[[]LadderCharacter]{ttl: 30 * time.Second, build: buildLadderCharacters}
)

func getRealmStatus() RealmStatus {
	return realmCache.get()
}

func getLadderCharacters() []LadderCharacter {
	return ladderCache.get()
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
	startLocRe   = regexp.MustCompile(`(?i)multiplayer_world\s*=\s*"?([a-zA-Z0-9_]+)"?`)
	regionRe     = regexp.MustCompile(`(?i)region\s*=\s*([a-zA-Z0-9_]+)`)
)

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

func callEC2(action string, extraParams map[string]string) ([]byte, int, error) {
	creds, err := loadAWSCreds(awsCredsFile)
	if err != nil {
		return nil, 0, fmt.Errorf("load credentials: %w", err)
	}
	endpoint := fmt.Sprintf("https://ec2.%s.amazonaws.com/", awsRegion)
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
	query, authz, amzDate := signV4(creds, awsRegion, "ec2", u.Host, u.Path, params, time.Now())
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

func getNAServerState() string {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, "systemctl", "is-active", "ds1mmo-na").CombinedOutput()
	state := strings.TrimSpace(string(out))
	if err != nil && state == "" {
		return "stopped"
	}
	switch state {
	case "active":
		return "running"
	case "inactive":
		return "stopped"
	case "failed":
		return "failed"
	case "activating":
		return "starting"
	case "deactivating":
		return "stopping"
	default:
		return state
	}
}

func getEUServerState() string {
	body, code, err := callEC2("DescribeInstances", map[string]string{
		"InstanceId.1": awsInstance,
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

func executeNAAction(action string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	var cmd *exec.Cmd
	switch action {
	case "start":
		cmd = exec.CommandContext(ctx, "sudo", "/usr/bin/systemctl", "start", "ds1mmo-na")
	case "restart":
		cmd = exec.CommandContext(ctx, "sudo", "/usr/bin/systemctl", "restart", "ds1mmo-na")
	case "stop":
		cmd = exec.CommandContext(ctx, "sudo", "/usr/bin/systemctl", "stop", "ds1mmo-na")
	default:
		return fmt.Errorf("invalid action: %s", action)
	}
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s failed: %v (%s)", action, err, strings.TrimSpace(string(out)))
	}
	return nil
}

func executeEUAction(action string) error {
	var ec2Action string
	switch action {
	case "start":
		ec2Action = "StartInstances"
	case "restart":
		st := getEUServerState()
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
	body, code, err := callEC2(ec2Action, map[string]string{
		"InstanceId.1": awsInstance,
	})
	if err != nil {
		return fmt.Errorf("ec2 %s: %w", ec2Action, err)
	}
	if code != 200 {
		return fmt.Errorf("ec2 %s returned HTTP %d: %s", ec2Action, code, string(body))
	}
	return nil
}

func main() {
	loadSessions()

	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/status", handleAPIStatus)
	mux.HandleFunc("GET /api/ladder", handleAPILadder)
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
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
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

	// 1. Fetch live portal status
	resp, err := portalHTTP.Get(portalURL + "/api/status")
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
				town := formatTownName(s.Game["start_location"])
				serverTown[s.ID] = town
				for k, v := range s.Game {
					if strings.HasPrefix(k, "pc") {
						parts := strings.Split(v, "|")
						if len(parts) > 0 && parts[0] != "" {
							rawAcct := strings.TrimSpace(parts[0])
							acctLower := strings.ToLower(rawAcct)
							verifiedPlayers[acctLower] = town
							heartbeatServers[acctLower] = s.ID
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
				if s.ID == "na" {
					res.NA = st
				} else if s.ID == "eu" {
					res.EU = st
				}
			}
		}
	}

	// Also inspect local shard heartbeat files directly IF fresh and server is online
	hbPaths := []struct {
		path string
		id   string
	}{
		{path: "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_na_server/game/SiegeHostLogs/heartbeat.txt", id: "na"},
		{path: "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_eu_server/game/SiegeHostLogs/heartbeat.txt", id: "eu"},
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

	finalOnlineSessions := make(map[string]string) // acctLower -> srvID
	serverVerifiedCount := make(map[string]int)

	// Anyone in game server heartbeat is 100% active and connected!
	for acctLower, srvID := range heartbeatServers {
		srvLower := strings.ToLower(srvID)
		if serverActive[srvLower] {
			finalOnlineSessions[acctLower] = srvLower
			serverVerifiedCount[srvLower]++
			if _, ok := sessionRawNames[acctLower]; !ok {
				sessionRawNames[acctLower] = canonicalAcctNames[acctLower]
			}
		}
	}

	// For servers that have connected players (pCount > 0) but heartbeat didn't provide pc* lines (e.g. EU),
	// match up to the remaining player count using active sessions on that server, ordered by newest session start
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
			hasUploadedInSession[c.acctLower] = true
		}
	}

	accounts := loadAccounts()

	for acctLower, srvID := range finalOnlineSessions {
		acct := accounts[acctLower]

		canonicalName := acctLower
		if n, ok := sessionRawNames[acctLower]; ok && n != "" {
			canonicalName = n
		}
		if acct != nil && acct.Name != "" {
			canonicalName = acct.Name
		}

		var newestChar *CharRec
		saveSHA := ""
		if acct != nil {
			saveSHA = acct.SaveSHA
			for _, ch := range acct.Chars {
				if newestChar == nil || ch.Updated.After(newestChar.Updated) {
					newestChar = ch
				}
			}
		}

		charName := canonicalName
		if hero, ok := heartbeatHeroes[acctLower]; ok && hero != "" {
			charName = hero
		} else if newestChar != nil && newestChar.Name != "" {
			charName = newestChar.Name
		}

		charDetails := resolveCharacterDetails(canonicalName, charName, saveSHA, newestChar)

		// If save file level is not available (level <= 1) and shard heartbeat reported XP, fallback to formula
		if charDetails.Level <= 1 {
			if hbXP, ok := heartbeatXP[acctLower]; ok && hbXP > 0 {
				calcLvl := int(math.Floor(math.Sqrt(float64(hbXP)/120.0))) + 1
				if calcLvl > 1 {
					charDetails.Level = calcLvl
				}
			}
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

	// Always rank players from higher level to lower (tie-breaker: character name alphabetical)
	sortPlayers := func(players []PlayerInfo) {
		sort.Slice(players, func(i, j int) bool {
			if players[i].Level != players[j].Level {
				return players[i].Level > players[j].Level
			}
			return strings.ToLower(players[i].CharacterName) < strings.ToLower(players[j].CharacterName)
		})
	}
	sortPlayers(res.NA.Players)
	sortPlayers(res.EU.Players)

	res.Ladder = getLadderCharacters()

	return res
}

func parseActiveSessions() (map[string]string, map[string]string, map[string]time.Time, map[string]bool) {
	sessions := make(map[string]string) // lower(account) -> server (na/eu)
	rawNames := make(map[string]string) // lower(account) -> original casing
	sessionStartTimes := make(map[string]time.Time)
	hasUploadedInSession := make(map[string]bool)
	lastSeen := make(map[string]time.Time)

	logPath := filepath.Join(portalDir, "logs", "portal.log")
	f, err := os.Open(logPath)
	if err != nil {
		return sessions, rawNames, sessionStartTimes, hasUploadedInSession
	}
	defer f.Close()

	stat, _ := f.Stat()
	offset := int64(0)
	if stat != nil && stat.Size() > 10*1024*1024 {
		offset = stat.Size() - 10*1024*1024
	}
	_, _ = f.Seek(offset, io.SeekStart)

	b, _ := io.ReadAll(f)
	lines := strings.Split(string(b), "\n")

	for _, line := range lines {
		var year, month, day, hour, min, sec int
		if len(line) >= 19 && line[4] == '/' && line[7] == '/' && line[10] == ' ' {
			fmt.Sscanf(line[:19], "%04d/%02d/%02d %02d:%02d:%02d", &year, &month, &day, &hour, &min, &sec)
		}
		lineTime := time.Date(year, time.Month(month), day, hour, min, sec, 0, time.UTC)

		if strings.Contains(line, "session start:") {
			idx := strings.Index(line, "session start: ")
			if idx >= 0 {
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
					lastSeen[acctLower] = lineTime
					sessionStartTimes[acctLower] = lineTime
					hasUploadedInSession[acctLower] = false
				}
			}
		} else if strings.Contains(line, "session end:") {
			idx := strings.Index(line, "session end: ")
			if idx >= 0 {
				acct := strings.TrimSpace(line[idx+len("session end: "):])
				acctLower := strings.ToLower(acct)
				delete(sessions, acctLower)
				delete(rawNames, acctLower)
				delete(lastSeen, acctLower)
				delete(sessionStartTimes, acctLower)
				delete(hasUploadedInSession, acctLower)
			}
		} else if strings.Contains(line, "save uploaded:") {
			idx := strings.Index(line, "save uploaded: ")
			if idx >= 0 {
				rem := line[idx+len("save uploaded: "):]
				parts := strings.SplitN(rem, " ", 2)
				if len(parts) > 0 {
					acct := strings.TrimSpace(parts[0])
					acctLower := strings.ToLower(acct)
					if _, ok := sessions[acctLower]; ok {
						lastSeen[acctLower] = lineTime
						hasUploadedInSession[acctLower] = true
					}
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
			delete(lastSeen, acctLower)
		}
	}

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

func resolveCharacterDetails(account, charName, saveSHA string, rec *CharRec) CachedChar {
	cacheKey := fmt.Sprintf("%s:%s", strings.ToLower(account), strings.ToLower(charName))

	cacheMu.RLock()
	cached, found := charCache[cacheKey]
	cacheMu.RUnlock()

	if found && (saveSHA == "" || cached.SaveSHA == saveSHA) && cached.Level >= 0 && cached.StartLoc != "" && cached.MapAreaId != "" && cached.Race != "" {
		return cached
	}

	lvl := 0
	if rec != nil && rec.XP > 0 {
		lvl = int(math.Floor(math.Sqrt(float64(rec.XP)/120.0))) + 1
	}
	res := CachedChar{
		Name:      charName,
		Class:     "Adventurer",
		Race:      "Human",
		Level:     lvl,
		SaveSHA:   saveSHA,
		StartLoc:  "",
		MapAreaId: "",
	}

	zipPath := filepath.Join(portalDir, "portal-data", "saves", strings.ToLower(account), "current.zip")
	r, err := zip.OpenReader(zipPath)
	if err == nil {
		defer r.Close()
		var targetFile *zip.File
		var fallbackFile *zip.File
		prefsLoc := ""
		prefsMapID := ""

		for _, f := range r.File {
			if strings.EqualFold(filepath.Base(f.Name), "prefs.gas") {
				if rc, err := f.Open(); err == nil {
					b, _ := io.ReadAll(rc)
					rc.Close()
					if m := startLocRe.FindStringSubmatch(string(b)); len(m) > 1 {
						prefsLoc, prefsMapID = regionToArea(m[1])
					}
				}
			}
			if strings.HasPrefix(f.Name, "Save/") && strings.HasSuffix(f.Name, ".dsparty") && !strings.HasSuffix(f.Name, ".bak") {
				if fallbackFile == nil {
					fallbackFile = f
				}
				cleanName := strings.TrimSuffix(strings.TrimPrefix(f.Name, "Save/"), ".dsparty")
				normClean := strings.ToLower(strings.ReplaceAll(cleanName, "_", " "))
				normChar := strings.ToLower(strings.ReplaceAll(charName, "_", " "))
				normAcct := strings.ToLower(strings.ReplaceAll(account, "_", " "))

				if normClean == normChar || strings.EqualFold(cleanName, charName) || (charName == "" && (normClean == normAcct || strings.EqualFold(cleanName, account))) {
					targetFile = f
				}
			}
		}

		if targetFile == nil && charName == "" {
			targetFile = fallbackFile
		}

		if targetFile != nil {
			cleanName := strings.TrimSuffix(strings.TrimPrefix(targetFile.Name, "Save/"), ".dsparty")
			rc, err := targetFile.Open()
			if err == nil {
				tmpDSP := fmt.Sprintf("/tmp/probe_%d_%s.dsparty", time.Now().UnixNano(), cleanName)
				outF, err := os.Create(tmpDSP)
				if err == nil {
					_, _ = io.Copy(outF, rc)
					outF.Close()
					rc.Close()

					tmpDir := fmt.Sprintf("/tmp/probe_out_%d_%s", time.Now().UnixNano(), cleanName)
					_ = os.MkdirAll(tmpDir, 0755)

					// 1. Extract party.gas (skills, class, level, screen_name, race)
					ctxParty, cancelParty := context.WithTimeout(context.Background(), 5*time.Second)
					cmdParty := exec.CommandContext(ctxParty, tankdumpBin, tmpDSP, "party.gas", "-e")
					cmdParty.Dir = tmpDir
					outParty, errParty := cmdParty.CombinedOutput()
					cancelParty()
					if errParty == nil || len(outParty) == 0 {
						gasPath := filepath.Join(tmpDir, "party.gas")
						if gasBytes, err := os.ReadFile(gasPath); err == nil {
							gasStr := string(gasBytes)
							if m := templateRe.FindStringSubmatch(gasStr); len(m) > 1 {
								res.Race = templateToRace(m[1])
							}
							if m := classRe.FindStringSubmatch(gasStr); len(m) > 1 {
								cls := strings.TrimSpace(m[1])
								if len(cls) > 0 {
									cls = strings.ToUpper(cls[:1]) + strings.ToLower(cls[1:])
								}
								res.Class = cls
							}
							if m := uberLvlRe.FindStringSubmatch(gasStr); len(m) > 1 {
								if fl, err := strconv.ParseFloat(m[1], 64); err == nil {
									res.Level = int(math.Floor(fl))
									if res.Level < 0 {
										res.Level = 0
									}
								}
							}
							if m := screenNmRe.FindStringSubmatch(gasStr); len(m) > 1 {
								res.Name = m[1]
							}
						}
					}

					// 2. Extract info.gas (actual current region player is in!)
					ctxInfo, cancelInfo := context.WithTimeout(context.Background(), 5*time.Second)
					cmdInfo := exec.CommandContext(ctxInfo, tankdumpBin, tmpDSP, "info.gas", "-e")
					cmdInfo.Dir = tmpDir
					outInfo, errInfo := cmdInfo.CombinedOutput()
					cancelInfo()
					if errInfo == nil || len(outInfo) == 0 {
						infoPath := filepath.Join(tmpDir, "info.gas")
						if infoBytes, err := os.ReadFile(infoPath); err == nil {
							infoStr := string(infoBytes)
							if m := regionRe.FindStringSubmatch(infoStr); len(m) > 1 {
								reg := strings.TrimSpace(m[1])
								if reg != "" {
									loc, mId := regionToArea(reg)
									res.StartLoc = loc
									res.MapAreaId = mId
								}
							}
						}
					}

					_ = os.Remove(tmpDSP)
					_ = os.RemoveAll(tmpDir)
				} else {
					rc.Close()
				}
			}
		}

		// Fallback location if info.gas did not specify a valid region
		if res.StartLoc == "" {
			if prefsLoc != "" {
				res.StartLoc = prefsLoc
				res.MapAreaId = prefsMapID
			} else {
				res.StartLoc = "Elddim"
				res.MapAreaId = "elddim"
			}
		}
	} else {
		res.StartLoc = "Elddim"
		res.MapAreaId = "elddim"
	}

	cacheMu.Lock()
	charCache[cacheKey] = res
	cacheMu.Unlock()

	return res
}

func getLiveOnlineSnapshot() (map[string]string, map[string]string, map[string]string) {
	onlineAccts := make(map[string]string)
	onlineHeroes := make(map[string]string)
	onlineMap := make(map[string]string)

	resp, err := portalHTTP.Get(portalURL + "/api/status")
	if err != nil {
		return onlineAccts, onlineHeroes, onlineMap
	}
	defer resp.Body.Close()

	var portalResp struct {
		Servers []struct {
			ID      string            `json:"id"`
			Online  bool              `json:"online"`
			Players string            `json:"players"`
			Game    map[string]string `json:"game"`
		} `json:"servers"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&portalResp); err != nil {
		return onlineAccts, onlineHeroes, onlineMap
	}

	activeServers := make(map[string]int)
	for _, s := range portalResp.Servers {
		sID := strings.ToLower(s.ID)
		pCount, _ := strconv.Atoi(s.Players)
		if s.Online && pCount > 0 {
			activeServers[sID] = pCount
			for k, v := range s.Game {
				if strings.HasPrefix(k, "pc") {
					parts := strings.Split(v, "|")
					if len(parts) > 0 && parts[0] != "" {
						acctLower := strings.ToLower(strings.TrimSpace(parts[0]))
						shardUpper := strings.ToUpper(s.ID)
						onlineAccts[acctLower] = shardUpper
						if len(parts) > 1 && parts[1] != "" {
							if dec, err := hex.DecodeString(parts[1]); err == nil && len(dec) > 0 {
								hero := string(dec)
								onlineHeroes[acctLower] = hero
								onlineMap[acctLower+":"+strings.ToLower(hero)] = shardUpper
							}
						}
					}
				}
			}
		}
	}

	// Also inspect local heartbeat file IF fresh and server is online
	hbPaths := []struct {
		path string
		id   string
	}{
		{path: "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_na_server/game/SiegeHostLogs/heartbeat.txt", id: "na"},
		{path: "/home/amp/.ampdata/instances/ADS01/localaux/ds1mmo_eu_server/game/SiegeHostLogs/heartbeat.txt", id: "eu"},
	}
	for _, hb := range hbPaths {
		if _, ok := activeServers[hb.id]; !ok {
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
			lines := strings.Split(content, "\n")
			for _, l := range lines {
				l = strings.TrimSpace(l)
				if strings.HasPrefix(l, "pc") && strings.Contains(l, "=") {
					val := strings.TrimSpace(strings.SplitN(l, "=", 2)[1])
					parts := strings.Split(val, "|")
					if len(parts) > 0 && parts[0] != "" {
						acctLower := strings.ToLower(strings.TrimSpace(parts[0]))
						shardUpper := strings.ToUpper(hb.id)
						onlineAccts[acctLower] = shardUpper
						if len(parts) > 1 && parts[1] != "" {
							if dec, err := hex.DecodeString(parts[1]); err == nil && len(dec) > 0 {
								hero := string(dec)
								onlineHeroes[acctLower] = hero
								onlineMap[acctLower+":"+strings.ToLower(hero)] = shardUpper
							}
						}
					}
				}
			}
		}
	}

	// For any active server needing players, consult active sessions
	sessions, _, startTimes, _ := parseActiveSessions()
	for sID, count := range activeServers {
		shardUpper := strings.ToUpper(sID)
		verifiedCount := 0
		for _, shard := range onlineAccts {
			if shard == shardUpper {
				verifiedCount++
			}
		}
		needed := count - verifiedCount
		if needed > 0 {
			type cand struct {
				acct  string
				start time.Time
			}
			var cands []cand
			for acct, s := range sessions {
				if strings.ToLower(s) == sID {
					if _, ok := onlineAccts[acct]; !ok {
						cands = append(cands, cand{acct: acct, start: startTimes[acct]})
					}
				}
			}
			sort.Slice(cands, func(i, j int) bool {
				return cands[i].start.After(cands[j].start)
			})
			for i := 0; i < len(cands) && i < needed; i++ {
				onlineAccts[cands[i].acct] = shardUpper
			}
		}
	}

	return onlineAccts, onlineHeroes, onlineMap
}

func formatRelativeTime(t time.Time) string {
	if t.IsZero() || t.Year() < 2020 {
		return "Unknown"
	}
	diff := time.Since(t)
	if diff < 0 || diff < 2*time.Minute {
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

func buildLadderCharacters() []LadderCharacter {
	accounts := loadAccounts()
	var list []LadderCharacter

	onlineAccts, onlineHeroes, onlineMap := getLiveOnlineSnapshot()

	for acctLower, acct := range accounts {
		canonicalAcctName := acct.Name
		if canonicalAcctName == "" {
			canonicalAcctName = acctLower
		}

		var newestChar *CharRec
		if acct.Chars != nil {
			for _, c := range acct.Chars {
				if newestChar == nil || c.Updated.After(newestChar.Updated) {
					newestChar = c
				}
			}
		}

		zipPath := filepath.Join(portalDir, "portal-data", "saves", acctLower, "current.zip")
		r, err := zip.OpenReader(zipPath)
		if err == nil {
			for _, f := range r.File {
				if strings.HasPrefix(f.Name, "Save/") && strings.HasSuffix(f.Name, ".dsparty") && !strings.HasSuffix(f.Name, ".bak") {
					cleanName := strings.TrimSuffix(strings.TrimPrefix(f.Name, "Save/"), ".dsparty")

					var charRec *CharRec
					if acct.Chars != nil {
						for k, c := range acct.Chars {
							if strings.EqualFold(k, cleanName) || strings.EqualFold(c.Name, cleanName) {
								charRec = c
								break
							}
						}
					}

					charDetails := resolveCharacterDetails(canonicalAcctName, cleanName, acct.SaveSHA, charRec)

					var xp int64 = 0
					if charRec != nil {
						xp = charRec.XP
					}

					race := charDetails.Race
					if race == "" {
						race = "Human"
					}

					var lastTime time.Time
					if charRec != nil && !charRec.Updated.IsZero() && charRec.Updated.Year() >= 2020 {
						lastTime = charRec.Updated
					}
					if f != nil {
						mod := f.ModTime()
						if !mod.IsZero() && mod.Year() >= 2020 && mod.After(lastTime) {
							lastTime = mod
						}
					}
					if !acct.SaveTime.IsZero() && acct.SaveTime.Year() >= 2020 && acct.SaveTime.After(lastTime) {
						lastTime = acct.SaveTime
					}
					if lastTime.IsZero() && !acct.LastLogin.IsZero() && acct.LastLogin.Year() >= 2020 {
						lastTime = acct.LastLogin
					}

					isOnline := false
					onlineShard := ""
					lookupKey := acctLower + ":" + strings.ToLower(charDetails.Name)
					if s, ok := onlineMap[lookupKey]; ok {
						isOnline = true
						onlineShard = s
					} else if s, ok := onlineMap[acctLower+":"+strings.ToLower(cleanName)]; ok {
						isOnline = true
						onlineShard = s
					} else if s, ok := onlineAccts[acctLower]; ok {
						if hero, ok2 := onlineHeroes[acctLower]; ok2 {
							if strings.EqualFold(hero, charDetails.Name) || strings.EqualFold(hero, cleanName) {
								isOnline = true
								onlineShard = s
							}
						} else {
							// If joinguard didn't specify hero, only the account's newest character is marked online
							if newestChar != nil && (strings.EqualFold(newestChar.Name, charDetails.Name) || strings.EqualFold(newestChar.Name, cleanName)) {
								isOnline = true
								onlineShard = s
							}
						}
					}

					lastSeenStr := formatRelativeTime(lastTime)
					if isOnline {
						if onlineShard != "" {
							lastSeenStr = "Online (" + onlineShard + ")"
						} else {
							lastSeenStr = "Online Now"
						}
					}

					var lastSeenUnix int64 = 0
					if !lastTime.IsZero() && lastTime.Year() >= 2020 {
						lastSeenUnix = lastTime.Unix()
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
			r.Close()
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

	zipPath := filepath.Join(portalDir, "portal-data", "saves", acctLower, "current.zip")
	r, err := zip.OpenReader(zipPath)
	if err == nil {
		for _, f := range r.File {
			if strings.HasPrefix(f.Name, "Save/") && strings.HasSuffix(f.Name, ".dsparty") && !strings.HasSuffix(f.Name, ".bak") {
				cleanName := strings.TrimSuffix(strings.TrimPrefix(f.Name, "Save/"), ".dsparty")
				var charRec *CharRec
				if acct != nil && acct.Chars != nil {
					for k, c := range acct.Chars {
						if strings.EqualFold(k, cleanName) || strings.EqualFold(c.Name, cleanName) {
							charRec = c
							break
						}
					}
				}
				details := resolveCharacterDetails(accountName, cleanName, saveSHA, charRec)
				race := details.Race
				if race == "" {
					race = "Human"
				}
				cls := details.Class
				if cls == "" {
					cls = "Adventurer"
				}
				lvl := details.Level
				if lvl < 1 {
					lvl = 1
				}
				name := details.Name
				if name == "" {
					name = cleanName
				}
				chars = append(chars, UserCharacter{
					Name:  name,
					Race:  race,
					Class: cls,
					Level: lvl,
				})
			}
		}
		r.Close()
	}

	if len(chars) == 0 && acct != nil && acct.Chars != nil && err != nil {
		for _, c := range acct.Chars {
			lvl := 1
			if c.XP > 0 {
				lvl = int(math.Floor(math.Sqrt(float64(c.XP)/120.0))) + 1
			}
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
			"authenticated": false,
			"username":      "",
			"is_gm":         false,
			"characters":    []UserCharacter{},
		})
		return
	}

	chars := getAccountCharacters(sess.Username)
	_ = json.NewEncoder(w).Encode(map[string]any{
		"authenticated": true,
		"username":      sess.Username,
		"is_gm":         sess.IsGM,
		"characters":    chars,
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
		"status":     "ok",
		"token":      token,
		"username":   sess.Username,
		"is_gm":      sess.IsGM,
		"characters": chars,
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
	if r.URL.Path != "/" && r.URL.Path != "/home" && r.URL.Path != "/realms" && r.URL.Path != "/world" && r.URL.Path != "/map" && r.URL.Path != "/world-map" && r.URL.Path != "/ladder" && r.URL.Path != "/gm" && r.URL.Path != "/gm-panel" {
		http.NotFound(w, r)
		return
	}
	http.ServeFile(w, r, filepath.Join(webDir, "index.html"))
}
