// leases.go — 0.7.2: session leases that survive a restart, and the rule for what a missing lease means.
//
// A lease says "this account is in a session, started by this PC, with this session id". It was held only in
// memory. Restarting the portal therefore invalidated every live session at once: the launchers kept playing
// and kept their old session ids, and from that moment every save upload they made was refused 409 for the
// rest of the session. Restarting the portal service would drop live sessions.
//
// Two changes fix it, and they cover different failures:
//
//   - Leases are written to leases.json and read back at startup, so a planned restart keeps every in-flight
//     session valid. Expiry is honoured on load, so a file left behind by a long-dead portal grants nothing.
//   - A save upload with no lease at all is accepted (see handlePutSave). Persistence cannot cover a crash,
//     a kill -9 between writes, or a lease that lapsed while a launcher was asleep, and in all of those cases
//     nobody holds the account, so there is no second PC to protect the save from. Refusing protects nothing
//     and costs the player their session.
//
// Session renew still refuses without a live lease. A launcher that has lost its lease should take a fresh
// one rather than carry on holding something nobody is tracking; only the save is unconditionally worth
// keeping.
package main

import (
	"encoding/json"
	"log"
	"os"
	"path/filepath"
	"time"
)

// storedLease is the on-disk form. The in-memory lease is keyed by account, so the account travels in the value.
type storedLease struct {
	Account string    `json:"account"`
	Session string    `json:"session"`
	Machine string    `json:"machine"`
	Server  string    `json:"server"`
	Expires time.Time `json:"expires"`
	Started time.Time `json:"started"`
}

func (p *Portal) leasesPath() string { return filepath.Join(p.cfg.DataDir, "leases.json") }

// saveLeasesLocked writes the leases that have not expired. Caller holds p.mu.
func (p *Portal) saveLeasesLocked() error {
	now := time.Now()
	list := make([]storedLease, 0, len(p.leases))
	for acct, l := range p.leases {
		if !now.Before(l.Expires) {
			continue
		}
		list = append(list, storedLease{Account: acct, Session: l.Session, Machine: l.Machine,
			Server: l.Server, Expires: l.Expires, Started: l.Started})
	}
	b, err := json.MarshalIndent(list, "", "  ")
	if err != nil {
		return err
	}
	return writeAtomic(p.leasesPath(), b)
}

// loadLeases restores leases that are still inside their expiry. Anything older is dropped, so a file from a
// portal that has been down longer than one lease hands nothing back.
func (p *Portal) loadLeases() {
	b, err := os.ReadFile(p.leasesPath())
	if err != nil {
		return
	}
	var list []storedLease
	if err := json.Unmarshal(b, &list); err != nil {
		log.Printf("leases: %s is not readable, starting with none: %v", p.leasesPath(), err)
		return
	}
	now := time.Now()
	kept, dropped := 0, 0
	for _, s := range list {
		if s.Account == "" || s.Session == "" || !now.Before(s.Expires) {
			dropped++
			continue
		}
		p.leases[s.Account] = &lease{Session: s.Session, Machine: s.Machine, Server: s.Server,
			Expires: s.Expires, Started: s.Started}
		kept++
	}
	if kept > 0 || dropped > 0 {
		log.Printf("leases: %d session(s) carried across the restart, %d expired and dropped", kept, dropped)
	}
}

// flushLeasesLocked writes now. Used where losing the change would matter: a session starting or ending.
// Caller holds p.mu.
func (p *Portal) flushLeasesLocked() {
	p.leasesDirty = false
	if err := p.saveLeasesLocked(); err != nil {
		log.Printf("leases: could not write %s: %v", p.leasesPath(), err)
	}
}

// leasesFlushLoop writes the frequent, low-stakes changes (an expiry pushed out by a renew or an upload) at
// most this often, the way charsFlushLoop does for experience records.
func (p *Portal) leasesFlushLoop() {
	for range time.Tick(5 * time.Second) {
		p.mu.Lock()
		if p.leasesDirty {
			p.flushLeasesLocked()
		}
		p.mu.Unlock()
	}
}
