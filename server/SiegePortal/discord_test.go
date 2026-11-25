package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func TestAnnouncedLevelForTransition(t *testing.T) {
	// Default: every 5 levels from 5 up to 150
	def := DiscordConfig{}
	if _, ok := announcedLevelForTransition(0, 1, def); ok {
		t.Fatal("0->1 must not announce under default 5-level step")
	}
	if _, ok := announcedLevelForTransition(3, 4, def); ok {
		t.Fatal("3->4 must not announce under default 5-level step")
	}
	if lvl, ok := announcedLevelForTransition(4, 5, def); !ok || lvl != 5 {
		t.Fatalf("4->5 should announce 5, got (%d, %v)", lvl, ok)
	}
	// Multi-level jump crossing a milestone (4->6) still catches Uber Level 5
	if lvl, ok := announcedLevelForTransition(4, 6, def); !ok || lvl != 5 {
		t.Fatalf("4->6 should catch crossed milestone 5, got (%d, %v)", lvl, ok)
	}
	if _, ok := announcedLevelForTransition(5, 6, def); ok {
		t.Fatal("5->6 must not announce under default 5-level step")
	}
	if lvl, ok := announcedLevelForTransition(69, 70, def); !ok || lvl != 70 {
		t.Fatalf("69->70 should announce 70, got (%d, %v)", lvl, ok)
	}
	if lvl, ok := announcedLevelForTransition(149, 150, def); !ok || lvl != 150 {
		t.Fatalf("149->150 should announce max level 150, got (%d, %v)", lvl, ok)
	}
	if _, ok := announcedLevelForTransition(150, 151, def); ok {
		t.Fatal("levels above max_level 150 must not announce")
	}

	// Explicit step=1 announces every level >= min_level
	every := DiscordConfig{MinLevel: 10, MilestoneStep: 1}
	if _, ok := announcedLevelForTransition(8, 9, every); ok {
		t.Fatal("level 9 must be filtered when MinLevel=10")
	}
	if lvl, ok := announcedLevelForTransition(9, 10, every); !ok || lvl != 10 {
		t.Fatalf("9->10 should announce 10, got (%d, %v)", lvl, ok)
	}
	if lvl, ok := announcedLevelForTransition(10, 11, every); !ok || lvl != 11 {
		t.Fatalf("10->11 should announce 11, got (%d, %v)", lvl, ok)
	}

	// Hybrid: every 5 below 50, every level at 50+
	hybrid := DiscordConfig{MinLevel: 5, MilestoneStep: 5, MilestoneUntil: 50}
	if _, ok := announcedLevelForTransition(48, 49, hybrid); ok {
		t.Fatal("48->49 must be filtered below MilestoneUntil=50")
	}
	if lvl, ok := announcedLevelForTransition(49, 50, hybrid); !ok || lvl != 50 {
		t.Fatalf("49->50 should announce 50, got (%d, %v)", lvl, ok)
	}
	if lvl, ok := announcedLevelForTransition(50, 51, hybrid); !ok || lvl != 51 {
		t.Fatalf("50->51 should announce 51, got (%d, %v)", lvl, ok)
	}
}

func TestFormatLevelMessage(t *testing.T) {
	if got := formatLevelMessage("Aragorn", 70, false); got != "Aragorn just reached Uber Level 70!" {
		t.Fatalf("plain format mismatch: %q", got)
	}
	if got := formatLevelMessage("Aragorn", 70, true); got != "**Aragorn** just reached **Uber Level 70**!" {
		t.Fatalf("bold format mismatch: %q", got)
	}
	got := formatLevelMessage("  *Dark_Knight*\n@everyone  ", 25, true)
	want := "**\\*Dark\\_Knight\\* @\u200Beveryone** just reached **Uber Level 25**!"
	if got != want {
		t.Fatalf("sanitized format mismatch:\n got: %q\nwant: %q", got, want)
	}
}

func TestHandleShardLevelUpAndDeduplication(t *testing.T) {
	type capturedReq struct {
		Auth   string
		Path   string
		Body   map[string]any
		Header http.Header
	}
	var mu sync.Mutex
	var received []capturedReq

	mockDiscord := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var body map[string]any
		_ = json.NewDecoder(r.Body).Decode(&body)
		mu.Lock()
		received = append(received, capturedReq{
			Auth:   r.Header.Get("Authorization"),
			Path:   r.URL.Path,
			Body:   body,
			Header: r.Header.Clone(),
		})
		mu.Unlock()
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"id":"123456"}`))
	}))
	defer mockDiscord.Close()

	dir := t.TempDir()
	var wg sync.WaitGroup
	p := &Portal{
		cfg: Config{
			DataDir: dir,
			Servers: []ServerEntry{{ID: "na", ShardKey: "secret-shard-key"}},
			Discord: &DiscordConfig{
				BotToken:  "test-bot-token",
				ChannelID: "999999999999999999",
				APIBase:   mockDiscord.URL,
			},
		},
		accounts:      map[string]*Account{},
		leases:        map[string]*lease{},
		shards:        map[string]*shardState{},
		refusals:      map[string]refusal{},
		discordLevels: map[string]int{},
		discordClient: mockDiscord.Client(),
		discordWG:     &wg,
	}

	post := func(shardID, shardKey, body string) *httptest.ResponseRecorder {
		req := httptest.NewRequest(http.MethodPost, "/api/shard/levelup", strings.NewReader(body))
		if shardID != "" {
			req.Header.Set("X-Shard-Id", shardID)
		}
		if shardKey != "" {
			req.Header.Set("X-Shard-Key", shardKey)
		}
		rec := httptest.NewRecorder()
		p.handleShardLevelUp(rec, req)
		return rec
	}

	// 1. Bad shard key -> 403
	if rr := post("na", "wrong-key", `{"account":"amom","character":"Aragorn","level":70,"old_level":69}`); rr.Code != 403 {
		t.Fatalf("expected 403 for bad key, got %d", rr.Code)
	}

	// 2. Invalid level transition -> 400
	if rr := post("na", "secret-shard-key", `{"account":"amom","character":"Aragorn","level":69,"old_level":69}`); rr.Code != 400 {
		t.Fatalf("expected 400 for non-increasing level, got %d", rr.Code)
	}

	// 3. Non-milestone level (68 -> 69) -> filtered silently
	if rr := post("na", "secret-shard-key", `{"account":"Amom","character":"Aragorn","level":69,"old_level":68}`); rr.Code != 200 || !bytes.Contains(rr.Body.Bytes(), []byte(`"filtered"`)) {
		t.Fatalf("expected 68->69 to be filtered, got %d %s", rr.Code, rr.Body.String())
	}

	// 4. Valid 5-level milestone (69 -> 70) -> 200 announced=true, delivered to Discord
	rr := post("na", "secret-shard-key", `{"account":"Amom","character":"Aragorn","level":70,"old_level":69}`)
	if rr.Code != 200 {
		t.Fatalf("expected 200, got %d (%s)", rr.Code, rr.Body.String())
	}
	var resp map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &resp); err != nil {
		t.Fatalf("bad json: %v", err)
	}
	if resp["announced"] != true || resp["message"] != "**Aragorn** just reached **Uber Level 70**!" {
		t.Fatalf("unexpected response: %+v", resp)
	}
	wg.Wait()

	mu.Lock()
	if len(received) != 1 {
		mu.Unlock()
		t.Fatalf("expected 1 Discord POST, got %d", len(received))
	}
	gotReq := received[0]
	mu.Unlock()

	if gotReq.Auth != "Bot test-bot-token" {
		t.Fatalf("bad Authorization header: %q", gotReq.Auth)
	}
	if gotReq.Path != "/channels/999999999999999999/messages" {
		t.Fatalf("bad Discord endpoint path: %q", gotReq.Path)
	}
	if gotReq.Body["content"] != "**Aragorn** just reached **Uber Level 70**!" {
		t.Fatalf("bad Discord content: %+v", gotReq.Body["content"])
	}
	am, ok := gotReq.Body["allowed_mentions"].(map[string]any)
	if !ok {
		t.Fatalf("missing allowed_mentions: %+v", gotReq.Body)
	}
	parseArr, ok := am["parse"].([]any)
	if !ok || len(parseArr) != 0 {
		t.Fatalf("allowed_mentions.parse must be empty array: %+v", am)
	}

	// 5. Duplicate / rollback milestone -> 200 announced=false reason=duplicate, NO extra Discord POST
	rrDup := post("na", "secret-shard-key", `{"account":"Amom","character":"Aragorn","level":70,"old_level":69}`)
	if rrDup.Code != 200 || !bytes.Contains(rrDup.Body.Bytes(), []byte(`"duplicate"`)) {
		t.Fatalf("expected duplicate suppression, got %d %s", rrDup.Code, rrDup.Body.String())
	}
	rrLower := post("na", "secret-shard-key", `{"account":"Amom","character":"Aragorn","level":65,"old_level":64}`)
	if rrLower.Code != 200 || !bytes.Contains(rrLower.Body.Bytes(), []byte(`"duplicate"`)) {
		t.Fatalf("expected lower-milestone suppression, got %d %s", rrLower.Code, rrLower.Body.String())
	}
	wg.Wait()
	mu.Lock()
	if len(received) != 1 {
		mu.Unlock()
		t.Fatalf("duplicate requests must not send Discord messages, got %d", len(received))
	}
	mu.Unlock()

	// 6. Simulate SiegePortal restart: reload discord_levels.json into fresh Portal
	p2 := &Portal{
		cfg:           p.cfg,
		accounts:      map[string]*Account{},
		leases:        map[string]*lease{},
		shards:        map[string]*shardState{},
		refusals:      map[string]refusal{},
		discordLevels: map[string]int{},
		discordClient: mockDiscord.Client(),
		discordWG:     &wg,
	}
	p2.loadDiscordLevels()

	reqAfterRestart := httptest.NewRequest(http.MethodPost, "/api/shard/levelup", strings.NewReader(`{"account":"Amom","character":"Aragorn","level":70,"old_level":69}`))
	reqAfterRestart.Header.Set("X-Shard-Id", "na")
	reqAfterRestart.Header.Set("X-Shard-Key", "secret-shard-key")
	recAfterRestart := httptest.NewRecorder()
	p2.handleShardLevelUp(recAfterRestart, reqAfterRestart)
	if recAfterRestart.Code != 200 || !bytes.Contains(recAfterRestart.Body.Bytes(), []byte(`"duplicate"`)) {
		t.Fatalf("restart must preserve dedup state, got %d %s", recAfterRestart.Code, recAfterRestart.Body.String())
	}

	// 7. Next milestone (74 -> 76 jumps over 75) after restart -> announces Uber Level 75!
	req75 := httptest.NewRequest(http.MethodPost, "/api/shard/levelup", strings.NewReader(`{"account":"Amom","character":"Aragorn","level":76,"old_level":74}`))
	req75.Header.Set("X-Shard-Id", "na")
	req75.Header.Set("X-Shard-Key", "secret-shard-key")
	rec75 := httptest.NewRecorder()
	p2.handleShardLevelUp(rec75, req75)
	if rec75.Code != 200 || !bytes.Contains(rec75.Body.Bytes(), []byte(`"announced":true`)) {
		t.Fatalf("expected level 75 milestone to announce, got %d %s", rec75.Code, rec75.Body.String())
	}
	wg.Wait()
	mu.Lock()
	if len(received) != 2 || received[1].Body["content"] != "**Aragorn** just reached **Uber Level 75**!" {
		mu.Unlock()
		t.Fatalf("expected 2nd Discord POST for level 75, got %+v", received)
	}
	mu.Unlock()
}

func TestHandleShardLevelUpDisabled(t *testing.T) {
	p := &Portal{
		cfg: Config{
			DataDir: t.TempDir(),
			Servers: []ServerEntry{{ID: "eu", ShardKey: "k"}},
			Discord: nil,
		},
		discordLevels: map[string]int{},
	}
	req := httptest.NewRequest(http.MethodPost, "/api/shard/levelup", strings.NewReader(`{"account":"a","character":"Hero","level":10,"old_level":9}`))
	req.Header.Set("X-Shard-Id", "eu")
	req.Header.Set("X-Shard-Key", "k")
	rec := httptest.NewRecorder()
	p.handleShardLevelUp(rec, req)
	if rec.Code != 200 || !bytes.Contains(rec.Body.Bytes(), []byte(`"disabled"`)) {
		t.Fatalf("expected disabled response, got %d %s", rec.Code, rec.Body.String())
	}
}
