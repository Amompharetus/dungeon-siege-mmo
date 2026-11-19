package main

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type fakeDiscordPipe struct {
	written bytes.Buffer
	toRead  bytes.Buffer
	closed  bool
}

func (f *fakeDiscordPipe) WriteAll(b []byte, _ time.Duration) error {
	_, _ = f.written.Write(b)
	return nil
}

func (f *fakeDiscordPipe) ReadFull(b []byte, _ time.Duration) error {
	_, err := f.toRead.Read(b)
	return err
}

func (f *fakeDiscordPipe) Close() error {
	f.closed = true
	return nil
}

func TestDiscordIPCFramingAndSetActivity(t *testing.T) {
	replyFrame, err := encodeDiscordFrame(discordOpFrame, map[string]any{
		"cmd":  "SET_ACTIVITY",
		"evt":  "",
		"data": map[string]any{"name": "Dungeon Siege MMO"},
	})
	if err != nil {
		t.Fatalf("encode reply: %v", err)
	}
	fp := &fakeDiscordPipe{}
	fp.toRead.Write(replyFrame)

	ipc := &discordIPC{pipe: fp}
	act := buildDiscordActivity(
		time.Unix(1795000000, 0),
		"",
		localCharInfo{Name: "Aragorn", Level: 70, Class: "Grand Master", Location: "Halls of the Great Clock"},
		"eu", "Central Europe", 4, 30,
	)
	if err := ipc.setActivity(1234, act, time.Second); err != nil {
		t.Fatalf("setActivity failed: %v", err)
	}

	raw := fp.written.Bytes()
	if len(raw) < 8 {
		t.Fatalf("frame too short: %d", len(raw))
	}
	op := binary.LittleEndian.Uint32(raw[0:4])
	ln := binary.LittleEndian.Uint32(raw[4:8])
	if op != discordOpFrame || int(ln) != len(raw)-8 {
		t.Fatalf("bad frame header: op=%d len=%d body=%d", op, ln, len(raw)-8)
	}
	var sent map[string]any
	if err := json.Unmarshal(raw[8:], &sent); err != nil {
		t.Fatalf("invalid json sent: %v", err)
	}
	if sent["cmd"] != "SET_ACTIVITY" {
		t.Fatalf("unexpected cmd: %v", sent["cmd"])
	}
}

func TestBuildDiscordActivityFormats(t *testing.T) {
	started := time.Unix(1795000000, 0)

	// In-game with full character details
	act := buildDiscordActivity(started, "", localCharInfo{
		Name: "Aragorn", Level: 70, Class: "Grand Master", Location: "Halls of the Great Clock",
	}, "na", "North America", 3, 30)
	if act.Details != "Level 70 Grand Master · Halls of the Great Clock" {
		t.Fatalf("unexpected details: %q", act.Details)
	}
	if act.State != "North America (3 / 30)" {
		t.Fatalf("unexpected state: %q", act.State)
	}
	if act.Party != nil {
		t.Fatalf("expected nil party (count formatted in state), got: %+v", act.Party)
	}
	if act.Assets.LargeImage != "logo" || act.Assets.LargeText != "Playing as Aragorn" {
		t.Fatalf("unexpected assets: %+v", act.Assets)
	}
	if len(act.Buttons) != 2 || act.Buttons[0].URL != discordWebsiteURL || act.Buttons[1].URL != discordInviteURL {
		t.Fatalf("unexpected buttons: %+v", act.Buttons)
	}

	// Staging lobby overrides details with phase
	stg := buildDiscordActivity(started, "Picking a character", localCharInfo{
		Name: "Aragorn", Level: 70, Class: "Grand Master", Location: "Elddim",
	}, "na", "North America", 2, 30)
	if stg.Details != "Picking a character" {
		t.Fatalf("expected staging details, got %q", stg.Details)
	}
	if stg.State != "North America (2 / 30)" {
		t.Fatalf("expected state without realm, got %q", stg.State)
	}

	// Brand-new character with no save yet
	fresh := buildDiscordActivity(started, "", localCharInfo{}, "eu", "Central Europe", 1, 30)
	if fresh.Details != "Adventuring" {
		t.Fatalf("expected Adventuring fallback, got %q", fresh.Details)
	}
	if fresh.State != "Central Europe (1 / 30)" {
		t.Fatalf("unexpected eu state: %q", fresh.State)
	}
	if fresh.Assets.LargeText != "Dungeon Siege: Legends of Aranna MMO" {
		t.Fatalf("unexpected fallback large text: %q", fresh.Assets.LargeText)
	}
}

// buildFakeDsparty constructs a minimal valid DSigTank with chunked zlib entries for testing.
func buildFakeDsparty(files map[string][]byte) []byte {
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
	// Align file table
	for dataBuf.Len()%4 != 0 {
		dataBuf.WriteByte(0)
	}
	fsOff := uint32(dataOff + dataBuf.Len())

	// Build file table: [count][offsets...][entries...]
	var ftab bytes.Buffer
	_ = binary.Write(&ftab, binary.LittleEndian, uint32(len(recs)))
	offsetsPos := ftab.Len()
	for range recs {
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))
	}
	entryOffsets := make([]uint32, len(recs))
	for i, r := range recs {
		entryOffsets[i] = uint32(ftab.Len())
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))  // parent
		_ = binary.Write(&ftab, binary.LittleEndian, r.size)     // size
		_ = binary.Write(&ftab, binary.LittleEndian, r.doff)     // doff
		_ = binary.Write(&ftab, binary.LittleEndian, uint32(0))  // crc
		_ = binary.Write(&ftab, binary.LittleEndian, uint64(0))  // filetime
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(1))  // fmt = zlib
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(0))  // flags
		_ = binary.Write(&ftab, binary.LittleEndian, uint16(len(r.name)))
		ftab.WriteString(r.name)
		ftab.WriteByte(0)
		for (2+len(r.name)+1)%4 != 0 {
			ftab.WriteByte(0)
			// pad to match ((2 + len + 1 + 3) &^ 3)
			break
		}
		// Ensure exact NString alignment from entry+28
		curFrom28 := ftab.Len() - int(entryOffsets[i]) - 28
		wantFrom28 := (2 + len(r.name) + 1 + 3) &^ 3
		for curFrom28 < wantFrom28 {
			ftab.WriteByte(0)
			curFrom28++
		}
		// Chunked zlib header: csize, chunkSize, then 1 chunk {ucs, cs, extra, coff}
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

func TestParseDspartyBytes(t *testing.T) {
	partyGas := []byte(`
[party]
{
	screen_name = "Amompharetus";
	actor_class = "GRAND MASTER";
	uber_level = 70.642;
}
`)
	infoGas := []byte(`
[info]
{
	region = a8_r2_fortress;
	teleport = world:a8_r2_fortress:0,0,0,0xB2F00005;
}
`)
	tank := buildFakeDsparty(map[string][]byte{
		"party.gas": partyGas,
		"info.gas":  infoGas,
	})
	ch := parseDspartyBytes(tank, "Fallback")
	if ch.Name != "Amompharetus" {
		t.Fatalf("name=%q", ch.Name)
	}
	if ch.Level != 70 {
		t.Fatalf("level=%d", ch.Level)
	}
	if ch.Class != "Grand Master" {
		t.Fatalf("class=%q", ch.Class)
	}
	if ch.Location != "Neener's Emporium" {
		t.Fatalf("location=%q (expected Neener's Emporium)", ch.Location)
	}
}

func TestReadGamePhase(t *testing.T) {
	dir := t.TempDir()
	logPath := filepath.Join(dir, "SiegeHost_999.log")
	now := time.Date(2026, 11, 19, 18, 0, 30, 0, time.Local)

	// Missing log
	if p, crashed := readGamePhase(logPath, now); p != "Starting up" || crashed {
		t.Fatalf("missing log: phase=%q crashed=%v", p, crashed)
	}

	// Fresh staging line
	_ = os.WriteFile(logPath, []byte("[2026-11-19 18:00:20] AutoJoin: worldstate=ws_mp_staging_area_client\n"), 0o644)
	if p, crashed := readGamePhase(logPath, now); p != "Picking a character" || crashed {
		t.Fatalf("fresh staging: phase=%q crashed=%v", p, crashed)
	}

	// Stale staging line (> 60s old -> player entered world)
	_ = os.WriteFile(logPath, []byte("[2026-11-19 17:58:00] AutoJoin: worldstate=ws_mp_staging_area_client\n"), 0o644)
	if p, crashed := readGamePhase(logPath, now); p != "" || crashed {
		t.Fatalf("stale staging: phase=%q crashed=%v", p, crashed)
	}

	// Crash clears presence
	_ = os.WriteFile(logPath, []byte("[2026-11-19 18:00:25] !!! CRASH at 0x00412345\n"), 0o644)
	if _, crashed := readGamePhase(logPath, now); !crashed {
		t.Fatalf("expected crash=true")
	}
}

func TestDiscordPresenceSettingDefault(t *testing.T) {
	old := settings.DiscordPresence
	defer func() { settings.DiscordPresence = old }()

	settings.DiscordPresence = nil
	if !discordPresenceEnabled() || discordPresenceLabel() != "On" {
		t.Fatalf("nil DiscordPresence should default to On")
	}
	f := false
	settings.DiscordPresence = &f
	if discordPresenceEnabled() || discordPresenceLabel() != "Off" {
		t.Fatalf("false DiscordPresence should be Off")
	}
}
