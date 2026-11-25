// discord.go — 0.7.3: real-time Uber Level-up announcements to Discord via REST API v10.
//
// Game servers (SiegeHost Role=server) POST /api/shard/levelup authenticated with X-Shard-Id and X-Shard-Key
// whenever an online human player's natural Uber Level increases. SiegePortal deduplicates against
// portal-data/discord_levels.json (surviving relogs, save rollbacks, and portal restarts) and delivers
// the formatted message asynchronously without blocking the shard request.
package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"
)

const defaultDiscordAPIBase = "https://discord.com/api/v10"

// DiscordConfig configures optional Discord level-up announcements in portal.json.
// Leave omitted (or bot_token / channel_id empty) to keep Discord posting completely disabled.
type DiscordConfig struct {
	BotToken       string `json:"bot_token,omitempty"`
	ChannelID      string `json:"channel_id,omitempty"`
	MinLevel       int    `json:"min_level,omitempty"`       // lowest Uber Level to announce (default: milestone_step, i.e. 5)
	MaxLevel       int    `json:"max_level,omitempty"`       // highest Uber Level to announce (default: 150)
	MilestoneStep  int    `json:"milestone_step,omitempty"`  // default: 5 (announce 5, 10, 15, ..., 150); set 1 for every level
	MilestoneUntil int    `json:"milestone_until,omitempty"` // if > 0, announce every level at or above this level even when milestone_step > 1
	Bold           *bool  `json:"bold,omitempty"`            // nil/true => "**Aragorn** just reached **Uber Level 70**!", false => plain text
	APIBase        string `json:"api_base,omitempty"`        // optional override for tests
}

func (c *DiscordConfig) useBold() bool {
	return c == nil || c.Bold == nil || *c.Bold
}

func (c DiscordConfig) effectiveStep() int {
	if c.MilestoneStep <= 0 {
		return 5
	}
	return c.MilestoneStep
}

func (c DiscordConfig) effectiveMinLevel() int {
	if c.MinLevel <= 0 {
		return c.effectiveStep()
	}
	return c.MinLevel
}

func (c DiscordConfig) effectiveMaxLevel() int {
	if c.MaxLevel <= 0 {
		return 150
	}
	return c.MaxLevel
}

type levelUpRequest struct {
	Account   string `json:"account"`
	Character string `json:"character"`
	Level     int    `json:"level"`
	OldLevel  int    `json:"old_level"`
}

func (c *DiscordConfig) enabled() bool {
	return c != nil && strings.TrimSpace(c.BotToken) != "" && strings.TrimSpace(c.ChannelID) != ""
}

// announcedLevelForTransition returns the Uber Level milestone to announce for a transition
// oldLevel -> newLevel. Using the crossed milestone ensures multi-level jumps (e.g. 4 -> 6)
// still announce Uber Level 5 instead of skipping it.
func announcedLevelForTransition(oldLevel, newLevel int, cfg DiscordConfig) (int, bool) {
	step := cfg.effectiveStep()
	minLvl := cfg.effectiveMinLevel()
	maxLvl := cfg.effectiveMaxLevel()

	cappedNew := newLevel
	if cappedNew > maxLvl {
		cappedNew = maxLvl
	}
	if cappedNew <= oldLevel || cappedNew < minLvl {
		return 0, false
	}
	if step <= 1 || (cfg.MilestoneUntil > 0 && cappedNew >= cfg.MilestoneUntil) {
		return cappedNew, true
	}
	milestone := (cappedNew / step) * step
	if milestone > oldLevel && milestone >= minLvl && milestone <= maxLvl {
		return milestone, true
	}
	return 0, false
}

func sanitizeCharacterName(name string) string {
	var b strings.Builder
	prevSpace := false
	runes := 0
	for _, r := range strings.TrimSpace(name) {
		if r == '\r' || r == '\n' || r == '\t' {
			r = ' '
		}
		if r < 0x20 || r == 0x7f {
			continue
		}
		if r == ' ' {
			if prevSpace {
				continue
			}
			prevSpace = true
		} else {
			prevSpace = false
		}
		b.WriteRune(r)
		runes++
		if runes >= 64 {
			break
		}
	}
	return strings.TrimSpace(b.String())
}

func escapeDiscordMarkdown(s string) string {
	var b strings.Builder
	b.Grow(len(s) + 8)
	for _, r := range s {
		switch r {
		case '\\', '*', '_', '`', '~', '|', '>':
			b.WriteByte('\\')
			b.WriteRune(r)
		case '@':
			// Belt-and-suspenders alongside allowed_mentions: {"parse": []}.
			b.WriteRune('@')
			b.WriteRune('\u200B')
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}

func formatLevelMessage(character string, level int, bold bool) string {
	clean := sanitizeCharacterName(character)
	if !utf8.ValidString(clean) {
		clean = strings.ToValidUTF8(clean, "")
	}
	safe := escapeDiscordMarkdown(clean)
	if bold {
		return fmt.Sprintf("**%s** just reached **Uber Level %d**!", safe, level)
	}
	return fmt.Sprintf("%s just reached Uber Level %d!", safe, level)
}

func (p *Portal) discordLevelsPath() string {
	return filepath.Join(p.cfg.DataDir, "discord_levels.json")
}

func (p *Portal) loadDiscordLevels() {
	if p.discordLevels == nil {
		p.discordLevels = map[string]int{}
	}
	b, err := os.ReadFile(p.discordLevelsPath())
	if errors.Is(err, os.ErrNotExist) {
		return
	}
	if err != nil {
		log.Printf("discord: could not read %s: %v", p.discordLevelsPath(), err)
		return
	}
	var m map[string]int
	if err := json.Unmarshal(b, &m); err != nil {
		log.Printf("discord: could not parse %s: %v", p.discordLevelsPath(), err)
		return
	}
	if m != nil {
		p.discordLevels = m
	}
	log.Printf("discord: loaded %d announced character records", len(p.discordLevels))
}

// saveDiscordLevelsLocked writes discord_levels.json atomically. Caller holds p.mu.
func (p *Portal) saveDiscordLevelsLocked() error {
	if p.cfg.DataDir == "" {
		return nil
	}
	b, err := json.MarshalIndent(p.discordLevels, "", "  ")
	if err != nil {
		return err
	}
	return writeAtomic(p.discordLevelsPath(), b)
}

func (p *Portal) logDiscordStatus() {
	cfg := p.cfg.Discord
	if !cfg.enabled() {
		log.Printf("discord level announcer: disabled")
		return
	}
	log.Printf("discord level announcer: enabled (channel=%s, min_level=%d, max_level=%d, milestone_step=%d, milestone_until=%d, bold=%v)",
		cfg.ChannelID, cfg.effectiveMinLevel(), cfg.effectiveMaxLevel(), cfg.effectiveStep(), cfg.MilestoneUntil, cfg.useBold())
}

func (p *Portal) sendDiscordMessage(content string) error {
	cfg := p.cfg.Discord
	if !cfg.enabled() {
		return errors.New("discord not configured")
	}
	base := strings.TrimSpace(cfg.APIBase)
	if base == "" {
		base = defaultDiscordAPIBase
	}
	endpoint := strings.TrimRight(base, "/") + "/channels/" + strings.TrimSpace(cfg.ChannelID) + "/messages"

	payload := struct {
		Content         string `json:"content"`
		AllowedMentions struct {
			Parse []string `json:"parse"`
		} `json:"allowed_mentions"`
	}{
		Content: content,
	}
	payload.AllowedMentions.Parse = []string{}

	body, err := json.Marshal(payload)
	if err != nil {
		return err
	}

	req, err := http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Authorization", "Bot "+strings.TrimSpace(cfg.BotToken))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", "DungeonSiegeMMO-SiegePortal/"+version)

	client := p.discordClient
	if client == nil {
		client = &http.Client{Timeout: 8 * time.Second}
	}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		respBody, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
		return fmt.Errorf("discord HTTP %d: %s", resp.StatusCode, strings.TrimSpace(string(respBody)))
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	return nil
}

// POST /api/shard/levelup (X-Shard-Id, X-Shard-Key)
// Body: {"account":"...","character":"Aragorn","level":70,"old_level":69}
func (p *Portal) handleShardLevelUp(w http.ResponseWriter, r *http.Request) {
	entry, ok := p.checkShardAuth(r)
	if !ok {
		jsonErr(w, 403, "unknown shard or bad key")
		return
	}

	var req levelUpRequest
	if err := json.NewDecoder(io.LimitReader(r.Body, 4096)).Decode(&req); err != nil {
		jsonErr(w, 400, "bad request")
		return
	}

	cleanChar := sanitizeCharacterName(req.Character)
	ck := charKey(cleanChar)
	if cleanChar == "" || ck == "" {
		jsonErr(w, 400, "missing character")
		return
	}
	if req.Level < 1 || req.Level > 200 || req.OldLevel < 0 || req.Level <= req.OldLevel {
		jsonErr(w, 400, "invalid level transition")
		return
	}

	cfg := p.cfg.Discord
	if !cfg.enabled() {
		jsonOut(w, map[string]any{"ok": true, "announced": false, "reason": "disabled"})
		return
	}
	announceLevel, ok := announcedLevelForTransition(req.OldLevel, req.Level, *cfg)
	if !ok {
		jsonOut(w, map[string]any{"ok": true, "announced": false, "reason": "filtered"})
		return
	}

	acctKey := sanitizeAccount(req.Account)
	dedupKey := acctKey + ":" + ck

	p.mu.Lock()
	if p.discordLevels == nil {
		p.discordLevels = map[string]int{}
	}
	if prev := p.discordLevels[dedupKey]; announceLevel <= prev {
		p.mu.Unlock()
		jsonOut(w, map[string]any{"ok": true, "announced": false, "reason": "duplicate"})
		return
	}
	p.discordLevels[dedupKey] = announceLevel
	if err := p.saveDiscordLevelsLocked(); err != nil {
		log.Printf("discord: failed saving discord_levels.json: %v", err)
	}
	p.mu.Unlock()

	msg := formatLevelMessage(cleanChar, announceLevel, cfg.useBold())
	log.Printf("discord levelup: shard=%s account=%s character=%q announced=%d (raw %d->%d)",
		entry.ID, acctKey, cleanChar, announceLevel, req.OldLevel, req.Level)

	if p.discordWG != nil {
		p.discordWG.Add(1)
	}
	go func() {
		if p.discordWG != nil {
			defer p.discordWG.Done()
		}
		if err := p.sendDiscordMessage(msg); err != nil {
			log.Printf("discord: failed posting levelup for %q (level %d): %v", cleanChar, announceLevel, err)
		}
	}()

	jsonOut(w, map[string]any{"ok": true, "announced": true, "level": announceLevel, "message": msg})
}
