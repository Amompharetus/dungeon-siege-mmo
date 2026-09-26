// joinguard.go — 0.5.0: join tickets and the character experience record.
//
// Only players who start the game through the launcher can join a game server:
//   - session/start and session/renew return a "join ticket" for the server the session is for, signed with that
//     server's join key (HMAC-SHA256 derived from its shard_key). The launcher writes it into SiegeHost.ini; the
//     player's SiegeHost sends it with the DirectPlay connect; the server's SiegeHost checks it and refuses the
//     connection without a valid one.
//   - The ticket carries the account's characters and the highest experience the MMO servers have seen for each.
//     The server kicks a character that arrives with more experience than recorded (played or edited outside the
//     MMO), or an unknown character that is not new.
//   - Game servers report the experience of verified players in their heartbeat (pcN=account|hexhero|xp); the portal
//     raises the record. Refusals/kicks (refN=unix|account|reason) are passed to that player's launcher.
//
// Characters that already existed in an account's online save when this version first started are "accepted once":
// the first time a server sees them, their experience becomes the record.
package main

import (
	"archive/zip"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

type CharRec struct {
	Name       string    `json:"name"`
	XP         int64     `json:"xp"`          // highest party experience a game server reported
	AcceptOnce bool      `json:"accept_once"` // accept whatever it has the next time a server sees it (existing characters, admin allow)
	Updated    time.Time `json:"updated"`
}

type refusal struct {
	when   time.Time
	server string
	reason string
}

// charKey must match SiegeHost's CharKey: ASCII letters lowercased, digits and non-ASCII bytes kept, everything else dropped.
func charKey(s string) string {
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c >= 'A' && c <= 'Z':
			b.WriteByte(c + 32)
		case (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80:
			b.WriteByte(c)
		}
	}
	return b.String()
}

func joinKey(shardKey string) []byte {
	m := hmac.New(sha256.New, []byte(shardKey))
	m.Write([]byte("ds1mmo-join-ticket-v1"))
	return m.Sum(nil)
}

// JoinKeyHex is what siegeserver writes into SiegeHost-server.ini ([JoinGuard] Key=). Same derivation in both programs.
func JoinKeyHex(shardKey string) string { return hex.EncodeToString(joinKey(shardKey)) }

// initCharsLocked marks the characters in the account's current online save as accept-once (first run of 0.5.0).
func (p *Portal) initCharsLocked(acct *Account) {
	if acct.CharsInit {
		return
	}
	acct.CharsInit = true
	if acct.Chars == nil {
		acct.Chars = map[string]*CharRec{}
	}
	zr, err := zip.OpenReader(filepath.Join(p.saveDir(acct), "current.zip"))
	if err != nil {
		return
	}
	defer zr.Close()
	var names []string
	for _, f := range zr.File {
		dir, file := filepath.Split(filepath.FromSlash(f.Name))
		if !strings.EqualFold(filepath.Clean(dir), "Save") || !strings.HasSuffix(strings.ToLower(file), ".dsparty") {
			continue
		}
		name := file[:len(file)-len(".dsparty")]
		if k := charKey(name); k != "" && acct.Chars[k] == nil {
			acct.Chars[k] = &CharRec{Name: name, XP: -1, AcceptOnce: true, Updated: time.Now().UTC()}
			names = append(names, name)
		}
	}
	if len(names) > 0 {
		log.Printf("join guard: %s: existing characters accepted once: %s", acct.Name, strings.Join(names, ", "))
	}
}

func (p *Portal) initAllChars() {
	p.mu.Lock()
	defer p.mu.Unlock()
	changed := false
	for _, a := range p.accounts {
		if !a.CharsInit {
			p.initCharsLocked(a)
			changed = true
		}
	}
	if changed {
		must(p.saveAccountsLocked())
	}
}

// ticketLocked builds the join ticket for acct on server id. Empty when that server has no shard key. Caller holds p.mu.
func (p *Portal) ticketLocked(acct *Account, serverID string) string {
	var key string
	for _, s := range p.cfg.Servers {
		if strings.EqualFold(s.ID, serverID) {
			key, serverID = s.ShardKey, s.ID
		}
	}
	if key == "" {
		return ""
	}
	var chars []string
	keys := make([]string, 0, len(acct.Chars))
	for k := range acct.Chars {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		c := acct.Chars[k]
		xp := c.XP
		if c.AcceptOnce {
			xp = -1
		}
		chars = append(chars, hex.EncodeToString([]byte(c.Name))+":"+strconv.FormatInt(xp, 10))
	}
	exp := time.Now().Add(time.Duration(p.cfg.JoinTicketMinutes) * time.Minute).Unix()
	payload := fmt.Sprintf("DSMT1|%s|%s|%d|%s|%d|%d|%d", acct.Name, serverID, exp, strings.Join(chars, ","),
		p.cfg.JoinNewCharMaxXP, p.cfg.JoinToleranceXP, p.cfg.JoinTolerancePct)
	m := hmac.New(sha256.New, joinKey(key))
	m.Write([]byte(payload))
	return payload + "|" + hex.EncodeToString(m.Sum(nil))
}

// shardReportLocked applies pcN / refN heartbeat lines from a game server and removes them from the public fields.
func (p *Portal) shardReportLocked(serverID string, fields map[string]string) {
	dirty := false
	for k, v := range fields {
		switch {
		case strings.HasPrefix(k, "pc") && isDigits(k[2:]):
			delete(fields, k)
			f := strings.Split(v, "|")
			if len(f) != 3 {
				continue
			}
			acct := p.accounts[strings.ToLower(f[0])]
			nameB, err1 := hex.DecodeString(f[1])
			xp, err2 := strconv.ParseInt(f[2], 10, 64)
			if acct == nil || err1 != nil || err2 != nil || xp < 0 || len(nameB) == 0 || len(nameB) > 64 {
				continue
			}
			name := string(nameB)
			key := charKey(name)
			if key == "" {
				continue
			}
			if acct.Chars == nil {
				acct.Chars = map[string]*CharRec{}
			}
			rec := acct.Chars[key]
			switch {
			case rec == nil:
				acct.Chars[key] = &CharRec{Name: name, XP: xp, Updated: time.Now().UTC()}
				log.Printf("join guard: %s: new character %q recorded on %s (experience %d)", acct.Name, name, serverID, xp)
				dirty = true
			case rec.AcceptOnce:
				rec.AcceptOnce, rec.XP, rec.Name, rec.Updated = false, xp, name, time.Now().UTC()
				log.Printf("join guard: %s: character %q first seen on %s, experience %d is now its record", acct.Name, name, serverID, xp)
				dirty = true
			case xp > rec.XP:
				rec.XP, rec.Updated = xp, time.Now().UTC()
				dirty = true
			}
		case strings.HasPrefix(k, "ref") && isDigits(k[3:]):
			delete(fields, k)
			f := strings.SplitN(v, "|", 3)
			if len(f) != 3 || f[1] == "-" {
				continue
			}
			unix, _ := strconv.ParseInt(f[0], 10, 64)
			when := time.Unix(unix, 0)
			key := strings.ToLower(f[1])
			if prev, ok := p.refusals[key]; ok && !when.After(prev.when) {
				continue
			}
			if time.Since(when) > 10*time.Minute {
				continue
			}
			p.refusals[key] = refusal{when: when, server: serverID, reason: f[2]}
			log.Printf("join guard: %s refused/kicked on %s: %s", f[1], serverID, f[2])
		}
	}
	if dirty && time.Since(p.charsSaved) > 30*time.Second {
		p.charsSaved = time.Now()
		if err := p.saveAccountsLocked(); err != nil {
			log.Printf("join guard: save accounts: %v", err)
		}
	} else if dirty {
		p.charsDirty = true
	}
}

func isDigits(s string) bool {
	if s == "" {
		return false
	}
	for _, c := range s {
		if c < '0' || c > '9' {
			return false
		}
	}
	return true
}

// charsFlushLoop writes experience records that arrived within 30 s of the previous write.
func (p *Portal) charsFlushLoop() {
	for range time.Tick(30 * time.Second) {
		p.mu.Lock()
		if p.charsDirty {
			p.charsDirty = false
			p.charsSaved = time.Now()
			if err := p.saveAccountsLocked(); err != nil {
				log.Printf("join guard: save accounts: %v", err)
			}
		}
		p.mu.Unlock()
	}
}

// refusalTextLocked returns (and forgets) a refusal for this account newer than since. Caller holds p.mu.
func (p *Portal) refusalTextLocked(acct *Account, since time.Time) string {
	key := strings.ToLower(acct.Name)
	r, ok := p.refusals[key]
	if !ok || r.when.Before(since.Add(-5*time.Second)) {
		return ""
	}
	delete(p.refusals, key)
	reason := r.reason
	if i := strings.Index(reason, ":"); i >= 0 && strings.HasPrefix(reason, "character:") {
		reason = reason[i+1:]
	} else {
		switch reason {
		case "no-ticket":
			reason = "no join pass was sent (use Dungeon Siege MMO.bat)"
		case "bad-ticket":
			reason = "the join pass was not valid"
		case "expired":
			reason = "the join pass had expired"
		case "wrong-server":
			reason = "the join pass was for another server"
		case "in-use":
			reason = "this account was already in the game on that server"
		}
	}
	return reason
}

// POST /api/admin/character {"Account":"...","Name":"..."} — loopback only. The character is accepted once again
// (its next experience becomes the record). For a player wrongly kicked, or a character restored from a backup.
func (p *Portal) handleAdminCharacter(w http.ResponseWriter, r *http.Request) {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	if ip := net.ParseIP(host); ip == nil || !ip.IsLoopback() {
		jsonErr(w, 403, "admin only")
		return
	}
	var req struct{ Account, Name string }
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	acct := p.accounts[strings.ToLower(strings.TrimSpace(req.Account))]
	key := charKey(req.Name)
	if acct == nil || key == "" {
		jsonErr(w, 404, "no such account (or empty character name)")
		return
	}
	if acct.Chars == nil {
		acct.Chars = map[string]*CharRec{}
	}
	rec := acct.Chars[key]
	if rec == nil {
		rec = &CharRec{Name: req.Name}
		acct.Chars[key] = rec
	}
	rec.AcceptOnce, rec.XP, rec.Updated = true, -1, time.Now().UTC()
	if err := p.saveAccountsLocked(); err != nil {
		jsonErr(w, 500, "could not store")
		return
	}
	log.Printf("admin: %s character %q will be accepted once", acct.Name, rec.Name)
	var list []string
	for _, c := range acct.Chars {
		list = append(list, fmt.Sprintf("%s (record %d%s)", c.Name, c.XP, map[bool]string{true: ", accept once"}[c.AcceptOnce]))
	}
	sort.Strings(list)
	jsonOut(w, map[string]any{"ok": true, "account": acct.Name, "characters": list})
}

// adminCharacterCLI: siegeportal -config portal.json -allow-character ACCOUNT/CHARACTER
func adminCharacterCLI(cfgPath, arg string) error {
	acc, name, ok := strings.Cut(arg, "/")
	if !ok {
		return errors.New("use -allow-character ACCOUNT/CHARACTER")
	}
	cfg := loadConfig(cfgPath)
	port := cfg.Listen
	if i := strings.LastIndex(port, ":"); i >= 0 {
		port = port[i+1:]
	}
	body, _ := json.Marshal(map[string]string{"Account": acc, "Name": name})
	resp, err := http.Post("http://127.0.0.1:"+port+"/api/admin/character", "application/json", strings.NewReader(string(body)))
	if err != nil {
		return fmt.Errorf("portal not reachable on 127.0.0.1:%s: %v", port, err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	if resp.StatusCode != 200 {
		return errors.New(fmt.Sprint(out["error"]))
	}
	fmt.Printf("account %v: character %q will be accepted once. Characters: %v\n", out["account"], name, out["characters"])
	return nil
}
