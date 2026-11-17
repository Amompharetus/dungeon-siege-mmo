"""Generate scaled LoA endgame templates (`mmo_loa_*`) for MpWorld integration.

Builds `regular/`, `veteran/`, and `elite/` GAS template files dynamically from
`Logic.dsres`, `Expansion.dsres`, `DS1_Difficulty_Patch.dsres`, `Unlocked Loot.dsres`,
and `Buffed Spells.dsres` so all 10 merged Legends of Aranna regions scale above
Grescal (58 / 85 / 110) and outperform Utraean Hades (`maljin_*`) at endgame.
"""

from __future__ import annotations

import os
import re
from typing import Dict, List, Optional, Tuple

from gas_parser import extract_blocks
from tank_io import LazyTank


TIERS: Tuple[Tuple[str, str, str], ...] = (
    ("1w", "", "regular"),
    ("2w", "2W_", "veteran"),
    ("3w", "3W_", "elite"),
)

# Enemies placed in LOA10 + generator children that get `mmo_loa_*` clones.
# NPCs, ambient critters, and passive props keep their vanilla template names.
ENEMY_STEMS: Tuple[str, ...] = (
    "dsx_automaton_flying_one",
    "dsx_automaton_flying_two",
    "dsx_automaton_one",
    "dsx_automaton_two",
    "dsx_blaster",
    "dsx_elemental_air",
    "dsx_elemental_air_minion",
    "dsx_elemental_earth",
    "dsx_elemental_earth_minion",
    "dsx_elemental_fire",
    "dsx_elemental_fire_ranged",
    "dsx_elemental_water",
    "dsx_elemental_water_minion",
    "dsx_fury",
    "dsx_gobbot_flamethrower",
    "dsx_gobbot_flamethrower_02",
    "dsx_gobbot_general",
    "dsx_gobbot_grenade",
    "dsx_gobbot_grenade_02",
    "dsx_gobbot_lightning_02",
    "dsx_gobbot_minigun",
    "dsx_gobbot_minigun_02",
    "dsx_goblin_blackguard",
    "dsx_goblin_guard",
    "dsx_goblin_mutant",
    "dsx_goblin_mutant_hurler",
    "dsx_golem_clockwork",
    "dsx_hassat_fighter",
    "dsx_hassat_panther",
    "dsx_horrid",
    "dsx_lava_spirit",
    "dsx_nosirrom",
    "dsx_perforator",
    "dsx_phrak_jungle",
    "dsx_phrak_piercer",
    "dsx_quadscale",
    "dsx_rock_beast",
    "dsx_scorpiot_melee",
    "dsx_scorpiot_ranged",
    "dsx_shadow_bigboss",
    "dsx_shadow_jumper_boss",
    "dsx_shadow_jumper_minion_five",
    "dsx_shadow_jumper_minion_four",
    "dsx_shadow_jumper_minion_one",
    "dsx_shadow_jumper_minion_two",
    "dsx_shard_blue",
    "dsx_shard_green",
    "dsx_shard_purple",
    "dsx_shard_teal",
    "dsx_slithermage_melee",
    "dsx_slithermage_ranged",
    "dsx_soul_stinger",
    "dsx_troll_masher",
    "dsx_troll_mountain",
    "dsx_zaurask_fighter",
    "dsx_zaurask_mage",
    "dsx_zaurask_ranger",
    "dsx_zaurask_slasher",
    "dsx_zepheryl",
    "dsx_ztrool",
    "gobbot_flamethrower",
    "gobbot_grenade",
    "gobbot_lightning",
    "heater",
    "perforator",
    "proxo",
    "rock_beast",
)

# Role classification drives XP, skill, and stat floors above Grescal/Hades.
ROLE_OVERRIDES: Dict[str, str] = {
    "dsx_shadow_bigboss": "final_boss",
    "dsx_shadow_jumper_boss": "boss",
    "dsx_nosirrom": "boss",
    "dsx_gobbot_general": "boss",
    "dsx_elemental_air": "elite",
    "dsx_elemental_earth": "elite",
    "dsx_elemental_fire": "elite",
    "dsx_elemental_fire_ranged": "elite",
    "dsx_elemental_water": "elite",
    "dsx_fury": "elite",
    "dsx_golem_clockwork": "elite",
    "dsx_shadow_jumper_minion_five": "elite",
    "dsx_shadow_jumper_minion_four": "elite",
    "dsx_shard_purple": "elite",
    "dsx_shard_green": "elite",
    "dsx_shard_blue": "elite",
    "dsx_shard_teal": "elite",
    "dsx_troll_masher": "elite",
    "dsx_horrid": "elite",
    "dsx_hassat_panther": "swarm",
    "dsx_phrak_jungle": "swarm",
    "dsx_phrak_piercer": "swarm",
    "dsx_zepheryl": "swarm",
    "dsx_ztrool": "swarm",
    "dsx_shadow_jumper_minion_one": "swarm",
    "proxo": "bomb",
}

GENERATOR_STEMS: Tuple[str, ...] = (
    "dsx_generator_elemental_water_minion",
    "dsx_generator_mound-phrak",
    "gen_explode_box1-heater",
    "gen_explode_box2-perforator",
    "gen_explode_box2-proxo",
    "gen_explode_box3-gobbot_flamethrower",
    "gen_explode_box3-gobbot_grenade",
    "gen_explode_box3-gobbot_lightning",
    "gen_explode_rock-rock_beast",
)

DARKGEN_STEMS: Tuple[str, ...] = (
    "dsx_darkgenerator_1",
    "dsx_darkgenerator_2",
    "dsx_darkgenerator_clockroom",
)

CONTAINER_STEMS: Tuple[str, ...] = (
    "crate_dsx_a8_r1-01",
    "crate_dsx_a8_r1-02",
    "crate_dsx_a8_r1-03",
    "crate_dsx_a8_r1-spec",
    "crate_dsx_a8_r2-01",
    "crate_dsx_a8_r2-02",
    "crate_dsx_a8_r2-03",
    "crate_dsx_a8_r2-04",
    "crate_dsx_a8_r3-01",
    "crate_dsx_a8_r3-02",
    "crate_dsx_a8_r3-03",
    "crate_dsx_a8_r3-04",
    "crate_dsx_a8_r3-spec",
    "crate_dsx_a8_secret-01",
    "crate_dsx_a8_secret-02",
    "crate_dsx_a8_secret-03",
    "crate_dsx_a8_secret-spec",
    "crate_dsx_a9_r1-01",
    "crate_dsx_a9_r1-02",
    "crate_dsx_a9_r1-03",
    "crate_dsx_a9_r1-04",
    "crate_dsx_a9_r1-05",
    "crate_dsx_a9_r1-spec",
    "crate_dsx_a9_r2-01",
    "crate_dsx_a9_r2-02",
    "crate_dsx_a9_r2-03",
    "crate_dsx_a9_r2-04",
    "crate_dsx_a9_r2-spec",
    "crate_dsx_a9_r2a-01",
    "crate_dsx_a9_r2a-02",
    "crate_dsx_a9_r3-01",
    "crate_dsx_a9_r3-03",
    "crate_dsx_a9_r3-spec_air",
    "crate_dsx_a9_r3-spec_clockroom",
    "crate_dsx_a9_r3-spec_earth",
    "crate_dsx_a9_r3-spec_fire",
    "crate_dsx_a9_r3-spec_water",
    "dsx_set_clockwork_2",
    "dsx_set_clockwork_2-mp",
    "dsx_set_clockwork_3",
    "dsx_set_clockwork_3-mp",
    "dsx_set_clockwork_4",
    "dsx_set_clockwork_4-mp",
    "dsx_set_demlock_3",
    "dsx_set_demlock_3-mp",
)

MERCHANT_STEMS: Tuple[str, ...] = (
    "neener",
    "dsx_goblin_vendingmachine",
)


def mmo_name(vanilla_name: str) -> str:
    """Convert a vanilla (possibly 2W_/3W_-prefixed) template name to unique `mmo_loa_*`."""
    lo = vanilla_name.lower().strip()
    prefix = ""
    stem = lo
    if lo.startswith("2w_"):
        prefix = "2W_"
        stem = lo[3:]
    elif lo.startswith("3w_"):
        prefix = "3W_"
        stem = lo[3:]
    if stem == "st_g_c_dsx_stars":
        return f"{prefix}mmo_loa_st_stars"
    return f"{prefix}mmo_loa_{stem}"


def _build_name_lookup(stems: Tuple[str, ...]) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for s in stems:
        for _, pfx, _ in TIERS:
            v = (pfx.lower() + s).lower()
            out[v] = mmo_name(v)
    return out


TEMPLATE_REMAP: Dict[str, str] = {}
TEMPLATE_REMAP.update(_build_name_lookup(ENEMY_STEMS))
TEMPLATE_REMAP.update(_build_name_lookup(GENERATOR_STEMS))
TEMPLATE_REMAP.update(_build_name_lookup(DARKGEN_STEMS))
TEMPLATE_REMAP.update(_build_name_lookup(CONTAINER_STEMS))
TEMPLATE_REMAP.update(_build_name_lookup(MERCHANT_STEMS))
for _, _pfx, _ in TIERS:
    _v = f"{_pfx.lower()}st_g_c_dsx_stars"
    TEMPLATE_REMAP[_v] = mmo_name(_v)


class TemplateVFS:
    """Priority-ordered read-only template index from base game/mod tanks."""

    TANK_ORDER: Tuple[str, ...] = (
        r"Resources\Logic.dsres",
        r"DSLOA\Expansion.dsres",
        r"DSLOA\DS1_Difficulty_Patch.dsres",
        r"DSLOA\Unlocked Loot.dsres",
        r"DSLOA\Buffed Spells.dsres",
    )

    def __init__(self, game_dir: str) -> None:
        self.blocks: Dict[str, str] = {}
        self.paths: Dict[str, str] = {}
        for rel in self.TANK_ORDER:
            p = os.path.join(game_dir, rel)
            if not os.path.exists(p):
                continue
            tank = LazyTank(p)
            try:
                for k in tank.keys():
                    kl = k.lower()
                    if kl.startswith("world/contentdb/templates/") and kl.endswith(".gas"):
                        txt = tank.get(k).decode("utf-8", "replace")
                        for _, _, _, blk in extract_blocks(txt, r"\[t:template,n:[^\]]+\]"):
                            hm = re.match(r"\[t:template,n:([^,\]]+)\]", blk.strip(), re.I)
                            if hm:
                                key = hm.group(1).strip().lower()
                                self.blocks[key] = blk
                                self.paths[key] = kl
            finally:
                tank.close()

    def get_block(self, name: str) -> str:
        return self.blocks[name.lower()]

    def find_attr(self, name: str, attr: str, seen: Optional[set] = None) -> Optional[str]:
        if seen is None:
            seen = set()
        kl = name.lower()
        if kl in seen or kl not in self.blocks:
            return None
        seen.add(kl)
        blk = self.blocks[kl]
        rx = re.compile(rf"(?mi)(?:^|[{{;])[ \t]*(?:[fbixs][ \t]+)?{re.escape(attr)}[ \t]*=[ \t]*([^;\r\n]+);")
        matches = rx.findall(blk)
        if matches:
            return matches[-1].strip()
        sm = re.search(r"specializes\s*=\s*([^;\s]+)", blk, re.I)
        if sm:
            return self.find_attr(sm.group(1).strip(), attr, seen)
        return None


def _brace_depth_at(text: str, pos: int) -> int:
    """Return GAS `{}` nesting depth immediately before character offset `pos`."""
    depth = 0
    i = 0
    in_str = False
    in_line = False
    in_block = False
    while i < pos:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < pos else ""
        if in_line:
            if ch in "\r\n":
                in_line = False
        elif in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                i += 1
        elif in_str:
            if ch == '"' and (i == 0 or text[i - 1] != "\\"):
                in_str = False
        else:
            if ch == "/" and nxt == "/":
                in_line = True
                i += 1
            elif ch == "/" and nxt == "*":
                in_block = True
                i += 1
            elif ch == '"':
                in_str = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
        i += 1
    return depth


def _extract_top_subblocks(blk: str, header: str) -> List[Tuple[int, int, str]]:
    """Return `(start, end, text)` for direct child `[header]` blocks (`depth == 1`)."""
    out: List[Tuple[int, int, str]] = []
    for _, s, e, sub in extract_blocks(blk, rf"^[ \t]*\[{re.escape(header)}\]"):
        if _brace_depth_at(blk, s) == 1:
            out.append((s, e, sub))
    return out


def _get_subblock(blk: str, header: str, top_level: bool = False) -> Optional[Tuple[int, int, str]]:
    """Find `[header]` sub-block inside `blk` (last occurrence)."""
    hits = _extract_top_subblocks(blk, header) if top_level else [
        (s, e, sub) for _, s, e, sub in extract_blocks(blk, rf"^[ \t]*\[{re.escape(header)}\]")
    ]
    if not hits:
        return None
    return hits[-1]


def _remove_subblocks(blk: str, header: str, top_level: bool = False) -> str:
    hits = _extract_top_subblocks(blk, header) if top_level else [
        (s, e, sub) for _, s, e, sub in extract_blocks(blk, rf"^[ \t]*\[{re.escape(header)}\]")
    ]
    out = blk
    for s, e, _ in reversed(hits):
        out = out[:s].rstrip("\r\n") + "\n" + out[e:].lstrip("\r\n")
    return out


def _upsert_subblock(blk: str, header: str, new_sub: str, top_level: bool = False) -> str:
    found = _get_subblock(blk, header, top_level=top_level)
    if found:
        s, e, _ = found
        return blk[:s] + new_sub + blk[e:]
    end_brace = blk.rfind("}")
    return blk[:end_brace] + "\n\t" + new_sub + "\n}"


def _set_scalar_in_block(sub: str, key: str, value: str, indent: str = "\t\t") -> str:
    rx = re.compile(rf"(?mi)((?:^|[{{;])[ \t]*)((?:[fbixs][ \t]+)?){re.escape(key)}[ \t]*=[^;\r\n]*;")
    if rx.search(sub):
        return rx.sub(lambda m: f"{m.group(1)}{m.group(2)}{key} = {value};", sub)
    end_brace = sub.rfind("}")
    return sub[:end_brace].rstrip() + f"\n{indent}{key} = {value};\n" + sub[end_brace:]


def _fmt_num(val: float) -> str:
    if abs(val - round(val)) < 0.05:
        return str(int(round(val)))
    return f"{val:.1f}"


# Role multipliers/floors keyed by (tier, role) -> dict of parameters.
_ROLE_PARAMS: Dict[Tuple[str, str], Dict[str, float]] = {
    ("1w", "swarm"): {"xp_mult": 18.0, "xp_floor": 45000, "hp_mult": 1.95, "hp_floor": 780, "def_mult": 1.80, "def_floor": 390, "dmg_mult": 1.65, "dmg_min_floor": 125.0, "dmg_max_floor": 155.0, "skill_floor": 38},
    ("2w", "swarm"): {"xp_mult": 2.6, "xp_floor": 165000, "hp_mult": 1.55, "hp_floor": 1450, "def_mult": 1.55, "def_floor": 780, "dmg_mult": 1.30, "dmg_min_floor": 230.0, "dmg_max_floor": 315.0, "skill_floor": 72},
    ("3w", "swarm"): {"xp_mult": 2.6, "xp_floor": 4200000, "hp_mult": 1.55, "hp_floor": 2250, "def_mult": 1.55, "def_floor": 1120, "dmg_mult": 1.30, "dmg_min_floor": 330.0, "dmg_max_floor": 450.0, "skill_floor": 106},

    ("1w", "bomb"): {"xp_mult": 5.5, "xp_floor": 45000, "hp_mult": 2.00, "hp_floor": 210, "def_mult": 1.80, "def_floor": 210, "dmg_mult": 1.60, "dmg_min_floor": 360.0, "dmg_max_floor": 430.0, "skill_floor": 40},
    ("2w", "bomb"): {"xp_mult": 2.5, "xp_floor": 180000, "hp_mult": 1.80, "hp_floor": 380, "def_mult": 1.60, "def_floor": 380, "dmg_mult": 1.32, "dmg_min_floor": 650.0, "dmg_max_floor": 860.0, "skill_floor": 76},
    ("3w", "bomb"): {"xp_mult": 2.5, "xp_floor": 4800000, "hp_mult": 1.80, "hp_floor": 560, "def_mult": 1.60, "def_floor": 540, "dmg_mult": 1.32, "dmg_min_floor": 920.0, "dmg_max_floor": 1220.0, "skill_floor": 110},

    ("1w", "core"): {"xp_mult": 36.0, "xp_floor": 108000, "hp_mult": 2.05, "hp_floor": 1750, "def_mult": 1.90, "def_floor": 670, "dmg_mult": 1.70, "dmg_min_floor": 190.0, "dmg_max_floor": 225.0, "skill_floor": 45},
    ("2w", "core"): {"xp_mult": 3.5, "xp_floor": 320000, "hp_mult": 1.55, "hp_floor": 2800, "def_mult": 1.55, "def_floor": 1220, "dmg_mult": 1.32, "dmg_min_floor": 330.0, "dmg_max_floor": 440.0, "skill_floor": 78},
    ("3w", "core"): {"xp_mult": 3.8, "xp_floor": 9800000, "hp_mult": 1.55, "hp_floor": 4000, "def_mult": 1.55, "def_floor": 1720, "dmg_mult": 1.32, "dmg_min_floor": 460.0, "dmg_max_floor": 610.0, "skill_floor": 112},

    ("1w", "elite"): {"xp_mult": 56.0, "xp_floor": 210000, "hp_mult": 2.05, "hp_floor": 2850, "def_mult": 1.80, "def_floor": 900, "dmg_mult": 1.45, "dmg_min_floor": 240.0, "dmg_max_floor": 300.0, "skill_floor": 48},
    ("2w", "elite"): {"xp_mult": 4.5, "xp_floor": 530000, "hp_mult": 1.55, "hp_floor": 4400, "def_mult": 1.50, "def_floor": 1500, "dmg_mult": 1.25, "dmg_min_floor": 420.0, "dmg_max_floor": 560.0, "skill_floor": 82},
    ("3w", "elite"): {"xp_mult": 4.8, "xp_floor": 16200000, "hp_mult": 1.55, "hp_floor": 6400, "def_mult": 1.50, "def_floor": 2100, "dmg_mult": 1.25, "dmg_min_floor": 590.0, "dmg_max_floor": 790.0, "skill_floor": 116},

    ("1w", "boss"): {"xp_mult": 105.0, "xp_floor": 325000, "hp_mult": 2.75, "hp_floor": 5600, "def_mult": 2.15, "def_floor": 1120, "dmg_mult": 1.65, "dmg_min_floor": 315.0, "dmg_max_floor": 395.0, "skill_floor": 52},
    ("2w", "boss"): {"xp_mult": 6.5, "xp_floor": 820000, "hp_mult": 1.90, "hp_floor": 8600, "def_mult": 1.75, "def_floor": 1800, "dmg_mult": 1.32, "dmg_min_floor": 520.0, "dmg_max_floor": 690.0, "skill_floor": 88},
    ("3w", "boss"): {"xp_mult": 6.5, "xp_floor": 25000000, "hp_mult": 1.90, "hp_floor": 12500, "def_mult": 1.75, "def_floor": 2550, "dmg_mult": 1.32, "dmg_min_floor": 740.0, "dmg_max_floor": 980.0, "skill_floor": 122},

    ("1w", "final_boss"): {"xp_mult": 140.0, "xp_floor": 560000, "hp_mult": 3.60, "hp_floor": 11000, "def_mult": 2.35, "def_floor": 1440, "dmg_mult": 1.85, "dmg_min_floor": 390.0, "dmg_max_floor": 495.0, "skill_floor": 62},
    ("2w", "final_boss"): {"xp_mult": 8.5, "xp_floor": 1350000, "hp_mult": 2.10, "hp_floor": 16800, "def_mult": 1.75, "def_floor": 2350, "dmg_mult": 1.40, "dmg_min_floor": 660.0, "dmg_max_floor": 880.0, "skill_floor": 98},
    ("3w", "final_boss"): {"xp_mult": 8.5, "xp_floor": 36000000, "hp_mult": 2.10, "hp_floor": 24500, "def_mult": 1.70, "def_floor": 3250, "dmg_mult": 1.40, "dmg_min_floor": 950.0, "dmg_max_floor": 1260.0, "skill_floor": 136},
}

# Act 9 / high-Act-8 core enemy XP floors so Act 9 enemies advance beyond Act 8 Ramparts.
_ACT9_CORE_XP_FLOORS: Dict[str, float] = {
    "1w": 138000,
    "2w": 390000,
    "3w": 11800000,
}

# Per-stem post-Grescal benchmark floors (matches/beats Utraean UP Act 9 counterparts).
_STEM_FLOOR_OVERRIDES: Dict[Tuple[str, str], Dict[str, float]] = {
    ("dsx_lava_spirit", "1w"): {"xp": 175000, "hp": 1950, "def": 820, "dmg_min": 255.0, "dmg_max": 295.0, "skill": 59},
    ("dsx_lava_spirit", "2w"): {"xp": 440000, "hp": 3100, "def": 1450, "dmg_min": 410.0, "dmg_max": 500.0, "skill": 92},
    ("dsx_lava_spirit", "3w"): {"xp": 13200000, "hp": 4300, "def": 1950, "dmg_min": 560.0, "dmg_max": 720.0, "skill": 124},
    ("dsx_fury", "1w"): {"hp": 3950},
    ("dsx_fury", "2w"): {"hp": 6800},
    ("dsx_fury", "3w"): {"hp": 9800},
}

_POTIONS_BY_TIER = {
    "1w": ("potion_health_large", "potion_mana_large", "potion_rejuvenation_medium"),
    "2w": ("potion_health_large", "potion_mana_large", "potion_rejuvenation_large"),
    "3w": ("potion_health_super", "potion_mana_super", "potion_rejuvenation_super"),
}

_LOOT_BY_TIER = {
    "1w": {"wpn": "68-92", "amr": "155-265", "rare_wpn": "85-115", "rare_amr": "225-350"},
    "2w": {"wpn": "175-245", "amr": "365-575", "rare_wpn": "225-285", "rare_amr": "485-680"},
    "3w": {"wpn": "255-350", "amr": "535-765", "rare_wpn": "305-395", "rare_amr": "675-895"},
}

# Gold drops keyed by (tier, bucket) -> (chance, min, max, piles_range_or_None).
# Calibrated against Volcanic Caverns (`hades`: 18.5k-37.8k trash, 58k-71k bosses in 1W;
# 111k-293k / 380k-429k in 2W; 335k-890k / 1.16M-1.30M in 3W) so post-Hades LoA regions
# never regress on either visible pile size or expected gold per kill.
_ENEMY_GOLD_TABLE: Dict[Tuple[str, str], Tuple[str, int, int, Optional[str]]] = {
    ("1w", "swarm"):      ("0.18", 10000,   16000,   None),
    ("1w", "core_lo"):    ("0.20", 22000,   30000,   None),
    ("1w", "core_mid"):   ("0.22", 26000,   36000,   None),
    ("1w", "elite"):      ("0.25", 35000,   48000,   None),
    ("1w", "boss"):       ("1.0",  75000,   95000,   "4 - 6"),
    ("1w", "final_boss"): ("1.0",  110000,  150000,  "6 - 8"),

    ("2w", "swarm"):      ("0.18", 95000,   135000,  None),
    ("2w", "core_lo"):    ("0.20", 145000,  215000,  None),
    ("2w", "core_mid"):   ("0.22", 175000,  255000,  None),
    ("2w", "elite"):      ("0.25", 230000,  320000,  None),
    ("2w", "boss"):       ("1.0",  430000,  540000,  "4 - 6"),
    ("2w", "final_boss"): ("1.0",  650000,  850000,  "6 - 8"),

    ("3w", "swarm"):      ("0.18", 300000,  440000,  None),
    ("3w", "core_lo"):    ("0.20", 440000,  650000,  None),
    ("3w", "core_mid"):   ("0.22", 520000,  760000,  None),
    ("3w", "elite"):      ("0.25", 680000,  950000,  None),
    ("3w", "boss"):       ("1.0",  1250000, 1550000, "4 - 6"),
    ("3w", "final_boss"): ("1.0",  1850000, 2400000, "6 - 8"),
}


def _resolve_enemy_gold(tier: str, role: str, act_band: str) -> Tuple[str, int, int, Optional[str]]:
    """Return `(chance, min, max, piles)` gold tuning for an enemy role and progression band."""
    if role in ("swarm", "bomb"):
        bucket = "swarm"
    elif role == "core":
        bucket = "core_lo" if act_band == "act8_lo" else "core_mid"
    else:
        bucket = role
    return _ENEMY_GOLD_TABLE[(tier, bucket)]


def _resolve_enemy_base_stats(vfs: TemplateVFS, tpl_name: str) -> Dict[str, float]:
    def f(attr: str, default: float = 0.0) -> float:
        raw = vfs.find_attr(tpl_name, attr)
        if raw is None:
            return default
        num = raw.split(",")[0].strip()
        try:
            return float(num)
        except ValueError:
            return default

    return {
        "xp": f("experience_value", 500.0),
        "hp": f("max_life", f("life", 200.0)),
        "mana": f("max_mana", f("mana", 0.0)),
        "def": f("defense", 100.0),
        "dmg_min": f("damage_min", 50.0),
        "dmg_max": f("damage_max", 70.0),
        "strength": f("strength", 15.0),
        "intelligence": f("intelligence", 10.0),
        "dexterity": f("dexterity", 12.0),
        "melee": f("melee", 0.0),
        "ranged": f("ranged", 0.0),
        "combat_magic": f("combat_magic", 0.0),
        "nature_magic": f("nature_magic", 0.0),
    }


def compute_scaled_stats(vfs: TemplateVFS, stem: str) -> Dict[str, Dict[str, float]]:
    """Compute monotonic 1W < 2W < 3W scaled stats for one enemy stem."""
    role = ROLE_OVERRIDES.get(stem, "core")
    raw_by_tier = {t: _resolve_enemy_base_stats(vfs, pfx.lower() + stem) for t, pfx, _ in TIERS}

    # Fix vanilla dsx_troll_mountain bug where 2W/3W left defense at 164
    for t_cur, t_prev, ratio in (("2w", "1w", 2.9), ("3w", "2w", 1.42)):
        if raw_by_tier[t_cur]["def"] <= raw_by_tier[t_prev]["def"] + 1.0:
            raw_by_tier[t_cur]["def"] = raw_by_tier[t_prev]["def"] * ratio

    scaled: Dict[str, Dict[str, float]] = {}
    prev: Optional[Dict[str, float]] = None
    for tier, _, _ in TIERS:
        p = _ROLE_PARAMS[(tier, role)]
        ov = _STEM_FLOOR_OVERRIDES.get((stem, tier), {})
        r = raw_by_tier[tier]
        act_xp_floor = _ACT9_CORE_XP_FLOORS[tier] if (role == "core" and (stem in _ACT9_MID_ENEMY_STEMS or stem in _ACT9_HI_ENEMY_STEMS)) else 0.0
        xp = max(r["xp"] * p["xp_mult"], p["xp_floor"], act_xp_floor, ov.get("xp", 0.0))
        hp = max(r["hp"] * p["hp_mult"], p["hp_floor"], ov.get("hp", 0.0))
        mana = max(r["mana"] * (1.25 if tier == "1w" else 1.20), r["mana"])
        df = max(r["def"] * p["def_mult"], p["def_floor"], ov.get("def", 0.0))
        dmin = max(r["dmg_min"] * p["dmg_mult"], p["dmg_min_floor"], ov.get("dmg_min", 0.0))
        dmax = max(r["dmg_max"] * p["dmg_mult"], p["dmg_max_floor"], ov.get("dmg_max", 0.0), dmin * 1.12)
        if stem.startswith("dsx_elemental_") and not stem.endswith("_minion") and tier == "3w":
            dmin = min(dmin, 1550.0)
            dmax = min(dmax, 2150.0)

        sk_floor = max(p["skill_floor"], ov.get("skill", 0.0))
        sk_boost = {"1w": 10.0, "2w": 11.0, "3w": 13.0}[tier]
        skills: Dict[str, float] = {}
        for sk in ("melee", "ranged", "combat_magic", "nature_magic"):
            if r[sk] > 0:
                skills[sk] = max(r[sk] + sk_boost, sk_floor)
        if not skills:
            skills["melee"] = sk_floor
        stat_scale = {"1w": 1.35, "2w": 1.25, "3w": 1.20}[tier]
        str_v = max(r["strength"] * stat_scale, sk_floor * 0.72)
        int_v = max(r["intelligence"] * stat_scale, sk_floor * 0.40)
        dex_v = max(r["dexterity"] * stat_scale, sk_floor * 0.65)

        if prev is not None:
            xp = max(xp, prev["xp"] * 1.5)
            hp = max(hp, prev["hp"] * 1.22)
            df = max(df, prev["def"] * 1.22)
            dmin = max(dmin, prev["dmg_min"] * 1.20)
            dmax = max(dmax, prev["dmg_max"] * 1.20, dmin * 1.1)
            for sk in skills:
                skills[sk] = max(skills[sk], prev.get(sk, sk_floor) + 8.0)
            str_v = max(str_v, prev["strength"] + 6.0)
            int_v = max(int_v, prev["intelligence"] + 3.0)
            dex_v = max(dex_v, prev["dexterity"] + 5.0)

        row = {
            "xp": round(xp),
            "hp": round(hp, 1),
            "mana": round(mana, 1),
            "def": round(df, 1),
            "dmg_min": round(dmin, 1),
            "dmg_max": round(dmax, 1),
            "strength": round(str_v, 1),
            "intelligence": round(int_v, 1),
            "dexterity": round(dex_v, 1),
            **{k: round(v) for k, v in skills.items()},
        }
        scaled[tier] = row
        prev = row
    return scaled


# Act 8 -> Act 9 additive Veteran (2W) drop bands for 1W monsters (post-Neener).
_ACT9_HI_ENEMY_STEMS = frozenset({
    "dsx_shadow_bigboss",
    "dsx_shadow_jumper_boss",
    "dsx_gobbot_general",
    "dsx_elemental_air",
    "dsx_elemental_earth",
    "dsx_elemental_fire",
    "dsx_elemental_fire_ranged",
    "dsx_elemental_water",
    "dsx_golem_clockwork",
    "dsx_shadow_jumper_minion_four",
    "dsx_shadow_jumper_minion_five",
    "dsx_troll_masher",
    "dsx_horrid",
})

_ACT9_MID_ENEMY_STEMS = frozenset({
    "dsx_automaton_flying_one",
    "dsx_automaton_flying_two",
    "dsx_automaton_one",
    "dsx_automaton_two",
    "dsx_blaster",
    "dsx_elemental_air_minion",
    "dsx_elemental_earth_minion",
    "dsx_elemental_water_minion",
    "dsx_gobbot_flamethrower_02",
    "dsx_gobbot_grenade_02",
    "dsx_gobbot_lightning_02",
    "dsx_gobbot_minigun_02",
    "dsx_goblin_blackguard",
    "dsx_goblin_guard",
    "dsx_goblin_mutant",
    "dsx_goblin_mutant_hurler",
    "dsx_perforator",
    "dsx_phrak_jungle",
    "dsx_phrak_piercer",
    "dsx_scorpiot_melee",
    "dsx_scorpiot_ranged",
    "dsx_troll_mountain",
    "gobbot_flamethrower",
    "gobbot_grenade",
    "gobbot_lightning",
    "heater",
    "perforator",
    "proxo",
    "dsx_nosirrom",
    "dsx_fury",
    "dsx_lava_spirit",
    "dsx_shard_blue",
    "dsx_shard_green",
    "dsx_shard_purple",
    "dsx_shard_teal",
})

_ENEMY_1W_VETERAN_DROPS: Dict[str, Dict[str, object]] = {
    "act8_lo": {
        "chance": ("0.14", "0.04", "0.015"),
        "norm": ("#weapon/185-250", "#armor/375-580", "#*/140-200", "#spell/51-78"),
        "rare": ("#weapon/-rare(1)/210-275", "#armor/-rare(1)/440-660", "#ring/-rare(1)/140-200", "#amulet/-rare(1)/140-200"),
        "uniq": ("#weapon/-unique(2)/215-285", "#armor/-unique(2)/460-680", "#*/-unique(2)/160-235"),
    },
    "act9_mid": {
        "chance": ("0.18", "0.06", "0.025"),
        "boss_chance": ("0.35", "0.20", "0.12"),
        "norm": ("#weapon/215-295", "#armor/460-720", "#body,ro/380-560", "#*/160-235", "#spell/62-92", "#spellbook/140-220"),
        "rare": ("#weapon/-rare(1)/235-315", "#armor/-rare(1)/530-810", "#ring/-rare(1)/160-235", "#amulet/-rare(1)/160-235"),
        "uniq": ("#weapon/-unique(2)/240-325", "#armor/-unique(2)/560-850", "#*/-unique(2)/175-245"),
    },
    "act9_hi": {
        "chance": ("0.26", "0.10", "0.05"),
        "boss_chance": ("0.55", "0.35", "0.25"),
        "norm": ("#weapon/235-355", "#armor/580-950", "#body,ro/465-975", "#*/175-250", "#spell/75-106", "#spellbook/160-250"),
        "rare": ("#weapon/-rare(1)/245-360", "#armor/-rare(1)/620-1077", "#ring/-rare(1)/180-250", "#amulet/-rare(1)/180-250"),
        "uniq": ("#weapon/-unique(2)/275-360", "#armor/-unique(2)/620-1077", "#*/-unique(2)/190-250"),
    },
}


def _enemy_1w_veteran_band(stem: str) -> str:
    """Select the post-Neener 2W loot progression band for a 1W enemy stem."""
    if stem in _ACT9_HI_ENEMY_STEMS:
        return "act9_hi"
    if stem in _ACT9_MID_ENEMY_STEMS:
        return "act9_mid"
    return "act8_lo"


def _format_oneof_drop_block(chance: str, il_mains: Tuple[str, ...]) -> str:
    lines = [
        "\t\t\t\t[oneof*]",
        "\t\t\t\t{",
        f"\t\t\t\t\tchance = {chance};",
        *(f"\t\t\t\t\til_main = {il};" for il in il_mains),
        "\t\t\t\t}\n",
    ]
    return "\n".join(lines)


def _build_pcontent_block(
    tier: str,
    is_boss: bool,
    es_lines: Tuple[str, ...] = (),
    vet_band: Optional[str] = None,
    role: str = "core",
    act_band: str = "act8_lo",
) -> str:
    lp = _LOOT_BY_TIER[tier]
    hp_pot, mp_pot, rj_pot = _POTIONS_BY_TIER[tier]
    g_chance, gmin, gmax, g_piles = _resolve_enemy_gold(tier, role, act_band)
    gear_chance = "0.60" if is_boss else "0.24"
    rare_chance = "0.35" if is_boss else "0.06"
    uniq_chance = "0.25" if is_boss else "0.02"
    es_section = ""
    if es_lines:
        body = "\n".join(f"\t\t\t\t{line}" for line in es_lines)
        es_section = f"\t\t\t[all*]\n\t\t\t{{\n{body}\n\t\t\t}}\n"

    vet_section = ""
    if tier == "1w" and vet_band:
        vd = _ENEMY_1W_VETERAN_DROPS[vet_band]
        ch_n, ch_r, ch_u = vd["boss_chance"] if (is_boss and "boss_chance" in vd) else vd["chance"]
        vet_section = (
            _format_oneof_drop_block(ch_n, vd["norm"])
            + _format_oneof_drop_block(ch_r, vd["rare"])
            + _format_oneof_drop_block(ch_u, vd["uniq"])
        )

    gold_extra = ""
    if g_piles:
        gold_extra = (
            f"\t\t\t\t\tmin_pile = {gmin // 10};\n"
            f"\t\t\t\t\tpiles = {g_piles};\n"
        )

    return (
        "\t\t[pcontent]\n"
        "\t\t{\n"
        + es_section
        + "\t\t\t[all*]\n"
        "\t\t\t{\n"
        "\t\t\t\t[gold*]\n"
        "\t\t\t\t{\n"
        f"\t\t\t\t\tchance = {g_chance};\n"
        f"\t\t\t\t\tmin = {gmin};\n"
        f"\t\t\t\t\tmax = {gmax};\n"
        + gold_extra
        + "\t\t\t\t}\n"
        "\t\t\t\t[oneof*]\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tchance = 0.22;\n"
        f"\t\t\t\t\til_main = {mp_pot};\n"
        f"\t\t\t\t\til_main = {hp_pot};\n"
        f"\t\t\t\t\til_main = {rj_pot};\n"
        "\t\t\t\t}\n"
        "\t\t\t\t[oneof*]\n"
        "\t\t\t\t{\n"
        f"\t\t\t\t\tchance = {gear_chance};\n"
        f"\t\t\t\t\til_main = #weapon/{lp['wpn']};\n"
        f"\t\t\t\t\til_main = #armor/{lp['amr']};\n"
        f"\t\t\t\t\til_main = #*/{lp['wpn']};\n"
        "\t\t\t\t}\n"
        "\t\t\t\t[oneof*]\n"
        "\t\t\t\t{\n"
        f"\t\t\t\t\tchance = {rare_chance};\n"
        f"\t\t\t\t\til_main = #weapon/-rare(1)/{lp['rare_wpn']};\n"
        f"\t\t\t\t\til_main = #armor/-rare(1)/{lp['rare_amr']};\n"
        f"\t\t\t\t\til_main = #*/-rare(1)/{lp['rare_wpn']};\n"
        "\t\t\t\t}\n"
        "\t\t\t\t[oneof*]\n"
        "\t\t\t\t{\n"
        f"\t\t\t\t\tchance = {uniq_chance};\n"
        f"\t\t\t\t\til_main = #weapon/-unique(2)/{lp['rare_wpn']};\n"
        f"\t\t\t\t\til_main = #armor/-unique(2)/{lp['rare_amr']};\n"
        f"\t\t\t\t\til_main = #*/-unique(2)/{lp['rare_wpn']};\n"
        "\t\t\t\t}\n"
        + vet_section
        + "\t\t\t}\n"
        "\t\t}"
    )


_SOURCE_STEM_OVERRIDE: Dict[str, str] = {
    "rock_beast": "rock_beast_base",
    "dsx_slithermage_ranged": "dsx_slithermage_ranged_base",
}


def _emit_troll_base_template(vfs: TemplateVFS) -> str:
    blk = vfs.get_block("dsx_troll")
    out = re.sub(r"^\[t:template,n:[^\]]+\]", "[t:template,n:mmo_loa_troll_base]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", '"mmo_loa_troll_base"', "\t")
    inv_hits = _extract_top_subblocks(out, "inventory")
    if inv_hits:
        s, e, inv_blk = inv_hits[-1]
        inv_blk = _remove_subblocks(inv_blk, "pcontent", top_level=True)
        out = out[:s] + inv_blk + out[e:]
    return out


def _emit_enemy_template(
    vfs: TemplateVFS,
    stem: str,
    tier: str,
    pfx: str,
    stats: Dict[str, float],
    monster_weapons: Dict[str, str],
    combat_spells: Dict[str, str],
) -> str:
    vanilla = pfx.lower() + stem
    src_stem = _SOURCE_STEM_OVERRIDE.get(stem, stem)
    src_name = pfx.lower() + src_stem
    blk = vfs.get_block(src_name)
    new_name = mmo_name(vanilla)
    out = re.sub(
        r"^\[t:template,n:[^\]]+\]",
        f"[t:template,n:{new_name}]",
        blk.strip(),
        count=1,
    )
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")
    if stem == "dsx_troll_mountain":
        out = _set_scalar_in_block(out, "specializes", "mmo_loa_troll_base", "\t")

    skill_lines = [
        f"\t\t\tstrength = {_fmt_num(stats['strength'])}, 0;",
        f"\t\t\tintelligence = {_fmt_num(stats['intelligence'])}, 0;",
        f"\t\t\tdexterity = {_fmt_num(stats['dexterity'])}, 0;",
    ]
    for sk in ("melee", "ranged", "nature_magic", "combat_magic"):
        if sk in stats:
            skill_lines.append(f"\t\t\t{sk} = {int(stats[sk])}, 0;")
    skills_sub = "\t\t[skills]\n\t\t{\n" + "\n".join(skill_lines) + "\n\t\t}"
    act_found = _get_subblock(out, "actor", top_level=True)
    if act_found:
        act_blk = _upsert_subblock(act_found[2], "skills", skills_sub, top_level=True)
        out = _upsert_subblock(out, "actor", act_blk, top_level=True)
    else:
        out = _upsert_subblock(out, "actor", "\t[actor]\n\t{\n" + skills_sub + "\n\t}", top_level=True)

    asp_found = _get_subblock(out, "aspect", top_level=True)
    asp_blk = asp_found[2] if asp_found else "\t[aspect]\n\t{\n\t}"
    asp_blk = _set_scalar_in_block(asp_blk, "experience_value", str(int(stats["xp"])))
    asp_blk = _set_scalar_in_block(asp_blk, "life", _fmt_num(stats["hp"]))
    asp_blk = _set_scalar_in_block(asp_blk, "max_life", _fmt_num(stats["hp"]))
    if stats["mana"] > 0:
        asp_blk = _set_scalar_in_block(asp_blk, "mana", _fmt_num(stats["mana"]))
        asp_blk = _set_scalar_in_block(asp_blk, "max_mana", _fmt_num(stats["mana"]))
    out = _upsert_subblock(out, "aspect", asp_blk, top_level=True)

    atk_found = _get_subblock(out, "attack", top_level=True)
    if atk_found:
        atk_blk = _set_scalar_in_block(atk_found[2], "damage_min", _fmt_num(stats["dmg_min"]))
        atk_blk = _set_scalar_in_block(atk_blk, "damage_max", _fmt_num(stats["dmg_max"]))
        out = _upsert_subblock(out, "attack", atk_blk, top_level=True)
    else:
        out = _upsert_subblock(
            out,
            "attack",
            f"\t[attack]\n\t{{\n\t\tdamage_max = {_fmt_num(stats['dmg_max'])};\n\t\tdamage_min = {_fmt_num(stats['dmg_min'])};\n\t}}",
            top_level=True,
        )

    def_found = _get_subblock(out, "defend", top_level=True)
    def_blk = def_found[2] if def_found else "\t[defend]\n\t{\n\t}"
    def_blk = _set_scalar_in_block(def_blk, "defense", _fmt_num(stats["def"]))
    out = _upsert_subblock(out, "defend", def_blk, top_level=True)

    def _remap_weapon_line(m: re.Match) -> str:
        wpn = m.group(2).strip().strip('"').lower()
        if wpn in monster_weapons or wpn == "st_g_c_dsx_stars":
            return f"{m.group(1)}{mmo_name(pfx.lower() + wpn)};"
        return m.group(0)

    def _remap_spell_line(m: re.Match) -> str:
        spl = m.group(2).strip().strip('"').lower()
        if spl in combat_spells:
            return f"{m.group(1)}{mmo_name(pfx.lower() + spl)};"
        return m.group(0)

    def _remap_spell_list(m: re.Match) -> str:
        items = [x.strip() for x in m.group(2).split(",") if x.strip()]
        remapped = [
            mmo_name(pfx.lower() + x.lower()) if x.lower() in combat_spells else x
            for x in items
        ]
        return f'{m.group(1)}"{",".join(remapped)}"'

    role = ROLE_OVERRIDES.get(stem, "core")
    is_boss = role in ("boss", "final_boss")

    # Gather es_* slots from vanilla [pcontent] across all [inventory] blocks before stripping old [pcontent].
    es_seen: List[str] = []
    inv_hits = _extract_top_subblocks(out, "inventory")
    if stem != "dsx_shadow_bigboss":
        for _, _, inv_sub in inv_hits:
            for _, _, pc_sub in _extract_top_subblocks(inv_sub, "pcontent"):
                for em in re.finditer(r"(?mi)^[ \t]*(es_[a-z0-9_]+\s*=\s*[^;\r\n]+;)", pc_sub):
                    line = re.sub(r"\s+", " ", em.group(1).strip())
                    line = re.sub(r"(?i)^(es_weapon_hand\s*=\s*)([^;\r\n]+);", _remap_weapon_line, line)
                    if line.lower() not in {x.lower() for x in es_seen}:
                        es_seen.append(line)

    # Strip old [pcontent] from all existing [inventory] blocks first so multi-[inventory] templates don't leak loot.
    if inv_hits:
        rebuilt = out
        for idx in range(len(inv_hits) - 1, -1, -1):
            s, e, inv_sub = inv_hits[idx]
            cleaned = _remove_subblocks(inv_sub, "pcontent", top_level=True)
            rebuilt = rebuilt[:s] + cleaned + rebuilt[e:]
        out = rebuilt

    act_band = _enemy_1w_veteran_band(stem)
    vet_band = act_band if tier == "1w" else None
    pcontent_sub = _build_pcontent_block(
        tier,
        is_boss,
        tuple(es_seen),
        vet_band=vet_band,
        role=role,
        act_band=act_band,
    )
    inv_found = _get_subblock(out, "inventory", top_level=True)
    if inv_found:
        inv_blk = inv_found[2]
        if stem == "dsx_shadow_bigboss":
            inv_blk = _set_scalar_in_block(inv_blk, "es_weapon_hand", mmo_name(pfx.lower() + "st_g_c_dsx_stars"), "\t\t\t")
            inv_blk = _set_scalar_in_block(inv_blk, "spew_equipped_kill_count", "", "\t\t")
        else:
            inv_blk = re.sub(
                r"(?mi)^([ \t]*es_weapon_hand\s*=\s*)([^;\r\n]+);",
                _remap_weapon_line,
                inv_blk,
            )
        inv_blk = _upsert_subblock(inv_blk, "pcontent", pcontent_sub, top_level=True)
        if stem != "dsx_shadow_bigboss" and re.search(r"(?i)\bes_weapon_hand\s*=", inv_blk):
            inv_blk = _set_scalar_in_block(inv_blk, "spew_equipped_kill_count", "<ignore>", "\t\t")
        out = _upsert_subblock(out, "inventory", inv_blk, top_level=True)
    else:
        out = _upsert_subblock(out, "inventory", "\t[inventory]\n\t{\n" + pcontent_sub + "\n\t}", top_level=True)

    out = re.sub(
        r"(?mi)^([ \t]*(?:il_active_primary_spell|il_spell_\d+)\s*=\s*)([^;\r\n]+);",
        _remap_spell_line,
        out,
    )
    out = re.sub(r'(?mi)([ \t]*&spells\s*=\s*)"([^"]+)"', _remap_spell_list, out)

    return out


def _emit_darkgen_template(vfs: TemplateVFS, stem: str, tier: str, pfx: str) -> str:
    vanilla = pfx.lower() + stem
    blk = vfs.get_block(vanilla)
    new_name = mmo_name(vanilla)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")
    hp_by_tier = {
        "1w": {"dsx_darkgenerator_1": 1600, "dsx_darkgenerator_2": 1800, "dsx_darkgenerator_clockroom": 1650},
        "2w": {"dsx_darkgenerator_1": 3600, "dsx_darkgenerator_2": 4000, "dsx_darkgenerator_clockroom": 3800},
        "3w": {"dsx_darkgenerator_1": 5200, "dsx_darkgenerator_2": 5800, "dsx_darkgenerator_clockroom": 5500},
    }[tier][stem]
    asp_found = _get_subblock(out, "aspect", top_level=True)
    if asp_found:
        asp_blk = _set_scalar_in_block(asp_found[2], "life", str(hp_by_tier))
        asp_blk = _set_scalar_in_block(asp_blk, "max_life", str(hp_by_tier))
        out = _upsert_subblock(out, "aspect", asp_blk, top_level=True)
    if stem == "dsx_darkgenerator_clockroom":
        out = re.sub(
            r"(creatureTemplate\s*=\s*\")([^\"]+)(\")",
            lambda m: m.group(1) + mmo_name(m.group(2)) + m.group(3),
            out,
            flags=re.I,
        )
    return out


def _emit_generator_template(vfs: TemplateVFS, stem: str, pfx: str) -> str:
    vanilla = (pfx.lower() + stem) if (pfx.lower() + stem) in vfs.blocks else stem
    blk = vfs.get_block(vanilla)
    new_name = mmo_name(pfx.lower() + stem)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")

    def repl_child(m: re.Match) -> str:
        raw = m.group(2).strip().strip('"').lower()
        target = raw
        if pfx and (pfx.lower() + raw) in TEMPLATE_REMAP:
            target = TEMPLATE_REMAP[pfx.lower() + raw]
        elif raw in TEMPLATE_REMAP:
            target = TEMPLATE_REMAP[raw]
        return f"{m.group(1)}{target};"

    out = re.sub(r"(child_template_name\s*=\s*)([^;\r\n]+);", repl_child, out, flags=re.I)
    return out


def _emit_stars_templates(vfs: TemplateVFS) -> Dict[str, str]:
    """Emit fixed Staff of Stars base + 1W/2W/3W variants without dsx_end_game victory trigger."""
    base_blk = vfs.get_block("st_g_c_dsx_stars_base")
    base_blk = re.sub(
        r"^\[t:template,n:[^\]]+\]",
        "[t:template,n:mmo_loa_st_stars_base]",
        base_blk.strip(),
        count=1,
    )
    it_found = _get_subblock(base_blk, "instance_triggers")
    if it_found:
        s, e, _ = it_found
        base_blk = base_blk[:s] + base_blk[e:]
    base_blk = _set_scalar_in_block(base_blk, "doc", '"MMO LoA Staff of Stars Base"', "\t")

    cfg = {
        "1w": ("mmo_loa_st_stars", "Staff of Stars", 225.0, 265.0, 115, 5, 35, 130),
        "2w": ("2W_mmo_loa_st_stars", "Super Staff of Stars", 315.0, 410.0, 280, 7, 90, 360),
        "3w": ("3W_mmo_loa_st_stars", "Supreme Staff of Stars", 395.0, 510.0, 460, 10, 145, 520),
    }
    by_tier = {"regular": [base_blk], "veteran": [], "elite": []}
    for tier, _, folder in TIERS:
        v_blk = vfs.get_block(f"{'2w_' if tier=='2w' else ('3w_' if tier=='3w' else '')}st_g_c_dsx_stars")
        nname, sname, dmin, dmax, hp_b, int_b, arm_b, mp_b = cfg[tier]
        out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{nname}]", v_blk.strip(), count=1)
        out = _set_scalar_in_block(out, "specializes", "mmo_loa_st_stars_base", "\t")
        out = _set_scalar_in_block(out, "doc", f'"{nname}"', "\t")
        out = _set_scalar_in_block(out, "screen_name", f'"{sname}"')
        out = _set_scalar_in_block(out, "damage_min", f"{dmin:.1f}")
        out = _set_scalar_in_block(out, "damage_max", f"{dmax:.1f}")
        for enc_name, val, desc in (
            ("health", hp_b, f"Adds {hp_b} to Health"),
            ("intelligence", int_b, f"Adds {int_b} to Intelligence"),
            ("armor", arm_b, f"Adds {arm_b} to Armor"),
            ("mana", mp_b, f"Adds {mp_b} to Mana"),
        ):
            sub_f = _get_subblock(out, enc_name)
            if sub_f:
                sb = _set_scalar_in_block(sub_f[2], "value", str(val))
                sb = _set_scalar_in_block(sb, "description", f'"{desc}"')
                out = _upsert_subblock(out, enc_name, sb)
        by_tier[folder].append(out)
    return {k: "\n\n".join(v) + "\n" for k, v in by_tier.items()}


_ROLE_PRIORITY: Dict[str, int] = {
    "swarm": 0,
    "bomb": 1,
    "core": 2,
    "elite": 3,
    "boss": 4,
    "final_boss": 5,
}

# (mult_over_base_or_prev, dmg_min_floor, dmg_max_floor) keyed by (tier, role).
_WEAPON_ROLE_SCALING: Dict[Tuple[str, str], Tuple[float, float, float]] = {
    ("1w", "swarm"): (1.45, 95.0, 115.0),
    ("2w", "swarm"): (1.48, 185.0, 235.0),
    ("3w", "swarm"): (1.38, 270.0, 345.0),
    ("1w", "bomb"): (1.45, 110.0, 130.0),
    ("2w", "bomb"): (1.48, 210.0, 265.0),
    ("3w", "bomb"): (1.38, 300.0, 385.0),
    ("1w", "core"): (1.40, 125.0, 150.0),
    ("2w", "core"): (1.48, 245.0, 315.0),
    ("3w", "core"): (1.38, 355.0, 460.0),
    ("1w", "elite"): (1.40, 155.0, 185.0),
    ("2w", "elite"): (1.48, 295.0, 375.0),
    ("3w", "elite"): (1.38, 420.0, 540.0),
    ("1w", "boss"): (1.55, 195.0, 235.0),
    ("2w", "boss"): (1.48, 350.0, 445.0),
    ("3w", "boss"): (1.38, 495.0, 635.0),
    ("1w", "final_boss"): (1.55, 225.0, 265.0),
    ("2w", "final_boss"): (1.48, 350.0, 445.0),
    ("3w", "final_boss"): (1.38, 495.0, 635.0),
}

_SPELL_ROLE_SCALING: Dict[Tuple[str, str], Tuple[float, float, float]] = {
    ("1w", "swarm"): (1.45, 105.0, 135.0),
    ("2w", "swarm"): (1.80, 205.0, 265.0),
    ("3w", "swarm"): (1.40, 295.0, 380.0),
    ("1w", "bomb"): (1.50, 140.0, 175.0),
    ("2w", "bomb"): (1.85, 275.0, 350.0),
    ("3w", "bomb"): (1.40, 390.0, 495.0),
    ("1w", "core"): (1.50, 140.0, 175.0),
    ("2w", "core"): (1.85, 275.0, 350.0),
    ("3w", "core"): (1.40, 390.0, 495.0),
    ("1w", "elite"): (1.55, 180.0, 220.0),
    ("2w", "elite"): (1.90, 355.0, 445.0),
    ("3w", "elite"): (1.40, 500.0, 630.0),
    ("1w", "boss"): (1.55, 215.0, 260.0),
    ("2w", "boss"): (1.85, 410.0, 515.0),
    ("3w", "boss"): (1.40, 580.0, 730.0),
    ("1w", "final_boss"): (1.75, 180.0, 230.0),
    ("2w", "final_boss"): (1.90, 360.0, 460.0),
    ("3w", "final_boss"): (1.42, 520.0, 660.0),
}

_CHUBBY_RAIN_SCALE: Dict[str, Tuple[float, float]] = {
    "1w": (85.0, 115.0),
    "2w": (165.0, 220.0),
    "3w": (235.0, 315.0),
}

# Preserve high-damage elemental blasts in 1W (zero nerf) without inflating 2W/3W disproportionately.
_ELEMENTAL_HIGH_SPELLS = frozenset({"dsx_spell_air_elemental_blast", "dsx_elemental_fire_blast"})


def _collect_enemy_equipment_roles(vfs: TemplateVFS) -> Tuple[Dict[str, str], Dict[str, str]]:
    """Collect `(monster_weapons, combat_spells)` mapped to each item's highest-priority user role."""
    weapon_roles: Dict[str, str] = {}
    spell_roles: Dict[str, str] = {}

    def _record(mapping: Dict[str, str], name: str, role: str) -> None:
        cur = mapping.get(name)
        if cur is None or _ROLE_PRIORITY[role] > _ROLE_PRIORITY[cur]:
            mapping[name] = role

    for stem in ENEMY_STEMS:
        role = ROLE_OVERRIDES.get(stem, "core")
        src_stem = _SOURCE_STEM_OVERRIDE.get(stem, stem)
        blk = vfs.get_block(src_stem)

        for wm in re.finditer(r"(?mi)es_weapon_hand\s*=\s*([^;\r\n]+);", blk):
            wpn = wm.group(1).strip().strip('"').lower()
            if wpn and wpn != "st_g_c_dsx_stars":
                _record(weapon_roles, wpn, role)

        cand_spells: List[str] = []
        for sm in re.finditer(r"(?mi)(?:il_active_primary_spell|il_spell_\d+)\s*=\s*([^;\r\n]+);", blk):
            cand_spells.append(sm.group(1).strip().strip('"').lower())
        lm = re.search(r'(?mi)&spells\s*=\s*"([^"]+)"', blk)
        if lm:
            cand_spells.extend(x.strip().lower() for x in lm.group(1).split(",") if x.strip())

        for spl in cand_spells:
            if spl in vfs.blocks and vfs.find_attr(spl, "damage_min") is not None:
                _record(spell_roles, spl, role)

    return weapon_roles, spell_roles


def _scale_equipment_damage(
    vfs: TemplateVFS,
    tpl_name: str,
    role: str,
    table: Dict[Tuple[str, str], Tuple[float, float, float]],
    is_spell: bool = False,
) -> Dict[str, Tuple[float, float]]:
    """Compute monotonic 1W < 2W < 3W attack damage bounds for a monster weapon or combat spell."""
    raw_min = float((vfs.find_attr(tpl_name, "damage_min") or "50").split(",")[0].strip())
    raw_max = float((vfs.find_attr(tpl_name, "damage_max") or str(raw_min * 1.15)).split(",")[0].strip())
    out: Dict[str, Tuple[float, float]] = {}

    if is_spell and tpl_name == "spell_sj_chubby_rain":
        return dict(_CHUBBY_RAIN_SCALE)

    if is_spell and tpl_name in _ELEMENTAL_HIGH_SPELLS:
        p1_min = max(round(raw_min * 1.05, 1), raw_min)
        p1_max = max(round(raw_max * 1.05, 1), raw_max, round(p1_min * 1.10, 1))
        p2_min = round(p1_min * 1.55, 1)
        p2_max = round(p1_max * 1.55, 1)
        p3_min = round(p2_min * 1.35, 1)
        p3_max = round(p2_max * 1.35, 1)
        return {"1w": (p1_min, p1_max), "2w": (p2_min, p2_max), "3w": (p3_min, p3_max)}

    prev_min = raw_min
    prev_max = raw_max
    for tier, _, _ in TIERS:
        mult, fl_min, fl_max = table[(tier, role)]
        if tier == "1w":
            dmin = max(raw_min * mult, fl_min, raw_min)
            dmax = max(raw_max * mult, fl_max, raw_max, dmin * 1.12)
        else:
            dmin = max(prev_min * mult, fl_min, prev_min * 1.25)
            dmax = max(prev_max * mult, fl_max, prev_max * 1.25, dmin * 1.18)
        dmin = round(dmin, 1)
        dmax = round(dmax, 1)
        out[tier] = (dmin, dmax)
        prev_min, prev_max = dmin, dmax
    return out


def _clone_monster_weapon_block(vfs: TemplateVFS, wpn: str, tier: str, pfx: str, dmin: float, dmax: float) -> str:
    """Emit a scaled, non-droppable, non-pcontent clone of a monster-wielded `es_weapon_hand`."""
    blk = vfs.get_block(wpn)
    new_name = mmo_name(pfx.lower() + wpn)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")
    out = re.sub(r"(?mi)^[ \t]*(?:gui:)?equip_requirements\s*=[^;\r\n]*;\r?\n?", "", out)

    atk_found = _get_subblock(out, "attack", top_level=True)
    if atk_found:
        atk_blk = _set_scalar_in_block(atk_found[2], "damage_min", f"{dmin:.1f}")
        atk_blk = _set_scalar_in_block(atk_blk, "damage_max", f"{dmax:.1f}")
        out = _upsert_subblock(out, "attack", atk_blk, top_level=True)
    else:
        out = _upsert_subblock(
            out,
            "attack",
            f"\t[attack]\n\t{{\n\t\tdamage_max = {dmax:.1f};\n\t\tdamage_min = {dmin:.1f};\n\t}}",
            top_level=True,
        )

    cmn_found = _get_subblock(out, "common", top_level=True)
    if cmn_found:
        out = _upsert_subblock(out, "common", _set_scalar_in_block(cmn_found[2], "is_pcontent_allowed", "false"), top_level=True)
    else:
        out = _set_scalar_in_block(out, "common:is_pcontent_allowed", "false", "\t")

    gui_found = _get_subblock(out, "gui", top_level=True)
    if gui_found:
        out = _upsert_subblock(out, "gui", _set_scalar_in_block(gui_found[2], "is_droppable", "false"), top_level=True)
    else:
        out = _set_scalar_in_block(out, "gui:is_droppable", "false", "\t")

    return out


def _clone_monster_spell_block(vfs: TemplateVFS, spl: str, tier: str, pfx: str, dmin: float, dmax: float) -> str:
    """Emit a scaled clone of a monster combat spell template."""
    blk = vfs.get_block(spl)
    new_name = mmo_name(pfx.lower() + spl)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")
    atk_found = _get_subblock(out, "attack", top_level=True)
    if atk_found:
        atk_blk = _set_scalar_in_block(atk_found[2], "damage_min", f"{dmin:.1f}")
        atk_blk = _set_scalar_in_block(atk_blk, "damage_max", f"{dmax:.1f}")
        out = _upsert_subblock(out, "attack", atk_blk, top_level=True)
    return out


def _emit_monster_weapons_by_folder(vfs: TemplateVFS, weapon_roles: Dict[str, str]) -> Dict[str, List[str]]:
    by_folder: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}
    for wpn in sorted(weapon_roles):
        bounds = _scale_equipment_damage(vfs, wpn, weapon_roles[wpn], _WEAPON_ROLE_SCALING, is_spell=False)
        for tier, pfx, folder in TIERS:
            dmin, dmax = bounds[tier]
            by_folder[folder].append(_clone_monster_weapon_block(vfs, wpn, tier, pfx, dmin, dmax))
    return by_folder


def _emit_monster_spells_by_folder(vfs: TemplateVFS, spell_roles: Dict[str, str]) -> Dict[str, List[str]]:
    by_folder: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}
    for spl in sorted(spell_roles):
        bounds = _scale_equipment_damage(vfs, spl, spell_roles[spl], _SPELL_ROLE_SCALING, is_spell=True)
        for tier, pfx, folder in TIERS:
            dmin, dmax = bounds[tier]
            by_folder[folder].append(_clone_monster_spell_block(vfs, spl, tier, pfx, dmin, dmax))
    return by_folder


def _upgrade_range_token(expr: str, scale_map: Dict[str, Tuple[int, int]]) -> str:
    """Upgrade a pcontent `il_main` expression safely inside verified VFS power bounds."""
    low = expr.strip().lower()
    if low == "#spellbook/" or low.startswith("#spellbook/"):
        lo, hi = scale_map["spellbook"]
        return f"#spellbook/{lo}-{hi}"
    m = re.match(r"^([^/]+(?:/[^0-9/][^/]*)*/)(\d+)-(\d+)$", expr.strip(), re.I)
    if not m:
        return expr
    head = m.group(1)
    hl = head.lower()
    if "#weapon" in hl or "#melee" in hl or "#ranged" in hl or "#staff" in hl:
        lo, hi = scale_map["rare_wpn"] if ("-rare" in hl or "-unique" in hl) else scale_map["wpn"]
    elif "#armor" in hl or "#body" in hl or "#helm" in hl or "#gloves" in hl or "#boots" in hl or "#shield" in hl:
        lo, hi = scale_map["rare_amr"] if ("-rare" in hl or "-unique" in hl) else scale_map["amr"]
    elif "#ring" in hl or "#amulet" in hl or "#*/" in hl:
        lo, hi = scale_map["rare_acc"] if ("-rare" in hl or "-unique" in hl) else scale_map["acc"]
    elif "#spell" in hl:
        lo, hi = scale_map["spell"]
    else:
        return expr
    return f"{head}{lo}-{hi}"


_CONTAINER_RANGES = {
    "1w": {"wpn": (70, 96), "rare_wpn": (88, 118), "amr": (175, 290), "rare_amr": (235, 360), "acc": (70, 96), "rare_acc": (88, 118), "spell": (28, 44), "spellbook": (70, 140)},
    "2w": {"wpn": (190, 260), "rare_wpn": (235, 305), "amr": (385, 620), "rare_amr": (510, 720), "acc": (190, 260), "rare_acc": (235, 305), "spell": (58, 86), "spellbook": (120, 200)},
    "3w": {"wpn": (285, 390), "rare_wpn": (325, 425), "amr": (560, 980), "rare_amr": (720, 1150), "acc": (285, 390), "rare_acc": (325, 425), "spell": (82, 108), "spellbook": (160, 240)},
}

# Additive Veteran (2W) ranges injected into 1W containers (post-Neener Act 8 -> Act 9)
# alongside existing 1W loot lines, never replacing them.
_CONTAINER_1W_VETERAN_BANDS: Dict[str, Dict[str, Tuple[int, int]]] = {
    "act8": {
        "wpn": (190, 265), "rare_wpn": (225, 295),
        "amr": (385, 620), "rare_amr": (485, 740),
        "acc": (130, 195), "rare_acc": (150, 225),
        "spell": (51, 82), "spellbook": (130, 210),
    },
    "act9": {
        "wpn": (235, 335), "rare_wpn": (245, 360),
        "amr": (480, 780), "rare_amr": (620, 1077),
        "acc": (165, 245), "rare_acc": (185, 250),
        "spell": (66, 106), "spellbook": (160, 250),
    },
}


def _veteran_container_companion(expr: str, vet_cfg: Dict[str, Tuple[int, int]]) -> Optional[str]:
    """Return an additive 2W pcontent expression for a container `il_main`, or None."""
    s = expr.strip()
    sl = s.lower()
    if not sl.startswith("#"):
        return None
    if sl.startswith("#book_glb_magic_"):
        lo, hi = vet_cfg["spell"]
        return f"#spell/{lo}-{hi}"
    if sl == "#spellbook/" or sl.startswith("#spellbook/"):
        lo, hi = vet_cfg["spellbook"]
        return f"#spellbook/{lo}-{hi}"

    # Match `#cat[,sub][/qual]/<lo>-<hi>[/<tail>]` including Elemental chests.
    m = re.match(r"^([^/]+(?:/[^0-9/][^/]*)*/)(\d+)-(\d+)(/.*)?$", s, re.I)
    if not m:
        return None
    head, _, _, tail = m.group(1), m.group(2), m.group(3), (m.group(4) or "")
    hl = head.lower()
    tl = tail.lower()
    is_rare = "-rare" in hl or "-unique" in hl or "-rare" in tl or "-unique" in tl or "+of" in tl

    if "#weapon" in hl or "#melee" in hl or "#ranged" in hl or "#staff" in hl:
        if "#staff" in hl:
            lo, hi = (200, 310) if is_rare else (185, 335)
        elif "#ranged" in hl:
            lo, hi = (195, 255)
        else:
            lo, hi = vet_cfg["rare_wpn"] if is_rare else vet_cfg["wpn"]
        return f"{head}{lo}-{hi}{tail}"

    if "#armor" in hl or "#body" in hl or "#helm" in hl or "#gloves" in hl or "#boots" in hl or "#shield" in hl:
        lo, hi = vet_cfg["rare_amr"] if is_rare else vet_cfg["amr"]
        # Replace fixed +of... suffixes (e.g. a9_r3-spec_earth) with 2W unique roll.
        if tl.startswith("/+"):
            tail = "/-unique(2)"
        return f"{head}{lo}-{hi}{tail}"

    if "#ring" in hl or "#amulet" in hl or "#*/" in hl:
        lo, hi = vet_cfg["rare_acc"] if is_rare else vet_cfg["acc"]
        return f"{head}{lo}-{hi}{tail}"

    if "#spell" in hl:
        lo, hi = vet_cfg["spell"]
        return f"{head}{lo}-{hi}{tail}"

    return None


_CONTAINER_GOLD_SCALING: Dict[Tuple[str, bool], Tuple[float, int]] = {
    ("1w", False): (11.0, 13500),
    ("2w", False): (72.0, 72000),
    ("3w", False): (230.0, 240000),
    ("1w", True):  (15.0, 28000),
    ("2w", True):  (95.0, 165000),
    ("3w", True):  (300.0, 500000),
}


def _extract_gold_bounds(blk: str) -> Optional[Tuple[int, int]]:
    """Return `(min, max)` from the `[gold*]` block in `blk`, if present."""
    g_sub = _get_subblock(blk, "gold*")
    if not g_sub:
        return None
    m_min = re.search(r"min\s*=\s*(\d+)", g_sub[2], re.I)
    m_max = re.search(r"max\s*=\s*(\d+)", g_sub[2], re.I)
    if not (m_min and m_max):
        return None
    return int(m_min.group(1)), int(m_max.group(1))


def _set_scalar_preserving_closing_indent(sub: str, key: str, value: str, indent: str) -> str:
    """Update or insert a GAS scalar while preserving the closing brace indentation."""
    rx = re.compile(rf"(?mi)((?:^|[{{;])[ \t]*)((?:[fbixs][ \t]+)?){re.escape(key)}[ \t]*=[^;\r\n]*;")
    if rx.search(sub):
        return _set_scalar_in_block(sub, key, value, indent)
    end_brace = sub.rfind("}")
    line_start = sub.rfind("\n", 0, end_brace)
    close_indent = sub[line_start + 1:end_brace] if line_start != -1 else ""
    return sub[:end_brace].rstrip() + f"\n{indent}{key} = {value};\n{close_indent}" + sub[end_brace:]


def _emit_container_template(vfs: TemplateVFS, stem: str, tier: str, pfx: str) -> str:
    vanilla = pfx.lower() + stem
    blk = vfs.get_block(vanilla)
    new_name = mmo_name(vanilla)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")

    if stem.startswith("dsx_set_") and not stem.endswith("-mp"):
        return out

    cfg = _CONTAINER_RANGES[tier]
    g_sub = _get_subblock(out, "gold*")
    if g_sub:
        _, _, gb = g_sub
        # Always scale from vanilla 1W container gold so 2W/3W crates (like a8_secret, a9_r3,
        # and set chests whose vanilla 2W/3W kept 1W gold) still progress cleanly across acts.
        base_bounds = _extract_gold_bounds(vfs.get_block(stem)) or _extract_gold_bounds(out)
        if base_bounds:
            v1, v2 = base_bounds
            is_spec = "-spec" in stem or stem.endswith("-mp")
            mult, floor_min = _CONTAINER_GOLD_SCALING[(tier, is_spec)]
            nmin = max(int(v1 * mult), floor_min)
            nmax = max(int(v2 * mult), int(nmin * 1.5))
            gb = _set_scalar_in_block(gb, "min", str(nmin), "\t\t\t\t")
            gb = _set_scalar_in_block(gb, "max", str(nmax), "\t\t\t\t")
            if is_spec:
                ch_m = re.search(r"chance\s*=\s*([0-9.]+)\s*;", gb, re.I)
                if ch_m and float(ch_m.group(1)) < 0.5:
                    gb = _set_scalar_in_block(gb, "chance", "0.5", "\t\t\t\t")
                gb = _set_scalar_preserving_closing_indent(gb, "piles", "3 - 5", "\t\t\t\t")
            out = _upsert_subblock(out, "gold*", gb)

    hp_pot, mp_pot, rj_pot = _POTIONS_BY_TIER[tier]
    vet_cfg = None
    if tier == "1w":
        band = "act9" if stem.startswith(("crate_dsx_a9_", "dsx_set_clockwork_4")) else "act8"
        vet_cfg = _CONTAINER_1W_VETERAN_BANDS[band]

    def _repl_il(m: re.Match) -> str:
        val = m.group(2).strip()
        vl = val.lower()
        if vl.startswith("potion_health"):
            return f"{m.group(1)}{hp_pot};"
        if vl.startswith("potion_mana"):
            return f"{m.group(1)}{mp_pot};"
        if vl.startswith("potion_rejuvenation"):
            return f"{m.group(1)}{rj_pot};"
        upgraded = _upgrade_range_token(val, cfg)
        base_line = f"{m.group(1)}{upgraded};"
        if vet_cfg is not None:
            comp = _veteran_container_companion(upgraded, vet_cfg)
            if comp and comp.lower() != upgraded.lower():
                indent_match = re.match(r"^[ \t]*", m.group(1))
                indent = indent_match.group(0) if indent_match else "\t\t\t\t\t"
                return f"{base_line}\n{indent}il_main = {comp};"
        return base_line

    out = re.sub(r"([ \t]*il_main\s*=\s*)([^;\r\n]+);", _repl_il, out, flags=re.I)
    return out


_MERCHANT_SPEC = {
    "1w": {
        "spells": [("0-12", 4, 2), ("13-22", 6, 4), ("23-32", 8, 6), ("33-42", 14, 12), ("43-50", 12, 9)],
        "books": [("40-120", 6, 4), ("90-140", 6, 4), ("110-165", 6, 4)],
        "acc": ("-mod(1)/65-95", 2, 1),
        "amr_lo": ("140-260", 4, 2),
        "amr_mid": ("265-350", 9, 6),
        "amr_hi": ("352-395", 5, 3),
        "robes_base": ("265-350", 5, 3),
        "shield_lo": ("140-260", 6, 4),
        "shield_mid": ("265-350", 8, 6),
        "shield_hi": ("352-395", 6, 4),
        "wpn_lo": ("75-115", 5, 3),
        "wpn_mid": ("118-165", 10, 7),
        "wpn_hi": ("168-192", 6, 4),
        "potions": [
            ("potion_health_medium", 10),
            ("potion_mana_medium", 10),
            ("potion_health_large", 15),
            ("potion_mana_large", 15),
            ("potion_rejuvenation_medium", 12),
            ("potion_rejuvenation_large", 10),
        ],
    },
    "2w": {
        "spells": [("0-22", 4, 2), ("23-45", 6, 3), ("46-66", 10, 6), ("67-80", 16, 14), ("81-92", 12, 9)],
        "books": [("40-120", 6, 4), ("110-165", 8, 6), ("140-230", 6, 4)],
        "acc": ("-mod(1)/165-245", 2, 1),
        "amr_lo": ("280-460", 4, 2),
        "amr_mid": ("465-580", 10, 7),
        "amr_hi": ("582-645", 6, 4),
        "robes_base": ("465-580", 5, 4),
        "shield_lo": ("280-460", 6, 4),
        "shield_mid": ("465-580", 10, 7),
        "shield_hi": ("582-645", 6, 4),
        "wpn_lo": ("165-215", 6, 4),
        "wpn_mid": ("218-285", 12, 9),
        "wpn_hi": ("288-322", 6, 4),
        "potions": [
            ("potion_health_large", 15),
            ("potion_mana_large", 15),
            ("potion_health_super", 12),
            ("potion_mana_super", 12),
            ("potion_rejuvenation_large", 18),
            ("potion_rejuvenation_super", 6),
        ],
    },
    "3w": {
        "spells": [("0-35", 4, 2), ("36-65", 6, 3), ("66-85", 10, 6), ("86-98", 16, 14), ("99-108", 14, 10)],
        "books": [("110-165", 6, 4), ("140-230", 8, 6), ("200-264", 6, 4)],
        "acc": ("-mod(1)/230-310", 2, 1),
        "amr_lo": ("420-560", 4, 2),
        "amr_mid": ("565-710", 10, 7),
        "amr_hi": ("715-785", 6, 4),
        "robes_base": ("565-710", 5, 4),
        "shield_lo": ("420-560", 6, 4),
        "shield_mid": ("565-710", 10, 7),
        "shield_hi": ("715-785", 6, 4),
        "wpn_lo": ("215-265", 6, 4),
        "wpn_mid": ("268-332", 12, 9),
        "wpn_hi": ("335-378", 8, 6),
        "potions": [
            ("potion_health_large", 15),
            ("potion_mana_large", 15),
            ("potion_health_super", 20),
            ("potion_mana_super", 20),
            ("potion_rejuvenation_large", 20),
            ("potion_rejuvenation_super", 15),
        ],
    },
}


# Additive UP + LoA Veteran (2W) stock appended on 1W shops after existing 1W stock.
# DSX_backpack_empty is intentionally omitted so SiegeHost ShopStock injects 4 at 1500g uniformly.
_MERCHANT_1W_VETERAN_EXTRAS: Dict[str, Dict[str, object]] = {
    "neener": {
        "spells": [("51-65", 6, 4), ("66-80", 6, 4)],
        "books": [("130-264", 5, 3)],
        "accs": [("-mod(1)/110-165", 2, 1), ("-mod(1)/140-185", 2, 1)],
        "robes": [("355-535", 5, 3)],
        "armor_slots": {
            slot: [("400-510", 4, 2), ("515-610", 3, 2)]
            for slot in ("body", "helm", "gloves", "boots")
        },
        "shields": [("400-510", 5, 3), ("515-610", 3, 2)],
        "weapons": {
            "melee": [("195-245", 5, 3), ("248-285", 4, 2)],
            "ranged": [("195-245", 5, 3), ("248-285", 4, 2)],
        },
    },
    "dsx_goblin_vendingmachine": {
        "spells": [("65-85", 8, 5), ("86-106", 7, 4)],
        "books": [("130-264", 5, 3), ("180-264", 5, 3)],
        "accs": [("-mod(1)/150-225", 2, 1), ("-mod(1)/165-245", 2, 1)],
        "robes": [("430-620", 4, 3), ("625-755", 3, 2)],
        "armor_slots": {
            slot: [("480-620", 4, 2), ("625-755", 3, 2)]
            for slot in ("body", "helm", "gloves", "boots")
        },
        "shields": [("480-660", 5, 3), ("665-860", 3, 2)],
        "weapons": {
            "melee": [("235-295", 5, 3), ("298-360", 4, 2)],
            "ranged": [("235-295", 5, 3), ("298-360", 4, 2)],
        },
    },
}


def _build_merchant_store_pcontent(tier: str, stem: str = "neener") -> str:
    ms = _MERCHANT_SPEC[tier]

    def _entry(il: str, mx: int, mn: int) -> str:
        return f"\t\t\t\t[all*]\n\t\t\t\t{{\n\t\t\t\t\til_main = {il};\n\t\t\t\t\tmax = {mx};\n\t\t\t\t\tmin = {mn};\n\t\t\t\t}}"

    magic_lines = [_entry(f"#spell/{r}", mx, mn) for r, mx, mn in ms["spells"]]
    misc_lines: List[str] = []
    for r, mx, mn in ms["books"]:
        misc_lines.append(_entry(f"#spellbook/{r}", mx, mn))
    misc_lines.append(_entry("book_glb_magic_01", 3, 2))
    misc_lines.append(_entry(f"#ring/{ms['acc'][0]}", ms["acc"][1], ms["acc"][2]))
    misc_lines.append(_entry(f"#amulet/{ms['acc'][0]}", ms["acc"][1], ms["acc"][2]))

    robe_rng, robe_mx, robe_mn = ms["robes_base"]
    armor_lines = [_entry(f"#body,ro/{robe_rng}", robe_mx, robe_mn)]
    for slot in ("body", "helm", "gloves", "boots"):
        for k in ("amr_lo", "amr_mid", "amr_hi"):
            r, mx, mn = ms[k]
            armor_lines.append(_entry(f"#{slot}/{r}", mx, mn))
    shield_lines = [_entry(f"#shield/{ms[k][0]}", ms[k][1], ms[k][2]) for k in ("shield_lo", "shield_mid", "shield_hi")]
    wpn_lines = []
    for fam in ("melee", "ranged"):
        for k in ("wpn_lo", "wpn_mid", "wpn_hi"):
            r, mx, mn = ms[k]
            wpn_lines.append(_entry(f"#{fam}/{r}", mx, mn))

    if tier == "1w" and stem in _MERCHANT_1W_VETERAN_EXTRAS:
        ex = _MERCHANT_1W_VETERAN_EXTRAS[stem]
        magic_lines.extend(_entry(f"#spell/{r}", mx, mn) for r, mx, mn in ex["spells"])
        misc_lines.extend(_entry(f"#spellbook/{r}", mx, mn) for r, mx, mn in ex["books"])
        for acc_spec, mx, mn in ex["accs"]:
            misc_lines.append(_entry(f"#ring/{acc_spec}", mx, mn))
            misc_lines.append(_entry(f"#amulet/{acc_spec}", mx, mn))
        armor_lines.extend(_entry(f"#body,ro/{r}", mx, mn) for r, mx, mn in ex["robes"])
        for slot, entries in ex["armor_slots"].items():
            armor_lines.extend(_entry(f"#{slot}/{r}", mx, mn) for r, mx, mn in entries)
        shield_lines.extend(_entry(f"#shield/{r}", mx, mn) for r, mx, mn in ex["shields"])
        for fam, entries in ex["weapons"].items():
            wpn_lines.extend(_entry(f"#{fam}/{r}", mx, mn) for r, mx, mn in entries)

    return (
        "\t\t[store_pcontent]\n"
        "\t\t{\n"
        "\t\t\tfull_ratio = 0;\n"
        "\t\t\t[magic]\n\t\t\t{\n" + "\n".join(magic_lines) + "\n\t\t\t}\n"
        "\t\t\t[misc]\n\t\t\t{\n" + "\n".join(misc_lines) + "\n\t\t\t}\n"
        "\t\t\t[armor]\n\t\t\t{\n" + "\n".join(armor_lines) + "\n\t\t\t}\n"
        "\t\t\t[shields]\n\t\t\t{\n" + "\n".join(shield_lines) + "\n\t\t\t}\n"
        "\t\t\t[weapons]\n\t\t\t{\n" + "\n".join(wpn_lines) + "\n\t\t\t}\n"
        "\t\t}"
    )


def _emit_merchant_template(vfs: TemplateVFS, stem: str, tier: str, pfx: str) -> str:
    vanilla = pfx.lower() + stem
    blk = vfs.get_block(vanilla)
    new_name = mmo_name(vanilla)
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{new_name}]", blk.strip(), count=1)
    out = _set_scalar_in_block(out, "doc", f'"{new_name}"', "\t")

    inv_found = _get_subblock(out, "inventory", top_level=True)
    sp_block = _build_merchant_store_pcontent(tier, stem)
    if inv_found:
        inv_blk = _upsert_subblock(inv_found[2], "store_pcontent", sp_block, top_level=True)
        out = _upsert_subblock(out, "inventory", inv_blk, top_level=True)

    ms = _MERCHANT_SPEC[tier]
    pot_lines = [f"\t\t\t{pname} = {qty};" for pname, qty in ms["potions"]]
    fixed_spells = [
        "\t\t\tscroll_resurrect = 6;",
        "\t\t\tspell_resurrect = 2;",
        "\t\t\tspell_revive = 2;",
        "\t\t\tspell_healing_hands = 1;",
        "\t\t\tspell_healing_wind = 1;",
        "\t\t\tspell_fireshot = 1;",
        "\t\t\tspell_zap = 1;",
    ]
    restock = "\t\t[item_restock]\n\t\t{\n" + "\n".join(pot_lines + fixed_spells) + "\n\t\t}"
    store_found = _get_subblock(out, "store", top_level=True)
    if store_found:
        st_blk = _upsert_subblock(store_found[2], "item_restock", restock, top_level=True)
        out = _upsert_subblock(out, "store", st_blk, top_level=True)
    return out


# High-tier PContent gear filling vanilla DS/LoA ranged, robe, helm, boot, glove, plate, and shield deserts.
# Tuple format: (template_name, donor_template, screen_name, section_key, stat_overrides, req_str, mod_max)
_PCONTENT_GEAR_SPECS: Tuple[Tuple[str, str, str, str, Dict[str, str], str, int], ...] = (
    # Bows (`bow/l` & `bow/m`): 1W Vet -> 2W -> 3W ranged/melee parity (~225..352 base power)
    ("mmo_loa_bw_l_vet_01", "bw_g_c_c_l_4_avg", "Aranna Hunter Longbow", "attack", {"damage_min": "272", "damage_max": "322"}, "strength:30,dexterity:56", 48),
    ("mmo_loa_bw_m_vet_02", "bw_g_c_c_m_4_avg", "Hassat Recurve Bow", "attack", {"damage_min": "268", "damage_max": "322"}, "strength:32,dexterity:62", 54),
    ("mmo_loa_bw_l_vet_03", "bw_g_c_s_l_c_avg", "Grescal Siege Longbow", "attack", {"damage_min": "330", "damage_max": "388"}, "strength:35,dexterity:69", 60),
    ("mmo_loa_bw_l_eli_01", "bw_g_c_c_l_r_avg", "Clockwork Precision Bow", "attack", {"damage_min": "362", "damage_max": "424"}, "strength:38,dexterity:76", 66),
    ("mmo_loa_bw_m_eli_02", "bw_g_c_c_m_4_avg", "Shadowjumper Composite Bow", "attack", {"damage_min": "352", "damage_max": "422"}, "strength:41,dexterity:83", 72),
    ("mmo_loa_bw_l_eli_03", "bw_g_c_c_l_4_avg", "Sovereign Aranna Longbow", "attack", {"damage_min": "428", "damage_max": "502"}, "strength:44,dexterity:90", 80),
    # Mage robes (`#body,ro`): 285..720 base power (vanilla robes stop at 207 base / 383 total)
    ("mmo_loa_bd_ro_m_vet_01", "bd_ro_m_g_c_avg_hewoven_dsx", "Astral Weaver Robe", "defend", {"defense": "285"}, "intelligence:54", 58),
    ("mmo_loa_bd_ro_m_vet_02", "bd_ro_m_g_c_avg_hewoven_dsx", "Zaurask Silk Mantle", "defend", {"defense": "365"}, "intelligence:62", 72),
    ("mmo_loa_bd_ro_m_vet_03", "bd_ro_m_g_c_avg_elbead_dsx", "Clockwork Mage Vestments", "defend", {"defense": "455"}, "intelligence:70", 86),
    ("mmo_loa_bd_ro_m_eli_01", "bd_ro_m_g_c_avg_hewoven_dsx", "Shadowvault Archmage Robe", "defend", {"defense": "550"}, "intelligence:80", 98),
    ("mmo_loa_bd_ro_m_eli_02", "bd_ro_m_g_c_avg_elbead_dsx", "Crownbound Arcanum Robe", "defend", {"defense": "645"}, "intelligence:90", 115),
    ("mmo_loa_bd_ro_m_eli_03", "bd_ro_m_g_c_avg_elbead_dsx", "Grand Aethelric Vestments", "defend", {"defense": "720"}, "intelligence:96", 125),
    # Helms (`*8.35`, `/0.65` ranger, `/0.45` mage): ~468..778 base power
    ("mmo_loa_he_f_vet_01", "he_fu_pl_horn_mag_02", "Aranna Vanguard Helm", "defend", {"defense": "56"}, "strength:58", 80),
    ("mmo_loa_he_r_vet_01", "he_op_pl_r_flywing_crown2_dsx", "Grescal Stalker Crown", "defend", {"defense": "41"}, "dexterity:64", 85),
    ("mmo_loa_he_m_vet_02", "he_vi_pl_m_ceramic_headdress2_dsx", "Clockwork Mystic Crown", "defend", {"defense": "31"}, "intelligence:74", 92),
    ("mmo_loa_he_f_vet_02", "he_fu_pl_horn_mag_02", "Stronghold Bastion Helm", "defend", {"defense": "73"}, "strength:70", 98),
    ("mmo_loa_he_r_eli_01", "he_op_pl_r_flywing_crown2_dsx", "Aranna Sentinel Crown", "defend", {"defense": "52"}, "dexterity:78", 106),
    ("mmo_loa_he_m_eli_02", "he_vi_pl_m_ceramic_headdress2_dsx", "Astral Conduit Headdress", "defend", {"defense": "38"}, "intelligence:90", 118),
    ("mmo_loa_he_f_eli_03", "he_fu_pl_horn_mag_02", "Sovereign Horned Greathelm", "defend", {"defense": "89"}, "strength:86", 125),
    ("mmo_loa_he_r_eli_03", "he_op_pl_r_flywing_crown2_dsx", "Shadowvault Ranger Circlet", "defend", {"defense": "61"}, "dexterity:94", 132),
    # Boots (`*9.4`): ~419..761 base power
    ("mmo_loa_bo_r_1w_01", "bo_sh_sc_r_g_c_avg_flywing2_dsx", "Aranna Scout Boots", "defend", {"defense": "29"}, "dexterity:54", 68),
    ("mmo_loa_bo_f_vet_01", "bo_gr_fp_f_g_c_avg", "Stronghold Plate Greaves", "defend", {"defense": "52"}, "strength:58", 82),
    ("mmo_loa_bo_m_vet_02", "bo_bo_sc_m_g_c_avg_geared2_dsx", "Clockwork Runic Boots", "defend", {"defense": "26"}, "intelligence:74", 88),
    ("mmo_loa_bo_r_vet_03", "bo_sh_sc_r_g_c_avg_flywing2_dsx", "Hassat Strider Boots", "defend", {"defense": "41"}, "dexterity:74", 95),
    ("mmo_loa_bo_f_eli_01", "bo_gr_fp_f_g_c_avg", "Grescal Bulwark Greaves", "defend", {"defense": "69"}, "strength:74", 105),
    ("mmo_loa_bo_m_eli_02", "bo_bo_sc_m_g_c_avg_geared2_dsx", "Aetherweave Sage Boots", "defend", {"defense": "34"}, "intelligence:86", 115),
    ("mmo_loa_bo_f_eli_03", "bo_gr_fp_f_g_c_avg", "Sovereign Bastion Greaves", "defend", {"defense": "81"}, "strength:84", 125),
    # Gloves (`*11.0`): ~355..759 base power
    ("mmo_loa_gl_r_1w_01", "gl_gl_sc_r_g_c_avg_leaf2_dsx", "Aranna Tracker Gloves", "defend", {"defense": "21"}, "dexterity:50", 60),
    ("mmo_loa_gl_f_vet_01", "gl_ga_fp_f_g_c_avg", "Stronghold Plate Gauntlets", "defend", {"defense": "50"}, "strength:60", 85),
    ("mmo_loa_gl_f_vet_02", "gl_ga_fp_f_g_c_avg", "Clockwork Siege Gauntlets", "defend", {"defense": "58"}, "strength:68", 95),
    ("mmo_loa_gl_r_eli_01", "gl_gl_sc_r_g_c_avg_leaf2_dsx", "Shadowvault Ranger Grips", "defend", {"defense": "40"}, "dexterity:76", 105),
    ("mmo_loa_gl_m_eli_02", "gl_ga_sc_m_g_c_avg_electric2_dsx", "Archmage Conductive Gloves", "defend", {"defense": "30"}, "intelligence:86", 115),
    ("mmo_loa_gl_f_eli_03", "gl_ga_fp_f_g_c_avg", "Sovereign War Gauntlets", "defend", {"defense": "69"}, "strength:84", 125),
    # Body armor non-robe (`#body`): ~520..785 base power
    ("mmo_loa_bd_f_vet_01", "bd_bp_f_g_c_avg", "Stronghold Warplate", "defend", {"defense": "520"}, "strength:64", 85),
    ("mmo_loa_bd_f_eli_01", "bd_bp_f_g_c_avg", "Clockwork Vanguard Plate", "defend", {"defense": "640"}, "strength:74", 98),
    ("mmo_loa_bd_r_eli_02", "bd_bp_r_g_c_avg_arbor_dsx", "Aranna Ranger Carapace", "defend", {"defense": "448"}, "dexterity:78", 105),
    ("mmo_loa_bd_m_eli_03", "bd_bp_m_g_c_avg_recon_dsx", "Astral Battle Aegis", "defend", {"defense": "332"}, "intelligence:88", 115),
    ("mmo_loa_bd_f_eli_04", "bd_bp_f_g_c_avg", "Sovereign Aranna Plate", "defend", {"defense": "785"}, "strength:90", 125),
    # Shields (`*5.6`, no `_m_` token so strength-scaled cleanly): ~196..829 base power
    ("mmo_loa_sh_f_1w_00", "sh_w_g_c_k_m_avg", "Aranna Guard Shield", "defend", {"defense": "35"}, "strength:30", 42),
    ("mmo_loa_sh_f_1w_01", "sh_w_g_c_k_m_avg", "Stronghold Heater Shield", "defend", {"defense": "61"}, "strength:46", 62),
    ("mmo_loa_sh_f_vet_01", "sh_w_g_c_k_m_avg", "Grescal Bastion Shield", "defend", {"defense": "75"}, "strength:54", 74),
    ("mmo_loa_sh_f_vet_02", "sh_w_g_c_k_m_avg", "Clockwork Tower Shield", "defend", {"defense": "92"}, "strength:62", 88),
    ("mmo_loa_sh_f_vet_03", "sh_w_g_c_k_m_avg", "Vanguard Bulwark Shield", "defend", {"defense": "104"}, "strength:66", 98),
    ("mmo_loa_sh_f_eli_01", "sh_w_g_c_k_m_avg", "Shadowvault Aegis", "defend", {"defense": "118"}, "strength:72", 110),
    ("mmo_loa_sh_f_eli_02", "sh_w_g_c_k_m_avg", "Royal Aranna Shield", "defend", {"defense": "128"}, "strength:76", 120),
    ("mmo_loa_sh_f_eli_03", "sh_w_g_c_k_m_avg", "Eternal Wall Shield", "defend", {"defense": "138"}, "strength:82", 130),
    ("mmo_loa_sh_f_eli_04", "sh_w_g_c_k_m_avg", "Sovereign Aegis", "defend", {"defense": "152"}, "strength:86", 142),
)


def _emit_pcontent_gear_block(
    vfs: TemplateVFS,
    name: str,
    donor: str,
    screen_name: str,
    sec_name: str,
    overrides: Dict[str, str],
    req_str: str,
    mod_max: int,
) -> str:
    blk = vfs.get_block(donor).replace("\r\n", "\n").strip()
    out = re.sub(r"^\[t:template,n:[^\]]+\]", f"[t:template,n:{name}]", blk, count=1)
    out = _set_scalar_in_block(out, "doc", f'"{screen_name}"', "\t")
    out = _remove_subblocks(out, "pcontent", top_level=True)

    cmn_found = _get_subblock(out, "common", top_level=True)
    cmn_blk = cmn_found[2] if cmn_found else "\t[common]\n\t{\n\t}"
    for ck, cv in (
        ("screen_name", f'"{screen_name}"'),
        ("is_pcontent_allowed", "true"),
        ("pcontent_special_type", "normal"),
        ("allow_modifiers", "true"),
    ):
        cmn_blk = _set_scalar_in_block(cmn_blk, ck, cv, "\t\t")
    out = _upsert_subblock(out, "common", cmn_blk, top_level=True)

    stat_found = _get_subblock(out, sec_name, top_level=True)
    if stat_found:
        stat_blk = stat_found[2]
        for sk, sv in overrides.items():
            stat_blk = _set_scalar_in_block(stat_blk, sk, sv, "\t\t")
        out = _upsert_subblock(out, sec_name, stat_blk, top_level=True)

    gui_found = _get_subblock(out, "gui", top_level=True)
    if gui_found:
        gui_blk = _set_scalar_in_block(gui_found[2], "equip_requirements", req_str, "\t\t")
        out = _upsert_subblock(out, "gui", gui_blk, top_level=True)

    pc_blk = (
        "\t[pcontent]\n"
        "\t{\n"
        "\t\t[base]\n"
        "\t\t{\n"
        "\t\t  f modifier_min = 0.000000;\n"
        f"\t\t  f modifier_max = {float(mod_max):.6f};\n"
        "\t\t}\n"
        "\t}"
    )
    return _upsert_subblock(out, "pcontent", pc_blk, top_level=True)


def _emit_pcontent_gear_templates(vfs: TemplateVFS) -> List[str]:
    return [
        _emit_pcontent_gear_block(vfs, name, donor, screen_name, sec_name, overrides, req_str, mod_max)
        for name, donor, screen_name, sec_name, overrides, req_str, mod_max in _PCONTENT_GEAR_SPECS
    ]


def generate_template_files(game_dir: str) -> Dict[str, bytes]:
    """Generate all virtual filesystem `.gas` template files for `Z_LoA_Expansion.dsres`."""
    vfs = TemplateVFS(game_dir)
    out: Dict[str, bytes] = {}

    weapon_roles, spell_roles = _collect_enemy_equipment_roles(vfs)
    stars_files = _emit_stars_templates(vfs)
    extra_wpns = _emit_monster_weapons_by_folder(vfs, weapon_roles)
    for folder, stars_txt in stars_files.items():
        pfx_fn = "" if folder == "regular" else ("2w_" if folder == "veteran" else "3w_")
        chunks = [stars_txt.rstrip()]
        if extra_wpns[folder]:
            chunks.append("\n\n".join(extra_wpns[folder]))
        path = f"world/contentdb/templates/{folder}/interactive/{pfx_fn}mmo_loa_weapons.gas"
        out[path] = ("\n\n".join(chunks) + "\n").encode("utf-8")

    gear_blocks = _emit_pcontent_gear_templates(vfs)
    out["world/contentdb/templates/regular/interactive/mmo_loa_pcontent_gear.gas"] = (
        "\n\n".join(gear_blocks) + "\n"
    ).encode("utf-8")

    spells_by_folder = _emit_monster_spells_by_folder(vfs, spell_roles)
    for _, pfx, folder in TIERS:
        fpfx = pfx.lower()
        path = f"world/contentdb/templates/{folder}/interactive/{fpfx}mmo_loa_spells.gas"
        out[path] = ("\n\n".join(spells_by_folder[folder]) + "\n").encode("utf-8")

    tier_actors: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}
    tier_actors["regular"].append(_emit_troll_base_template(vfs))
    tier_gens: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}
    tier_ctns: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}
    tier_npcs: Dict[str, List[str]] = {f: [] for _, _, f in TIERS}

    for stem in ENEMY_STEMS:
        scaled = compute_scaled_stats(vfs, stem)
        for tier, pfx, folder in TIERS:
            tier_actors[folder].append(
                _emit_enemy_template(vfs, stem, tier, pfx, scaled[tier], weapon_roles, spell_roles)
            )

    for stem in DARKGEN_STEMS:
        for tier, pfx, folder in TIERS:
            tier_actors[folder].append(_emit_darkgen_template(vfs, stem, tier, pfx))

    for stem in GENERATOR_STEMS:
        for _, pfx, folder in TIERS:
            tier_gens[folder].append(_emit_generator_template(vfs, stem, pfx))

    for stem in CONTAINER_STEMS:
        for tier, pfx, folder in TIERS:
            tier_ctns[folder].append(_emit_container_template(vfs, stem, tier, pfx))

    for stem in MERCHANT_STEMS:
        for tier, pfx, folder in TIERS:
            tier_npcs[folder].append(_emit_merchant_template(vfs, stem, tier, pfx))

    for _, pfx, folder in TIERS:
        fpfx = pfx.lower()
        out[f"world/contentdb/templates/{folder}/actors/evil/mmo_loa/{fpfx}mmo_loa_enemies.gas"] = (
            "\n\n".join(tier_actors[folder]) + "\n"
        ).encode("utf-8")
        out[f"world/contentdb/templates/{folder}/generators/{fpfx}mmo_loa_generators.gas"] = (
            "\n\n".join(tier_gens[folder]) + "\n"
        ).encode("utf-8")
        out[f"world/contentdb/templates/{folder}/interactive/containers/{fpfx}mmo_loa_containers.gas"] = (
            "\n\n".join(tier_ctns[folder]) + "\n"
        ).encode("utf-8")
        out[f"world/contentdb/templates/{folder}/actors/good/npc/{fpfx}mmo_loa_npcs.gas"] = (
            "\n\n".join(tier_npcs[folder]) + "\n"
        ).encode("utf-8")

    return out


def _self_test() -> None:
    game_dir = os.environ.get("DSLOA_GAME_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
    vfs = TemplateVFS(game_dir)
    files = generate_template_files(game_dir)
    parsed: Dict[str, str] = {}
    for k, raw in files.items():
        txt = raw.decode("utf-8")
        assert txt.count("{") == txt.count("}"), f"Unbalanced braces in {k}"
        for _, _, _, blk in extract_blocks(txt, r"\[t:template,n:[^\]]+\]"):
            m = re.match(r"\[t:template,n:([^,\]]+)\]", blk.strip(), re.I)
            assert m, f"Bad header in {k}"
            parsed[m.group(1).lower()] = blk

    all_known = set(vfs.blocks.keys()) | set(parsed.keys())
    errors: List[str] = []
    if len({v.lower() for v in TEMPLATE_REMAP.values()}) != len(TEMPLATE_REMAP):
        errors.append("TEMPLATE_REMAP has non-unique target names!")
    for name, blk in parsed.items():
        sm_all = re.findall(r"(?mi)(?:^|[{\s;])specializes\s*=\s*([^;\s]+)", blk)
        if len(sm_all) != 1 or sm_all[0].lower() not in all_known:
            errors.append(f"Bad/duplicate specializes in {name}: {sm_all}")
        if "#spellbook/;" in blk.lower():
            errors.append(f"Blank #spellbook/ in {name}")
        if re.search(r"victory_condition_met", blk, re.I):
            errors.append(f"Forbidden victory_condition_met in {name}")
        ir = _get_subblock(blk, "item_restock")
        if ir:
            pots = re.findall(r"^\s*(potion_[^=\s]+)\s*=", ir[2], re.M | re.I)
            if len(pots) != len(set(p.lower() for p in pots)):
                errors.append(f"Duplicate potion in {name}: {pots}")

        # Walk specializes parent chain to verify zero ancestor loot/restock/store leaks.
        cur = name
        seen_chain = {cur}
        while True:
            cur_blk = parsed.get(cur) or vfs.blocks.get(cur)
            if not cur_blk:
                break
            pm = re.search(r"specializes\s*=\s*([^;\s]+)", cur_blk, re.I)
            if not pm:
                break
            par = pm.group(1).lower()
            if par in seen_chain:
                break
            seen_chain.add(par)
            par_blk = parsed.get(par) or vfs.blocks.get(par)
            if par_blk:
                pl = par_blk.lower()
                if "[item_restock]" in pl or "[store_pcontent]" in pl:
                    errors.append(f"Parent store leak {name} -> {par}")
                if "[pcontent]" in pl and par != "mmo_loa_st_stars_base":
                    errors.append(f"Parent pcontent leak {name} -> {par}")
            cur = par

    expected_es = {
        "dsx_gobbot_flamethrower": "minigun_napalm",
        "dsx_gobbot_flamethrower_02": "dsx_minigun_flamethrower2",
        "dsx_gobbot_grenade_02": "dsx_minigun_grenade_launcher",
        "dsx_gobbot_lightning_02": "minigun_lightning",
        "dsx_gobbot_minigun": "dsx_minigun_dragon",
        "dsx_gobbot_minigun_02": "minigun_dragon",
        "dsx_troll_mountain": "cb_un_dsx_2h_troll",
        "gobbot_flamethrower": "minigun_flamethrower",
        "gobbot_grenade": "minigun_grenade_launcher",
        "gobbot_lightning": "minigun_lightning",
    }

    for stem in ENEMY_STEMS:
        role = ROLE_OVERRIDES.get(stem, "core")
        vals = []
        for tname_key, pfx, _ in TIERS:
            tname = mmo_name(pfx.lower() + stem).lower()
            b = parsed[tname]
            pc_cnt = len(re.findall(r"(?mi)^\s*\[pcontent\]", b))
            if pc_cnt != 1:
                errors.append(f"Expected exactly 1 [pcontent] in {tname}, got {pc_cnt}")
            for scalar in ("experience_value", "max_life", "defense", "damage_min", "damage_max"):
                hits = re.findall(rf"(?mi)(?:^|[{{;])[ \t]*(?:[fbixs][ \t]+)?{scalar}[ \t]*=[ \t]*([0-9.]+);", b)
                if len(hits) != 1:
                    errors.append(f"Expected 1 {scalar} in {tname}, got {hits}")
            if stem in expected_es:
                exp_wpn = mmo_name(pfx.lower() + expected_es[stem]).lower()
                pc = _get_subblock(b, "pcontent")
                if not pc or exp_wpn not in pc[2].lower():
                    errors.append(f"Missing remapped es_weapon_hand {exp_wpn} in {tname} [pcontent]")
            if stem != "dsx_shadow_bigboss" and re.search(r"(?i)\bes_weapon_hand\s*=", b):
                if "spew_equipped_kill_count = <ignore>;" not in b.lower():
                    errors.append(f"Non-Stars weapon wielder {tname} missing spew_equipped_kill_count = <ignore>;")
            xp = float(re.search(r"experience_value\s*=\s*([0-9.]+)", b).group(1))
            hp = float(re.search(r"max_life\s*=\s*([0-9.]+)", b).group(1))
            df = float(re.search(r"\bdefense\s*=\s*([0-9.]+)", b).group(1))
            dmin = float(re.search(r"damage_min\s*=\s*([0-9.]+)", b).group(1))
            dmax = float(re.search(r"damage_max\s*=\s*([0-9.]+)", b).group(1))
            vals.append((xp, hp, df, dmin, dmax))
            if tname_key == "1w":
                rp = _ROLE_PARAMS[("1w", role)]
                if xp < rp["xp_floor"] or hp < rp["hp_floor"] or df < rp["def_floor"] or dmin < rp["dmg_min_floor"]:
                    errors.append(f"1W floor violation on {tname} ({role}): {(xp, hp, df, dmin)}")
        if stem == "dsx_lava_spirit" and (vals[0][0] < 175000 or vals[0][1] < 1950 or vals[0][3] < 255):
            errors.append(f"dsx_lava_spirit below Utraean parity: {vals[0]}")
        if stem == "dsx_fury" and (vals[0][1] < 3950 or vals[1][1] < 6800 or vals[2][1] < 9800):
            errors.append(f"dsx_fury below HP target: {[v[1] for v in vals]}")
        if stem.startswith("dsx_elemental_") and not stem.endswith("_minion"):
            if vals[2][3] > 1550.0 or vals[2][4] > 2150.0:
                errors.append(f"3W elemental cap exceeded on {stem}: {vals[2]}")
        for i in range(2):
            for col, cname in enumerate(("xp", "hp", "def", "dmin", "dmax")):
                if not (vals[i][col] < vals[i + 1][col]):
                    errors.append(f"Non-monotonic {cname} on {stem}: {vals}")

    # Verify cloned monster weapons & combat spells exist, are non-droppable/non-pcontent, and scale monotonically.
    weapon_roles, spell_roles = _collect_enemy_equipment_roles(vfs)
    if len(weapon_roles) < 20 or len(spell_roles) < 30:
        errors.append(f"Unexpected weapon/spell collection count: w={len(weapon_roles)} s={len(spell_roles)}")
    for wpn in sorted(weapon_roles):
        w_raw_min = float((vfs.find_attr(wpn, "damage_min") or "0").split(",")[0].strip())
        dmins: List[float] = []
        dmaxs: List[float] = []
        for _, pfx, _ in TIERS:
            wn = mmo_name(pfx.lower() + wpn).lower()
            wb = parsed.get(wn)
            if not wb:
                errors.append(f"Missing cloned monster weapon {wn}")
                break
            wl = wb.lower()
            if "is_pcontent_allowed = false;" not in wl:
                errors.append(f"Monster weapon {wn} missing is_pcontent_allowed = false;")
            if "is_droppable = false;" not in wl:
                errors.append(f"Monster weapon {wn} missing is_droppable = false;")
            if "equip_requirements" in wl:
                errors.append(f"Monster weapon {wn} leaked equip_requirements")
            dmins.append(float(re.search(r"(?mi)damage_min\s*=\s*([0-9.]+);", wb).group(1)))
            dmaxs.append(float(re.search(r"(?mi)damage_max\s*=\s*([0-9.]+);", wb).group(1)))
        if len(dmins) == 3 and (dmins[0] < w_raw_min or not (dmins[0] < dmins[1] < dmins[2]) or not (dmaxs[0] < dmaxs[1] < dmaxs[2])):
            errors.append(f"Invalid weapon scaling for {wpn}: raw={w_raw_min} min={dmins} max={dmaxs}")

    for spl in sorted(spell_roles):
        s_raw_min = float((vfs.find_attr(spl, "damage_min") or "0").split(",")[0].strip())
        dmins = []
        dmaxs = []
        for _, pfx, _ in TIERS:
            sn = mmo_name(pfx.lower() + spl).lower()
            sb = parsed.get(sn)
            if not sb:
                errors.append(f"Missing cloned monster spell {sn}")
                break
            dmins.append(float(re.search(r"(?mi)damage_min\s*=\s*([0-9.]+);", sb).group(1)))
            dmaxs.append(float(re.search(r"(?mi)damage_max\s*=\s*([0-9.]+);", sb).group(1)))
        if len(dmins) == 3 and (dmins[0] < s_raw_min or not (dmins[0] < dmins[1] < dmins[2]) or not (dmaxs[0] < dmaxs[1] < dmaxs[2])):
            errors.append(f"Invalid spell scaling for {spl}: raw={s_raw_min} min={dmins} max={dmaxs}")

    # Verify Staff of Stars scaling and Big Boss linkage.
    stars_dmins = []
    for _, pfx, _ in TIERS:
        sname = mmo_name(pfx.lower() + "st_g_c_dsx_stars").lower()
        sb = parsed[sname]
        dmins = re.findall(r"(?mi)damage_min\s*=\s*([0-9.]+);", sb)
        if len(dmins) != 1:
            errors.append(f"Expected 1 damage_min in {sname}, got {dmins}")
        stars_dmins.append(float(dmins[0]))
        bname = mmo_name(pfx.lower() + "dsx_shadow_bigboss").lower()
        if sname not in parsed[bname].lower():
            errors.append(f"Big boss {bname} missing equipped {sname}")
    if not (stars_dmins[0] < stars_dmins[1] < stars_dmins[2]):
        errors.append(f"Non-monotonic Stars damage_min: {stars_dmins}")

    for mstem in MERCHANT_STEMS:
        b1 = parsed[mmo_name(mstem).lower()].lower()
        if "dsx_backpack_empty" in b1:
            errors.append(f"1W merchant {mstem} must rely on SiegeHost ShopStock, not static DSX_backpack_empty")
        for req_1w in ("#spell/43-50", "#spellbook/110-165", "#ring/-mod(1)/65-95", "#body/352-395", "#melee/168-192", "#ranged/168-192", "potion_health_large"):
            if req_1w not in b1:
                errors.append(f"1W merchant {mstem} lost baseline 1W item {req_1w}")
        b2 = parsed[mmo_name(f"2w_{mstem}").lower()].lower()
        b3 = parsed[mmo_name(f"3w_{mstem}").lower()]
        if "dsx_backpack_empty" in b2 or "dsx_backpack_empty" in b3.lower():
            errors.append(f"2W/3W merchant {mstem} leaked static DSX_backpack_empty")
        if "-rare(1)" in b1 or "-rare(1)" in b2 or "-rare(1)" in b3.lower() or "-unique(2)" in b3.lower():
            errors.append(f"Merchant {mstem} uses unsupported accessory rarity filter (-rare/-unique)")
        if "#body/715-785" not in b3 or "#melee/335-378" not in b3 or "#ranged/335-378" not in b3:
            errors.append(f"3W merchant {mstem} did not beat Sultrin or lacks ranged parity")
        if "#ranged/288-322" not in b2 or "potion_rejuvenation_super" not in b2:
            errors.append(f"2W merchant {mstem} missing ranged parity or super rejuvenation potion")

    neener_1w = parsed["mmo_loa_neener"].lower()
    vend_1w = parsed["mmo_loa_dsx_goblin_vendingmachine"].lower()
    if "#melee/248-285" not in neener_1w or "#ranged/248-285" not in neener_1w or "#spell/66-80" not in neener_1w or "#ring/-mod(1)/140-185" not in neener_1w:
        errors.append("mmo_loa_neener missing additive early/mid 2W stock or ranged parity")
    if "#melee/298-360" not in vend_1w or "#ranged/298-360" not in vend_1w or "#spell/86-106" not in vend_1w or "#shield/665-860" not in vend_1w:
        errors.append("mmo_loa_dsx_goblin_vendingmachine missing additive peak 2W stock or ranged parity")
    for gname, *_ in _PCONTENT_GEAR_SPECS:
        gb = parsed.get(gname.lower(), "").lower()
        if not gb or "is_pcontent_allowed = true;" not in gb:
            errors.append(f"Custom pcontent gear {gname} missing or not pcontent-allowed")

    # Verify additive 2W drops on 1W monsters and containers while preserving 1W drops and set chests.
    for estem, req_vet in (
        ("dsx_zaurask_fighter", "#weapon/185-250"),
        ("dsx_troll_mountain", "#weapon/215-295"),
        ("dsx_shadow_bigboss", "#weapon/-unique(2)/275-360"),
    ):
        eb = parsed[mmo_name(estem).lower()].lower()
        if "#weapon/68-92" not in eb or req_vet not in eb:
            errors.append(f"Enemy {estem} missing additive 1W+2W loot: req={req_vet}")
    if "ring_dsx_set_demlock" not in parsed["mmo_loa_dsx_set_demlock_3"].lower():
        errors.append("mmo_loa_dsx_set_demlock_3 lost fixed set ring")
    for cstem, req_1w, req_2w in (
        ("crate_dsx_a8_r2-01", "#weapon/70-96", "#weapon/190-265"),
        ("crate_dsx_a9_r1-01", "#weapon/70-96", "#weapon/235-335"),
        ("crate_dsx_a9_r3-spec_fire", "#melee,f/79-87/-rare(1)", "#melee,f/245-360/-rare(1)"),
        ("crate_dsx_a9_r3-spec_water", "#ring/79-87/-unique(2)", "#ring/185-250/-unique(2)"),
    ):
        cb = parsed[mmo_name(cstem).lower()].lower()
        if req_1w not in cb or req_2w not in cb:
            errors.append(f"Container {cstem} missing 1W ({req_1w}) or additive 2W ({req_2w}) entry")

    # Verify post-Hades enemy gold and XP progression across Act 8 -> Act 9 and all 3 tiers.
    gold_checks = (
        ("mmo_loa_dsx_zaurask_fighter", 22000, 30000),
        ("mmo_loa_dsx_troll_mountain", 26000, 36000),
        ("mmo_loa_dsx_golem_clockwork", 35000, 48000),
        ("mmo_loa_dsx_shadow_bigboss", 110000, 150000),
        ("2w_mmo_loa_dsx_zaurask_fighter", 145000, 215000),
        ("3w_mmo_loa_dsx_shadow_bigboss", 1850000, 2400000),
    )
    for ename, g_lo, g_hi in gold_checks:
        gb = _extract_gold_bounds(parsed[ename])
        if gb != (g_lo, g_hi):
            errors.append(f"Enemy {ename} gold mismatch: expected {(g_lo, g_hi)}, got {gb}")

    for cstem in CONTAINER_STEMS:
        g_tiers = [_extract_gold_bounds(parsed[mmo_name(pfx.lower() + cstem).lower()]) for _, pfx, _ in TIERS]
        if any(g is not None for g in g_tiers):
            if any(g is None for g in g_tiers) or not (g_tiers[0][0] < g_tiers[1][0] < g_tiers[2][0]):
                errors.append(f"Non-monotonic container gold on {cstem}: {g_tiers}")
    for tier_key, pfx, _ in TIERS:
        g_r2a = _extract_gold_bounds(parsed[mmo_name(pfx.lower() + "crate_dsx_a9_r2a-01").lower()])
        g_r3 = _extract_gold_bounds(parsed[mmo_name(pfx.lower() + "crate_dsx_a9_r3-01").lower()])
        g_c4 = _extract_gold_bounds(parsed[mmo_name(pfx.lower() + "dsx_set_clockwork_4-mp").lower()])
        if not (g_r2a and g_r3 and g_c4 and g_r3[0] > g_r2a[0] and g_c4[0] > g_r2a[0]):
            errors.append(f"Tier {tier_key} Act 9 r3/set chest regression vs r2a: {(g_r2a, g_r3, g_c4)}")

    assert not errors, "Self-test errors:\n" + "\n".join(errors)
    print(f"OK: {len(files)} files, {len(parsed)} templates validated with 0 errors ({len(weapon_roles)} weapon clones/tier, {len(spell_roles)} spell clones/tier).")


if __name__ == "__main__":
    _self_test()
