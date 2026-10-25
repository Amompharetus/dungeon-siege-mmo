// ondemand.go — start a game server (AWS EC2 instance) when a player wants it, stop it when it is empty.
//
// The central portal is always on; an on-demand shard (EU on AWS) is not. Flow:
//
//	launcher -> POST /api/servers/wake?id=eu (logged in) -> EC2 StartInstances
//	shard boots, SiegeServer pushes heartbeats -> /api/status shows it online with its new public IP
//	no players, no play sessions and no wake request for IdleMinutes -> EC2 StopInstances
//
// A running instance that never reports "in game" within BootTimeoutMinutes is stopped too (broken or forgotten).
// Maintenance: while <data_dir>/maintenance-<id> exists the portal never stops that instance (installs, updates).
//
// AWS access: the IAM user only has ec2:StartInstances/StopInstances on this one instance + ec2:DescribeInstances.
// Requests are signed with Signature Version 4 using the Go standard library (no SDK).
package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/csv"
	"encoding/hex"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

type OnDemand struct {
	Provider           string `json:"provider"`             // "aws", "systemd"
	Region             string `json:"region,omitempty"`     // "eu-central-1"
	InstanceID         string `json:"instance_id,omitempty"` // "i-00000000000000001"
	CredentialsFile    string `json:"credentials_file,omitempty"` // AWS console "Download .csv file" (Access key ID,Secret access key)
	Endpoint           string `json:"endpoint,omitempty"`   // optional override (tests); default https://ec2.<region>.amazonaws.com/
	IdleMinutes        int    `json:"idle_minutes"`         // stop after this long with nobody on it (default 5)
	BootTimeoutMinutes int    `json:"boot_timeout_minutes"` // stop a running instance that never reaches in-game (default 15)
	Unit               string `json:"unit,omitempty"`       // systemd unit (default instance_id if set)

	cmdRunner func(name string, args ...string) ([]byte, error) `json:"-"`
}

func (od *OnDemand) runCmd(name string, args ...string) ([]byte, error) {
	if od.cmdRunner != nil {
		return od.cmdRunner(name, args...)
	}
	return exec.Command(name, args...).CombinedOutput()
}

type odState struct {
	mu         sync.Mutex
	state      string // pending | running | stopping | stopped | shutting-down | terminated | unknown
	checked    time.Time
	lastErr    string
	lastWake   time.Time
	runSince   time.Time // first time we saw it running (or pending) in this cycle
	idleSince  time.Time
	wantStart  bool // woken while stopping: start as soon as it is stopped
	stopReason string
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
	return awsCreds{}, errors.New("no access key row found (expected the AWS console CSV: Access key ID,Secret access key)")
}

// ---------- SigV4 (EC2 Query API, GET) ----------

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

func sha256Hex(s string) string {
	h := sha256.Sum256([]byte(s))
	return hex.EncodeToString(h[:])
}

// signV4 returns the canonical query string and the Authorization header for a GET with no body.
func signV4(c awsCreds, region, service, host, path string, params map[string]string, now time.Time) (query, authz, amzDate string) {
	amzDate = now.UTC().Format("20060102T150405Z")
	date := amzDate[:8]
	keys := make([]string, 0, len(params))
	for k := range params {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	parts := make([]string, 0, len(keys))
	for _, k := range keys {
		parts = append(parts, awsEscape(k)+"="+awsEscape(params[k]))
	}
	query = strings.Join(parts, "&")
	canonical := strings.Join([]string{"GET", path, query, "host:" + host + "\n" + "x-amz-date:" + amzDate + "\n", "host;x-amz-date", sha256Hex("")}, "\n")
	scope := date + "/" + region + "/" + service + "/aws4_request"
	toSign := "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + sha256Hex(canonical)
	k := hmacSHA([]byte("AWS4"+c.secret), date)
	k = hmacSHA(k, region)
	k = hmacSHA(k, service)
	k = hmacSHA(k, "aws4_request")
	sig := hex.EncodeToString(hmacSHA(k, toSign))
	authz = fmt.Sprintf("AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-date, Signature=%s", c.id, scope, sig)
	return
}

var ec2Client = &http.Client{Timeout: 20 * time.Second}

func (od *OnDemand) call(action string) ([]byte, error) {
	if od.Provider == "systemd" || od.Provider == "local" {
		unit := od.Unit
		if unit == "" {
			unit = od.InstanceID
		}
		if unit == "" {
			return nil, errors.New("systemd on-demand requires unit or instance_id")
		}
		switch action {
		case "StartInstances":
			out, err := od.runCmd("sudo", "/bin/systemctl", "start", unit)
			if err != nil {
				return nil, fmt.Errorf("systemctl start %s: %v: %s", unit, err, strings.TrimSpace(string(out)))
			}
			return nil, nil
		case "StopInstances":
			out, err := od.runCmd("sudo", "/bin/systemctl", "stop", unit)
			if err != nil {
				return nil, fmt.Errorf("systemctl stop %s: %v: %s", unit, err, strings.TrimSpace(string(out)))
			}
			return nil, nil
		default:
			return nil, fmt.Errorf("unknown action %s", action)
		}
	}
	c, err := loadAWSCreds(od.CredentialsFile)
	if err != nil {
		return nil, fmt.Errorf("credentials: %v", err)
	}
	endpoint := od.Endpoint
	if endpoint == "" {
		endpoint = "https://ec2." + od.Region + ".amazonaws.com/"
	}
	u, err := url.Parse(endpoint)
	if err != nil {
		return nil, err
	}
	path := u.Path
	if path == "" {
		path = "/"
	}
	params := map[string]string{"Action": action, "Version": "2016-11-15", "InstanceId.1": od.InstanceID}
	query, authz, amzDate := signV4(c, od.Region, "ec2", u.Host, path, params, time.Now())
	req, _ := http.NewRequest("GET", u.Scheme+"://"+u.Host+path+"?"+query, nil)
	req.Header.Set("X-Amz-Date", amzDate)
	req.Header.Set("Authorization", authz)
	resp, err := ec2Client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if resp.StatusCode != 200 {
		var e struct {
			Errors []struct {
				Code    string `xml:"Code"`
				Message string `xml:"Message"`
			} `xml:"Errors>Error"`
		}
		_ = xml.Unmarshal(body, &e)
		if len(e.Errors) > 0 {
			return nil, fmt.Errorf("EC2 %s: HTTP %d %s: %s", action, resp.StatusCode, e.Errors[0].Code, e.Errors[0].Message)
		}
		return nil, fmt.Errorf("EC2 %s: HTTP %d", action, resp.StatusCode)
	}
	return body, nil
}

func (od *OnDemand) describe() (string, error) {
	if od.Provider == "systemd" || od.Provider == "local" {
		unit := od.Unit
		if unit == "" {
			unit = od.InstanceID
		}
		if unit == "" {
			return "unknown", errors.New("systemd on-demand requires unit or instance_id")
		}
		out, err := od.runCmd("systemctl", "is-active", unit)
		status := strings.TrimSpace(string(out))
		switch status {
		case "active":
			return "running", nil
		case "activating":
			return "pending", nil
		case "deactivating":
			return "stopping", nil
		case "inactive", "failed":
			return "stopped", nil
		default:
			if err != nil && status == "" {
				return "unknown", fmt.Errorf("systemctl is-active %s: %v", unit, err)
			}
			return "stopped", nil
		}
	}
	body, err := od.call("DescribeInstances")
	if err != nil {
		return "unknown", err
	}
	var r struct {
		Instances []struct {
			ID    string `xml:"instanceId"`
			State string `xml:"instanceState>name"`
		} `xml:"reservationSet>item>instancesSet>item"`
	}
	if err := xml.Unmarshal(body, &r); err != nil {
		return "unknown", err
	}
	for _, in := range r.Instances {
		if in.ID == od.InstanceID {
			return in.State, nil
		}
	}
	return "unknown", fmt.Errorf("instance %s not found", od.InstanceID)
}

// ---------- portal integration ----------

func (p *Portal) onDemandState(id string) *odState {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.od == nil {
		p.od = map[string]*odState{}
	}
	s := p.od[id]
	if s == nil {
		s = &odState{state: "unknown"}
		p.od[id] = s
	}
	return s
}

func (p *Portal) startOnDemandLoops() {
	for i := range p.cfg.Servers {
		s := &p.cfg.Servers[i]
		if s.OnDemand == nil {
			continue
		}
		od := s.OnDemand
		if od.IdleMinutes <= 0 {
			od.IdleMinutes = 5
		}
		if od.BootTimeoutMinutes <= 0 {
			od.BootTimeoutMinutes = 15
		}
		if od.Provider == "systemd" || od.Provider == "local" {
			unit := od.Unit
			if unit == "" {
				unit = od.InstanceID
			}
			log.Printf("on-demand %s: systemd unit %s idle=%dm boot_timeout=%dm", s.ID, unit, od.IdleMinutes, od.BootTimeoutMinutes)
		} else {
			if _, err := loadAWSCreds(od.CredentialsFile); err != nil {
				log.Printf("on-demand %s: WARNING credentials unusable (%v) — wake/stop will fail", s.ID, err)
			}
			log.Printf("on-demand %s: %s %s %s idle=%dm boot_timeout=%dm", s.ID, od.Provider, od.Region, od.InstanceID, od.IdleMinutes, od.BootTimeoutMinutes)
		}
		go p.onDemandLoop(s.ID, od)
	}
}

// shardSnapshot returns (online in game, players, heartbeat seen recently) for a shard.
func (p *Portal) shardSnapshot(id string) (bool, int, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	st := p.shards[id]
	if st == nil {
		return false, 0, false
	}
	fresh := time.Since(st.seen) < 60*time.Second
	players, _ := strconv.Atoi(st.fields["players"])
	return fresh && st.fields["phase"] == "ingame", players, fresh
}

func (p *Portal) activeLeases(serverID string) int {
	p.mu.Lock()
	defer p.mu.Unlock()
	n := 0
	for _, l := range p.leases {
		if l.Server == serverID && time.Now().Before(l.Expires) {
			n++
		}
	}
	return n
}

func (p *Portal) inMaintenance(id string) bool {
	_, err := os.Stat(filepath.Join(p.cfg.DataDir, "maintenance-"+id))
	return err == nil
}

func (p *Portal) onDemandLoop(id string, od *OnDemand) {
	s := p.onDemandState(id)
	for {
		p.onDemandTick(id, od, s)
		time.Sleep(30 * time.Second)
	}
}

func (p *Portal) onDemandTick(id string, od *OnDemand, s *odState) {
	state, err := od.describe()
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	prev := s.state
	s.checked = now
	if err != nil {
		if s.lastErr != err.Error() {
			log.Printf("on-demand %s: describe failed: %v", id, err)
		}
		s.lastErr = err.Error()
		return
	}
	s.lastErr = ""
	s.state = state
	if prev != state {
		log.Printf("on-demand %s: instance %s -> %s", id, prev, state)
	}
	switch state {
	case "stopped":
		s.runSince, s.idleSince = time.Time{}, time.Time{}
		if s.wantStart {
			s.wantStart = false
			if _, err := od.call("StartInstances"); err != nil {
				log.Printf("on-demand %s: deferred start failed: %v", id, err)
			} else {
				s.state = "pending"
				log.Printf("on-demand %s: start requested (queued while stopping)", id)
			}
		}
	case "pending", "running":
		if s.runSince.IsZero() {
			s.runSince = now
		}
		if p.inMaintenance(id) {
			s.idleSince = time.Time{}
			return
		}
		inGame, players, _ := p.shardSnapshot(id)
		leases := p.activeLeases(id)
		busy := players > 0 || leases > 0 || now.Sub(s.lastWake) < time.Duration(od.IdleMinutes)*time.Minute
		if !inGame {
			// Booting, restarting the game, or broken. Give it BootTimeout from start / last wake / last activity.
			ref := s.runSince
			if s.lastWake.After(ref) {
				ref = s.lastWake
			}
			if s.idleSince.After(ref) {
				ref = s.idleSince
			}
			if players == 0 && leases == 0 && now.Sub(ref) > time.Duration(od.BootTimeoutMinutes)*time.Minute {
				p.stopLocked(id, od, s, fmt.Sprintf("not in game for %d min", od.BootTimeoutMinutes))
			}
			return
		}
		if busy {
			s.idleSince = time.Time{}
			return
		}
		if s.idleSince.IsZero() {
			s.idleSince = now
			log.Printf("on-demand %s: empty — stopping in %d min unless someone joins", id, od.IdleMinutes)
			return
		}
		if now.Sub(s.idleSince) >= time.Duration(od.IdleMinutes)*time.Minute {
			p.stopLocked(id, od, s, fmt.Sprintf("empty for %d min", od.IdleMinutes))
		}
	}
}

func (p *Portal) stopLocked(id string, od *OnDemand, s *odState, reason string) {
	if _, err := od.call("StopInstances"); err != nil {
		log.Printf("on-demand %s: stop (%s) failed: %v", id, reason, err)
		return
	}
	s.state, s.stopReason, s.idleSince, s.runSince = "stopping", reason, time.Time{}, time.Time{}
	p.mu.Lock()
	delete(p.shards, id) // its last heartbeat is no longer true
	p.mu.Unlock()
	log.Printf("on-demand %s: STOP requested (%s)", id, reason)
}

// POST /api/servers/wake?id=eu  (logged in)
func (p *Portal) handleWake(w http.ResponseWriter, r *http.Request, acct *Account) {
	id := r.URL.Query().Get("id")
	var od *OnDemand
	for i := range p.cfg.Servers {
		if p.cfg.Servers[i].ID == id {
			od = p.cfg.Servers[i].OnDemand
		}
	}
	if od == nil {
		jsonErr(w, 404, "this server is always on")
		return
	}
	s := p.onDemandState(id)
	s.mu.Lock()
	defer s.mu.Unlock()
	s.lastWake = time.Now()
	state, err := od.describe()
	if err != nil {
		log.Printf("on-demand %s: wake by %s: describe failed: %v", id, acct.Name, err)
		jsonErr(w, 502, "could not reach the cloud provider, try again in a minute")
		return
	}
	s.state, s.checked = state, time.Now()
	switch state {
	case "stopped":
		if _, err := od.call("StartInstances"); err != nil {
			log.Printf("on-demand %s: wake by %s: start failed: %v", id, acct.Name, err)
			jsonErr(w, 502, "could not start the server, try again in a minute")
			return
		}
		s.state, s.runSince = "pending", time.Now()
		log.Printf("on-demand %s: START requested by %s", id, acct.Name)
	case "stopping":
		s.wantStart = true
		log.Printf("on-demand %s: wake by %s while stopping — will start when stopped", id, acct.Name)
	default:
		log.Printf("on-demand %s: wake by %s (already %s)", id, acct.Name, state)
	}
	jsonOut(w, map[string]any{"ok": true, "state": s.state})
}

func (p *Portal) onDemandInfo(id string) (string, bool) {
	p.mu.Lock()
	s := p.od[id]
	p.mu.Unlock()
	if s == nil {
		return "unknown", false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.state, true
}
