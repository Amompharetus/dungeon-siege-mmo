package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"log"
	"math"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// ---------- Catalog Data (wiki_items.json + wardrobe_catalog.json) ----------

type wikiItemEntry struct {
	ID             string      `json:"id"`
	Name           string      `json:"name"`
	Doc            string      `json:"doc"`
	Desc           string      `json:"desc"`
	Category       string      `json:"category"`
	SubCategory    string      `json:"sub_category"`
	Source         string      `json:"source"`
	Rarity         string      `json:"rarity"`
	Tags           []string    `json:"tags"`
	StatBonus      string      `json:"stat_bonus"`
	SetName        string      `json:"set_name"`
	SetCompareName string      `json:"set_compare_name"`
	SetNumItems    int         `json:"set_num_items"`
	DamageMin      *float64    `json:"damage_min"`
	DamageMax      *float64    `json:"damage_max"`
	Defense        *float64    `json:"defense"`
	AttackSpeed    string      `json:"attack_speed"`
	IsTwoHanded    bool        `json:"is_two_handed"`
	ManaCost       any         `json:"mana_cost"`
	RequiredLevel  *int        `json:"required_level"`
	MaxLevel       *int        `json:"max_level"`
	Requirements   any         `json:"requirements"`
	Icon           string      `json:"icon"`
}

type statPair struct {
	A string `json:"a"`
	V string `json:"v"`
}

type wardrobeModifier struct {
	Name    string     `json:"name"`
	Type    string     `json:"type"`
	Power   int        `json:"power"`
	Effects []string   `json:"effects"`
	Stats   []statPair `json:"stats,omitempty"`
}

type wardrobeExtraItem struct {
	ID             string     `json:"id"`
	Name           string     `json:"name"`
	Icon           string     `json:"icon"`
	Rarity         string     `json:"rarity"`
	EquipSlot      string     `json:"equip_slot"`
	Requirements   string     `json:"requirements"`
	Defense        *float64   `json:"defense"`
	DamageMin      *float64   `json:"damage_min"`
	DamageMax      *float64   `json:"damage_max"`
	ManaCost       any        `json:"mana_cost"`
	Enchantments   []string   `json:"enchantments"`
	Stats          []statPair `json:"stats,omitempty"`
	SetName        string     `json:"set_name"`
	SetCompareName string     `json:"set_compare_name"`
	SetNumItems    int        `json:"set_num_items"`
}

type wardrobeVariant struct {
	Icon         string   `json:"icon"`
	Name         string   `json:"name"`
	Defense      *float64 `json:"defense"`
	DamageMin    *float64 `json:"damage_min"`
	DamageMax    *float64 `json:"damage_max"`
	Requirements string   `json:"requirements"`
}

type setTierLine struct {
	Req  int     `json:"req"`
	Text string  `json:"text"`
	A    string  `json:"a,omitempty"`
	Val  float64 `json:"val,omitempty"`
}

type wardrobeSetBonus struct {
	SetName        string                   `json:"set_name"`
	SetCompareName string                   `json:"set_compare_name"`
	SetNumItems    int                      `json:"set_num_items"`
	Tiers          map[string][]setTierLine `json:"tiers"`
}

type wardrobeCatalogFile struct {
	Modifiers        map[string]wardrobeModifier           `json:"modifiers"`
	ExtraItems       map[string]wardrobeExtraItem          `json:"extra_items"`
	ItemEnchantments map[string][]string                   `json:"item_enchantments"`
	ItemStats        map[string][]statPair                 `json:"item_stats"`
	VariantOverrides map[string]map[string]wardrobeVariant `json:"variant_overrides"`
	SetBonuses       map[string]wardrobeSetBonus           `json:"set_bonuses"`
}

type armoryCatalog struct {
	wikiByID map[string]wikiItemEntry
	cat      wardrobeCatalogFile
}

var (
	catalogOnce sync.Once
	catalogData *armoryCatalog
)

func getArmoryCatalog() *armoryCatalog {
	catalogOnce.Do(func() {
		ac := &armoryCatalog{
			wikiByID: make(map[string]wikiItemEntry),
			cat: wardrobeCatalogFile{
				Modifiers:        make(map[string]wardrobeModifier),
				ExtraItems:       make(map[string]wardrobeExtraItem),
				ItemEnchantments: make(map[string][]string),
				ItemStats:        make(map[string][]statPair),
				VariantOverrides: make(map[string]map[string]wardrobeVariant),
				SetBonuses:       make(map[string]wardrobeSetBonus),
			},
		}
		wikiPath := filepath.Join(webDir, "data", "wiki_items.json")
		if _, err := os.Stat(wikiPath); os.IsNotExist(err) {
			wikiPath = filepath.Join(webDir, "wiki_items.json")
		}
		if b, err := os.ReadFile(wikiPath); err == nil {
			var list []wikiItemEntry
			if err := json.Unmarshal(b, &list); err == nil {
				for _, it := range list {
					if it.ID != "" {
						ac.wikiByID[strings.ToLower(it.ID)] = it
					}
				}
			} else {
				log.Printf("[armory] ERROR unmarshaling %s: %v", wikiPath, err)
			}
		}
		catPath := filepath.Join(webDir, "data", "wardrobe_catalog.json")
		if b, err := os.ReadFile(catPath); err == nil {
			if err := json.Unmarshal(b, &ac.cat); err != nil {
				log.Printf("[armory] ERROR unmarshaling %s: %v", catPath, err)
			}
		}
		catalogData = ac
	})
	return catalogData
}

// ---------- Minimal Structured GAS Parser (for party.gas) ----------

type gasBlock struct {
	Header   string
	Attrs    map[string]string
	Children []*gasBlock
}

func parseGAS(text string) *gasBlock {
	root := &gasBlock{Header: "root", Attrs: make(map[string]string)}
	n := len(text)
	i := 0

	skipWSAndComments := func() {
		for i < n {
			c := text[i]
			if c == ' ' || c == '\t' || c == '\r' || c == '\n' {
				i++
				continue
			}
			if c == '/' && i+1 < n && text[i+1] == '/' {
				i += 2
				for i < n && text[i] != '\n' {
					i++
				}
				continue
			}
			if c == '/' && i+1 < n && text[i+1] == '*' {
				i += 2
				for i+1 < n && !(text[i] == '*' && text[i+1] == '/') {
					i++
				}
				if i+1 < n {
					i += 2
				}
				continue
			}
			break
		}
	}

	var parseBody func(b *gasBlock)
	parseBody = func(b *gasBlock) {
		for i < n {
			skipWSAndComments()
			if i >= n {
				return
			}
			if text[i] == '}' {
				i++
				return
			}
			if text[i] == '[' {
				end := strings.IndexByte(text[i+1:], ']')
				if end < 0 {
					return
				}
				hdr := strings.TrimSpace(text[i+1 : i+1+end])
				i = i + 1 + end + 1
				skipWSAndComments()
				if i < n && text[i] == '{' {
					i++
					child := &gasBlock{Header: hdr, Attrs: make(map[string]string)}
					parseBody(child)
					b.Children = append(b.Children, child)
				}
				continue
			}
			start := i
			inQ := false
			for i < n {
				c := text[i]
				if c == '"' {
					inQ = !inQ
				} else if !inQ && (c == ';' || c == '}' || c == '\n') {
					if c == ';' {
						i++
					}
					break
				}
				i++
			}
			stmt := strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(text[start:i]), ";"))
			if eq := strings.IndexByte(stmt, '='); eq > 0 {
				k := strings.TrimSpace(stmt[:eq])
				if parts := strings.Fields(k); len(parts) == 2 {
					switch strings.ToLower(parts[0]) {
					case "f", "i", "b", "x", "p", "q", "d":
						k = parts[1]
					}
				}
				k = strings.ToLower(k)
				v := strings.TrimSpace(stmt[eq+1:])
				if len(v) >= 2 && v[0] == '"' && v[len(v)-1] == '"' {
					v = v[1 : len(v)-1]
				}
				b.Attrs[k] = v
			}
		}
	}

	parseBody(root)
	return root
}

func (b *gasBlock) child(name string) *gasBlock {
	if b == nil {
		return nil
	}
	for _, c := range b.Children {
		if strings.EqualFold(c.Header, name) {
			return c
		}
	}
	return nil
}

// ---------- Armory Response Types ----------

type ArmorySkills struct {
	Uber         float64 `json:"uber"`
	Strength     float64 `json:"strength"`
	Dexterity    float64 `json:"dexterity"`
	Intelligence float64 `json:"intelligence"`
	Melee        float64 `json:"melee"`
	Ranged       float64 `json:"ranged"`
	CombatMagic  float64 `json:"combat_magic"`
	NatureMagic  float64 `json:"nature_magic"`
}

type ArmoryCombatStats struct {
	Health          int  `json:"health"`
	Mana            int  `json:"mana"`
	MeleeDamageMin  int  `json:"melee_damage_min"`
	MeleeDamageMax  int  `json:"melee_damage_max"`
	RangedDamageMin int  `json:"ranged_damage_min"`
	RangedDamageMax int  `json:"ranged_damage_max"`
	HasRangedWeapon bool `json:"has_ranged_weapon"`
	ArmorRating     int  `json:"armor_rating"`
}

type ArmoryItem struct {
	Slot             string        `json:"slot"`
	Template         string        `json:"template"`
	BaseName         string        `json:"base_name"`
	FullName         string        `json:"full_name"`
	PrefixName       string        `json:"prefix_name,omitempty"`
	SuffixName       string        `json:"suffix_name,omitempty"`
	Variation        string        `json:"variation,omitempty"`
	Icon             string        `json:"icon"`
	Rarity           string        `json:"rarity"`
	Category         string        `json:"category,omitempty"`
	SubCategory      string        `json:"sub_category,omitempty"`
	DamageMin        *float64      `json:"damage_min,omitempty"`
	DamageMax        *float64      `json:"damage_max,omitempty"`
	Defense          *float64      `json:"defense,omitempty"`
	AttackSpeed      string        `json:"attack_speed,omitempty"`
	IsTwoHanded      bool          `json:"is_two_handed,omitempty"`
	Requirements     string        `json:"requirements,omitempty"`
	Desc             string        `json:"desc,omitempty"`
	InnateBonuses    []string      `json:"innate_bonuses,omitempty"`
	PrefixBonuses    []string      `json:"prefix_bonuses,omitempty"`
	SuffixBonuses    []string      `json:"suffix_bonuses,omitempty"`
	SetName          string        `json:"set_name,omitempty"`
	SetCompareName   string        `json:"set_compare_name,omitempty"`
	SetNumItems      int           `json:"set_num_items,omitempty"`
	SetEquippedCount int           `json:"set_equipped_count,omitempty"`
	SetItemBonuses   []setTierLine `json:"set_item_bonuses,omitempty"`
	SellValue        *int          `json:"sell_value,omitempty"`
	BuyValue         any           `json:"buy_value,omitempty"`
	CanSell          *bool         `json:"can_sell,omitempty"`
	rawStats         []statPair
}

type ArmorySetBonusLine struct {
	Req     int     `json:"req"`
	Active  bool    `json:"active"`
	Text    string  `json:"text"`
	Source  string  `json:"source,omitempty"`
	statKey string
	statVal float64
}

type ArmoryActiveSet struct {
	SetName       string               `json:"set_name"`
	CompareName   string               `json:"compare_name"`
	EquippedCount int                  `json:"equipped_count"`
	TotalPieces   int                  `json:"total_pieces"`
	EquippedItems []string             `json:"equipped_items"`
	Bonuses       []ArmorySetBonusLine `json:"bonuses"`
}

type ArmorySpell struct {
	Slot          int    `json:"slot"`
	Template      string `json:"template"`
	Name          string `json:"name"`
	Icon          string `json:"icon"`
	Category      string `json:"category,omitempty"`
	Desc          string `json:"desc,omitempty"`
	ManaCost      string `json:"mana_cost,omitempty"`
	RequiredLevel *int   `json:"required_level,omitempty"`
	MaxLevel      *int   `json:"max_level,omitempty"`
	IsPrimary     bool   `json:"is_primary,omitempty"`
	IsSecondary   bool   `json:"is_secondary,omitempty"`
}

type ArmorySpellbook struct {
	BookName   string        `json:"book_name"`
	BookIcon   string        `json:"book_icon"`
	ActiveSlot int           `json:"active_slot"`
	Spells     []ArmorySpell `json:"spells"`
}

type ArmoryTeleporterUnlock struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Regular bool   `json:"regular"`
	Veteran bool   `json:"veteran"`
	Elite   bool   `json:"elite"`
}

type ArmoryDisplacerUnlock struct {
	ID       string `json:"id"`
	Name     string `json:"name"`
	Subtitle string `json:"subtitle"`
	Unlocked bool   `json:"unlocked"`
}

type ArmoryUnlocks struct {
	Teleporters []ArmoryTeleporterUnlock `json:"teleporters"`
	Displacers  []ArmoryDisplacerUnlock  `json:"displacers"`
}

type ArmoryProfile struct {
	AccountName    string                 `json:"account_name"`
	CharacterName  string                 `json:"character_name"`
	Level          int                    `json:"level"`
	UberExact      float64                `json:"uber_exact"`
	XP             int64                  `json:"xp"`
	Race           string                 `json:"race"`
	Class          string                 `json:"class"`
	Rank           int                    `json:"rank"`
	TotalLadder    int                    `json:"total_ladder"`
	IsOnline       bool                   `json:"is_online"`
	OnlineShard    string                 `json:"online_shard,omitempty"`
	LastSeen       string                 `json:"last_seen"`
	LastSeenTime   int64                  `json:"last_seen_time,omitempty"`
	Location       string                 `json:"location"`
	MapAreaId      string                 `json:"map_area_id"`
	HasPortrait    bool                   `json:"has_portrait"`
	PortraitURL    string                 `json:"portrait_url,omitempty"`
	SelectedWeapon string                 `json:"selected_weapon"`
	Skills         ArmorySkills           `json:"skills"`
	CombatStats    ArmoryCombatStats      `json:"combat_stats"`
	Equipment      map[string]*ArmoryItem `json:"equipment"`
	ActiveSets     []ArmoryActiveSet      `json:"active_sets"`
	Spellbook      *ArmorySpellbook       `json:"spellbook,omitempty"`
	Unlocks        ArmoryUnlocks          `json:"unlocks"`
}

// ---------- Modifier & Item Resolution Helpers ----------

var (
	uberFormulaRe = regexp.MustCompile(`(?i)(\d+(?:\.\d+)?)\s*\+\s*\(\s*#uberlevel\s*\*\s*(\d+(?:\.\d+)?)\s*\)`)
	uberMultRe    = regexp.MustCompile(`(?i)\(\s*#uberlevel\s*\*\s*(\d+(?:\.\d+)?)\s*\)`)
)

func evalModifierEffect(effect string, uberLevel float64) string {
	s := strings.TrimSpace(effect)
	if !strings.Contains(strings.ToLower(s), "#uberlevel") {
		return s
	}
	s = uberFormulaRe.ReplaceAllStringFunc(s, func(match string) string {
		m := uberFormulaRe.FindStringSubmatch(match)
		if len(m) == 3 {
			base, err1 := strconv.ParseFloat(m[1], 64)
			mult, err2 := strconv.ParseFloat(m[2], 64)
			if err1 == nil && err2 == nil {
				val := math.Floor(base + uberLevel*mult)
				return strconv.Itoa(int(val))
			}
		}
		return match
	})
	s = uberMultRe.ReplaceAllStringFunc(s, func(match string) string {
		m := uberMultRe.FindStringSubmatch(match)
		if len(m) == 2 {
			mult, err := strconv.ParseFloat(m[1], 64)
			if err == nil {
				val := math.Floor(uberLevel * mult)
				return strconv.Itoa(int(val))
			}
		}
		return match
	})
	return s
}

func formatSuffixDisplay(s string) string {
	s = strings.TrimSpace(s)
	if s == "" {
		return ""
	}
	if strings.HasPrefix(strings.ToLower(s), "of ") {
		return "of " + strings.TrimSpace(s[3:])
	}
	return s
}

func formatRequirementsAny(raw any) string {
	if raw == nil {
		return ""
	}
	switch v := raw.(type) {
	case string:
		return formatRequirements(v)
	case map[string]any:
		if len(v) == 0 {
			return ""
		}
		keys := make([]string, 0, len(v))
		for k := range v {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		var out []string
		for _, k := range keys {
			skill := titleWords(strings.ReplaceAll(strings.TrimSpace(k), "_", " "))
			switch num := v[k].(type) {
			case float64:
				if math.Abs(num-math.Round(num)) < 1e-4 {
					out = append(out, fmt.Sprintf("%s %d", skill, int(math.Round(num))))
				} else {
					out = append(out, fmt.Sprintf("%s %.1f", skill, num))
				}
			default:
				out = append(out, fmt.Sprintf("%s %v", skill, num))
			}
		}
		return strings.Join(out, ", ")
	default:
		return formatRequirements(fmt.Sprint(v))
	}
}

func formatRequirements(raw string) string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return ""
	}
	parts := strings.Split(raw, ",")
	var out []string
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		kv := strings.SplitN(p, ":", 2)
		if len(kv) == 2 {
			skill := titleWords(strings.ReplaceAll(strings.TrimSpace(kv[0]), "_", " "))
			val := strings.TrimSpace(kv[1])
			out = append(out, fmt.Sprintf("%s %s", skill, val))
		} else {
			out = append(out, titleWords(strings.ReplaceAll(p, "_", " ")))
		}
	}
	return strings.Join(out, ", ")
}

func formatManaCost(v any) string {
	if v == nil {
		return ""
	}
	switch x := v.(type) {
	case float64:
		if x <= 0 {
			return ""
		}
		if math.Abs(x-math.Round(x)) < 1e-4 {
			return strconv.Itoa(int(math.Round(x)))
		}
		return fmt.Sprintf("%.1f", x)
	case string:
		return strings.TrimSpace(x)
	default:
		return fmt.Sprint(x)
	}
}

func stripDifficultyPrefix(tpl string) string {
	l := strings.ToLower(strings.TrimSpace(tpl))
	if strings.HasPrefix(l, "2w_") || strings.HasPrefix(l, "3w_") {
		return l[3:]
	}
	return l
}

// calcSpellbookSellValue mirrors Z_LoA_Expansion world/contentdb/pcontent.skrit calc_gold_value$ for PT_SPELLBOOK.
func calcSpellbookSellValue(m0, m1 float64) int {
	val := 0.0
	if m0 > 0 {
		val += math.Pow(m0*14.0*0.19, 3.5)
	}
	if m1 > 0 {
		val += math.Pow(m1*14.0*0.19, 3.5)
	}
	if val > 1000000.0 {
		ex := val - 1000000.0
		val = 1000000.0 + (2150000.0 * (ex / (ex + 3500000.0)))
	}
	return 5 + int(math.Round(val))
}

func resolveArmoryItem(slot string, attrs map[string]string, uberLevel float64, ac *armoryCatalog) *ArmoryItem {
	tplRaw := strings.TrimSpace(attrs["template_name"])
	if tplRaw == "" {
		return nil
	}
	tpl := strings.ToLower(tplRaw)
	stripped := stripDifficultyPrefix(tpl)
	varName := strings.ToLower(strings.TrimSpace(attrs["variation_name"]))
	prefKey := strings.ToLower(strings.TrimSpace(attrs["modifier_prefix_name"]))
	suffKey := strings.ToLower(strings.TrimSpace(attrs["modifier_suffix_name"]))

	item := &ArmoryItem{
		Slot:      slot,
		Template:  tpl,
		BaseName:  titleWords(strings.ReplaceAll(tpl, "_", " ")),
		Variation: varName,
		Rarity:    "normal",
	}

	var iconKey string

	// 1. Lookup in wiki_items.json (exact match first, then stripped 2w_/3w_ fallback)
	if w, ok := ac.wikiByID[tpl]; ok {
		item.BaseName = w.Name
		iconKey = strings.ToLower(strings.TrimSpace(w.Icon))
		if w.Rarity != "" {
			item.Rarity = strings.ToLower(w.Rarity)
		}
		item.Category = w.Category
		item.SubCategory = w.SubCategory
		item.DamageMin = w.DamageMin
		item.DamageMax = w.DamageMax
		item.Defense = w.Defense
		item.AttackSpeed = w.AttackSpeed
		item.IsTwoHanded = w.IsTwoHanded
		item.Requirements = formatRequirementsAny(w.Requirements)
		item.Desc = w.Desc
		if enchs := ac.cat.ItemEnchantments[tpl]; len(enchs) > 0 {
			for _, e := range enchs {
				item.InnateBonuses = append(item.InnateBonuses, evalModifierEffect(e, uberLevel))
			}
		} else if w.StatBonus != "" {
			for _, sb := range strings.Split(w.StatBonus, ",") {
				if sb = strings.TrimSpace(sb); sb != "" {
					item.InnateBonuses = append(item.InnateBonuses, sb)
				}
			}
		}
		if st := ac.cat.ItemStats[tpl]; len(st) > 0 {
			item.rawStats = append(item.rawStats, st...)
		}
		item.SetName = w.SetName
		item.SetCompareName = strings.ToLower(strings.TrimSpace(w.SetCompareName))
		item.SetNumItems = w.SetNumItems
	} else if ex, ok := ac.cat.ExtraItems[tpl]; ok {
		if ex.Name != "" {
			item.BaseName = ex.Name
		}
		iconKey = strings.ToLower(strings.TrimSpace(ex.Icon))
		if ex.Rarity != "" {
			item.Rarity = strings.ToLower(ex.Rarity)
		}
		item.DamageMin = ex.DamageMin
		item.DamageMax = ex.DamageMax
		item.Defense = ex.Defense
		item.Requirements = formatRequirements(ex.Requirements)
		for _, e := range ex.Enchantments {
			item.InnateBonuses = append(item.InnateBonuses, evalModifierEffect(e, uberLevel))
		}
		if len(ex.Stats) > 0 {
			item.rawStats = append(item.rawStats, ex.Stats...)
		}
		item.SetName = ex.SetName
		item.SetCompareName = strings.ToLower(strings.TrimSpace(ex.SetCompareName))
		item.SetNumItems = ex.SetNumItems
		// Borrow category/subcategory/desc from stripped base wiki item if available
		if stripped != tpl {
			if bw, ok2 := ac.wikiByID[stripped]; ok2 {
				item.Category = bw.Category
				item.SubCategory = bw.SubCategory
				item.AttackSpeed = bw.AttackSpeed
				item.IsTwoHanded = bw.IsTwoHanded
				if iconKey == "" {
					iconKey = strings.ToLower(strings.TrimSpace(bw.Icon))
				}
				if item.SetName == "" {
					item.SetName = bw.SetName
					item.SetCompareName = strings.ToLower(strings.TrimSpace(bw.SetCompareName))
					item.SetNumItems = bw.SetNumItems
				}
			}
		}
	} else if stripped != tpl {
		if w, ok := ac.wikiByID[stripped]; ok {
			item.BaseName = w.Name
			iconKey = strings.ToLower(strings.TrimSpace(w.Icon))
			if w.Rarity != "" {
				item.Rarity = strings.ToLower(w.Rarity)
			}
			item.Category = w.Category
			item.SubCategory = w.SubCategory
			item.DamageMin = w.DamageMin
			item.DamageMax = w.DamageMax
			item.Defense = w.Defense
			item.AttackSpeed = w.AttackSpeed
			item.IsTwoHanded = w.IsTwoHanded
			item.Requirements = formatRequirementsAny(w.Requirements)
			item.Desc = w.Desc
			if enchs := ac.cat.ItemEnchantments[stripped]; len(enchs) > 0 {
				for _, e := range enchs {
					item.InnateBonuses = append(item.InnateBonuses, evalModifierEffect(e, uberLevel))
				}
			} else if w.StatBonus != "" {
				for _, sb := range strings.Split(w.StatBonus, ",") {
					if sb = strings.TrimSpace(sb); sb != "" {
						item.InnateBonuses = append(item.InnateBonuses, sb)
					}
				}
			}
			if st := ac.cat.ItemStats[stripped]; len(st) > 0 {
				item.rawStats = append(item.rawStats, st...)
			}
			item.SetName = w.SetName
			item.SetCompareName = strings.ToLower(strings.TrimSpace(w.SetCompareName))
			item.SetNumItems = w.SetNumItems
		}
	}

	// 2. Apply pcontent variation override (icon, defense, damage, requirements)
	if varName != "" {
		var vmap map[string]wardrobeVariant
		if vm, ok := ac.cat.VariantOverrides[tpl]; ok {
			vmap = vm
		} else if stripped != tpl {
			vmap = ac.cat.VariantOverrides[stripped]
		}
		if vo, ok := vmap[varName]; ok {
			if vo.Icon != "" {
				iconKey = strings.ToLower(strings.TrimSpace(vo.Icon))
			}
			if vo.Name != "" {
				item.BaseName = vo.Name
			}
			if vo.Defense != nil {
				item.Defense = vo.Defense
			}
			if vo.DamageMin != nil {
				item.DamageMin = vo.DamageMin
			}
			if vo.DamageMax != nil {
				item.DamageMax = vo.DamageMax
			}
			if vo.Requirements != "" {
				item.Requirements = formatRequirements(vo.Requirements)
			}
		}
	}

	// 3. Apply prefix & suffix modifiers from pcontent.gas
	var prefPower, suffPower float64
	if prefKey != "" {
		if pm, ok := ac.cat.Modifiers[prefKey]; ok {
			item.PrefixName = pm.Name
			prefPower = float64(pm.Power)
			for _, eff := range pm.Effects {
				item.PrefixBonuses = append(item.PrefixBonuses, evalModifierEffect(eff, uberLevel))
			}
			if len(pm.Stats) > 0 {
				item.rawStats = append(item.rawStats, pm.Stats...)
			}
		} else {
			item.PrefixName = titleWords(strings.ReplaceAll(prefKey, "_", " "))
		}
	}
	if suffKey != "" {
		if sm, ok := ac.cat.Modifiers[suffKey]; ok {
			item.SuffixName = formatSuffixDisplay(sm.Name)
			suffPower = float64(sm.Power)
			for _, eff := range sm.Effects {
				item.SuffixBonuses = append(item.SuffixBonuses, evalModifierEffect(eff, uberLevel))
			}
			if len(sm.Stats) > 0 {
				item.rawStats = append(item.rawStats, sm.Stats...)
			}
		} else {
			item.SuffixName = formatSuffixDisplay(titleWords(strings.ReplaceAll(suffKey, "_", " ")))
		}
	}

	// 3b. Compute exact rolled Sell & Buy gold price for equipped Spellbooks (matching pcontent.skrit + Z_LoA_Expansion cap)
	if slot == "spellbook" {
		if tpl == "book_glb_magic_umberteen" {
			f := false
			item.CanSell = &f
		} else if item.Rarity != "set" && strings.HasPrefix(stripped, "book_glb_magic_") {
			sell := calcSpellbookSellValue(prefPower, suffPower)
			buy := sell * 3
			item.SellValue = &sell
			item.BuyValue = buy
		}
	}

	// 4. Promote Standard items with modifiers to Magic rarity (matching DS1 tooltips)
	if item.Rarity == "normal" && (prefKey != "" || suffKey != "") {
		item.Rarity = "magic"
	}

	// 5. Build full display name: [Prefix] BaseName [Suffix]
	var nameParts []string
	if item.PrefixName != "" {
		nameParts = append(nameParts, item.PrefixName)
	}
	nameParts = append(nameParts, item.BaseName)
	if item.SuffixName != "" {
		nameParts = append(nameParts, item.SuffixName)
	}
	item.FullName = strings.Join(nameParts, " ")

	if iconKey != "" {
		item.Icon = "/static/wiki_icons/" + iconKey + ".png"
	}

	return item
}

func resolveArmorySpell(slot int, attrs map[string]string, activeSlot int, ac *armoryCatalog) ArmorySpell {
	tpl := strings.ToLower(strings.TrimSpace(attrs["template_name"]))
	sp := ArmorySpell{
		Slot:        slot,
		Template:    tpl,
		Name:        titleWords(strings.ReplaceAll(strings.TrimPrefix(tpl, "spell_"), "_", " ")),
		IsPrimary:   slot == 1,
		IsSecondary: slot == 2,
	}
	_ = activeSlot
	if w, ok := ac.wikiByID[tpl]; ok {
		sp.Name = w.Name
		if w.Icon != "" {
			sp.Icon = "/static/wiki_icons/" + strings.ToLower(strings.TrimSpace(w.Icon)) + ".png"
		}
		sp.Category = w.Category
		sp.Desc = w.Desc
		sp.ManaCost = formatManaCost(w.ManaCost)
		sp.RequiredLevel = w.RequiredLevel
		sp.MaxLevel = w.MaxLevel
	} else if ex, ok := ac.cat.ExtraItems[tpl]; ok {
		if ex.Name != "" {
			sp.Name = ex.Name
		}
		if ex.Icon != "" {
			sp.Icon = "/static/wiki_icons/" + strings.ToLower(strings.TrimSpace(ex.Icon)) + ".png"
		}
		sp.ManaCost = formatManaCost(ex.ManaCost)
	}
	return sp
}

// ---------- Unlocks (Teleporters & Displacers from [questbits]) ----------

type teleporterDef struct {
	ID   string
	Key  string
	Name string
}

var utraeanTeleporters = []teleporterDef{
	{ID: "river_forest_path2mines", Key: "teleport_river_forest_path2mines_active", Name: "Iliarth Valley ↔ Elddim Glade"},
	{ID: "river_forest_path2crypts", Key: "teleport_river_forest_path2crypts_active", Name: "Iliarth Valley ↔ Iliarth Divide"},
	{ID: "mine_town_mines2", Key: "teleporter_mine_town_mines2_active", Name: "Crystwind ↔ Crystwind Old Mines"},
	{ID: "snow_town_sf_r2", Key: "teleporter_snow_town_sf_r2_active", Name: "Fallraen ↔ Fallraen Forest"},
	{ID: "snow_town_sno_r1", Key: "teleporter_snow_town_sno_r1_active", Name: "Fallraen ↔ Meren Coast"},
	{ID: "ocean_dark_forest", Key: "teleport_ocean_dark_forest_active", Name: "Meren ↔ Cloud Forest"},
	{ID: "swamp_town_gi_a_r2", Key: "teleporter_swamp_town_gi_a_r2_active", Name: "Lang ↔ Goblin Warrens"},
	{ID: "swamp_town_of_r1", Key: "teleporter_swamp_town_of_r1_active", Name: "Lang ↔ Redwood Gap"},
	{ID: "castle_town_desert", Key: "teleport_castle_town_desert_active", Name: "Hiroth ↔ Mesa Desert"},
}

func extractQuestBits(member *gasBlock) map[string]bool {
	bits := make(map[string]bool)
	qb := member.child("questbits")
	if qb == nil {
		return bits
	}
	for _, mapNode := range qb.Children {
		valBlock := mapNode.child("_value")
		if valBlock == nil {
			continue
		}
		for _, entry := range valBlock.Children {
			k := strings.ToLower(strings.TrimSpace(entry.Attrs["_key"]))
			v := strings.ToLower(strings.TrimSpace(entry.Attrs["_value"]))
			if k != "" && (v == "true" || v == "1") {
				bits[k] = true
			}
		}
	}
	return bits
}

func buildArmoryUnlocks(bits map[string]bool) ArmoryUnlocks {
	tps := make([]ArmoryTeleporterUnlock, 0, len(utraeanTeleporters))
	for _, td := range utraeanTeleporters {
		tps = append(tps, ArmoryTeleporterUnlock{
			ID:      td.ID,
			Name:    td.Name,
			Regular: bits[td.Key],
			Veteran: bits["2w_"+td.Key],
			Elite:   bits["3w_"+td.Key],
		})
	}
	disps := []ArmoryDisplacerUnlock{
		{
			ID:       "disp_a8",
			Name:     "Zaurask Stronghold Displacer",
			Subtitle: "Grescal ↔ Neener's Emporium",
			Unlocked: bits["disp_a8_used"],
		},
		{
			ID:       "disp_clock",
			Name:     "The Great Clock Displacer",
			Subtitle: "Island of the Utraeans",
			Unlocked: bits["disp_clock_used"] || bits["xp_king_activate_disp"],
		},
	}
	return ArmoryUnlocks{
		Teleporters: tps,
		Displacers:  disps,
	}
}

// ---------- Pure-Go BMP (24/32-bit BI_RGB) -> PNG Portrait Converter ----------

func decodeBMPToPNG(bmp []byte) ([]byte, error) {
	if len(bmp) < 54 || bmp[0] != 'B' || bmp[1] != 'M' {
		return nil, fmt.Errorf("not a valid BMP")
	}
	pixOff := int(binary.LittleEndian.Uint32(bmp[10:14]))
	hdrSize := int(binary.LittleEndian.Uint32(bmp[14:18]))
	if hdrSize < 40 || pixOff < 54 || pixOff >= len(bmp) {
		return nil, fmt.Errorf("unsupported BMP header")
	}
	w := int(int32(binary.LittleEndian.Uint32(bmp[18:22])))
	hSigned := int(int32(binary.LittleEndian.Uint32(bmp[22:26])))
	bpp := int(binary.LittleEndian.Uint16(bmp[28:30]))
	comp := binary.LittleEndian.Uint32(bmp[30:34])
	if comp != 0 || (bpp != 24 && bpp != 32) {
		return nil, fmt.Errorf("unsupported BMP bpp=%d comp=%d", bpp, comp)
	}
	topDown := hSigned < 0
	h := hSigned
	if h < 0 {
		h = -h
	}
	if w <= 0 || h <= 0 || w > 512 || h > 512 {
		return nil, fmt.Errorf("invalid BMP dimensions %dx%d", w, h)
	}
	bytesPerPixel := bpp / 8
	rowStride := ((w*bytesPerPixel + 3) / 4) * 4
	if pixOff+rowStride*h > len(bmp) {
		return nil, fmt.Errorf("truncated BMP pixel data")
	}

	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		srcY := y
		if !topDown {
			srcY = h - 1 - y
		}
		rowStart := pixOff + srcY*rowStride
		for x := 0; x < w; x++ {
			p := rowStart + x*bytesPerPixel
			b, g, r := bmp[p], bmp[p+1], bmp[p+2]
			a := uint8(255)
			if bytesPerPixel == 4 {
				a = bmp[p+3]
				if a == 0 {
					a = 255
				}
			}
			img.SetNRGBA(x, y, color.NRGBA{R: r, G: g, B: b, A: a})
		}
	}
	var out bytes.Buffer
	if err := png.Encode(&out, img); err != nil {
		return nil, err
	}
	return out.Bytes(), nil
}

// ---------- Profile & Portrait Loader (Cached by .dsparty CRC32) ----------

type cachedPortrait struct {
	EntrySig string
	ETag     string
	PNG      []byte
}

var (
	portraitMu    sync.RWMutex
	portraitCache = make(map[string]cachedPortrait)
)

func openCharDspartyBytes(account, charName string) (canonicalAcct string, acct *Account, rec *CharRec, cleanName string, entrySig string, rawBytes []byte, found bool) {
	accounts := loadAccounts()
	acctLower := strings.ToLower(strings.TrimSpace(account))
	acct = accounts[acctLower]
	if acct == nil {
		return "", nil, nil, "", "", nil, false
	}
	canonicalAcct = acct.Name
	if canonicalAcct == "" {
		canonicalAcct = account
	}

	zipPath := filepath.Join(portalDir, "portal-data", "saves", acctLower, "current.zip")
	r, err := zip.OpenReader(zipPath)
	if err != nil {
		return canonicalAcct, acct, nil, "", "", nil, false
	}
	defer r.Close()

	normTarget := normalizeCharName(charName)
	var matchFile *zip.File
	var matchClean string

	// Pass 1: filename match
	for _, f := range r.File {
		cn, ok := isDspartyZipEntry(f.Name)
		if !ok {
			continue
		}
		if strings.EqualFold(cn, charName) || (normTarget != "" && normalizeCharName(cn) == normTarget) {
			matchFile = f
			matchClean = cn
			break
		}
	}

	// Pass 2: screen_name match via cached account zip
	if matchFile == nil {
		if az, ok := loadAccountZip(canonicalAcct, acct.SaveSHA); ok {
			for _, e := range az.Entries {
				if strings.EqualFold(e.Raw.Name, charName) || (normTarget != "" && normalizeCharName(e.Raw.Name) == normTarget) {
					for _, f := range r.File {
						cn, ok2 := isDspartyZipEntry(f.Name)
						if ok2 && strings.EqualFold(cn, e.CleanName) {
							matchFile = f
							matchClean = cn
							break
						}
					}
					break
				}
			}
		}
	}

	if matchFile == nil {
		return canonicalAcct, acct, nil, "", "", nil, false
	}

	if acct.Chars != nil {
		rec = matchCharRec(acct.Chars, matchClean)
		if rec == nil {
			rec = matchCharRec(acct.Chars, charName)
		}
	}
	rc, err := matchFile.Open()
	if err != nil {
		return canonicalAcct, acct, rec, matchClean, "", nil, false
	}
	rawBytes, err = io.ReadAll(io.LimitReader(rc, 8*1024*1024))
	rc.Close()
	if err != nil || len(rawBytes) == 0 {
		return canonicalAcct, acct, rec, matchClean, "", nil, false
	}
	entrySig = fmt.Sprintf("%08x:%d", matchFile.CRC32, matchFile.UncompressedSize64)
	return canonicalAcct, acct, rec, matchClean, entrySig, rawBytes, true
}

func parseFloatAttr(m map[string]string, k string) float64 {
	v, err := strconv.ParseFloat(strings.TrimSpace(m[k]), 64)
	if err != nil || math.IsNaN(v) || math.IsInf(v, 0) {
		return 0
	}
	return math.Round(v*100) / 100
}

func buildArmoryProfile(account, charName string) (*ArmoryProfile, error) {
	canonicalAcct, acct, rec, cleanName, entrySig, rawBytes, found := openCharDspartyBytes(account, charName)
	if !found || acct == nil || acct.Banned {
		return nil, os.ErrNotExist
	}

	files := extractTankFiles(rawBytes, map[string]bool{
		"party.gas":      true,
		"info.gas":       true,
		"portrait-0.bmp": true,
	})
	partyBytes := files["party.gas"]
	infoBytes := files["info.gas"]
	if len(partyBytes) == 0 {
		fbP, fbI := extractTankFilesFallback(rawBytes, cleanName)
		partyBytes = fbP
		if len(infoBytes) == 0 {
			infoBytes = fbI
		}
	}
	if len(partyBytes) == 0 {
		return nil, fmt.Errorf("unable to parse character save")
	}

	// Only allow verified MMO characters (matching Ladder policy — blocks non-MMO local saves)
	root := parseGAS(string(partyBytes))
	var member *gasBlock
	if pb := root.child("party"); pb != nil {
		if mb := pb.child("members"); mb != nil {
			if m0 := mb.child("0"); m0 != nil {
				member = m0.child("member")
			}
		}
	}
	if member == nil {
		return nil, fmt.Errorf("missing party member block")
	}

	displayName := strings.TrimSpace(member.Attrs["screen_name"])
	if displayName == "" {
		displayName = cleanName
	}
	if rec == nil && acct.Chars != nil {
		rec = matchCharRec(acct.Chars, displayName)
	}

	// Pull live RealmStatus to ensure 100% consistency for Rank, Online status, Live XP, Level, and Location
	status := getRealmStatus()
	rank := 0
	totalLadder := len(status.Ladder)
	isOnline := false
	onlineShard := ""
	lastSeen := "Unknown"
	var lastSeenTime int64
	var ladderXP int64
	var ladderLevel int
	hasLadderMatch := false
	dispNorm := normalizeCharName(displayName)
	cleanNorm := normalizeCharName(cleanName)

	for idx, lc := range status.Ladder {
		if !strings.EqualFold(lc.AccountName, canonicalAcct) {
			continue
		}
		lcNorm := normalizeCharName(lc.CharacterName)
		if strings.EqualFold(lc.CharacterName, displayName) || lcNorm == dispNorm || lcNorm == cleanNorm {
			rank = idx + 1
			isOnline = lc.IsOnline
			onlineShard = lc.OnlineShard
			lastSeen = lc.LastSeen
			lastSeenTime = lc.LastSeenTime
			ladderXP = lc.XP
			ladderLevel = lc.Level
			hasLadderMatch = true
			break
		}
	}

	if rec == nil && !hasLadderMatch {
		return nil, os.ErrNotExist
	}

	// Parse skills & total 7-skill XP (JoinGuard integrity check)
	skills := ArmorySkills{}
	var saveTotalXP int64
	if sb := member.child("skills"); sb != nil {
		skills.Uber = parseFloatAttr(sb.Attrs, "uber_level")
		skills.Strength = parseFloatAttr(sb.Attrs, "strength_level")
		skills.Dexterity = parseFloatAttr(sb.Attrs, "dexterity_level")
		skills.Intelligence = parseFloatAttr(sb.Attrs, "intelligence_level")
		skills.Melee = parseFloatAttr(sb.Attrs, "melee_level")
		skills.Ranged = parseFloatAttr(sb.Attrs, "ranged_level")
		skills.CombatMagic = parseFloatAttr(sb.Attrs, "combat_magic_level")
		skills.NatureMagic = parseFloatAttr(sb.Attrs, "nature_magic_level")
		var sumXP float64
		for _, xk := range []string{"strength_xp", "dexterity_xp", "intelligence_xp", "melee_xp", "ranged_xp", "combat_magic_xp", "nature_magic_xp"} {
			if raw, ok := sb.Attrs[xk]; ok {
				if v, err := strconv.ParseFloat(strings.TrimSpace(raw), 64); err == nil && !math.IsNaN(v) && !math.IsInf(v, 0) && v > 0 {
					sumXP += v
				}
			}
		}
		if sumXP > 0 {
			saveTotalXP = int64(math.Round(sumXP))
		}
	}

	effectiveXP := effectiveXPForChar(rec, ladderXP)
	rawLevel := int(math.Floor(skills.Uber))
	if rawLevel < 0 {
		rawLevel = 0
	}
	if isTamperedCharacterSave(rawLevel, saveTotalXP, true, effectiveXP) {
		return nil, os.ErrNotExist
	}
	displayXP := displayXPForChar(CachedChar{
		RawLevel:    rawLevel,
		SaveTotalXP: saveTotalXP,
		HasPartyGas: true,
	}, effectiveXP)
	level := applyAntiCheatLevel(rawLevel, true, effectiveXP)
	if hasLadderMatch && ladderLevel > level {
		level = ladderLevel
	}

	cls := titleWords(member.Attrs["actor_class"])
	cls = applyAntiCheatClass(cls, level)
	race := templateToRace(member.Attrs["template_name"])
	if race == "" {
		race = "Human"
	}

	// Location from info.gas (prefer live Realms location if currently online!)
	location := "Elddim"
	mapAreaId := "elddim"
	if len(infoBytes) > 0 {
		infoStr := string(infoBytes)
		if m := regionRe.FindStringSubmatch(infoStr); len(m) > 1 {
			reg := strings.TrimSpace(m[1])
			if reg != "" {
				if strings.EqualFold(reg, "a8_r2_fortress") {
					reg = resolveFortressRegion(infoStr)
				} else if strings.EqualFold(reg, "oasis") {
					reg = resolveOasisRegion(infoStr)
				}
				location, mapAreaId = regionToArea(reg)
			}
		}
	}
	if isOnline {
		checkPlayers := status.NA.Players
		if strings.EqualFold(onlineShard, "EU") {
			checkPlayers = status.EU.Players
		}
		for _, p := range checkPlayers {
			if strings.EqualFold(p.AccountName, canonicalAcct) && normalizeCharName(p.CharacterName) == dispNorm {
				if p.Location != "" {
					location = p.Location
					mapAreaId = p.MapAreaId
				}
				break
			}
		}
	}

	// Parse ONLY equipped items & equipped spellbook (backpack inventory & gold are strictly excluded!)
	ac := getArmoryCatalog()
	equipment := make(map[string]*ArmoryItem)
	var spellbook *ArmorySpellbook
	selectedWeapon := "melee"
	if sel := strings.ToLower(strings.TrimSpace(member.Attrs["selected_active_location"])); strings.Contains(sel, "ranged") {
		selectedWeapon = "ranged"
	} else if strings.Contains(sel, "spell") {
		selectedWeapon = "spell"
	}

	if inv := member.child("inventory"); inv != nil {
		for _, itemBlk := range inv.Children {
			es := strings.ToLower(strings.TrimSpace(itemBlk.Attrs["equip_slot"]))
			il := strings.ToLower(strings.TrimSpace(itemBlk.Attrs["inv_location"]))

			slotKey := ""
			switch {
			case es == "es_head":
				slotKey = "head"
			case es == "es_chest":
				slotKey = "chest"
			case es == "es_forearms":
				slotKey = "forearms"
			case es == "es_feet":
				slotKey = "feet"
			case es == "es_amulet":
				slotKey = "amulet"
			case es == "es_ring_0":
				slotKey = "ring_0"
			case es == "es_ring_1":
				slotKey = "ring_1"
			case es == "es_ring_2":
				slotKey = "ring_2"
			case es == "es_ring_3":
				slotKey = "ring_3"
			case es == "es_shield_hand" || il == "il_shield":
				slotKey = "shield"
			case es == "es_spellbook" || il == "il_spellbook":
				slotKey = "spellbook"
			case il == "il_active_ranged_weapon":
				slotKey = "ranged"
			case il == "il_active_melee_weapon":
				slotKey = "melee"
			case es == "es_weapon_hand":
				// Fallback if weapon has es_weapon_hand without il_active_*
				if _, hasMelee := equipment["melee"]; !hasMelee {
					slotKey = "melee"
				}
			}

			if slotKey == "" {
				continue // Skip backpack items (il_main without equip_slot)
			}

			resolved := resolveArmoryItem(slotKey, itemBlk.Attrs, float64(level), ac)
			if resolved != nil {
				equipment[slotKey] = resolved
			}

			// Extract spells inside equipped spellbook
			if slotKey == "spellbook" {
				sbBlk := itemBlk.child("spellbook")
				sbInfo := &ArmorySpellbook{
					BookName:   "Spell Book",
					BookIcon:   "/static/wiki_icons/b_gui_ig_i_it_magic-book.png",
					ActiveSlot: 1,
					Spells:     []ArmorySpell{},
				}
				if resolved != nil {
					if resolved.FullName != "" {
						sbInfo.BookName = resolved.FullName
					}
					if resolved.Icon != "" {
						sbInfo.BookIcon = resolved.Icon
					}
				}
				if sbBlk != nil {
					if sel := strings.ToLower(strings.TrimSpace(sbBlk.Attrs["selected_active_location"])); strings.HasPrefix(sel, "il_spell_") {
						if n, err := strconv.Atoi(strings.TrimPrefix(sel, "il_spell_")); err == nil && n >= 1 && n <= 12 {
							sbInfo.ActiveSlot = n
						}
					}
					if sbInv := sbBlk.child("inventory"); sbInv != nil {
						for _, spBlk := range sbInv.Children {
							spLoc := strings.ToLower(strings.TrimSpace(spBlk.Attrs["inv_location"]))
							if !strings.HasPrefix(spLoc, "il_spell_") {
								continue
							}
							slotNum, err := strconv.Atoi(strings.TrimPrefix(spLoc, "il_spell_"))
							if err != nil || slotNum < 1 || slotNum > 12 {
								continue
							}
							sbInfo.Spells = append(sbInfo.Spells, resolveArmorySpell(slotNum, spBlk.Attrs, sbInfo.ActiveSlot, ac))
						}
						sort.Slice(sbInfo.Spells, func(i, j int) bool {
							return sbInfo.Spells[i].Slot < sbInfo.Spells[j].Slot
						})
					}
				}
				spellbook = sbInfo
			}
		}
	}

	// Calculate Active Set Bonuses across all equipped items
	// Note: if player has both melee and ranged equipped, DS1 counts equipped worn slots + currently active weapon set,
	// or all equipped gear slots; count unique set templates equipped so holding two copies of the same ring doesn't double-count falsely.
	type setAccum struct {
		name      string
		compare   string
		total     int
		items     []*ArmoryItem
		seenTpls  map[string]bool
	}
	setsByCmp := make(map[string]*setAccum)
	slotOrder := []string{"head", "chest", "forearms", "feet", "melee", "shield", "ranged", "amulet", "ring_0", "ring_1", "ring_2", "ring_3", "spellbook"}
	for _, sk := range slotOrder {
		it := equipment[sk]
		if it == nil || it.SetCompareName == "" {
			continue
		}
		acc := setsByCmp[it.SetCompareName]
		if acc == nil {
			acc = &setAccum{
				name:     it.SetName,
				compare:  it.SetCompareName,
				total:    it.SetNumItems,
				seenTpls: make(map[string]bool),
			}
			setsByCmp[it.SetCompareName] = acc
		}
		if it.SetNumItems > acc.total {
			acc.total = it.SetNumItems
		}
		baseTpl := stripDifficultyPrefix(it.Template)
		if !acc.seenTpls[baseTpl] {
			acc.seenTpls[baseTpl] = true
			acc.items = append(acc.items, it)
		}
	}

	var activeSets []ArmoryActiveSet
	for _, acc := range setsByCmp {
		eqCount := len(acc.items)
		var itemNames []string
		var bonuses []ArmorySetBonusLine
		for _, it := range acc.items {
			it.SetEquippedCount = eqCount
			itemNames = append(itemNames, it.BaseName)

			baseTpl := stripDifficultyPrefix(it.Template)
			sb, ok := ac.cat.SetBonuses[it.Template]
			if !ok {
				sb, ok = ac.cat.SetBonuses[baseTpl]
			}
			if ok {
				// Current active bonuses at eqCount
				tierKey := strconv.Itoa(eqCount)
				if eqCount > sb.SetNumItems && sb.SetNumItems > 0 {
					tierKey = strconv.Itoa(sb.SetNumItems)
				}
				if lines, ok2 := sb.Tiers[tierKey]; ok2 {
					it.SetItemBonuses = lines
					for _, ln := range lines {
						bonuses = append(bonuses, ArmorySetBonusLine{
							Req:     ln.Req,
							Active:  true,
							Text:    ln.Text,
							Source:  it.BaseName,
							statKey: ln.A,
							statVal: ln.Val,
						})
					}
				}
				// Also show locked higher-tier bonuses from max tier so player sees what next set piece unlocks
				maxKey := strconv.Itoa(sb.SetNumItems)
				if maxLines, ok3 := sb.Tiers[maxKey]; ok3 {
					for _, ln := range maxLines {
						if ln.Req > eqCount {
							bonuses = append(bonuses, ArmorySetBonusLine{
								Req:    ln.Req,
								Active: false,
								Text:   ln.Text,
								Source: it.BaseName,
							})
						}
					}
				}
			}
		}
		sort.SliceStable(bonuses, func(i, j int) bool {
			if bonuses[i].Active != bonuses[j].Active {
				return bonuses[i].Active
			}
			return bonuses[i].Req < bonuses[j].Req
		})
		activeSets = append(activeSets, ArmoryActiveSet{
			SetName:       acc.name,
			CompareName:   acc.compare,
			EquippedCount: eqCount,
			TotalPieces:   acc.total,
			EquippedItems: itemNames,
			Bonuses:       bonuses,
		})
	}
	sort.Slice(activeSets, func(i, j int) bool {
		if activeSets[i].EquippedCount != activeSets[j].EquippedCount {
			return activeSets[i].EquippedCount > activeSets[j].EquippedCount
		}
		return activeSets[i].SetName < activeSets[j].SetName
	})

	// Compute exact DS1 Character Sheet combat vitals (Health, Mana, Melee Damage, Ranged Damage, Armor Rating)
	combatStats := computeArmoryCombatStats(skills, equipment, activeSets, selectedWeapon, float64(level))

	// Unlocks
	qbits := extractQuestBits(member)
	unlocks := buildArmoryUnlocks(qbits)

	// Portrait availability
	hasPortrait := len(files["portrait-0.bmp"]) > 54
	portraitURL := ""
	if hasPortrait {
		portraitURL = fmt.Sprintf("/api/character/portrait?account=%s&name=%s&v=%s",
			awsEscape(canonicalAcct), awsEscape(displayName), awsEscape(entrySig))
	}

	return &ArmoryProfile{
		AccountName:    canonicalAcct,
		CharacterName:  displayName,
		Level:          level,
		UberExact:      skills.Uber,
		XP:             displayXP,
		Race:           race,
		Class:          cls,
		Rank:           rank,
		TotalLadder:    totalLadder,
		IsOnline:       isOnline,
		OnlineShard:    onlineShard,
		LastSeen:       lastSeen,
		LastSeenTime:   lastSeenTime,
		Location:       location,
		MapAreaId:      mapAreaId,
		HasPortrait:    hasPortrait,
		PortraitURL:    portraitURL,
		SelectedWeapon: selectedWeapon,
		Skills:         skills,
		CombatStats:    combatStats,
		Equipment:      equipment,
		ActiveSets:     activeSets,
		Spellbook:      spellbook,
		Unlocks:        unlocks,
	}, nil
}

func evalStatExpr(expr string, uberLevel float64) float64 {
	s := strings.TrimSpace(expr)
	if s == "" {
		return 0
	}
	if v, err := strconv.ParseFloat(s, 64); err == nil {
		return v
	}
	resolved := evalModifierEffect(s, uberLevel)
	if v, err := strconv.ParseFloat(strings.TrimSpace(resolved), 64); err == nil {
		return v
	}
	return 0
}

func computeArmoryCombatStats(skills ArmorySkills, equipment map[string]*ArmoryItem, activeSets []ArmoryActiveSet, selectedWeapon string, uberLevel float64) ArmoryCombatStats {
	var bStr, bDex, bInt float64
	var bHP, bMP, bArmor float64
	var bMeleeMin, bMeleeMax float64
	var bRangedMin, bRangedMax float64

	applyPair := func(key string, val float64, includeDamage bool, damageScope string) {
		switch key {
		case "str":
			bStr += val
		case "dex":
			bDex += val
		case "int":
			bInt += val
		case "hp":
			bHP += val
		case "mp":
			bMP += val
		case "arm":
			bArmor += val
		case "mmin":
			if includeDamage && (damageScope == "all" || damageScope == "melee") {
				bMeleeMin += val
			}
		case "mmax":
			if includeDamage && (damageScope == "all" || damageScope == "melee") {
				bMeleeMax += val
			}
		case "rmin":
			if includeDamage && (damageScope == "all" || damageScope == "ranged") {
				bRangedMin += val
			}
		case "rmax":
			if includeDamage && (damageScope == "all" || damageScope == "ranged") {
				bRangedMax += val
			}
		}
	}

	// Passive worn slots (always active)
	passiveSlots := []string{"head", "chest", "forearms", "feet", "shield", "amulet", "ring_0", "ring_1", "ring_2", "ring_3", "spellbook"}
	for _, sk := range passiveSlots {
		it := equipment[sk]
		if it == nil {
			continue
		}
		for _, sp := range it.rawStats {
			applyPair(sp.A, evalStatExpr(sp.V, uberLevel), true, "all")
		}
	}

	// Active weapon contributes its attribute/HP/MP/Armor bonuses; each weapon contributes its own damage bonuses to its weapon line
	activeWeaponSlot := "melee"
	if selectedWeapon == "ranged" {
		activeWeaponSlot = "ranged"
	}
	for _, wsk := range []string{"melee", "ranged"} {
		it := equipment[wsk]
		if it == nil {
			continue
		}
		isActive := (wsk == activeWeaponSlot) || (selectedWeapon == "spell" && wsk == "melee")
		for _, sp := range it.rawStats {
			val := evalStatExpr(sp.V, uberLevel)
			if sp.A == "mmin" || sp.A == "mmax" {
				if wsk == "melee" {
					applyPair(sp.A, val, true, "melee")
				}
			} else if sp.A == "rmin" || sp.A == "rmax" {
				if wsk == "ranged" {
					applyPair(sp.A, val, true, "ranged")
				}
			} else if isActive {
				applyPair(sp.A, val, false, "")
			}
		}
	}

	// Active set bonuses
	for _, st := range activeSets {
		for _, bn := range st.Bonuses {
			if bn.Active && bn.statKey != "" && bn.statVal != 0 {
				applyPair(bn.statKey, bn.statVal, true, "all")
			}
		}
	}

	clamp180 := func(v float64) float64 {
		if v < 0 {
			return 0
		}
		if v > 180 {
			return 180
		}
		return v
	}
	effStr := clamp180(skills.Strength + bStr)
	effDex := clamp180(skills.Dexterity + bDex)
	effInt := clamp180(skills.Intelligence + bInt)

	// DS1 Health & Mana (formulas.gas [recalculation_constants])
	adjAttr := func(a float64) float64 {
		if a >= 10 {
			return a - 9
		}
		if a <= 0 {
			return 0
		}
		return a / 9.0
	}
	health := int(math.Round((adjAttr(effStr)*2.1+adjAttr(effDex)*0.7+adjAttr(effInt)*0.7)*14.0 + bHP))
	if health < 1 {
		health = 1
	}
	mana := int(math.Round(adjAttr(effStr)*1.0 + adjAttr(effDex)*4.0 + adjAttr(effInt)*25.0 + bMP))
	if mana < 0 {
		mana = 0
	}

	// DS1 Melee Damage (DSLOA.exe Rules::GetDamageRange 0x5b4d76..0x5b4df6)
	sFloor := math.Floor(effStr)
	strMin, strMax := 2.0, 4.0
	if sFloor >= 10 {
		strMin = math.Floor((sFloor-10)*1.4 + 2.0)
		strMax = math.Floor((sFloor-10)*1.6 + 4.0)
	}
	var mWepMin, mWepMax float64
	if mw := equipment["melee"]; mw != nil {
		if mw.DamageMin != nil {
			mWepMin = *mw.DamageMin
		}
		if mw.DamageMax != nil {
			mWepMax = *mw.DamageMax
		}
	}
	meleeMin := int(math.Round(strMin + mWepMin + bMeleeMin))
	meleeMax := int(math.Round(strMax + mWepMax + bMeleeMax))
	if meleeMin < 1 {
		meleeMin = 1
	}
	if meleeMax < meleeMin {
		meleeMax = meleeMin
	}

	// DS1 Ranged Damage (DSLOA.exe Rules::GetDamageRange 0x5b4c20..0x5b4c98)
	hasRanged := false
	rangedMin, rangedMax := 0, 0
	if rw := equipment["ranged"]; rw != nil {
		hasRanged = true
		var rWepMin, rWepMax float64
		if rw.DamageMin != nil {
			rWepMin = *rw.DamageMin
		}
		if rw.DamageMax != nil {
			rWepMax = *rw.DamageMax
		}
		rangedMin = int(math.Round(rWepMin + bRangedMin))
		rangedMax = int(math.Round(rWepMax + bRangedMax))
		if rangedMin < 1 {
			rangedMin = 1
		}
		if rangedMax < rangedMin {
			rangedMax = rangedMin
		}
	}

	// DS1 Armor Rating (DSLOA.exe Rules::GetTotalDefense 0x5b4450..0x5b44bf)
	var armor float64
	if effDex >= 10 {
		armor = (effDex-10)*3.5 + 9.0
	} else {
		armor = effDex * (2.0 / 3.0)
	}
	for _, ask := range []string{"head", "chest", "forearms", "feet", "shield"} {
		if it := equipment[ask]; it != nil && it.Defense != nil {
			armor += *it.Defense
		}
	}
	armor += bArmor
	armorRating := int(math.Round(armor))
	if armorRating < 0 {
		armorRating = 0
	}

	return ArmoryCombatStats{
		Health:          health,
		Mana:            mana,
		MeleeDamageMin:  meleeMin,
		MeleeDamageMax:  meleeMax,
		RangedDamageMin: rangedMin,
		RangedDamageMax: rangedMax,
		HasRangedWeapon: hasRanged,
		ArmorRating:     armorRating,
	}
}

// ---------- HTTP Handlers ----------

func handleAPICharacter(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	_, authed := getSession(r)
	if !authed {
		w.WriteHeader(http.StatusUnauthorized)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": "Sign in with your MMO account to view character armory",
		})
		return
	}

	account := strings.TrimSpace(r.URL.Query().Get("account"))
	name := strings.TrimSpace(r.URL.Query().Get("name"))
	if account == "" || name == "" {
		w.WriteHeader(http.StatusBadRequest)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": "Missing account or character name",
		})
		return
	}

	profile, err := buildArmoryProfile(account, name)
	if err != nil {
		if os.IsNotExist(err) {
			w.WriteHeader(http.StatusNotFound)
			_ = json.NewEncoder(w).Encode(map[string]any{
				"status":  "error",
				"message": "Character not found",
			})
			return
		}
		w.WriteHeader(http.StatusInternalServerError)
		_ = json.NewEncoder(w).Encode(map[string]any{
			"status":  "error",
			"message": "Unable to load character profile",
		})
		return
	}

	_ = json.NewEncoder(w).Encode(profile)
}

func handleAPICharacterPortrait(w http.ResponseWriter, r *http.Request) {
	_, authed := getSession(r)
	if !authed {
		http.Error(w, "Unauthorized", http.StatusUnauthorized)
		return
	}
	account := strings.TrimSpace(r.URL.Query().Get("account"))
	name := strings.TrimSpace(r.URL.Query().Get("name"))
	if account == "" || name == "" {
		http.NotFound(w, r)
		return
	}

	canonicalAcct, acct, rec, cleanName, entrySig, rawBytes, found := openCharDspartyBytes(account, name)
	if !found || acct == nil || acct.Banned {
		http.NotFound(w, r)
		return
	}

	// Enforce the same verified MMO & non-tampered save policy as Ladder / Armory
	pFiles := extractTankFiles(rawBytes, map[string]bool{"party.gas": true})
	var parsed CachedChar
	parseGasIntoChar(pFiles["party.gas"], nil, &parsed)
	if rec == nil && acct.Chars != nil && parsed.Name != "" {
		rec = matchCharRec(acct.Chars, parsed.Name)
	}
	effXP := effectiveXPForChar(rec, 0)
	if rec == nil || isTamperedCharacterSave(parsed.RawLevel, parsed.SaveTotalXP, parsed.HasPartyGas, effXP) {
		// Check live Ladder XP in case the character is actively leveling online right now
		dispNorm := normalizeCharName(parsed.Name)
		cleanNorm := normalizeCharName(cleanName)
		var ladderXP int64
		matchedLadder := false
		for _, lc := range getRealmStatus().Ladder {
			if !strings.EqualFold(lc.AccountName, canonicalAcct) {
				continue
			}
			lcNorm := normalizeCharName(lc.CharacterName)
			if strings.EqualFold(lc.CharacterName, parsed.Name) || lcNorm == dispNorm || lcNorm == cleanNorm {
				ladderXP = lc.XP
				matchedLadder = true
				break
			}
		}
		effXP = effectiveXPForChar(rec, ladderXP)
		if (rec == nil && !matchedLadder) || isTamperedCharacterSave(parsed.RawLevel, parsed.SaveTotalXP, parsed.HasPartyGas, effXP) {
			http.NotFound(w, r)
			return
		}
	}

	cacheKey := strings.ToLower(canonicalAcct) + ":" + strings.ToLower(cleanName)
	portraitMu.RLock()
	cached, ok := portraitCache[cacheKey]
	portraitMu.RUnlock()

	var pngBytes []byte
	var etag string
	if ok && cached.EntrySig == entrySig && len(cached.PNG) > 0 {
		pngBytes = cached.PNG
		etag = cached.ETag
	} else {
		files := extractTankFiles(rawBytes, map[string]bool{"portrait-0.bmp": true})
		bmp := files["portrait-0.bmp"]
		if len(bmp) == 0 {
			http.NotFound(w, r)
			return
		}
		var err error
		pngBytes, err = decodeBMPToPNG(bmp)
		if err != nil {
			http.NotFound(w, r)
			return
		}
		sum := sha1.Sum(pngBytes)
		etag = `"` + hex.EncodeToString(sum[:8]) + `"`
		portraitMu.Lock()
		portraitCache[cacheKey] = cachedPortrait{EntrySig: entrySig, ETag: etag, PNG: pngBytes}
		portraitMu.Unlock()
	}

	w.Header().Set("ETag", etag)
	w.Header().Set("Cache-Control", "private, max-age=300")
	if r.Header.Get("If-None-Match") == etag {
		w.WriteHeader(http.StatusNotModified)
		return
	}
	w.Header().Set("Content-Type", "image/png")
	_, _ = w.Write(pngBytes)
}
