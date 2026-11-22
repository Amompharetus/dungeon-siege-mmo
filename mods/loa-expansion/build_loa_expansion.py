"""Build Z_LoA_Expansion.dsres (priority 0x5500) for multiplayer_world.

Combines:
- 383 remapped/patched templates from loa_templates.py
- 424 remapped/patched region files across 10 LoA regions + oasis from loa_remapper.py
- Merged map metadata: start_positions.gas, world_locations.gas, quests.gas,
  lore.gas, index/stitch_index.gas
- Global resources: world/global/elevators.gas,
  world/global/moods/multiplayer_world/moods1.gas,
  world/global/moods/dsx_mood_manager.skrit,
  world/global/siege_nodes/loa_expansion/loa_expansion_nodes.gas
"""
from __future__ import annotations

import argparse
import os
import re
from typing import Dict, List, Set, Tuple

from gas_parser import extract_blocks
from loa_geom import SNOIndex, parse_node_mesh_index
from loa_remapper import REGION_REMAP, generate_remapped_files
from loa_templates import generate_template_files
from tank_io import LazyTank, build_tank_with_priority

DEFAULT_GAME_DIR = os.environ.get("DSLOA_GAME_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
DEFAULT_OUT_PATH = os.environ.get("DSLOA_MOD_OUT", os.path.abspath(os.path.join(os.path.dirname(__file__), "dist", "Z_LoA_Expansion.dsres")))

# LOA region -> unique 4-digit world_locations IDs (1501..1509; excludes annex)
WORLD_LOCATION_IDS: Dict[str, int] = {
    "a8_r1_ramparts": 1501,
    "a8_r1b_secretcave": 1502,
    "a8_r2_fortress": 1503,
    "a8_r2b_fortress": 1504,
    "a8_r3_tunnels": 1505,
    "a9_r1_mesa": 1506,
    "a9_r2_cave": 1507,
    "a9_r2a_clockdungeon": 1508,
    "a9_r3_clockroom": 1509,
}

# Displacer / elevator node GUIDs registered in world/global/elevators.gas for multiplayer_world
MPWORLD_ELEVATOR_GUIDS: List[str] = [
    "0xB0760002",  # oasis displacer pad
    "0xB2F00006",  # a8_r2_fortress Bay 1 pad (from oasis)
    "0xB2F00008",  # a8_r2_fortress Bay 2 pad (to clockroom)
    "0x8D3A85E1",  # a8_r3_tunnels elevator
    "0x910144C5",  # a9_r1_mesa elevator
    "0x73851E47",  # a9_r2a_clockdungeon elevator
    "0x3E9D518D",  # a9_r3_clockroom displacer pad
]


def _indent_block(blk: str, prefix: str = "\t") -> str:
    """Indent non-empty lines of a GAS block cleanly."""
    lines = blk.replace("\r\n", "\n").split("\n")
    out = []
    for i, line in enumerate(lines):
        if i == 0:
            out.append(prefix + line.lstrip())
        elif line.strip():
            out.append(prefix + line)
        else:
            out.append("")
    return "\n".join(out)


def _insert_before_block_close(text: str, header_rx: str, payload: str) -> str:
    """Insert payload just before the closing brace of the first matching block."""
    matches = extract_blocks(text, header_rx)
    if not matches:
        raise RuntimeError(f"Could not find GAS block matching {header_rx!r}")
    _, _, end, _ = matches[0]
    close_idx = end - 1
    if text[close_idx] != "}":
        raise RuntimeError(f"Expected '}}' at end of {header_rx!r}, found {text[close_idx]!r}")
    sep = "" if (close_idx > 0 and text[close_idx - 1] == "\n") else "\r\n"
    crlf_payload = payload.replace("\r\n", "\n").replace("\n", "\r\n")
    if not crlf_payload.endswith("\r\n"):
        crlf_payload += "\r\n"
    return text[:close_idx] + sep + crlf_payload + text[close_idx:]


def build_start_positions(mp_map: LazyTank, exp_map: LazyTank) -> bytes:
    """Merge Expansion [t:start_group,n:fortress] into MpWorld start_positions.gas."""
    mp_sp = mp_map.get("world/maps/multiplayer_world/info/start_positions.gas").decode("latin1")
    exp_sp = exp_map.get("world/maps/map_expansion/info/start_positions.gas").decode("latin1")

    groups = extract_blocks(exp_sp, r"\[t:start_group,n:fortress\]")
    if len(groups) != 1:
        raise RuntimeError(f"Expected 1 fortress start_group, found {len(groups)}")
    _, _, _, blk = groups[0]

    # Set group ID = 9 and keep exact vanilla screen_name / description ("Neener's Emporium")
    blk = re.sub(r"(\bi\s+id\s*=\s*)\d+\s*;", r"\g<1>9;", blk, count=1)

    # Extract [start_position] entries, sort by original ID, and renumber sequentially 1..8
    sp_blocks = extract_blocks(blk, r"\[start_position\]")
    if len(sp_blocks) != 8:
        raise RuntimeError(f"Expected 8 fortress start_position entries, found {len(sp_blocks)}")

    parsed_sp: List[Tuple[int, str]] = []
    for _, _, _, sp_blk in sp_blocks:
        m = re.search(r"\bi\s+id\s*=\s*(\d+)\s*;", sp_blk)
        if not m:
            raise RuntimeError("Missing i id inside fortress start_position")
        orig_id = int(m.group(1))
        parsed_sp.append((orig_id, sp_blk))
    parsed_sp.sort(key=lambda item: item[0])

    renumbered_sps = []
    for new_id, (_, sp_blk) in enumerate(parsed_sp, start=1):
        new_sp = re.sub(r"(\bi\s+id\s*=\s*)\d+\s*;", rf"\g<1>{new_id};", sp_blk, count=1)
        renumbered_sps.append("\t" + new_sp.lstrip())

    new_group = (
        "\t[t:start_group,n:fortress]\n"
        "\t{\n"
        "\t  b default = false;\n"
        '\t\tdescription = "Neener\'s Emporium";\n'
        "\t  b dev_only = false;\n"
        "\t  i id = 9;\n"
        '\t\tscreen_name = "Neener\'s Emporium";\n'
        "\t\t[world_levels]\n"
        "\t\t{\n"
        "\t\t\t[regular]\n"
        "\t\t\t{\n"
        "\t\t\t\trequired_level = 58;\n"
        "\t\t\t}\n"
        "\t\t\t[veteran]\n"
        "\t\t\t{\n"
        "\t\t\t\trequired_level = 85;\n"
        "\t\t\t}\n"
        "\t\t\t[elite]\n"
        "\t\t\t{\n"
        "\t\t\t\trequired_level = 110;\n"
        "\t\t\t}\n"
        "\t\t}\n"
        + "\n".join(renumbered_sps)
        + "\n\t}"
    )

    merged = _insert_before_block_close(mp_sp, r"(?m)^\[start_positions\]", new_group)
    return merged.encode("latin1")


def build_world_locations(mp_map: LazyTank, exp_map: LazyTank) -> bytes:
    """Merge Expansion hub & 9 LOA region locations into MpWorld world_locations.gas."""
    mp_wl = mp_map.get("world/maps/multiplayer_world/info/world_locations.gas").decode("latin1")
    exp_wl = exp_map.get("world/maps/map_expansion/info/world_locations.gas").decode("latin1")

    exp_locs: Dict[str, str] = {}
    for m, _, _, blk in extract_blocks(exp_wl, r"\[t:location,n:([a-zA-Z0-9_]+)\]"):
        exp_locs[m.group(1).lower()] = blk

    to_add: List[str] = [
        "\n// Legends of Aranna expansion locations",
        "\t[t:location,n:neener_emporium]",
        "\t{",
        "\t\tid = 1500;",
        "\t\tscreen_name = \"Neener's Emporium\";",
        "\t}",
    ]
    for hub in ("ramparts", "stronghold", "clock"):
        if hub not in exp_locs:
            raise RuntimeError(f"Missing hub location {hub!r} in Expansion world_locations.gas")
        to_add.append("\t" + exp_locs[hub].lstrip())

    for reg, new_id in WORLD_LOCATION_IDS.items():
        if reg not in exp_locs:
            raise RuntimeError(f"Missing region location {reg!r} in Expansion world_locations.gas")
        blk = re.sub(r"(\bid\s*=\s*)\d+\s*;", rf"\g<1>{new_id};", exp_locs[reg], count=1)
        to_add.append("\t" + blk.lstrip())

    merged = _insert_before_block_close(mp_wl, r"(?m)^\[world_locations\]", "\n".join(to_add))
    return merged.encode("latin1")


def build_quests(mp_map: LazyTank, exp_map: LazyTank) -> bytes:
    """Merge a8_pad into stock MpWorld quests.gas without activating [chapters]."""
    mp_q = mp_map.get("world/maps/multiplayer_world/quests/quests.gas").decode("latin1")
    exp_q = exp_map.get("world/maps/map_expansion/quests/quests.gas").decode("latin1")

    a8_pad = extract_blocks(exp_q, r"(?m)^[ \t]*\[a8_pad\]")
    if len(a8_pad) != 1:
        raise RuntimeError(f"Expected 1 [a8_pad] block in Expansion, found {len(a8_pad)}")

    quests_matches = extract_blocks(mp_q, r"(?m)^[ \t]*\[quests\]")
    if len(quests_matches) != 1:
        raise RuntimeError(f"Expected 1 active [quests] block in MpWorld, found {len(quests_matches)}")

    # Preserve stock MpWorld with no active [chapters] block so UIJournal::Populate
    # stays on its flat quest list path (showing all 27 UP quests alongside a8_pad).
    merged = _insert_before_block_close(
        mp_q,
        r"(?m)^[ \t]*\[quests\]",
        "\t" + a8_pad[0][3].lstrip(),
    )
    return merged.encode("latin1")


def build_lore(mp_map: LazyTank, exp_map: LazyTank) -> bytes:
    """Merge all Expansion lore book entries into MpWorld lore.gas.

    Replaces MpWorld's placeholder stub for [lore_utraean] ("To Be Added") with
    Expansion's full [lore_utraean] text so there are no duplicate keys and every
    vanilla UP + LoA lore book (including Nosirrom's Journals) renders properly.
    """
    mp_lore_txt = mp_map.get("world/maps/multiplayer_world/info/lore.gas").decode("latin1")
    exp_lore_txt = exp_map.get("world/maps/map_expansion/info/lore.gas").decode("latin1")

    mp_root = extract_blocks(mp_lore_txt, r"(?m)^[ \t]*\[lore\]")
    exp_root = extract_blocks(exp_lore_txt, r"(?m)^[ \t]*\[lore\]")
    if len(mp_root) != 1:
        raise RuntimeError(f"Expected 1 active [lore] block in MpWorld, found {len(mp_root)}")
    if len(exp_root) != 1:
        raise RuntimeError(f"Expected 1 active [lore] block in Expansion, found {len(exp_root)}")

    exp_blocks: List[Tuple[str, str]] = []
    exp_keys: Set[str] = set()
    for m, _, _, blk in extract_blocks(exp_root[0][3], r"(?m)^[ \t]*\[([a-zA-Z0-9_]+)\]"):
        key = m.group(1).lower()
        if key == "lore":
            continue
        if key in exp_keys:
            raise RuntimeError(f"Duplicate lore key {key!r} in Expansion lore.gas")
        exp_keys.add(key)
        exp_blocks.append((key, "\t" + blk.lstrip()))
    if not exp_blocks:
        raise RuntimeError("No lore entries found in Expansion lore.gas")

    # Remove any MpWorld lore entry overridden by Expansion (specifically the "To Be Added"
    # [lore_utraean] stub) using exact spans inside the active [lore] block.
    _, mp_start, mp_end, mp_blk = mp_root[0]
    mp_open = mp_blk.find("{")
    if mp_open < 0:
        raise RuntimeError("Malformed [lore] block in MpWorld lore.gas")
    body_base = mp_start + mp_open + 1
    body_txt = mp_lore_txt[body_base : mp_end - 1]

    removals: List[Tuple[int, int]] = []
    for m, s, e, _ in extract_blocks(body_txt, r"(?m)^[ \t]*\[([a-zA-Z0-9_]+)\]"):
        key = m.group(1).lower()
        if key in exp_keys:
            # Also trim trailing newline right after the removed block
            e2 = e
            while e2 < len(body_txt) and body_txt[e2] in ("\r", "\n"):
                e2 += 1
            removals.append((s, e2))

    for s, e in reversed(removals):
        body_txt = body_txt[:s] + body_txt[e:]

    cleaned_mp = mp_lore_txt[:body_base] + body_txt + mp_lore_txt[mp_end - 1 :]
    payload = "\n// Legends of Aranna expansion lore entries\n" + "\n".join(blk for _, blk in exp_blocks)
    merged = _insert_before_block_close(cleaned_mp, r"(?m)^[ \t]*\[lore\]", payload)
    return merged.encode("latin1")


def _collect_loa10_nodes(exp_map: LazyTank) -> Set[int]:
    """Return lowercase int node GUIDs for all 10 LoA regions."""
    nodes: Set[int] = set()
    for reg in REGION_REMAP:
        ntxt = exp_map.get(f"world/maps/map_expansion/regions/{reg}/terrain_nodes/nodes.gas").decode("latin1")
        for m in re.finditer(r"\bguid\s*=\s*(0x[0-9a-fA-F]+)\s*;", ntxt, re.I):
            nodes.add(int(m.group(1), 16))
    return nodes


def build_stitch_index(mp_map: LazyTank, exp_map: LazyTank) -> bytes:
    """Merge LOA10 stitch_index & node_connect_info entries, stripping a7_r3_cityinterior links."""
    mp_si = mp_map.get("world/maps/multiplayer_world/index/stitch_index.gas").decode("latin1")
    exp_si = exp_map.get("world/maps/map_expansion/index/stitch_index.gas").decode("latin1")
    loa10_nodes = _collect_loa10_nodes(exp_map)

    extra_blocks: List[str] = []

    # 1. Filtered [t:node_connect_info,n:<guid>]
    for m, _, _, blk in extract_blocks(exp_si, r"\[t:node_connect_info,n:(0x[0-9a-fA-F]+)\]"):
        ng = int(m.group(1), 16)
        if ng not in loa10_nodes:
            continue
        # Remove any [t:neighbor_connections,n:<far>] targeting outside loa10_nodes
        nb_blocks = extract_blocks(blk, r"\[t:neighbor_connections,n:(0x[0-9a-fA-F]+)\]")
        kept_nb = [nb_blk for nm, _, _, nb_blk in nb_blocks if int(nm.group(1), 16) in loa10_nodes]
        if not kept_nb:
            continue
        if len(kept_nb) == len(nb_blocks):
            extra_blocks.append("\t" + blk.lstrip())
        else:
            rebuilt = (
                f"\t[t:node_connect_info,n:{m.group(1)}]\n"
                "\t{\n\t\t"
                + "\n\t\t".join(nb.lstrip() for nb in kept_nb)
                + "\n\t}"
            )
            extra_blocks.append(rebuilt)

    # 2. Remapped [t:stitch_index,n:0x000000B0..B9], stripping non-LOA10 edges (a7_r3_cityinterior)
    old_to_new_hex = {f"0x{old:08x}": f"0x{new:08x}" for old, new in REGION_REMAP.values()}
    for m, _, _, blk in extract_blocks(exp_si, r"\[t:stitch_index,n:(0x[0-9a-fA-F]+)\]"):
        old_hex = m.group(1).lower()
        if old_hex not in old_to_new_hex:
            continue
        new_hex = old_to_new_hex[old_hex]
        kept_lines = []
        for line in blk.splitlines():
            em = re.match(r"^\s*\*\s*=\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*;", line)
            if em:
                a = int(em.group(1), 16)
                b = int(em.group(3), 16)
                if a not in loa10_nodes or b not in loa10_nodes:
                    continue
            kept_lines.append(line)
        if len(kept_lines) < 3:
            continue
        rebuilt_si = "\n".join(kept_lines)
        rebuilt_si = re.sub(
            r"\[t:stitch_index,n:0x[0-9a-fA-F]+\]",
            f"[t:stitch_index,n:{new_hex}]",
            rebuilt_si,
            count=1,
        )
        extra_blocks.append("\t" + rebuilt_si.lstrip())

    merged = _insert_before_block_close(mp_si, r"(?m)^\[stitch_index\]", "\n".join(extra_blocks))
    return merged.encode("latin1")


def build_elevators(exp_res: LazyTank) -> bytes:
    """Extend world/global/elevators.gas with [multiplayer_world] displacer & LoA lift GUIDs."""
    exp_elev = exp_res.get("world/global/elevators.gas").decode("latin1")
    mp_block = (
        "\t\t[multiplayer_world]\n"
        "\t\t{\n"
        "\t\t\t[elevatorGuids]\n"
        "\t\t\t{\n"
        + "".join(f"\t\t\t\t* = {g.lower()};\n" for g in MPWORLD_ELEVATOR_GUIDS)
        + "\t\t\t}\n"
        "\t\t}"
    )
    merged = _insert_before_block_close(exp_elev, r"(?m)^[ \t]*\[maps\]", mp_block)
    return merged.encode("latin1")


def build_moods(logic_res: LazyTank, exp_res: LazyTank) -> bytes:
    """Merge map_expansion mood_setting blocks into multiplayer_world/moods1.gas."""
    mp_moods = logic_res.get("world/global/moods/multiplayer_world/moods1.gas").decode("latin1")
    exp_moods = exp_res.get("world/global/moods/map_expansion/moods1.gas").decode("latin1")

    existing_names = {
        m.group(1).lower()
        for m in re.finditer(r"^\s*mood_name\s*=\s*([a-zA-Z0-9_]+)\s*;", mp_moods, re.M)
    }

    added: List[str] = []
    for _, _, _, blk in extract_blocks(exp_moods, r"(?m)^[ \t]*\[mood_setting\*\]"):
        nm = re.search(r"^\s*mood_name\s*=\s*([a-zA-Z0-9_]+)\s*;", blk, re.M)
        if not nm:
            continue
        if nm.group(1).lower() in existing_names:
            continue
        existing_names.add(nm.group(1).lower())
        added.append(blk.replace("\r\n", "\n"))

    payload = "\r\n\r\n" + "\r\n\r\n".join(b.replace("\n", "\r\n") for b in added) + "\r\n"
    if not mp_moods.endswith(("\n", "\r")):
        mp_moods += "\r\n"
    return (mp_moods + payload).encode("latin1")


def build_mood_manager(exp_res: LazyTank) -> bytes:
    """Scope Area 9 wasteland weather/sky randomization to a9_r1_mesa instead of global GameAuditor bool.

    Vanilla Expansion.dsres dsx_mood_manager.skrit checks GameAuditor.GetDb.GetBool("in_area_nine"),
    which is a world-wide flag set by a9_r1_mesa triggers and never cleared on Displacer/Recall/Death.
    On a multiplayer server that causes every outdoor player in Utraean Peninsula (UP) to get
    random flashing sky/fog colors, lightning, rock/hail storms, and distorted TimeOfDay speed.
    """
    txt = exp_res.get("world/global/moods/dsx_mood_manager.skrit").decode("latin1")

    old_vars = (
        "bool rock_storm$\t\t\t\t\t= false;\t// is the rock storm going?\r\n"
        "bool hail_storm$\t\t\t\t\t= false;\t// is the hail storm going?"
    )
    new_vars = (
        "bool rock_storm$\t\t\t\t\t= false;\t// is the rock storm going?\r\n"
        "bool hail_storm$\t\t\t\t\t= false;\t// is the hail storm going?\r\n"
        "bool was_randomizing$\t\t\t\t= false;\t// tracks leaving a9_r1_mesa so fog/TimeOfDay reset immediately"
    )
    if old_vars not in txt:
        raise RuntimeError("dsx_mood_manager.skrit: failed to locate storm state globals")
    txt = txt.replace(old_vars, new_vars, 1)

    old_fn = (
        "bool RandomizeWeather$( Goid target$ )\r\n"
        "{\r\n"
        "\treturn !Victory.IsQuestCompleted( \"a1_staff\", target$ ) && GameAuditor.GetDb.GetBool( \"in_area_nine\" );\r\n"
        "}"
    )
    new_fn = (
        "bool RandomizeWeather$( Goid target$ )\r\n"
        "{\r\n"
        "\tif ( !target$.IsValid() || !target$.Go.HasPlacement() ) { return false; }\r\n"
        "\tstring regionName$ = WorldMap.GetRegionNameForNode( target$.Go.Placement.Position.Node );\r\n"
        "\tif ( !StringTool.SameNoCase( regionName$, \"a9_r1_mesa\" ) ) { return false; }\r\n"
        "\treturn !Victory.IsQuestCompleted( \"a1_staff\", target$ );\r\n"
        "}"
    )
    if old_fn not in txt:
        raise RuntimeError("dsx_mood_manager.skrit: failed to locate RandomizeWeather$()")
    txt = txt.replace(old_fn, new_fn, 1)

    old_tick = "\t// Time to play with weather\r\n"
    new_tick = (
        "\t// Reset fog/weather/clock immediately when leaving a9_r1_mesa back to UP or interior areas\r\n"
        "\tbool isRandomizing$ = RandomizeWeather$( focusedGo$ );\r\n"
        "\tif ( was_randomizing$ && !isRandomizing$ )\r\n"
        "\t{\r\n"
        "\t\trock_storm$ = false;\r\n"
        "\t\thail_storm$ = false;\r\n"
        "\t\tLastHour$ = -1;\r\n"
        "\t\tstring exitMood$ = Mood.GetGoRequestedMood( focusedGo$ );\r\n"
        "\t\tif ( StringTool.SameNoCase( exitMood$, \"a9_random_weather\" ) )\r\n"
        "\t\t{\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).bLightning = false;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).bRainEnabled = false;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).bSnowEnabled = false;\r\n"
        "\t\t\tMood.ForceUpdateRain( 0 );\r\n"
        "\t\t\tMood.ForceUpdateSnow( 0 );\r\n"
        "\t\t}\r\n"
        "\t\telse\r\n"
        "\t\t{\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).fogDensity = Mood.GetOriginalMoodSetting( exitMood$ ).fogDensity;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).fogNearDist = Mood.GetOriginalMoodSetting( exitMood$ ).fogNearDist;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).fogFarDist = Mood.GetOriginalMoodSetting( exitMood$ ).fogFarDist;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).fogLowDetNearDist = Mood.GetOriginalMoodSetting( exitMood$ ).fogLowDetNearDist;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).fogLowDetFarDist = Mood.GetOriginalMoodSetting( exitMood$ ).fogLowDetFarDist;\r\n"
        "\t\t\tMood.GetGoMoodSetting( focusedGo$ ).bLightning = Mood.GetOriginalMoodSetting( exitMood$ ).bLightning;\r\n"
        "\t\t\tMood.ForceUpdateRain( 0 );\r\n"
        "\t\t}\r\n"
        "\t\t#only(game) [[\r\n"
        "\t\tTimeOfDay.SetRealMinutesModifier( 1.0 );\r\n"
        "\t\t]];\r\n"
        "\t}\r\n"
        "\twas_randomizing$ = isRandomizing$;\r\n"
        "\r\n"
        "\t// Time to play with weather\r\n"
    )
    if old_tick not in txt:
        raise RuntimeError("dsx_mood_manager.skrit: failed to locate weather tick anchor")
    txt = txt.replace(old_tick, new_tick, 1)

    return txt.encode("latin1")


def build_pcontent_skrit(exp_res: LazyTank) -> bytes:
    """Patch Expansion.dsres world/contentdb/pcontent.skrit calc_gold_value$ so Elite
    shop items never overflow max_party_gold (9,999,999 buy / 3,333,333 sell) into
    'Priceless', while keeping prices strictly increasing and level-befitting."""
    txt = exp_res.get("world/contentdb/pcontent.skrit").decode("latin1")
    anchor = "int calc_gold_value$( ePContentType type$ )"
    idx = txt.find(anchor)
    if idx == -1:
        raise RuntimeError("pcontent.skrit: failed to locate calc_gold_value$")

    new_calc_gold_value = """int calc_gold_value$( ePContentType type$ )
{
\t// given power and the item itself, calculate the gold value of the
\t// item based on its power and the power of its modifiers. note that
\t// the incoming item_power is the unmodified "native" power of the
\t// item and its variation.
\t
\t// input: item_power_min$\t\t\t<< from original query
\t// input: item_power_max$\t\t\t<< from original query
\t// input: item_power$\t\t\t\t<< modified for gold value calc, based on innate item traits
\t// input: item_specific_type$
\t// input: item_subtype$
\t// input: item_length$
\t// input: item_material$
\t// input: item_two_handed$
\t// input: modifier_power_0$;
\t// input: modifier_power_1$;
\t
\tfloat value$ = 0;
\tfloat tot$ = 0;
\tfloat exp$ = 0;
\tfloat mod_gold$ = 0;
\tfloat cap_mod$ = 0;
\tfloat ex$ = 0;
\tfloat excess$ = 0;
\t
\tif ( type$ == PT_WEAPON )
\t{
\t\tif ( item_power$ > 0 )
\t\t{
\t\t\titem_power$ = item_power$ - (item_power$ * ((((item_power_max$ - item_power_min$) / 2) / item_power$) / 10));
\t\t}
\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t
\t\tif ( item_specific_type$ == PT_RANGED )
\t\t{
\t\t\tif ( item_subtype$ ~= "bow" ) && (item_length$ ~= "s")
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 7 + ((((tot$ * 0.90909) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 7 + (((tot$ * 0.90909) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = ((tot$ * 0.90909) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_subtype$ ~= "bow" ) && (item_length$ ~= "m")
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 10 + ((((tot$ * 0.847458) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 10 + (((tot$ * 0.847458) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 2 + ((tot$ * 0.847458) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_subtype$ ~= "bow" ) && (item_length$ ~= "l")
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 15 + ((((tot$ * 0.763359) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 15 + (((tot$ * 0.763359) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 4 + ((tot$ * 0.763359) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_subtype$ ~= "crossbow" ) && (item_length$ ~= "m")
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 20 + ((((tot$ * 1.282051) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 20 + (((tot$ * 1.282051) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 8 + ((tot$ * 1.282051) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_subtype$ ~= "crossbow" ) && (item_length$ ~= "l")
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + ((((tot$ * 1.052632) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + (((tot$ * 1.052632) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 10 + ((tot$ * 1.052632) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_subtype$ ~= "minigun" )
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + ((((tot$ * 0.7) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 121 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + (((tot$ * 0.7) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 103 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + (((tot$ * 0.8) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + (((tot$ * 1.1) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse
\t\t\t\t{
\t\t\t\t\tvalue$ = 22;
\t\t\t\t}
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + ((((tot$ * 1) * 1.17) * 0.2) ** (2.93 + (1/(tot$/117))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 25 + (((tot$ * 1) * 0.2) ** 3.6);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 10 + ((tot$ * 1) * 1.5);
\t\t\t\t}
\t\t\t}
\t\t}
\t\telse if ( item_specific_type$ == PT_MELEE )
\t\t{
\t\t\tif ( item_subtype$ ~= "staff" )
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 5 + ((((tot$ * 0.862) * 1.19) * 0.19) ** (2.9 + (1/(tot$/109))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 5 + (((tot$ * 0.862) * 0.19) ** 3.5);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 1 + ((tot$ * 0.862) * 1.2);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( item_two_handed$ )
\t\t\t{
\t\t\t\tif ( tot$ >= 227.5 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 5 + ((((tot$ * 0.79365) * 1.19) * 0.19) ** (2.9 + (1/(tot$/109))));
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 6 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 5 + (((tot$ * 0.79365) * 0.19) ** 3.5);
\t\t\t\t}
\t\t\t\telse if ( item_power$ >= 0 )
\t\t\t\t{
\t\t\t\t\tvalue$ = 1 + ((tot$ * 0.79365) * 1.2);
\t\t\t\t}
\t\t\t}
\t\t\telse if ( tot$ >= 227.5 )
\t\t\t{
\t\t\t\tvalue$ = 5 + ((((tot$ * 1) * 1.19) * 0.19) ** (2.9 + (1/(tot$/109))));
\t\t\t}
\t\t\telse if ( item_power$ >= 6 )
\t\t\t{
\t\t\t\tvalue$ = 5 + ((tot$ * 0.19) ** 3.5);
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = (tot$ * 1.2);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 10;
\t\t\t}
\t\t}
\t}
\telse if ( type$ == PT_ARMOR )
\t{
\t\tfloat MAGE_ARMOR_CONST$ = 0.45;
\t\tfloat RANGER_ARMOR_CONST$ = 0.65;
\t\t
\t\tif ( item_specific_type$ == PT_SHIELD )
\t\t{
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / MAGE_ARMOR_CONST$;
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / RANGER_ARMOR_CONST$;
\t\t\t}
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\t
\t\t\tif ( tot$ >= 97 )
\t\t\t{
\t\t\t\texp$ = 3.6 - (tot$ / 575);
\t\t\t\tif ( exp$ < 3.05 ) { exp$ = 3.05; }
\t\t\t\tvalue$ = 10 + (((tot$ * 5.54) * 0.092) ** exp$) * 0.18;
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = 10 + ((((tot$ * 5.54) * 0.092) ** 3.439) * 0.18);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 9;
\t\t\t}
\t\t\t
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 30 ) { value$ *= 1.6; }
\t\t\t\telse { value$ *= 1.8; }
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 37 ) { value$ *= 1.3; }
\t\t\t\telse { value$ *= 1.45; }
\t\t\t}
\t\t}
\t\telse if ( item_specific_type$ == PT_HELM )
\t\t{
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / MAGE_ARMOR_CONST$;
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / RANGER_ARMOR_CONST$;
\t\t\t}
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\t
\t\t\tif ( tot$ >= 68 )
\t\t\t{
\t\t\t\texp$ = 3.6 - (tot$ / 400);
\t\t\t\tif ( exp$ < 3.05 ) { exp$ = 3.05; }
\t\t\t\tvalue$ = 7 + (((tot$ * 8.31) * 0.092) ** exp$) * 0.12;
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = 7 + ((((tot$ * 8.31) * 0.092) ** 3.439) * 0.12);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 6;
\t\t\t}
\t\t\t
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 19 ) { value$ *= 1.6; }
\t\t\t\telse { value$ *= 1.8; }
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 23 ) { value$ *= 1.3; }
\t\t\t\telse { value$ *= 1.45; }
\t\t\t}
\t\t}
\t\telse if ( item_specific_type$ == PT_GLOVES )
\t\t{
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / MAGE_ARMOR_CONST$;
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / RANGER_ARMOR_CONST$;
\t\t\t}
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\t
\t\t\tif ( tot$ >= 53 )
\t\t\t{
\t\t\t\texp$ = 3.6 - (tot$ / 315);
\t\t\t\tif ( exp$ < 3.05 ) { exp$ = 3.05; }
\t\t\t\tvalue$ = 2 + (((tot$ * 11.083) * 0.092) ** exp$) * 0.09;
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = 2 + ((((tot$ * 11.083) * 0.092) ** 3.439) * 0.09);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 1;
\t\t\t}
\t\t\t
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 13 ) { value$ *= 1.6; }
\t\t\t\telse { value$ *= 1.8; }
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 18 ) { value$ *= 1.3; }
\t\t\t\telse { value$ *= 1.45; }
\t\t\t}
\t\t}
\t\telse if ( item_specific_type$ == PT_BOOTS )
\t\t{
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / MAGE_ARMOR_CONST$;
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\titem_power$ = item_power$ / RANGER_ARMOR_CONST$;
\t\t\t}
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\t
\t\t\tif ( tot$ >= 20 )
\t\t\t{
\t\t\t\texp$ = 3.6 - (tot$ / 360);
\t\t\t\tif ( exp$ < 3.05 ) { exp$ = 3.05; }
\t\t\t\tvalue$ = 3 + (((tot$ * 9.5) * 0.092) ** exp$) * 0.11;
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = 3 + ((((tot$ * 9.5) * 0.092) ** 3.439) * 0.11);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 2;
\t\t\t}
\t\t\t
\t\t\tif ( item_subtype$ ~= "m" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 14 ) { value$ *= 1.6; }
\t\t\t\telse { value$ *= 1.8; }
\t\t\t}
\t\t\tif ( item_subtype$ ~= "r" )
\t\t\t{
\t\t\t\tif ( item_power$ >= 19 ) { value$ *= 1.3; }
\t\t\t\telse { value$ *= 1.45; }
\t\t\t}
\t\t}
\t\telse if ( item_material$ ~= "ro" )
\t\t{
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\tif ( tot$ >= 40 )
\t\t\t{
\t\t\t\texp$ = 3.4 - (tot$ / 860);
\t\t\t\tif ( exp$ < 2.85 ) { exp$ = 2.85; }
\t\t\t\tvalue$ = 25 + (((tot$ * 2.8) * 0.117) ** exp$);
\t\t\t}
\t\t\telse if ( item_power$ >= 0.1 )
\t\t\t{
\t\t\t\tvalue$ = 10 + (((tot$ * 2.8) * 0.117) ** 3.4);
\t\t\t}
\t\t\telse
\t\t\t{
\t\t\t\tvalue$ = 8;
\t\t\t}
\t\t}
\t\telse if ( item_subtype$ ~= "m" )
\t\t{
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\tif ( tot$ >= 62.2 )
\t\t\t{
\t\t\t\texp$ = 3.4 - (tot$ / 1600);
\t\t\t\tif ( exp$ < 2.90 ) { exp$ = 2.90; }
\t\t\t\tvalue$ = 10 + (((tot$ * 2.216) * 0.117) ** exp$);
\t\t\t}
\t\t\telse if ( item_power$ >= 10 )
\t\t\t{
\t\t\t\tvalue$ = 6 + (((tot$ * 2.216) * 0.117) ** 3.4);
\t\t\t}
\t\t\telse if ( item_power$ >= 0 )
\t\t\t{
\t\t\t\tvalue$ = 4 + (tot$ * 3);
\t\t\t}
\t\t}
\t\telse if ( item_subtype$ ~= "r" )
\t\t{
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\tif ( tot$ >= 99 )
\t\t\t{
\t\t\t\texp$ = 3.45 - (tot$ / 2500);
\t\t\t\tif ( exp$ < 2.95 ) { exp$ = 2.95; }
\t\t\t\tvalue$ = 10 + (((tot$ * 1.537) * 0.102) ** exp$);
\t\t\t}
\t\t\telse if ( item_power$ >= 11 )
\t\t\t{
\t\t\t\tvalue$ = 6 + (((tot$ * 1.537) * 0.102) ** 3.45);
\t\t\t}
\t\t\telse if ( item_power$ >= 0 )
\t\t\t{
\t\t\t\tvalue$ = 4.5 + (tot$ * 1.5);
\t\t\t}
\t\t}
\t\telse if ( item_subtype$ ~= "f" )
\t\t{
\t\t\ttot$ = item_power$ + modifier_power_0$ + modifier_power_1$;
\t\t\tif ( tot$ >= 268.3 )
\t\t\t{
\t\t\t\texp$ = 3.497 - (tot$ / 3350);
\t\t\t\tif ( exp$ < 3.00 ) { exp$ = 3.00; }
\t\t\t\tvalue$ = 8 + ((tot$ * 0.092) ** exp$);
\t\t\t}
\t\t\telse if ( item_power$ >= 35 )
\t\t\t{
\t\t\t\tvalue$ = 4 + ((tot$ * 0.092) ** 3.439);
\t\t\t}
\t\t\telse if ( item_power$ >= 16 )
\t\t\t{
\t\t\t\tvalue$ = ((tot$ - 2) * 2);
\t\t\t}
\t\t\telse if ( item_power$ >= 0 )
\t\t\t{
\t\t\t\tvalue$ = 3.5 + (tot$ * 1.5);
\t\t\t}
\t\t}
\t\telse
\t\t{
\t\t\tvalue$ = 2.0;
\t\t}
\t}

\t// add in modifier gold values (if any), smoothly tapering the 2-mod surcharge on
\t// weapons/armor so Elite rolls stay proportional to item tier instead of exploding
\tmod_gold$ = CalcModifierGoldValue$( type$, modifier_power_0$, modifier_power_1$ );
\tif ( (type$ == PT_WEAPON) || (type$ == PT_ARMOR) )
\t{
\t\tif ( mod_gold$ > 0 )
\t\t{
\t\t\tcap_mod$ = (value$ * 0.65) + 500.0;
\t\t\tif ( mod_gold$ > cap_mod$ )
\t\t\t{
\t\t\t\tex$ = mod_gold$ - cap_mod$;
\t\t\t\tmod_gold$ = cap_mod$ + ((value$ * 0.35) * (ex$ / (ex$ + cap_mod$)));
\t\t\t}
\t\t}
\t}
\tvalue$ += mod_gold$;

\t// Smooth asymptotic ceiling above 1,000,000 sell (3,000,000 buy):
\t// strictly increasing with power/level, asymptotically approaching 3,150,000 sell
\t// (9,450,000 buy at 2x shop markup) so no item exceeds max_party_gold (9,999,999).
\tif ( value$ > 1000000.0 )
\t{
\t\texcess$ = value$ - 1000000.0;
\t\tvalue$ = 1000000.0 + (2150000.0 * (excess$ / (excess$ + 3500000.0)));
\t}

\treturn ( Math.Round( value$ ) );
}

//////////////////////////////////////////////////////////////////////
""".replace("\r\n", "\n").replace("\n", "\r\n")

    out = txt[:idx] + new_calc_gold_value
    return out.encode("latin1")


def build_siege_nodes(
    reg_files: Dict[str, bytes],
    terrain_res: LazyTank,
    exp_res: LazyTank,
) -> Tuple[bytes, int]:
    """Create world/global/siege_nodes/loa_expansion/loa_expansion_nodes.gas."""
    sno_idx = SNOIndex([terrain_res, exp_res])
    entries: Dict[int, str] = {
        0xBEEF8001: "t_xxx_wal_displacer-a",
        0xBEEF8002: "t_xxx_wal_displacer_pad",
    }

    for guid in entries:
        if guid in sno_idx.global_guids:
            raise RuntimeError(f"Global siege_nodes GUID collision on 0x{guid:08x}")

    for reg in REGION_REMAP:
        nmi_key = f"world/maps/multiplayer_world/regions/{reg}/index/node_mesh_index.gas"
        nmi_txt = reg_files[nmi_key].decode("latin1")
        nmi = parse_node_mesh_index(nmi_txt)
        for guid, sno in nmi.items():
            if guid in sno_idx.global_guids and sno_idx.global_guids[guid].lower() != sno.lower():
                raise RuntimeError(
                    f"Global siege_nodes collision in {reg}: 0x{guid:08x} ({sno} vs {sno_idx.global_guids[guid]})"
                )
            if guid in entries and entries[guid].lower() != sno.lower():
                raise RuntimeError(
                    f"LOA10 NMI conflict in {reg}: 0x{guid:08x} ({sno} vs {entries[guid]})"
                )
            entries[guid] = sno

    lines = [
        "// LOA Expansion Siege Nodes (displacers + 10 LoA region node_mesh_index entries)",
        "[t:siege_nodes,n:*]",
        "{",
    ]
    for guid in sorted(entries):
        lines.append(f"\t[mesh_file*] {{ filename={entries[guid]}; guid=0x{guid:08x}; }}")
    lines.append("}")
    return ("\r\n".join(lines) + "\r\n").encode("latin1"), len(entries)


def build_swamp2gi_lever_fix(mp_map: LazyTank) -> Dict[str, bytes]:
    """Lower the floating Goblin Dungeon lever (0x08300045) and its use_point (0x08300055)
    in swamp2gi onto the stone walkway surface (y=0.356485 / y=0.4 on node 0x9f7a1006)
    across regular, veteran, and elite."""
    out: Dict[str, bytes] = {}
    patches = (
        ("interactive.gas", "0x08300045", "0.166115,0.983203,2.50956,0x9f7a1006", "0.166115,0.356485,2.50956,0x9f7a1006"),
        ("special.gas",     "0x08300055", "0.278544,0.983199,1.33057,0x9f7a1006", "0.278544,0.4,1.33057,0x9f7a1006"),
    )
    for diff in ("regular", "veteran", "elite"):
        for fn, scid, old_pos, new_pos in patches:
            key = f"world/maps/multiplayer_world/regions/swamp2gi/objects/{diff}/{fn}"
            raw = mp_map.get(key)
            if raw is None:
                raise RuntimeError(f"Missing {key} in MpWorld.dsmap")
            txt = raw.decode("latin1")
            cnt = txt.count(old_pos)
            if cnt != 1:
                raise RuntimeError(f"{key} ({scid}): expected 1 occurrence of {old_pos}, found {cnt}")
            out[key] = txt.replace(old_pos, new_pos, 1).encode("latin1")
    return out


def build_expansion_tank(game_dir: str, out_path: str) -> Tuple[int, int]:
    """Assemble and write Z_LoA_Expansion.dsres at priority 0x5500."""
    mp_map = LazyTank(os.path.join(game_dir, "Maps", "MpWorld.dsmap"))
    exp_map = LazyTank(os.path.join(game_dir, "DSLOA", "Expansion.dsmap"))
    logic_res = LazyTank(os.path.join(game_dir, "Resources", "Logic.dsres"))
    terrain_res = LazyTank(os.path.join(game_dir, "Resources", "Terrain.dsres"))
    exp_res = LazyTank(os.path.join(game_dir, "DSLOA", "Expansion.dsres"))

    try:
        files: Dict[str, bytes] = {}

        # 1. Templates (15 files, 383 templates)
        tpl_files = generate_template_files(game_dir)
        files.update(tpl_files)

        # 2. Remapped 10 LoA regions + patched oasis (424 files)
        reg_files = generate_remapped_files(game_dir)
        for path in reg_files:
            if path in files:
                raise RuntimeError(f"Duplicate output path between templates and regions: {path}")
        files.update(reg_files)

        # 2b. Vanilla UP bugfixes (swamp2gi floating lever 0x08300045 + use_point 0x08300055)
        swamp_fix = build_swamp2gi_lever_fix(mp_map)
        for path in swamp_fix:
            if path in files:
                raise RuntimeError(f"Duplicate output path for swamp2gi fix: {path}")
        files.update(swamp_fix)

        # 3. Map & global metadata merges
        files["world/maps/multiplayer_world/info/start_positions.gas"] = build_start_positions(mp_map, exp_map)
        files["world/maps/multiplayer_world/info/world_locations.gas"] = build_world_locations(mp_map, exp_map)
        files["world/maps/multiplayer_world/info/lore.gas"] = build_lore(mp_map, exp_map)
        files["world/maps/multiplayer_world/quests/quests.gas"] = build_quests(mp_map, exp_map)
        files["world/maps/multiplayer_world/index/stitch_index.gas"] = build_stitch_index(mp_map, exp_map)
        files["world/global/elevators.gas"] = build_elevators(exp_res)
        files["world/global/moods/multiplayer_world/moods1.gas"] = build_moods(logic_res, exp_res)
        files["world/global/moods/dsx_mood_manager.skrit"] = build_mood_manager(exp_res)
        files["world/contentdb/pcontent.skrit"] = build_pcontent_skrit(exp_res)
        siege_bytes, siege_count = build_siege_nodes(reg_files, terrain_res, exp_res)
        files["world/global/siege_nodes/loa_expansion/loa_expansion_nodes.gas"] = siege_bytes

        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        nfiles, size_bytes = build_tank_with_priority(
            files=files,
            out_path=out_path,
            title="Z_LoA_Expansion",
            author="Dungeon Siege MMO",
            description="Legends of Aranna Stronghold and Great Clock expansion integrated into Utraean Peninsula (multiplayer_world).",
            priority=0x5500,
        )
        print(
            f"Built {out_path}: {nfiles} files ({len(tpl_files)} template, "
            f"{len(reg_files)} region, 10 metadata; {siege_count} siege_nodes), "
            f"{size_bytes:,} bytes, priority=0x5500"
        )
        return nfiles, size_bytes
    finally:
        for tk in (mp_map, exp_map, logic_res, terrain_res, exp_res):
            tk.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Build Z_LoA_Expansion.dsres")
    parser.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    parser.add_argument("--out", default=DEFAULT_OUT_PATH)
    args = parser.parse_args()
    build_expansion_tank(args.game_dir, args.out)


if __name__ == "__main__":
    main()
