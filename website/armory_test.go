package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func buildTest24BitBMP(w, h int) []byte {
	rowStride := ((w*3 + 3) / 4) * 4
	imgSize := rowStride * h
	fileSize := 54 + imgSize
	buf := make([]byte, fileSize)
	buf[0], buf[1] = 'B', 'M'
	binary.LittleEndian.PutUint32(buf[2:6], uint32(fileSize))
	binary.LittleEndian.PutUint32(buf[10:14], 54)
	binary.LittleEndian.PutUint32(buf[14:18], 40)
	binary.LittleEndian.PutUint32(buf[18:22], uint32(w))
	binary.LittleEndian.PutUint32(buf[22:26], uint32(h))
	binary.LittleEndian.PutUint16(buf[26:28], 1)
	binary.LittleEndian.PutUint16(buf[28:30], 24)
	// Put a red pixel at top-left (in bottom-up BMP, row h-1, x=0 -> BGR = 0,0,255)
	p := 54 + (h-1)*rowStride
	buf[p], buf[p+1], buf[p+2] = 0x10, 0x20, 0xF0
	return buf
}

func TestDecodeBMPToPNG(t *testing.T) {
	bmp := buildTest24BitBMP(8, 8)
	out, err := decodeBMPToPNG(bmp)
	if err != nil {
		t.Fatalf("decodeBMPToPNG failed: %v", err)
	}
	img, err := png.Decode(bytes.NewReader(out))
	if err != nil {
		t.Fatalf("invalid PNG output: %v", err)
	}
	if img.Bounds().Dx() != 8 || img.Bounds().Dy() != 8 {
		t.Fatalf("unexpected bounds: %v", img.Bounds())
	}
	r, g, b, a := img.At(0, 0).RGBA()
	if uint8(r>>8) != 0xF0 || uint8(g>>8) != 0x20 || uint8(b>>8) != 0x10 || uint8(a>>8) != 0xFF {
		t.Fatalf("unexpected top-left pixel: rgba=(%x,%x,%x,%x)", r>>8, g>>8, b>>8, a>>8)
	}
}

func TestArmoryBuildProfileAndPrivacy(t *testing.T) {
	tmp := resetWebTestState(t)
	oldWebDir := webDir
	t.Cleanup(func() {
		webDir = oldWebDir
		catalogData = nil
		catalogOnce = sync.Once{}
	})
	webDir = tmp
	catalogData = nil
	catalogOnce = sync.Once{}

	if err := os.MkdirAll(filepath.Join(tmp, "data"), 0o755); err != nil {
		t.Fatal(err)
	}
	wikiJSON := `[
		{"id":"sh_w_g_c_k_l_avg","name":"Large Kite Shield","category":"shields","sub_category":"Shield","rarity":"normal","defense":46,"icon":"b_gui_ig_i_a_shd_116"},
		{"id":"ax_g_c_dsx_bon_avg_dsx_set_zaurask","name":"Axe of the Zaurask Champion","category":"melee","sub_category":"Axe","rarity":"set","set_name":"Fit of the Zaurask Champion","set_compare_name":"zaurask","set_num_items":3,"damage_min":125,"damage_max":275,"icon":"b_gui_ig_i_w_axe_090"},
		{"id":"he_g_c_dsx_set_zaurask","name":"Helm of the Zaurask Champion","category":"armor","sub_category":"Helm","rarity":"set","set_name":"Fit of the Zaurask Champion","set_compare_name":"zaurask","set_num_items":3,"defense":95,"icon":"b_gui_ig_i_a_hlmt_200"},
		{"id":"book_glb_magic_01","name":"Spell Book","category":"magic","rarity":"normal","icon":"b_gui_ig_i_it_magic-book"},
		{"id":"spell_haste","name":"Haste","category":"nature_magic","desc":"Increases speed","mana_cost":15,"required_level":18,"max_level":100,"icon":"b_gui_ig_i_ic_sp_224_inv"}
	]`
	catJSON := `{
		"modifiers":{
			"repellant":{"name":"Repellant","type":"prefix","power":2,"effects":["Adds 6 to Armor"]},
			"ofogresmight":{"name":"Of Ogres Might","type":"suffix","power":5,"effects":["Adds 1 + (#uberlevel*0.05) to Strength"]}
		},
		"extra_items":{
			"ring_common":{"id":"ring_common","name":"Ring","icon":"b_gui_ig_i_it_ring_006","rarity":"normal"}
		},
		"variant_overrides":{
			"sh_w_g_c_k_l_avg":{"o_avg":{"icon":"b_gui_ig_i_a_shd_119","defense":56,"requirements":"strength:42"}},
			"ring_common":{"b_d_fin":{"icon":"b_gui_ig_i_it_ring_004"}}
		},
		"set_bonuses":{
			"ax_g_c_dsx_bon_avg_dsx_set_zaurask":{
				"set_name":"Fit of the Zaurask Champion","set_compare_name":"zaurask","set_num_items":3,
				"tiers":{
					"2":[{"req":1,"text":"Adds 5 Melee Damage"},{"req":2,"text":"7% Chance to Freeze"}],
					"3":[{"req":1,"text":"Adds 25 Melee Damage"},{"req":2,"text":"14% Chance to Freeze"},{"req":3,"text":"3-Piece Mastery"}]
				}
			}
		}
	}`
	_ = os.WriteFile(filepath.Join(tmp, "data", "wiki_items.json"), []byte(wikiJSON), 0o644)
	_ = os.WriteFile(filepath.Join(tmp, "data", "wardrobe_catalog.json"), []byte(catJSON), 0o644)

	partyGas := `
[party]
{
	[members]
	{
		[0]
		{
			[member]
			{
				actor_class = "GRAND MASTER";
				gold = 999999;
				screen_name = "HeroOne";
				selected_active_location = il_active_melee_weapon;
				template_name = halfgiant;
				[inventory]
				{
					[0]
					{
						grid_x = 100;
						inv_location = il_main;
						template_name = secret_backpack_item;
					}
					[1]
					{
						equip_slot = es_shield_hand;
						inv_location = il_shield;
						modifier_prefix_name = repellant;
						modifier_suffix_name = ofogresmight;
						template_name = sh_w_g_c_k_l_avg;
						variation_name = o_avg;
					}
					[2]
					{
						equip_slot = es_weapon_hand;
						inv_location = il_active_melee_weapon;
						template_name = ax_g_c_dsx_bon_avg_dsx_set_zaurask;
					}
					[3]
					{
						equip_slot = es_head;
						inv_location = il_main;
						template_name = he_g_c_dsx_set_zaurask;
					}
					[4]
					{
						equip_slot = es_ring_0;
						inv_location = il_main;
						template_name = ring_common;
						variation_name = b_d_fin;
					}
					[5]
					{
						equip_slot = es_spellbook;
						inv_location = il_main;
						template_name = book_glb_magic_01;
						[spellbook]
						{
							selected_active_location = il_spell_1;
							[inventory]
							{
								[0]
								{
									inv_location = il_spell_1;
									template_name = spell_haste;
								}
							}
						}
					}
				}
				[questbits]
				{
					[0]
					{
						_key = "multiplayer_world";
						[_value]
						{
							[0] { _key = "disp_a8_used"; _value = "true"; }
							[1] { _key = "teleporter_snow_town_sno_r1_active"; _value = "true"; }
							[2] { _key = "2w_teleporter_snow_town_sno_r1_active"; _value = "true"; }
						}
					}
				}
				[skills]
				{
					melee_level = 60.4;
					strength_level = 50.2;
					dexterity_level = 25.0;
					intelligence_level = 15.0;
					ranged_level = 10.0;
					combat_magic_level = 5.0;
					nature_magic_level = 18.5;
					uber_level = 60.4;
				}
			}
		}
	}
}
`
	infoGas := "[info]\n{\nregion = a9_r3_clockroom;\nsave_time = 1795000000;\n}\n"
	dsp := buildTestDsparty(map[string][]byte{
		"party.gas":      []byte(partyGas),
		"info.gas":       []byte(infoGas),
		"portrait-0.bmp": buildTest24BitBMP(64, 64),
	})
	writeTestSaveZip(t, filepath.Join(tmp, "portal-data", "saves", "tester", "current.zip"), map[string][]byte{
		"Save/HeroOne.dsparty": dsp,
	})
	accts := []*Account{{
		Name:    "Tester",
		SaveSHA: "shatest",
		Chars:   map[string]*CharRec{"heroone": {Name: "HeroOne", XP: 50000000, Updated: time.Now().UTC()}},
	}}
	ab, _ := json.Marshal(accts)
	_ = os.WriteFile(filepath.Join(tmp, "portal-data", "accounts.json"), ab, 0o644)

	// Point portalURL to dummy closed server so buildRealmStatus stays offline/fast
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(w).Encode(map[string]any{"server_name": "Test", "servers": []any{}})
	}))
	defer ts.Close()
	portalURL = ts.URL
	realmCache = &cachedBuild[RealmStatus]{ttl: 3 * time.Second, build: buildRealmStatus}

	prof, err := buildArmoryProfile("Tester", "HeroOne")
	if err != nil {
		t.Fatalf("buildArmoryProfile failed: %v", err)
	}
	if prof.Level != 60 || prof.Class != "Grand Master" || prof.Race != "Half-Giant" || prof.Rank != 1 {
		t.Fatalf("unexpected header stats: lvl=%d cls=%q race=%q rank=%d", prof.Level, prof.Class, prof.Race, prof.Rank)
	}
	if !prof.HasPortrait || prof.PortraitURL == "" {
		t.Fatalf("expected portrait URL")
	}

	// Backpack item must NEVER appear
	for sk, it := range prof.Equipment {
		if it.Template == "secret_backpack_item" {
			t.Fatalf("backpack item leaked into slot %s", sk)
		}
	}

	// Shield variation override + prefix/suffix + #uberlevel evaluation (1 + floor(60*0.05) = 4) + Magic rarity
	sh := prof.Equipment["shield"]
	if sh == nil || sh.FullName != "Repellant Large Kite Shield of Ogres Might" || sh.Rarity != "magic" {
		t.Fatalf("unexpected shield: %+v", sh)
	}
	if sh.Icon != "/static/wiki_icons/b_gui_ig_i_a_shd_119.png" || sh.Defense == nil || *sh.Defense != 56 {
		t.Fatalf("shield variation not applied: %+v", sh)
	}
	if len(sh.SuffixBonuses) == 0 || sh.SuffixBonuses[0] != "Adds 4 to Strength" {
		t.Fatalf("expected #uberlevel evaluated to 4, got %+v", sh.SuffixBonuses)
	}

	// Ring variation icon
	rg := prof.Equipment["ring_0"]
	if rg == nil || rg.Icon != "/static/wiki_icons/b_gui_ig_i_it_ring_004.png" {
		t.Fatalf("ring variation icon missing: %+v", rg)
	}

	// Active Set: 2/3 Fit of the Zaurask Champion
	if len(prof.ActiveSets) != 1 || prof.ActiveSets[0].EquippedCount != 2 || prof.ActiveSets[0].TotalPieces != 3 {
		t.Fatalf("unexpected active sets: %+v", prof.ActiveSets)
	}
	// Exact DS1 Combat Vitals (Health, Mana, Melee Damage, Ranged Damage, Armor Rating)
	if prof.CombatStats.Health <= 0 || prof.CombatStats.Mana <= 0 || prof.CombatStats.MeleeDamageMin <= 0 || prof.CombatStats.MeleeDamageMax < prof.CombatStats.MeleeDamageMin || prof.CombatStats.ArmorRating <= 56 {
		t.Fatalf("unexpected combat_stats: %+v", prof.CombatStats)
	}

	// Spellbook: Haste in slot 1
	if prof.Spellbook == nil || len(prof.Spellbook.Spells) != 1 || prof.Spellbook.Spells[0].Name != "Haste" {
		t.Fatalf("unexpected spellbook: %+v", prof.Spellbook)
	}

	// Unlocks: snow_town_sno_r1 Regular+Veteran unlocked, disp_a8 unlocked
	foundSnow := false
	for _, tp := range prof.Unlocks.Teleporters {
		if tp.ID == "snow_town_sno_r1" {
			foundSnow = true
			if !tp.Regular || !tp.Veteran || tp.Elite {
				t.Fatalf("unexpected snow teleporter unlock flags: %+v", tp)
			}
		}
	}
	if !foundSnow {
		t.Fatalf("missing snow_town_sno_r1 teleporter")
	}
	if !prof.Unlocks.Displacers[0].Unlocked || prof.Unlocks.Displacers[1].Unlocked {
		t.Fatalf("unexpected displacer unlocks: %+v", prof.Unlocks.Displacers)
	}

	// Auth gating check: unauthenticated -> 401; authenticated user -> 200; can_browse_armory only for amompharetus
	reqUnauth := httptest.NewRequest("GET", "/api/character?account=Tester&name=HeroOne", nil)
	wUnauth := httptest.NewRecorder()
	handleAPICharacter(wUnauth, reqUnauth)
	if wUnauth.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 unauthenticated, got %d", wUnauth.Code)
	}

	sessionsMu.Lock()
	sessions["tok_other"] = WebSession{Token: "tok_other", Username: "OtherPlayer", ExpiresAt: time.Now().Add(time.Hour)}
	sessions["tok_amom"] = WebSession{Token: "tok_amom", Username: "Amompharetus", IsGM: true, ExpiresAt: time.Now().Add(time.Hour)}
	sessionsMu.Unlock()
	defer func() {
		sessionsMu.Lock()
		delete(sessions, "tok_other")
		delete(sessions, "tok_amom")
		sessionsMu.Unlock()
	}()

	// OtherPlayer with direct link CAN view Armory (200)
	reqOther := httptest.NewRequest("GET", "/api/character?account=Tester&name=HeroOne", nil)
	reqOther.AddCookie(&http.Cookie{Name: "ds1mmo_session", Value: "tok_other"})
	wOther := httptest.NewRecorder()
	handleAPICharacter(wOther, reqOther)
	if wOther.Code != http.StatusOK {
		t.Fatalf("expected 200 for logged-in direct link viewer, got %d: %s", wOther.Code, wOther.Body.String())
	}

	// can_browse_armory is false when unauthenticated, true for all authenticated MMO accounts
	checkBrowse := func(tok string, want bool) {
		r := httptest.NewRequest("GET", "/api/auth/status", nil)
		if tok != "" {
			r.AddCookie(&http.Cookie{Name: "ds1mmo_session", Value: tok})
		}
		w := httptest.NewRecorder()
		handleAuthStatus(w, r)
		var resp map[string]any
		_ = json.Unmarshal(w.Body.Bytes(), &resp)
		if resp["can_browse_armory"] != want {
			t.Fatalf("tok=%s can_browse_armory=%v, want %v", tok, resp["can_browse_armory"], want)
		}
	}
	checkBrowse("", false)
	checkBrowse("tok_other", true)
	checkBrowse("tok_amom", true)
}
