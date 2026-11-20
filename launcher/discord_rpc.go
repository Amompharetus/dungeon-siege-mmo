// Discord Rich Presence client for SiegeLauncher — native Go implementation talking directly to
// Discord's local IPC named pipe (\\.\pipe\discord-ipc-0..9 on Windows, Unix socket elsewhere).
//
// Zero external dependencies, zero background logon tasks, zero PowerShell: runs only while
// DSLOA.exe is open and clears presence immediately on exit or crash.

package main

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

const (
	discordClientID   = "1573029410196693093" // Official "Dungeon Siege MMO" Discord Application
	discordLargeImage = "logo"
	discordWebsiteURL = "https://dungeonsiegemmo.online/"
	discordInviteURL  = "https://discord.gg/btrVCJAnE8"

	discordOpHandshake = 0
	discordOpFrame     = 1
	discordOpClose     = 2
)

type discordPipe interface {
	WriteAll(b []byte, timeout time.Duration) error
	ReadFull(b []byte, timeout time.Duration) error
	Close() error
}

type discordIPC struct {
	pipe  discordPipe
	nonce uint64
}

func encodeDiscordFrame(op uint32, payload any) ([]byte, error) {
	body, err := json.Marshal(payload)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, 8+len(body))
	binary.LittleEndian.PutUint32(buf[0:4], op)
	binary.LittleEndian.PutUint32(buf[4:8], uint32(len(body)))
	copy(buf[8:], body)
	return buf, nil
}

func (d *discordIPC) sendFrame(op uint32, payload any, timeout time.Duration) error {
	if d.pipe == nil {
		return errors.New("discord ipc not connected")
	}
	frame, err := encodeDiscordFrame(op, payload)
	if err != nil {
		return err
	}
	return d.pipe.WriteAll(frame, timeout)
}

type discordReply struct {
	Evt     string          `json:"evt"`
	Cmd     string          `json:"cmd"`
	Message string          `json:"message"`
	Data    json.RawMessage `json:"data"`
}

func (d *discordIPC) readFrame(timeout time.Duration) (uint32, discordReply, error) {
	if d.pipe == nil {
		return 0, discordReply{}, errors.New("discord ipc not connected")
	}
	var hdr [8]byte
	if err := d.pipe.ReadFull(hdr[:], timeout); err != nil {
		return 0, discordReply{}, err
	}
	op := binary.LittleEndian.Uint32(hdr[0:4])
	length := binary.LittleEndian.Uint32(hdr[4:8])
	if length > 1024*1024 {
		return op, discordReply{}, fmt.Errorf("discord ipc frame too large: %d", length)
	}
	body := make([]byte, length)
	if length > 0 {
		if err := d.pipe.ReadFull(body, timeout); err != nil {
			return op, discordReply{}, err
		}
	}
	var rep discordReply
	_ = json.Unmarshal(body, &rep)
	if op == discordOpClose {
		return op, rep, fmt.Errorf("discord closed ipc: %s", rep.Message)
	}
	return op, rep, nil
}

func (d *discordIPC) connect(clientID string) (int, string, error) {
	d.close()
	for idx := 0; idx < 10; idx++ {
		p, err := dialDiscordPipe(idx, 300*time.Millisecond)
		if err != nil {
			continue
		}
		d.pipe = p
		err = d.sendFrame(discordOpHandshake, map[string]any{
			"v":         1,
			"client_id": clientID,
		}, 2*time.Second)
		if err != nil {
			d.close()
			continue
		}
		_, rep, err := d.readFrame(3 * time.Second)
		if err != nil || rep.Evt != "READY" {
			d.close()
			continue
		}
		var readyData struct {
			User struct {
				Username string `json:"username"`
			} `json:"user"`
		}
		_ = json.Unmarshal(rep.Data, &readyData)
		return idx, readyData.User.Username, nil
	}
	return -1, "", errors.New("discord desktop client not running")
}

func (d *discordIPC) setActivity(pid int, act *discordActivity, timeout time.Duration) error {
	n := atomic.AddUint64(&d.nonce, 1)
	args := map[string]any{
		"pid": pid,
	}
	if act != nil {
		args["activity"] = act
	} else {
		args["activity"] = nil
	}
	err := d.sendFrame(discordOpFrame, map[string]any{
		"cmd":   "SET_ACTIVITY",
		"args":  args,
		"nonce": fmt.Sprintf("dsmmo-%d-%d", pid, n),
	}, timeout)
	if err != nil {
		return err
	}
	_, rep, err := d.readFrame(timeout)
	if err != nil {
		return err
	}
	if rep.Evt == "ERROR" {
		return fmt.Errorf("discord SET_ACTIVITY rejected: %s", string(rep.Data))
	}
	return nil
}

func (d *discordIPC) close() {
	if d.pipe != nil {
		_ = d.pipe.Close()
		d.pipe = nil
	}
}

type discordActivity struct {
	Details    string             `json:"details,omitempty"`
	State      string             `json:"state,omitempty"`
	Timestamps *discordTimestamps `json:"timestamps,omitempty"`
	Assets     *discordAssets     `json:"assets,omitempty"`
	Party      *discordParty      `json:"party,omitempty"`
	Buttons    []discordButton    `json:"buttons,omitempty"`
}

type discordTimestamps struct {
	Start int64 `json:"start,omitempty"`
}

type discordAssets struct {
	LargeImage string `json:"large_image,omitempty"`
	LargeText  string `json:"large_text,omitempty"`
}

type discordParty struct {
	ID   string `json:"id,omitempty"`
	Size [2]int `json:"size,omitempty"`
}

type discordButton struct {
	Label string `json:"label"`
	URL   string `json:"url"`
}

// ---------- Local .dsparty & SiegeHost log parsing ----------

type localCharInfo struct {
	Name     string
	Level    int
	HasLevel bool
	Class    string
	Location string
}

var (
	rpcScreenNameRe = regexp.MustCompile(`(?i)screen_name\s*=\s*"([^"]+)"`)
	rpcClassRe      = regexp.MustCompile(`(?i)actor_class\s*=\s*"([^"]+)"`)
	rpcUberLvlRe    = regexp.MustCompile(`(?i)uber_level\s*=\s*([0-9.]+)`)
	rpcRegionRe     = regexp.MustCompile(`(?i)\bregion\s*=\s*([a-zA-Z0-9_]+)`)
	rpcTeleportRe   = regexp.MustCompile(`(?i)teleport\s*=\s*[^:;\r\n]*:[^:;\r\n]*:[^,]+,[^,]+,([^,]+),(0x[0-9a-fA-F]+)\s*;`)
	rpcStartLocRe   = regexp.MustCompile(`(?i)multiplayer_world\s*=\s*"?([a-zA-Z0-9_' -]+)"?`)
	rpcLogLineRe    = regexp.MustCompile(`worldstate=|reached ws_|!!! CRASH`)
	rpcLogTimeRe    = regexp.MustCompile(`^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)`)
)

// extractTankFiles reads specific files (lowercase keys in want) out of a DSigTank (.dsparty) byte slice.
// Pure Go implementation matching tank_io.LazyTank (raw fmt=0 and chunked/stream zlib fmt=1).
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
		// NString alignment: 2-byte len + name + mandatory NUL, rounded up to 4-byte boundary.
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

func titleWords(s string) string {
	fields := strings.Fields(strings.TrimSpace(s))
	for i, w := range fields {
		if len(w) > 0 {
			fields[i] = strings.ToUpper(w[:1]) + strings.ToLower(w[1:])
		}
	}
	return strings.Join(fields, " ")
}

var (
	rpcNeenerNodes = map[string]bool{
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
	rpcOasisDunesNodes = map[string]bool{
		"0xb0760001": true, "0xb0760002": true, "0x45251ca5": true, "0x0b564081": true,
		"0x199a544b": true, "0x4a87e83e": true, "0x6b3c29f9": true, "0x7b8430e1": true,
		"0x7dbea029": true, "0xa58b325b": true, "0xa5a825cd": true, "0xb74b6738": true,
		"0xc2b31546": true, "0xd3184fc9": true, "0xef4617cd": true, "0xf8f76bd9": true,
		"0x664556bd": true, "0x8247bce7": true, "0xaae7f3fa": true, "0xc255164c": true,
	}
)

func discordRegionName(region string) string {
	switch strings.ToLower(strings.TrimSpace(region)) {
	case "quarry":
		return "Dornek's Stone Quarry"
	case "sr2":
		return "Sulfur Tunnels"
	case "sf_r1":
		return "Mt. Elspen Forest"
	case "newbie_dun":
		return "Ancient Temple"
	case "mega_forest", "mega_forest_r2", "mega_forest_r3":
		return "Great Northern Forest"
	case "gd_r1":
		return "Hovart's Folly"
	case "sd_r1", "crypts2sd", "abc_r1":
		return "Ancient Crypt"
	case "town_center", "elddim":
		return "Elddim"
	case "farmland_and_chapel":
		return "Elddim Lowlands"
	case "orchard_cellar":
		return "Orchard Cellar"
	case "path2mines":
		return "Elddim Glade"
	case "swamp_town", "lang":
		return "Lang"
	case "ds_r2", "swamp2gi":
		return "Lang Mire"
	case "gi_a", "gi_a_r2":
		return "Goblin Warrens"
	case "of_r1":
		return "Redwood Gap"
	case "wd_r1":
		return "Flooded Sanctuary"
	case "md_r1", "md_r2":
		return "Pit of Despair"
	case "river_forest", "river_forest_r2":
		return "Iliarth Valley"
	case "path2crypts":
		return "Iliarth Divide"
	case "river_canyon":
		return "Iliarth Canyon"
	case "grs_dry_hard_r1":
		return "Iliarth Falls"
	case "mine_town", "crystwind":
		return "Crystwind"
	case "mines", "mines3":
		return "Crystwind Mines"
	case "mines2":
		return "Crystwind Old Mines"
	case "fd_r4":
		return "Fury Den"
	case "ocean", "meren":
		return "Meren"
	case "sr_r5", "sno_r1":
		return "Meren Coast"
	case "dark_forest", "dark_forest_r2", "df2ds":
		return "Cloud Forest"
	case "snow_town", "fallraen":
		return "Fallraen"
	case "path2ice", "path2ice_r2":
		return "Mt. Utrae Forest"
	case "sf_r2":
		return "Fallraen Forest"
	case "ice_cave", "ice_cave_2":
		return "Glacial Caverns"
	case "id_r2":
		return "Crystal Caverns"
	case "des_r1", "quillrabe":
		return "Quillrabe"
	case "des_r2", "grs2des":
		return "Quillrabe Canyons"
	case "gp_r1", "gp_r2":
		return "Forgotten Mesas"
	case "castle_town", "hiroth":
		return "Hiroth"
	case "castle":
		return "Castle Hiroth"
	case "castle_dun":
		return "Hiroth Mausoleum"
	case "dragon", "dragon_r2", "dl_r1":
		return "Drake Nest"
	case "desert":
		return "Mesa Desert"
	case "oasis", "grescal":
		return "Grescal"
	case "oasis_cave":
		return "Grescal Oasis"
	case "dunes", "dunes2", "eg_seck_dungeon", "endless_dunes", "the endless dunes", "endless dunes":
		return "Endless Desert"
	case "dunes2hades", "hades":
		return "Volcanic Caverns"
	case "dunes2island", "island":
		return "Eastern Island"
	case "new_hub", "hub_quest_dun", "hub":
		return "Utraean Basilicus"
	case "fortress", "neener_emporium", "neeners_emporium", "neener's emporium":
		return "Neener's Emporium"
	case "ramparts", "the ramparts":
		return "The Ramparts"
	case "a8_r1_ramparts", "stronghold gate", "the stronghold gate":
		return "Stronghold Gate"
	case "a8_r1b_secretcave", "z_a8_r1b_to_a9_r2a", "stronghold tunnel", "the stronghold tunnel":
		return "Stronghold Tunnel"
	case "stronghold", "zaurask stronghold", "the zaurask stronghold", "a8_r2_fortress", "the stronghold":
		return "Zaurask Stronghold"
	case "a8_r2b_fortress", "inner stronghold", "the inner stronghold":
		return "Inner Stronghold"
	case "a8_r3_tunnels", "stronghold caverns", "the stronghold caverns":
		return "Stronghold Caverns"
	case "a9_r1_mesa", "great clock wastelands", "the great clock wastelands":
		return "Great Clock Wastelands"
	case "a9_r2_cave", "tunnels of the great clock", "the tunnels of the great clock":
		return "Tunnels of the Great Clock"
	case "clock", "lands of the great clock", "the lands of the great clock":
		return "Lands of the Great Clock"
	case "a9_r2a_clockdungeon", "halls of the great clock", "the halls of the great clock":
		return "Halls of the Great Clock"
	case "a9_r3_clockroom", "great clock", "the great clock":
		return "The Great Clock"
	default:
		clean := strings.TrimSpace(region)
		if clean == "" {
			return ""
		}
		return titleWords(strings.ReplaceAll(clean, "_", " "))
	}
}

func parseDspartyBytes(data []byte, fallbackName string) localCharInfo {
	info := localCharInfo{Name: fallbackName}
	files := extractTankFiles(data, map[string]bool{
		"party.gas": true,
		"info.gas":  true,
	})
	if partyBytes, ok := files["party.gas"]; ok {
		s := string(partyBytes)
		parsedParty := false
		if m := rpcScreenNameRe.FindStringSubmatch(s); len(m) > 1 && strings.TrimSpace(m[1]) != "" {
			info.Name = strings.TrimSpace(m[1])
			parsedParty = true
		}
		if m := rpcClassRe.FindStringSubmatch(s); len(m) > 1 {
			info.Class = titleWords(m[1])
			parsedParty = true
		}
		if m := rpcUberLvlRe.FindStringSubmatch(s); len(m) > 1 {
			if fl, err := strconv.ParseFloat(m[1], 64); err == nil && fl >= 0 {
				info.Level = int(math.Floor(fl))
				info.HasLevel = true
			}
		} else if parsedParty {
			// Fresh level-0 characters may omit uber_level in party.gas.
			info.Level = 0
			info.HasLevel = true
		}
	}
	if infoBytes, ok := files["info.gas"]; ok {
		s := string(infoBytes)
		if m := rpcRegionRe.FindStringSubmatch(s); len(m) > 1 {
			reg := strings.ToLower(strings.TrimSpace(m[1]))
			if reg == "a8_r2_fortress" {
				if tm := rpcTeleportRe.FindStringSubmatch(s); len(tm) > 2 {
					node := strings.ToLower(strings.TrimSpace(tm[2]))
					if rpcNeenerNodes[node] {
						reg = "neener_emporium"
					} else if node == "0x7e13bba2" {
						if lz, err := strconv.ParseFloat(strings.TrimSpace(tm[1]), 64); err == nil && lz > 0.13 {
							reg = "neener_emporium"
						}
					}
				}
			} else if reg == "oasis" {
				if tm := rpcTeleportRe.FindStringSubmatch(s); len(tm) > 2 {
					if rpcOasisDunesNodes[strings.ToLower(strings.TrimSpace(tm[2]))] {
						reg = "dunes"
					}
				}
			}
			info.Location = discordRegionName(reg)
		}
	}
	return info
}

type saveFileCache struct {
	path    string
	modTime time.Time
	size    int64
	info    localCharInfo
}

func (c *saveFileCache) read(saveDir, prefsPath string) localCharInfo {
	entries, err := os.ReadDir(saveDir)
	if err != nil {
		return localCharInfo{}
	}
	var newestPath, newestBase string
	var newestMod time.Time
	var newestSize int64
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		lower := strings.ToLower(name)
		if !strings.HasSuffix(lower, ".dsparty") {
			continue
		}
		fi, err := e.Info()
		if err != nil {
			continue
		}
		if newestPath == "" || fi.ModTime().After(newestMod) {
			newestPath = filepath.Join(saveDir, name)
			newestBase = name[:len(name)-len(".dsparty")]
			newestMod = fi.ModTime()
			newestSize = fi.Size()
		}
	}
	if newestPath == "" {
		return localCharInfo{}
	}
	if c.path == newestPath && c.modTime.Equal(newestMod) && c.size == newestSize {
		return c.info
	}
	data, err := os.ReadFile(newestPath)
	if err != nil {
		if c.path == newestPath {
			return c.info
		}
		return localCharInfo{Name: newestBase}
	}
	info := parseDspartyBytes(data, newestBase)
	if !info.HasLevel {
		// Guard against catching DSLOA mid-write on .dsparty: keep previous cached info
		// and do not cache the failed read so the next 5s tick retries cleanly.
		if c.path == newestPath && c.info.HasLevel {
			return c.info
		}
		return info
	}
	if info.Location == "" && prefsPath != "" {
		if pb, err := os.ReadFile(prefsPath); err == nil {
			if m := rpcStartLocRe.FindStringSubmatch(string(pb)); len(m) > 1 {
				info.Location = discordRegionName(m[1])
			}
		}
	}
	c.path, c.modTime, c.size, c.info = newestPath, newestMod, newestSize, info
	return info
}

// readGamePhase inspects the tail of SiegeHost_<pid>.log to tell whether the player is
// connecting, picking a character, entering/leaving the world, in-game (phase == ""), or crashed.
func readGamePhase(logPath string, now time.Time) (phase string, crashed bool) {
	f, err := os.Open(logPath)
	if err != nil {
		return "Starting up", false
	}
	defer f.Close()
	if st, err := f.Stat(); err == nil && st.Size() > 65536 {
		_, _ = f.Seek(st.Size()-65536, io.SeekStart)
	}
	raw, err := io.ReadAll(f)
	if err != nil {
		return "Starting up", false
	}
	last := ""
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimRight(line, "\r")
		if rpcLogLineRe.MatchString(line) {
			last = line
		}
	}
	if last == "" {
		return "Starting up", false
	}
	if strings.Contains(last, "!!! CRASH") {
		return "", true
	}
	age := 999.0
	if m := rpcLogTimeRe.FindStringSubmatch(last); len(m) > 1 {
		if ts, err := time.ParseInLocation("2006-01-02 15:04:05", m[1], now.Location()); err == nil {
			age = now.Sub(ts).Seconds()
		}
	}
	switch {
	case strings.Contains(last, "ws_mp_staging_area_client"):
		if age < 60 {
			return "Picking a character", false
		}
		return "", false
	case strings.Contains(last, "ws_game_ended"):
		return "Leaving the world", false
	case strings.Contains(last, "ws_load_map") || strings.Contains(last, "ws_mp_ingame"):
		if age < 20 {
			return "Entering the world", false
		}
		return "", false
	case strings.Contains(last, "ws_init") || strings.Contains(last, "ws_mp_"):
		return "Connecting", false
	default:
		return "", false
	}
}

func truncateRunes(s string, maxLen int) string {
	r := []rune(s)
	if len(r) <= maxLen {
		return s
	}
	if maxLen <= 1 {
		return string(r[:maxLen])
	}
	return string(r[:maxLen-1]) + "…"
}

func buildDiscordActivity(started time.Time, phase string, ch localCharInfo, serverID, realmName string, players, maxPlayers int) *discordActivity {
	_ = serverID
	details := phase
	if details == "" {
		var parts []string
		hasLevel := ch.HasLevel || ch.Level > 0
		if hasLevel && ch.Class != "" {
			parts = append(parts, fmt.Sprintf("Level %d %s", ch.Level, ch.Class))
		} else if hasLevel {
			parts = append(parts, fmt.Sprintf("Level %d Adventurer", ch.Level))
		} else if ch.Class != "" {
			parts = append(parts, ch.Class)
		}
		if ch.Location != "" {
			parts = append(parts, ch.Location)
		}
		if len(parts) > 0 {
			details = strings.Join(parts, " · ")
		} else {
			details = "Adventuring"
		}
	}
	details = truncateRunes(details, 128)

	largeText := "Dungeon Siege: Legends of Aranna MMO"
	if ch.Name != "" {
		largeText = truncateRunes("Playing as "+ch.Name, 128)
	}

	act := &discordActivity{
		Details:    details,
		Timestamps: &discordTimestamps{Start: started.Unix()},
		Assets: &discordAssets{
			LargeImage: discordLargeImage,
			LargeText:  largeText,
		},
		Buttons: []discordButton{
			{Label: "Play Dungeon Siege MMO", URL: discordWebsiteURL},
			{Label: "Join the community", URL: discordInviteURL},
		},
	}

	if realmName != "" {
		state := realmName
		if players > 0 && maxPlayers > 0 {
			state = fmt.Sprintf("%s (%d / %d)", realmName, players, maxPlayers)
		}
		act.State = truncateRunes(state, 128)
	}
	return act
}

type discordPresenceConfig struct {
	GamePID    int
	GameDir    string
	ProfileDir string
	ServerID   string
	RealmName  string
	Players    int
	MaxPlayers int
	Started    time.Time
}

func discordPresenceEnabled() bool {
	if settings.DiscordPresence == nil {
		return true
	}
	return *settings.DiscordPresence
}

func discordPresenceLabel() string {
	if discordPresenceEnabled() {
		return "On"
	}
	return "Off"
}

func fetchRealmCount(serverID, fallbackName string, fallbackPlayers, fallbackMax int) (string, int, int) {
	var st struct {
		ServerName string `json:"server_name"`
		MaxPlayers int    `json:"max_players"`
		Game       map[string]string `json:"game"`
		Servers    []struct {
			ID         string `json:"id"`
			Name       string `json:"name"`
			Players    string `json:"players"`
			MaxPlayers int    `json:"max_players"`
		} `json:"servers"`
	}
	if err := getJSON("/api/status", "", &st); err != nil {
		return fallbackName, fallbackPlayers, fallbackMax
	}
	for _, sv := range st.Servers {
		if strings.EqualFold(sv.ID, serverID) {
			name := sv.Name
			if name == "" {
				name = fallbackName
			}
			p, _ := strconv.Atoi(strings.TrimSpace(sv.Players))
			mx := sv.MaxPlayers
			if mx <= 0 {
				mx = fallbackMax
			}
			return name, p, mx
		}
	}
	if len(st.Servers) == 0 && st.ServerName != "" {
		p, _ := strconv.Atoi(strings.TrimSpace(st.Game["players"]))
		mx := st.MaxPlayers
		if mx <= 0 {
			mx = fallbackMax
		}
		return st.ServerName, p, mx
	}
	return fallbackName, fallbackPlayers, fallbackMax
}

// startDiscordPresence runs Discord Rich Presence in a background goroutine for the lifetime of
// DSLOA.exe. Calling the returned stop function clears presence and closes the IPC pipe.
func startDiscordPresence(cfg discordPresenceConfig) func() {
	if !discordPresenceEnabled() {
		return func() {}
	}
	stopCh := make(chan struct{})
	doneCh := make(chan struct{})

	go func() {
		defer close(doneCh)
		var ipc discordIPC
		defer func() {
			if ipc.pipe != nil {
				_ = ipc.setActivity(os.Getpid(), nil, 1*time.Second)
				ipc.close()
				logf("discord: presence cleared")
			}
		}()

		saveDir := filepath.Join(cfg.ProfileDir, "Save")
		prefsPath := filepath.Join(cfg.ProfileDir, "prefs.gas")
		logPath := filepath.Join(cfg.GameDir, "SiegeHostLogs", fmt.Sprintf("SiegeHost_%d.log", cfg.GamePID))
		var cache saveFileCache

		realmName := cfg.RealmName
		players := cfg.Players
		maxPlayers := cfg.MaxPlayers
		if players < 1 && maxPlayers > 0 {
			players = 1 // we just started playing
		}
		lastRealmCheck := time.Now()
		nextConnectAttempt := time.Time{}
		lastSent := ""
		crashedCleared := false

		tick := time.NewTicker(5 * time.Second)
		defer tick.Stop()

		update := func() {
			now := time.Now()
			phase, crashed := readGamePhase(logPath, now)
			if crashed {
				if !crashedCleared && ipc.pipe != nil {
					_ = ipc.setActivity(os.Getpid(), nil, 1*time.Second)
					lastSent = ""
					crashedCleared = true
					logf("discord: game crashed, presence cleared")
				}
				return
			}
			crashedCleared = false

			if ipc.pipe == nil {
				if now.Before(nextConnectAttempt) {
					return
				}
				idx, user, err := ipc.connect(discordClientID)
				if err != nil {
					nextConnectAttempt = now.Add(30 * time.Second)
					return
				}
				logf("discord: connected (discord-ipc-%d, user=%s)", idx, user)
				lastSent = ""
			}

			if now.Sub(lastRealmCheck) >= 60*time.Second {
				lastRealmCheck = now
				realmName, players, maxPlayers = fetchRealmCount(cfg.ServerID, realmName, players, maxPlayers)
			}

			ch := cache.read(saveDir, prefsPath)
			act := buildDiscordActivity(cfg.Started, phase, ch, cfg.ServerID, realmName, players, maxPlayers)
			raw, _ := json.Marshal(act)
			if string(raw) == lastSent {
				return
			}
			if err := ipc.setActivity(os.Getpid(), act, 2*time.Second); err != nil {
				logf("discord: update failed (%v), will reconnect", err)
				ipc.close()
				lastSent = ""
				nextConnectAttempt = now.Add(15 * time.Second)
				return
			}
			lastSent = string(raw)
			logf("discord: %s | %s", act.Details, act.State)
		}

		update()
		for {
			select {
			case <-stopCh:
				return
			case <-tick.C:
				update()
			}
		}
	}()

	var stopped int32
	return func() {
		if atomic.CompareAndSwapInt32(&stopped, 0, 1) {
			close(stopCh)
			select {
			case <-doneCh:
			case <-time.After(1500 * time.Millisecond):
			}
		}
	}
}
