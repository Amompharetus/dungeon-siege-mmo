package main

import (
	"archive/zip"
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func buildTestDsparty(files map[string][]byte) []byte {
	const dataOff = 64
	var dataBuf bytes.Buffer
	type rec struct {
		name      string
		size      uint32
		doff      uint32
		compSize  uint32
		chunkSize uint32
	}
	var recs []rec
	for name, content := range files {
		var zb bytes.Buffer
		zw := zlib.NewWriter(&zb)
		_, _ = zw.Write(content)
		_ = zw.Close()
		cb := zb.Bytes()
		recs = append(recs, rec{
			name:      name,
			size:      uint32(len(content)),
			doff:      uint32(dataBuf.Len()),
			compSize:  uint32(len(cb)),
			chunkSize: 16384,
		})
		dataBuf.Write(cb)
	}
	for dataBuf.Len()%4 != 0 {
		dataBuf.WriteByte(0)
	}
	fsOff := uint32(dataOff + dataBuf.Len())

	var ftab bytes.Buffer
	_ = binary.Write(&ftab, binary.LittleEndian, uint32(len(recs)))
	offsetsPos := ftab.Len()
	for range recs {
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
	}
	entryOffsets := make([]uint32, len(recs))
	for i, r := range recs {
		entryOffsets[i] = uint32(ftab.Len())
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
		_ = binary.Write(&ftab, binary.LittleEndian, r.size)
		_ = binary.Write(&ftab, binary.LittleEndian, r.doff)
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
		_ = binary.Write(&ftab, binary.LittleEndian, uint64(0))
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(1))
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(0))
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(len(r.name)))
		ftab.WriteString(r.name)
		ftab.WriteByte(0)
		curFrom28 := ftab.Len() - int(entryOffsets[i]) - 28
		wantFrom28 := (2 + len(r.name) + 1 + 3) &^ 3
		for curFrom28 < wantFrom28 {
			ftab.WriteByte(0)
			curFrom28++
		}
		_ = binary.Write(&ftab, binary.LittleEndian, r.compSize)
		_ = binary.Write(&ftab, binary.LittleEndian, r.chunkSize)
		_ = binary.Write(&ftab, binary.LittleEndian, r.size)
		_ = binary.Write(&ftab, binary.LittleEndian, r.compSize)
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
	}
	ftabBytes := ftab.Bytes()
	for i, off := range entryOffsets {
		binary.LittleEndian.PutUint32(ftabBytes[offsetsPos+i*4:], off)
	}

	hdr := make([]byte, dataOff)
	copy(hdr[0:8], "DSigTank")
	binary.LittleEndian.PutUint32(hdr[16:20], fsOff)
	binary.LittleEndian.PutUint32(hdr[24:28], dataOff)

	var out bytes.Buffer
	out.Write(hdr)
	out.Write(dataBuf.Bytes())
	out.Write(ftabBytes)
	return out.Bytes()
}

func writeTestSaveZip(t *testing.T, zipPath string, entries map[string][]byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(zipPath), 0o755); err != nil {
		t.Fatal(err)
	}
	f, err := os.Create(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	for name, data := range entries {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write(data); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
}

func resetWebTestState(t *testing.T) string {
	t.Helper()
	tmp := t.TempDir()
	oldPortalDir := portalDir
	oldPortalURL := portalURL
	oldTankdump := tankdumpBin
	t.Cleanup(func() {
		portalDir = oldPortalDir
		portalURL = oldPortalURL
		tankdumpBin = oldTankdump
		cacheMu.Lock()
		charCache = make(map[string]CachedChar)
		cacheMu.Unlock()
		accountZipMu.Lock()
		accountZipCache = make(map[string]cachedAccountZip)
		accountZipMu.Unlock()
		portalLogMu.Lock()
		portalLogPath = ""
		portalLogSess = nil
		portalLogMu.Unlock()
	})
	portalDir = tmp
	tankdumpBin = "" // verify pure-Go DSigTank parser needs zero external tankdump binary
	cacheMu.Lock()
	charCache = make(map[string]CachedChar)
	cacheMu.Unlock()
	accountZipMu.Lock()
	accountZipCache = make(map[string]cachedAccountZip)
	accountZipMu.Unlock()
	portalLogMu.Lock()
	portalLogPath = ""
	portalLogSess = nil
	portalLogMu.Unlock()
	return tmp
}

func TestPureGoParseTitleCaseAndNoCachePoisoning(t *testing.T) {
	tmp := resetWebTestState(t)
	zipPath := filepath.Join(tmp, "portal-data", "saves", "alice", "current.zip")
	dsp := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"Alice\";\nactor_class = \"GRAND MASTER\";\ntemplate_name = halfgiant;\nuber_level = 5.4;\n"),
		"info.gas":  []byte("region = a9_r3_clockroom;\nsave_time = 1795000000;\n"),
	})
	writeTestSaveZip(t, zipPath, map[string][]byte{"Save/Alice.dsparty": dsp})

	// 1. Save uploaded while accounts.json XP is still 0 -> clamped to 0, MUST NOT poison charCache
	recZero := &CharRec{Name: "Alice", XP: 0, Updated: time.Now().UTC()}
	d1 := resolveCharacterDetails("Alice", "Alice", "sha1", recZero, 0)
	if d1.Level != 0 {
		t.Fatalf("expected anti-cheat clamp to 0 when xp=0, got %d", d1.Level)
	}
	if d1.Class != "Grand Master" {
		t.Fatalf("expected title-cased multi-word class 'Grand Master', got %q", d1.Class)
	}
	if d1.Race != "Half-Giant" || d1.StartLoc != "The Great Clock" || d1.MapAreaId != "great_clock" {
		t.Fatalf("unexpected metadata: %+v", d1)
	}

	// 2. Seconds later live heartbeat XP arrives (same zip / same SaveSHA!) -> immediately unclamps to Level 5
	d2 := resolveCharacterDetails("Alice", "Alice", "sha1", recZero, 4000)
	if d2.Level != 5 {
		t.Fatalf("charCache was poisoned! expected Level 5 once live XP=4000 arrived, got %d", d2.Level)
	}

	// 3. Genuine non-MMO character (save lvl 43, xp 27) remains clamped to 0
	dspHack := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"Hacker\";\nactor_class = \"Siegemaster\";\nuber_level = 43.0;\n"),
	})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "hacker", "current.zip"), map[string][]byte{"Save/Hacker.dsparty": dspHack})
	dHack := resolveCharacterDetails("Hacker", "Hacker", "shaH", &CharRec{Name: "Hacker", XP: 27}, 27)
	if dHack.Level != 0 || dHack.Class != "Adventurer" {
		t.Fatalf("expected hacked char clamped to Level 0 Adventurer, got %+v", dHack)
	}
}

func TestLevelZeroAndNoAltInheritance(t *testing.T) {
	tmp := resetWebTestState(t)
	dspMain := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"MainHero\";\nactor_class = \"Arch Templar\";\nuber_level = 60.2;\n"),
		"info.gas":  []byte("region = oasis;\nsave_time = 1795000000;\n"),
	})
	dspAlt := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"FreshAlt\";\nactor_class = \"Farmer\";\nuber_level = 0.0001;\n"),
		"info.gas":  []byte("region = town_center;\nsave_time = 1795000100;\n"),
	})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "bob", "current.zip"), map[string][]byte{
		"Save/MainHero.dsparty": dspMain,
		"Save/FreshAlt.dsparty": dspAlt,
	})
	accts := []*Account{{
		Name:    "Bob",
		SaveSHA: "shabob",
		Chars: map[string]*CharRec{
			"mainhero": {Name: "MainHero", XP: 45000000, Updated: time.Now().UTC()},
			"freshalt": {Name: "FreshAlt", XP: 0, Updated: time.Now().UTC()},
		},
	}}
	ab, _ := json.Marshal(accts)
	if err := os.WriteFile(filepath.Join(tmp, "portal-data", "accounts.json"), ab, 0o644); err != nil {
		t.Fatal(err)
	}

	// Brand-new Level 0 alt not yet in accounts.json must NOT inherit MainHero's XP
	acctNoAltRec := &Account{
		Name:    "Bob",
		SaveSHA: "shabob",
		Chars:   map[string]*CharRec{"mainhero": {Name: "MainHero", XP: 45000000, Updated: time.Now().UTC()}},
	}
	_, rec, det := selectActiveCharacter("Bob", acctNoAltRec, "shabob", "BrandNewAlt", 0)
	if rec != nil || det.Level != 0 {
		t.Fatalf("new alt inherited MainHero record! rec=%+v det=%+v", rec, det)
	}

	// getAccountCharacters preserves Level 0 and title-cases Arch Templar
	chars := getAccountCharacters("Bob")
	if len(chars) != 2 {
		t.Fatalf("expected 2 chars, got %d", len(chars))
	}
	if chars[0].Name != "MainHero" || chars[0].Level != 60 || chars[0].Class != "Arch Templar" {
		t.Fatalf("unexpected char[0]: %+v", chars[0])
	}
	if chars[1].Name != "FreshAlt" || chars[1].Level != 0 || chars[1].Class != "Farmer" {
		t.Fatalf("expected FreshAlt Level 0 Farmer, got %+v", chars[1])
	}
}

func TestUnifiedRealtimeLadderRealmsGhostFilterAndStaging(t *testing.T) {
	tmp := resetWebTestState(t)

	// Two Level 10 chars: HeroA offline (XP 20000), HeroB online (stale accounts.json XP 18000, live pcN XP 25000)
	// Plus Departed (ghost in 60s pcN tail, session already ended) and StagingUser (in staging screen, no pcN yet)
	dspA := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"HeroA\";\nactor_class = \"Hedge Wizard\";\nuber_level = 10.2;\n"),
		"info.gas":  []byte("region = mine_town;\nsave_time = 1795000000;\n"),
	})
	dspB := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"HeroB\";\nactor_class = \"Master Sharpshooter\";\nuber_level = 10.5;\n"),
		"info.gas":  []byte("region = a9_r3_clockroom;\nsave_time = 1795000100;\n"),
	})
	dspDep := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"Departed\";\nactor_class = \"Fighter\";\nuber_level = 5.0;\n"),
		"info.gas":  []byte("region = town_center;\nsave_time = 1795000050;\n"),
	})
	dspStg := buildTestDsparty(map[string][]byte{
		"party.gas": []byte("screen_name = \"StgHero\";\nactor_class = \"Farmer\";\nuber_level = 0.0001;\n"),
		"info.gas":  []byte("region = town_center;\nsave_time = 1795000010;\n"),
	})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "usera", "current.zip"), map[string][]byte{"Save/HeroA.dsparty": dspA})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "userb", "current.zip"), map[string][]byte{"Save/HeroB.dsparty": dspB})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "departed", "current.zip"), map[string][]byte{"Save/Departed.dsparty": dspDep})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "stguser", "current.zip"), map[string][]byte{"Save/StgHero.dsparty": dspStg})

	past := time.Now().UTC().Add(-10 * time.Minute)
	accts := []*Account{
		{Name: "UserA", SaveSHA: "sa", SaveTime: past, Chars: map[string]*CharRec{"heroa": {Name: "HeroA", XP: 20000, Updated: past}}},
		{Name: "UserB", SaveSHA: "sb", SaveTime: past, Chars: map[string]*CharRec{"herob": {Name: "HeroB", XP: 18000, Updated: past}}},
		{Name: "Departed", SaveSHA: "sd", SaveTime: past, Chars: map[string]*CharRec{"departed": {Name: "Departed", XP: 4000, Updated: past}}},
		{Name: "StgUser", SaveSHA: "ss", SaveTime: past, Chars: map[string]*CharRec{"stghero": {Name: "StgHero", XP: 0, Updated: past}}},
	}
	ab, _ := json.Marshal(accts)
	if err := os.WriteFile(filepath.Join(tmp, "portal-data", "accounts.json"), ab, 0o644); err != nil {
		t.Fatal(err)
	}

	nowStr := time.Now().UTC().Format("2006/01/02 15:04:05")
	if err := os.MkdirAll(filepath.Join(tmp, "logs"), 0o755); err != nil {
		t.Fatal(err)
	}
	portalLog := nowStr + " session start: Departed on PC1 (server \"eu\", join ticket true)\n" +
		nowStr + " session end: Departed\n" +
		nowStr + " session start: UserB on PC2 (server \"eu\", join ticket true)\n" +
		nowStr + " session start: StgUser on PC3 (server \"na\", join ticket true)\n"
	if err := os.WriteFile(filepath.Join(tmp, "logs", "portal.log"), []byte(portalLog), 0o644); err != nil {
		t.Fatal(err)
	}

	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{
			"server_name": "Dungeon Siege MMO",
			"servers": []map[string]any{
				{
					"id": "na", "name": "North America", "region": "NA",
					"online": true, "players": "1", "max_players": 30,
					"game": map[string]string{
						"joinguard":      "on",
						"start_location": "elddim",
					},
				},
				{
					"id": "eu", "name": "Central Europe", "region": "EU",
					"online": true, "players": "1", "max_players": 30,
					"game": map[string]string{
						"joinguard":      "on",
						"start_location": "elddim",
						"pc0":            "Departed|" + hex.EncodeToString([]byte("Departed")) + "|4000",
						"pc1":            "UserB|" + hex.EncodeToString([]byte("HeroB")) + "|25000",
					},
				},
			},
		})
	}))
	defer ts.Close()
	portalURL = ts.URL

	st := buildRealmStatus()

	// 1. EU has ONLY HeroB (Departed ghost filtered out!), Level 10, XP 25000, Master Sharpshooter, The Great Clock
	if len(st.EU.Players) != 1 || st.EU.Players[0].CharacterName != "HeroB" {
		t.Fatalf("expected only HeroB on EU (Departed ghost dropped), got %+v", st.EU.Players)
	}
	if st.EU.Players[0].Level != 10 || st.EU.Players[0].XP != 25000 || st.EU.Players[0].Class != "Master Sharpshooter" || st.EU.Players[0].MapAreaId != "great_clock" {
		t.Fatalf("unexpected EU player info: %+v", st.EU.Players[0])
	}

	// 2. NA has StgHero at Level 0 in Staging Screen (not falsely forced into Elddim!)
	if len(st.NA.Players) != 1 || st.NA.Players[0].CharacterName != "StgHero" || st.NA.Players[0].Level != 0 || st.NA.Players[0].MapAreaId != "staging" {
		t.Fatalf("expected StgHero Level 0 in staging on NA, got %+v", st.NA.Players)
	}

	// 3. Ladder ranks HeroB (#1, live XP 25000, Online EU) ahead of HeroA (#2, XP 20000, offline) immediately!
	if len(st.Ladder) != 4 {
		t.Fatalf("expected 4 ladder entries, got %d: %+v", len(st.Ladder), st.Ladder)
	}
	if st.Ladder[0].CharacterName != "HeroB" || st.Ladder[0].XP != 25000 || !st.Ladder[0].IsOnline || st.Ladder[0].OnlineShard != "EU" {
		t.Fatalf("expected HeroB ranked #1 online with live XP 25000, got %+v", st.Ladder[0])
	}
	if st.Ladder[1].CharacterName != "HeroA" || st.Ladder[1].Class != "Hedge Wizard" || st.Ladder[1].IsOnline {
		t.Fatalf("expected HeroA ranked #2 offline Hedge Wizard, got %+v", st.Ladder[1])
	}
	if st.Ladder[2].CharacterName != "Departed" || st.Ladder[2].IsOnline {
		t.Fatalf("expected Departed offline on ladder, got %+v", st.Ladder[2])
	}
	if st.Ladder[3].CharacterName != "StgHero" || st.Ladder[3].Level != 0 || !st.Ladder[3].IsOnline || st.Ladder[3].OnlineShard != "NA" {
		t.Fatalf("expected StgHero Level 0 online NA on ladder, got %+v", st.Ladder[3])
	}
}
