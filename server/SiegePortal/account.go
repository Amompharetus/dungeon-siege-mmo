// account.go — account self-service (0.4.0): change password / rename while logged in, admin password reset.
// A valid login token is the proof of identity; the old password is not asked (by design).
package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

// POST /api/account/password {"NewPassword":"..."}
func (p *Portal) handleChangePassword(w http.ResponseWriter, r *http.Request, acct *Account) {
	var req struct{ NewPassword string }
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	if len(req.NewPassword) < 6 {
		jsonErr(w, 400, "password must be at least 6 characters")
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	oldSalt, oldHash := acct.Salt, acct.Hash
	salt := make([]byte, 16)
	_, _ = rand.Read(salt)
	acct.Salt = hex.EncodeToString(salt)
	acct.Hash = hashPassword(req.NewPassword, acct.Salt)
	if err := p.saveAccountsLocked(); err != nil {
		acct.Salt, acct.Hash = oldSalt, oldHash
		jsonErr(w, 500, "could not store the new password")
		return
	}
	log.Printf("account %s: password changed from %s", acct.Name, r.RemoteAddr)
	jsonOut(w, map[string]any{"ok": true})
}

// POST /api/account/rename {"NewName":"..."} -> {"name","token"}. Moves the online saves with the account.
func (p *Portal) handleRename(w http.ResponseWriter, r *http.Request, acct *Account) {
	var req struct{ NewName string }
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	newName := strings.TrimSpace(req.NewName)
	if !nameRe.MatchString(newName) {
		jsonErr(w, 400, "account name must be 3-20 letters, digits or _")
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	oldKey, newKey := strings.ToLower(acct.Name), strings.ToLower(newName)
	if newName == acct.Name {
		jsonErr(w, 400, "that is already your account name")
		return
	}
	if oldKey != newKey {
		if _, taken := p.accounts[newKey]; taken {
			jsonErr(w, 409, "that account name is already taken")
			return
		}
	}
	if l := p.leases[oldKey]; l != nil && time.Now().Before(l.Expires) {
		jsonErr(w, 409, "this account is in a game right now; close the game first")
		return
	}
	oldName := acct.Name
	oldDir, newDir := p.saveDir(acct), p.saveDir(&Account{Name: newName})
	moved := false
	if oldKey != newKey {
		if _, err := os.Stat(oldDir); err == nil {
			if _, err := os.Stat(newDir); err == nil {
				jsonErr(w, 409, "save folder for that name already exists; ask an admin")
				return
			}
			if err := os.Rename(oldDir, newDir); err != nil {
				jsonErr(w, 500, "could not move your saves")
				return
			}
			moved = true
		}
	}
	acct.Name = newName
	delete(p.accounts, oldKey)
	p.accounts[newKey] = acct
	delete(p.leases, oldKey)
	if err := p.saveAccountsLocked(); err != nil {
		acct.Name = oldName
		delete(p.accounts, newKey)
		p.accounts[oldKey] = acct
		if moved {
			_ = os.Rename(newDir, oldDir)
		}
		jsonErr(w, 500, "could not store the new name")
		return
	}
	log.Printf("account renamed: %s -> %s (saves moved=%v) from %s", oldName, newName, moved, r.RemoteAddr)
	jsonOut(w, map[string]any{"ok": true, "name": newName, "token": p.token(newName)})
}

// POST /api/admin/reset-password {"Name":"..."} — only from the server itself (127.0.0.1). Returns a temporary password.
func (p *Portal) handleAdminReset(w http.ResponseWriter, r *http.Request) {
	host, _, _ := net.SplitHostPort(r.RemoteAddr)
	if ip := net.ParseIP(host); ip == nil || !ip.IsLoopback() {
		jsonErr(w, 403, "admin only")
		return
	}
	var req struct{ Name string }
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	acct := p.accounts[strings.ToLower(strings.TrimSpace(req.Name))]
	if acct == nil {
		jsonErr(w, 404, "no such account")
		return
	}
	b := make([]byte, 6)
	_, _ = rand.Read(b)
	temp := hex.EncodeToString(b)
	salt := make([]byte, 16)
	_, _ = rand.Read(salt)
	acct.Salt = hex.EncodeToString(salt)
	acct.Hash = hashPassword(temp, acct.Salt)
	if err := p.saveAccountsLocked(); err != nil {
		jsonErr(w, 500, "could not store")
		return
	}
	log.Printf("admin: password reset for %s", acct.Name)
	jsonOut(w, map[string]any{"ok": true, "name": acct.Name, "temporary_password": temp})
}

// adminResetCLI: siegeportal -config portal.json -reset-password NAME (asks the running portal on this machine).
func adminResetCLI(cfgPath, name string) error {
	cfg := loadConfig(cfgPath)
	port := cfg.Listen
	if i := strings.LastIndex(port, ":"); i >= 0 {
		port = port[i+1:]
	}
	body := strings.NewReader(fmt.Sprintf(`{"Name":%q}`, name))
	resp, err := http.Post("http://127.0.0.1:"+port+"/api/admin/reset-password", "application/json", body)
	if err != nil {
		return fmt.Errorf("portal not reachable on 127.0.0.1:%s: %v", port, err)
	}
	defer resp.Body.Close()
	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	if resp.StatusCode != 200 {
		return errors.New(fmt.Sprint(out["error"]))
	}
	fmt.Printf("account %v: temporary password %v (tell the player; they can change it in the launcher with A)\n", out["name"], out["temporary_password"])
	return nil
}
