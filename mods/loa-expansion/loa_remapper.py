"""Remap 10 LoA endgame regions into multiplayer_world and wire Displacers.

This module:
1. Copies and remaps all 10 Expansion.dsmap endgame regions (a8_r1_ramparts ..
   z_a8_r1b_to_a9_r2a) into `world/maps/multiplayer_world/regions/<region>/...`
   with fresh non-colliding SCID prefixes & region GUIDs (`0x0B0` .. `0x0B9`,
   decimal `176` .. `185`), while never disturbing node GUIDs, light GUIDs, or
   decal GUIDs that happen to share those 3-hex prefixes.
2. Remaps all LoA10 templates (including `child_template_name` on generators) to
   their cloned `mmo_loa_*` / `2W_mmo_loa_*` / `3W_mmo_loa_*` counterparts from
   `loa_templates.TEMPLATE_REMAP`.
3. Converts `[check_quest_bits]` (`map_name = "multiplayer_world"`, remapped
   decimal `region = ...`), keeps `"map_expansion_*"` mood & actioninfo names
   intact, strips `a7_r3_cityinterior` from `a8_r1_ramparts/editor/stitch_helper.gas`,
   and seals Ramparts Door 7 (`0xBD7F66F6`) with blocking rubble boulders.
4. Patches Oasis (`MpWorld.dsmap` `multiplayer_world/regions/oasis`) with the
   `apply_oasis_displacer` geometry and wires a level-gated (`58/85/110`)
   Displacer (`0x07600B00`) -> Fortress Foyer Bay 1 (`0x0B200B00`).
5. Patches Fortress (`a8_r2_fortress`) with `apply_fortress_foyer_annex`
   geometry and wires Bay 1 (`0x0B200B00`) <-> Oasis and Bay 2 (`0x0B200B20`)
   <-> Clockroom (`0x0B8002A8`), rewired from Frozen Lake (`a1_r2_frozenlake`).
"""

from __future__ import annotations

import os
import re
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

from gas_parser import extract_blocks
from loa_geom import (
    SNOIndex,
    apply_fortress_foyer_annex,
    apply_oasis_displacer,
    parse_lnc,
)
from loa_templates import TEMPLATE_REMAP
from tank_io import LazyTank


# Ordered list of the 10 LoA regions -> (old 16-bit prefix, new 16-bit prefix)
REGION_REMAP: Dict[str, Tuple[int, int]] = {
    "a8_r1_ramparts":      (0x071, 0x0B0),  # 113 -> 176
    "a8_r1b_secretcave":   (0x72A, 0x0B1),  # 1834 -> 177
    "a8_r2_fortress":      (0x072, 0x0B2),  # 114 -> 178
    "a8_r2b_fortress":     (0x72B, 0x0B3),  # 1835 -> 179
    "a8_r3_tunnels":       (0x073, 0x0B4),  # 115 -> 180
    "a9_r1_mesa":          (0x081, 0x0B5),  # 129 -> 181
    "a9_r2_cave":          (0x082, 0x0B6),  # 130 -> 182
    "a9_r2a_clockdungeon": (0x82A, 0x0B7),  # 2090 -> 183
    "a9_r3_clockroom":     (0x083, 0x0B8),  # 131 -> 184
    "z_a8_r1b_to_a9_r2a":  (0x99D, 0x0B9),  # 2461 -> 185
}

OLD_PFX3_TO_NEW: Dict[str, str] = {
    f"{old:03x}": f"{new:03x}" for old, new in REGION_REMAP.values()
}
REGION_HEX_REMAP: Dict[str, str] = {
    f"0x00000{old:03x}": f"0x00000{new:03X}" for old, new in REGION_REMAP.values()
}
REGION_DEC_REMAP: Dict[int, int] = {
    old: new for old, new in REGION_REMAP.values()
}

DIFFS: Tuple[str, ...] = ("regular", "veteran", "elite")

# Oasis Displacer SCIDs (oasis max existing SCID is 0x07600A21)
OASIS_DP_SCID = 0x07600B00
OASIS_DP_LEVER = 0x07600B01
OASIS_DP_UP1 = 0x07600B02
OASIS_DP_UP2 = 0x07600B03
OASIS_DP_GATE = 0x07600B04
OASIS_DP_TRIG = 0x07600B05
OASIS_DP_SWITCH = 0x07600B06
OASIS_DP_POFF = 0x07600B07
OASIS_DP_PON = 0x07600B08
# + 0xB09..0xB0C glow emitters, 0xB0D..0xB10 particle emitters

# Fortress Annex Bay 1 (Oasis link) & Bay 2 (Clockroom link) SCIDs
# (a8_r2_fortress max remapped SCID is 0x0B2004C4)
BAY1_DP_SCID = 0x0B200B00
BAY1_DP_LEVER = 0x0B200B01
BAY1_DP_UP1 = 0x0B200B02
BAY1_DP_UP2 = 0x0B200B03
BAY1_DP_GATE = 0x0B200B04
BAY1_DP_TRIG = 0x0B200B05
BAY1_DP_SWITCH = 0x0B200B06
BAY1_DP_POFF = 0x0B200B07
BAY1_DP_PON = 0x0B200B08

BAY2_DP_SCID = 0x0B200B20
BAY2_DP_LEVER = 0x0B200B21
BAY2_DP_UP1 = 0x0B200B22
BAY2_DP_UP2 = 0x0B200B23
BAY2_DP_QBIT_ELV = 0x0B200B24
BAY2_DP_TRIG = 0x0B200B25
BAY2_DP_SWITCH = 0x0B200B26
BAY2_DP_POFF = 0x0B200B27
BAY2_DP_PON = 0x0B200B28
BAY2_DP_QBIT_LGT = 0x0B200B29
BAY2_DP_SET_QBIT = 0x0B200B2A

CLOCKROOM_DP_SCID = 0x0B8002A8

# Exact local offsets inside t_xxx_wal_displacer-a (pit) and t_xxx_wal_displacer_pad
# copied verbatim from Expansion.dsmap (a1_r2_frozenlake / a9_r3_clockroom).
_GLOW_SPECS = (
    ("0,-0.707107,0,0.707107", "2.39875,2.92572,-2.36019", "1.1"),
    ("0,1,0,0",                "-2.2874,2.87224,2.33997",  "1.5"),
    ("0,0.382683,0,-0.92388",  "-2.31364,2.88702,-2.37841","1.1"),
    ("0,1,0,0",                "2.44382,2.89297,2.36928",  "1.5"),
)
_PARTICLE_SPECS = (
    ("0,-0.999962,0,0.00872676",  "2.50589,2.6876,2.29766",   90, 0),
    ("0,-0.707107,0,-0.707107",   "-2.44004,2.71047,2.49944", 0, 90),
    ("0,0.00872681,0,0.999962",   "-2.39126,2.82689,-2.51874",0, 90),
    ("0,-0.707106,0,0.707107",    "2.52295,2.79408,-2.52833", 0, 90),
)


def _open_tanks(game_dir: str) -> Tuple[List[LazyTank], LazyTank, LazyTank]:
    """Open base VFS tanks plus Expansion.dsmap and MpWorld.dsmap."""
    paths = [
        ("Objects",   os.path.join(game_dir, "Resources", "Objects.dsres")),
        ("Logic",     os.path.join(game_dir, "Resources", "Logic.dsres")),
        ("Terrain",   os.path.join(game_dir, "Resources", "Terrain.dsres")),
        ("ExpRes",    os.path.join(game_dir, "DSLOA", "Expansion.dsres")),
        ("MpWorld",   os.path.join(game_dir, "Maps", "MpWorld.dsmap")),
        ("Expansion", os.path.join(game_dir, "DSLOA", "Expansion.dsmap")),
    ]
    opened = [LazyTank(p) for _, p in paths]
    return opened, opened[4], opened[5]


def _collect_loa10_scid_and_guid_sets(exp_tank: LazyTank) -> Tuple[Set[str], Set[str]]:
    """Collect all defined/referenced SCIDs and all forbidden node/light/decal GUIDs."""
    scids: Set[str] = set()
    forbidden: Set[str] = set()
    for reg in REGION_REMAP:
        pfx = f"world/maps/map_expansion/regions/{reg}/"
        for m in re.finditer(
            r"\bguid\s*=\s*(0x[0-9a-fA-F]{8})\b",
            exp_tank.get(pfx + "terrain_nodes/nodes.gas").decode("latin1", errors="replace"),
        ):
            forbidden.add(m.group(1).lower())
        lk = pfx + "lights/lights.gas"
        if lk in exp_tank.keys():
            for m in re.finditer(
                r"\[t:siege_light,n:(0x[0-9a-fA-F]{8})\]",
                exp_tank.get(lk).decode("latin1", errors="replace"),
            ):
                forbidden.add(m.group(1).lower())
        dk = pfx + "decals/decals.gas"
        if dk in exp_tank.keys():
            for m in re.finditer(
                r"\bguid\s*=\s*(0x[0-9a-fA-F]{8})\b",
                exp_tank.get(dk).decode("latin1", errors="replace"),
            ):
                forbidden.add(m.group(1).lower())

        for k in exp_tank.keys():
            if k.startswith(pfx + "objects/") and k.endswith(".gas"):
                txt = exp_tank.get(k).decode("latin1", errors="replace")
                for m in re.finditer(r"\[t:[^,\]]+,n:(0x[0-9a-fA-F]{8})\]", txt):
                    scids.add(m.group(1).lower())
                for m in re.finditer(r"\binitial_command\s*=\s*(0x[0-9a-fA-F]{8})\b", txt):
                    h = m.group(1).lower()
                    if h[2:5] in OLD_PFX3_TO_NEW:
                        scids.add(h)

    overlap = scids & forbidden
    if overlap:
        raise RuntimeError(f"SCID/GUID collision detected: {sorted(overlap)[:10]}")
    bad = [s for s in scids if s[2:5] not in OLD_PFX3_TO_NEW]
    if bad:
        raise RuntimeError(f"Unexpected LOA10 SCID prefix(es): {bad[:10]}")
    return scids, forbidden


def _remap_scid(scid_hex: str) -> str:
    """Remap one 10-char SCID literal (`0x{old_pfx}XXXXX` -> `0x{new_pfx}XXXXX`)."""
    low = scid_hex.lower()
    new_p3 = OLD_PFX3_TO_NEW[low[2:5]]
    return "0x" + (new_p3 + low[5:]).upper()


def _remap_region_hex_str(text: str) -> str:
    """Replace 8-hex region GUIDs inside actioninfo / fade_nodes / party_member_within_node."""
    def _sub_call(m: re.Match) -> str:
        prefix, guid = m.group(1), m.group(2).lower()
        return prefix + REGION_HEX_REMAP.get(guid, m.group(2))

    out = re.sub(
        r"(\bfade_nodes(?:_global)?\s*\(\s*)(0x[0-9a-fA-F]{8})\b",
        _sub_call,
        text,
    )
    out = re.sub(
        r"(\bparty_member_within_node\s*\(\s*)(0x[0-9a-fA-F]{8})\b",
        _sub_call,
        out,
    )

    def _sub_ai(m: re.Match) -> str:
        full = m.group(0)
        for old_h, new_h in REGION_HEX_REMAP.items():
            full = re.sub(re.escape(old_h), new_h, full, flags=re.IGNORECASE)
        return full

    out = re.sub(r"\bmoving\d+_actioninfo\s*=\s*\"[^\"]*\"", _sub_ai, out)
    return out


def _remap_check_quest_bits(text: str) -> str:
    """Switch `map_name` & decimal `region` inside `[check_quest_bits]` blocks."""
    out_parts: List[str] = []
    pos = 0
    for m, start, end, blk in extract_blocks(text, r"\[check_quest_bits\]"):
        out_parts.append(text[pos:start])
        blk2 = re.sub(
            r'(map_name\s*=\s*")map_expansion(";)',
            r'\1multiplayer_world\2',
            blk,
        )
        def _sub_reg(rm: re.Match) -> str:
            old_d = int(rm.group(2))
            new_d = REGION_DEC_REMAP.get(old_d, old_d)
            return f"{rm.group(1)}{new_d}{rm.group(3)}"

        blk2 = re.sub(r"(\bregion\s*=\s*)(\d+)(\s*;)", _sub_reg, blk2)
        out_parts.append(blk2)
        pos = end
    out_parts.append(text[pos:])
    return "".join(out_parts)


def _remap_templates_in_object_gas(text: str) -> str:
    """Remap `[t:<type>,n:...]` headers and `child_template_name = ...` values."""
    def _sub_hdr(m: re.Match) -> str:
        tname = m.group(1)
        mapped = TEMPLATE_REMAP.get(tname.lower())
        return f"[t:{mapped if mapped else tname},n:{m.group(2)}]"

    out = re.sub(r"\[t:([^,\]]+),n:(0x[0-9a-fA-F]+)\]", _sub_hdr, text)

    def _sub_child(m: re.Match) -> str:
        prefix, val, suffix = m.group(1), m.group(2), m.group(3)
        mapped = TEMPLATE_REMAP.get(val.lower())
        return f"{prefix}{mapped if mapped else val}{suffix}"

    out = re.sub(
        r'(\bchild_template_name\s*=\s*"?)([^"\s;]+)("?\s*;)',
        _sub_child,
        out,
        flags=re.IGNORECASE,
    )
    return out


def _remap_scids_in_object_gas(text: str, scids: Set[str]) -> str:
    """Replace only verified LOA10 SCIDs in an object `.gas` file (never node GUIDs)."""
    def _sub_hex(m: re.Match) -> str:
        h = m.group(0)
        if h.lower() in scids:
            return _remap_scid(h)
        return h

    return re.sub(r"\b0x[0-9a-fA-F]{8}\b", _sub_hex, text)


def _remap_snc(text: str, scids: Set[str]) -> str:
    """Remap RHS SCID values in `streamer_node_content_index.gas`, leaving LHS node GUIDs."""
    def _sub_line(m: re.Match) -> str:
        lhs, rhs = m.group(1), m.group(2)
        if rhs.lower() in scids:
            rhs = _remap_scid(rhs)
        return f"{lhs}{rhs};"

    return re.sub(
        r"(x\s+0x[0-9a-fA-F]+\s*=\s*)(0x[0-9a-fA-F]+)\s*;",
        _sub_line,
        text,
    )


def _remap_main_gas(text: str, region_name: str) -> str:
    """Update `guid` and `scid_range` in region `main.gas` (keep `mesh_range` intact)."""
    _, new_id = REGION_REMAP[region_name]
    new_hex = f"0x00000{new_id:03X}"
    out = re.sub(r"(\bx\s+guid\s*=\s*)0x[0-9a-fA-F]+(\s*;)", rf"\g<1>{new_hex}\2", text)
    out = re.sub(r"(\bx\s+scid_range\s*=\s*)0x[0-9a-fA-F]+(\s*;)", rf"\g<1>{new_hex}\2", out)
    return out


def strip_cityinterior_stitch_helper(text: str) -> str:
    """Update `source_region_guid` and remove `a7_r3_cityinterior` from stitch_helper.gas."""
    _, new_id = REGION_REMAP["a8_r1_ramparts"]
    out = re.sub(
        r"(source_region_guid\s*=\s*)0x[0-9a-fA-F]+(\s*;)",
        rf"\g<1>0x00000{new_id:03X}\2",
        text,
    )
    parts: List[str] = []
    pos = 0
    for m, start, end, _ in extract_blocks(out, r"\[t:stitch_editor,n:a7_r3_cityinterior\]"):
        parts.append(out[pos:start])
        pos = end
    parts.append(out[pos:])
    return "".join(parts)


def _append_snc_entries(snc_txt: str, entries: Sequence[Tuple[int, int]]) -> str:
    """Append `(node_guid, scid)` entries before closing `}` of `[streamer_node_content_index]`."""
    lines = [f"  x 0x{ng:08x} = 0x{scid:08X};" for ng, scid in entries]
    idx = snc_txt.rfind("}")
    if idx < 0:
        raise ValueError("Malformed streamer_node_content_index.gas")
    prefix = snc_txt[:idx]
    if not prefix.endswith("\n"):
        prefix += "\n"
    return prefix + "\r\n".join(lines) + "\r\n}\r\n"


def _format_displacer_rig(
    *,
    base_scid: int,
    pit_guid: int,
    pad_guid: int,
    region_guid: int,
    sec_dec: int,
    is_shaft2: bool,
    target_pit_guid: int,
    target_pad_guid: int,
    other_elevator_scid: int,
    loc1_down_actioninfo: str,
    loc2_down_actioninfo: str,
    world_location: str,
    mood_name: str,
    update_map_marker_scid: Optional[int] = None,
    lever_required_levels: Optional[Tuple[int, int, int]] = None,
    gate_mode: str = "party_node",  # "party_node" | "qbit_clock"
) -> Dict[str, object]:
    """Generate Displacer `.gas` object blocks & `(node_guid, scid)` index pairs.

    SCID slot offsets from `base_scid`:
      +0 elevator_instant_4s_1c
      +1 lever_glb_06_message
      +2, +3 use_point
      +4 gate object (`trigger_rec_msg_activate` or `check_quest_bits` for elevator)
      +5 trigger_generic (pad enter/leave, location, mood, light/qbit trigger)
      +6 trigger_generic (SWITCHER FOR DISP LIGHTS)
      +7 trigger_generic (TURN OFF PARTICLE SHOOTERS)
      +8 trigger_generic (TURN ON PARTICLE SHOOTERS)
      +9..+12 emt_glow_act (4)
      +13..+16 emt_particle_act (4)
      (only when `gate_mode == "qbit_clock"`):
        +9` -> wait: keep offsets non-overlapping:
        +17 check_quest_bits (for lights)
        +18 set_quest_bits (`disp_clock_used`)
    """
    elv_scid = base_scid + 0
    lev_scid = base_scid + 1
    up1_scid = base_scid + 2
    up2_scid = base_scid + 3
    gate_scid = base_scid + 4
    trig_scid = base_scid + 5
    sw_scid = base_scid + 6
    poff_scid = base_scid + 7
    pon_scid = base_scid + 8
    glow_scids = [base_scid + 9 + i for i in range(4)]
    part_scids = [base_scid + 13 + i for i in range(4)]
    qlgt_scid = base_scid + 17
    qset_scid = base_scid + 18

    if is_shaft2:
        c1, c2, e1, e2c1, e2c2, e2d = (12, 11, 2, 9, 10, None)
    else:
        c1, c2, e1, e2c1, e2c2, e2d = (9, 10, None, 12, 11, 2)

    e_lines = [
        f"[t:elevator_instant_4s_1c,n:0x{elv_scid:08X}]",
        "{",
        "\t[common] { dev_instance_text = \"DISPLACER GIZMO\"; }",
        "\t[elevator_instant_4s_1c]",
        "\t{",
        f"\t  i connect_door_location1_level1 = {c1};",
        f"\t  i connect_door_location1_level2 = {c2};",
        f"\t  i connect_door_location2_level1 = {c1};",
        f"\t  i connect_door_location2_level2 = {c2};",
        f"\t  x connect_node_location1 = 0x{pit_guid:08X};",
        f"\t  x connect_node_location2 = 0x{target_pit_guid:08X};",
        "\t  f duration_location1_moving_to_lv1 = 22;",
        "\t  f duration_location1_moving_to_lv2 = 3;",
        "\t  f duration_location2_moving_to_lv1 = 22;",
        "\t  f duration_location2_moving_to_lv2 = 3;",
        f"\t  i e2_connect_door_location1_level1 = {e2c1};",
        f"\t  i e2_connect_door_location1_level2 = {e2c2};",
        f"\t  i e2_connect_door_location2_level1 = {e2c1};",
        f"\t  i e2_connect_door_location2_level2 = {e2c2};",
    ]
    if e2d is not None:
        e_lines.extend([
            f"\t  i e2_elevator_door_location1_level1 = {e2d};",
            f"\t  i e2_elevator_door_location1_level2 = {e2d};",
            f"\t  i e2_elevator_door_location2_level1 = {e2d};",
            f"\t  i e2_elevator_door_location2_level2 = {e2d};",
        ])
    e_lines.append(f"\t  x e2_elevator_node = 0x{target_pad_guid:08X};")
    if e1 is not None:
        e_lines.extend([
            f"\t  i elevator_door_location1_level1 = {e1};",
            f"\t  i elevator_door_location1_level2 = {e1};",
            f"\t  i elevator_door_location2_level1 = {e1};",
            f"\t  i elevator_door_location2_level2 = {e1};",
        ])
    e_lines.extend([
        f"\t  x elevator_node = 0x{pad_guid:08X};",
        "\t  f location1_arrived_at_lv2_delay = 1;",
        f"\t  x location1_arrived_at_lv2_scid = 0x{poff_scid:08X};",
        f"\t\tlocation1_moving_to_lv1_actioninfo = \"{loc1_down_actioninfo}\";",
        "\t\tlocation1_moving_to_lv2_actioninfo = \"map_expansion_teleport_up\";",
        "\t  f location1_moving_to_lv2_delay = 0;",
        f"\t  x location1_moving_to_lv2_scid = 0x{pon_scid:08X};",
        f"\t\tlocation2_moving_to_lv1_actioninfo = \"{loc2_down_actioninfo}\";",
        "\t\tlocation2_moving_to_lv2_actioninfo = \"map_expansion_teleport_up\";",
        "\t\tmaterial = \"hub\";",
        f"\t  x other_elevator = 0x{other_elevator_scid:08X};",
        "\t}",
        "\t[placement]",
        "\t{",
        "\t  q orientation = 0,1,0,0;",
        f"\t  p position = 0,7.56596,0,0x{pad_guid:08x};",
        "\t}",
        "}",
    ])
    elevator_blk = "\r\n".join(e_lines)

    lev_fields = [
        "\t\toff_message = we_req_activate;",
        "\t\toff_message_2 = we_req_activate;",
        f"\t  x off_scid = 0x{gate_scid:08X};",
        f"\t  x on_scid = 0x{gate_scid:08X};",
    ]
    if lever_required_levels is not None:
        r1, r2, r3 = lever_required_levels
        lev_fields.extend([
            f"\t  f required_level = {r1};",
            f"\t  f required_level_2 = {r2};",
            f"\t  f required_level_3 = {r3};",
        ])
    lev_fields.extend([
        "\t  f reset_time = 20;",
        "\t  f reset_time_2 = 20;",
    ])
    lever_blk = "\r\n".join([
        f"[t:lever_glb_06_message,n:0x{lev_scid:08X}]",
        "{",
        "\t[common] { screen_name = \"Displacer Transport Switch\"; }",
        "\t[on_off_lever]",
        "\t{",
        *lev_fields,
        "\t}",
        "\t[placement]",
        "\t{",
        "\t  q orientation = -0.270617,0.653273,0.653273,0.270617;",
        f"\t  p position = 0.00972565,6.43187,-0.162395,0x{pad_guid:08x};",
        f"\t\tuse_point_scids = 0x{up2_scid:08x},0x{up1_scid:08x};",
        "\t}",
        "}",
    ])

    up1_blk = (
        f"[t:use_point,n:0x{up1_scid:08X}]\r\n{{\r\n\t[placement]\r\n\t{{\r\n"
        f"\t  p position = -1.17926,4.33244,-0.150321,0x{pad_guid:08x};\r\n\t}}\r\n}}"
    )
    up2_blk = (
        f"[t:use_point,n:0x{up2_scid:08X}]\r\n{{\r\n\t[placement]\r\n\t{{\r\n"
        f"\t  p position = 1.17117,4.39385,-0.124236,0x{pad_guid:08x};\r\n\t}}\r\n}}"
    )

    special_blocks = [up1_blk, up2_blk]
    snc_pairs: List[Tuple[int, int]] = [
        (pad_guid, elv_scid),
        (pad_guid, lev_scid),
        (pad_guid, up1_scid),
        (pad_guid, up2_scid),
        (pit_guid, trig_scid),
        (pit_guid, sw_scid),
        (pit_guid, poff_scid),
        (pit_guid, pon_scid),
    ]

    if gate_mode == "party_node":
        gate_blk = "\r\n".join([
            f"[t:trigger_rec_msg_activate,n:0x{gate_scid:08X}]",
            "{",
            "\t[common]",
            "\t{",
            "\t\tdev_instance_text = \"DISPLACER NODE GATE\";",
            "\t\t[instance_triggers]",
            "\t\t{",
            "\t\t\t[*]",
            "\t\t\t{",
            f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{elv_scid:08X},0f,\"default\",\"\",0);",
            "\t\t\t  b can_self_destruct = true;",
            "\t\t\t\tcondition* = receive_world_message(\"we_req_activate\",0,\"on_every_message\");",
            f"\t\t\t\tcondition* = party_member_within_node(0x{region_guid:08X},{sec_dec},-1,-1,\"wait_for_every_message\");",
            "\t\t\t  f delay = 0.000000;",
            "\t\t\t  b flip_flop = false;",
            "\t\t\t  b multi_player = true;",
            "\t\t\t  b no_trig_bits = false;",
            "\t\t\t\toccupants_group = ;",
            "\t\t\t  f reset_duration = 0.000000;",
            "\t\t\t  b single_player = true;",
            "\t\t\t  b single_shot = false;",
            "\t\t\t  b start_active = true;",
            "\t\t\t}",
            "\t\t}",
            "\t}",
            "\t[placement]",
            "\t{",
            f"\t  p position = 0,2,0,0x{pad_guid:08x};",
            "\t}",
            "}",
        ])
        special_blocks.append(gate_blk)
        snc_pairs.append((pad_guid, gate_scid))
        enter_actions = [f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{sw_scid:08X},0f,\"default\",\"every\",0);"]
    elif gate_mode == "qbit_clock":
        gate_blk = "\r\n".join([
            f"[t:check_quest_bits,n:0x{gate_scid:08X}]",
            "{",
            "\t[check_quest_bits]",
            "\t{",
            "\t\tbool_variable = \"disp_clock_used\";",
            "\t  i level = 1;",
            "\t\tmap_name = \"multiplayer_world\";",
            "\t  i object = 1;",
            f"\t  i region = {region_guid};",
            f"\t  i section = {sec_dec};",
            f"\t  x send_to_scid = 0x{elv_scid:08X};",
            "\t}",
            "\t[common] { dev_instance_text = \"QBIT CHECKER FOR ELEVATOR ACTIVATION\"; }",
            "\t[placement]",
            "\t{",
            "\t  q orientation = 0,0.99863,0,-0.0523361;",
            f"\t  p position = -0.0853618,7.79303,-1.60541,0x{pad_guid:08x};",
            "\t}",
            "}",
        ])
        qlgt_blk = "\r\n".join([
            f"[t:check_quest_bits,n:0x{qlgt_scid:08X}]",
            "{",
            "\t[check_quest_bits]",
            "\t{",
            "\t\tbool_variable = \"disp_clock_used\";",
            "\t  i level = 1;",
            "\t\tmap_name = \"multiplayer_world\";",
            "\t  i object = 1;",
            f"\t  i region = {region_guid};",
            f"\t  i section = {sec_dec};",
            f"\t  x send_to_scid = 0x{sw_scid:08X};",
            "\t}",
            "\t[common] { dev_instance_text = \"QBIT CHECKER FOR DISP LIGHTS\"; }",
            "\t[placement]",
            "\t{",
            "\t  q orientation = 0,0.743145,0,0.669131;",
            f"\t  p position = 1.45562,0,3.43178,0x{pit_guid:08x};",
            "\t}",
            "}",
        ])
        qset_blk = "\r\n".join([
            f"[t:set_quest_bits,n:0x{qset_scid:08X}]",
            "{",
            "\t[common] { dev_instance_text = \"ALLOW MULTIPLAYER PAD USE\"; }",
            "\t[placement]",
            "\t{",
            f"\t  p position = 3.63955,0,-1.26369,0x{pit_guid:08x};",
            "\t}",
            "\t[set_quest_bits] { bool_variable = \"disp_clock_used\"; }",
            "}",
        ])
        special_blocks.extend([gate_blk, qlgt_blk, qset_blk])
        snc_pairs.extend([(pad_guid, gate_scid), (pit_guid, qlgt_scid), (pit_guid, qset_scid)])
        enter_actions = [f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{qlgt_scid:08X},0.1f,\"0x4003\",\"every\",0);"]
    else:
        raise ValueError(f"Unknown gate_mode: {gate_mode}")

    if update_map_marker_scid is not None:
        enter_actions.append(
            f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{update_map_marker_scid:08X},0f,\"all_conditions\",\"every\",0);"
        )

    trig_entries = [
        "\t\t\t[*]",
        "\t\t\t{",
        *enter_actions,
        f"\t\t\t\taction* = mood_change(\"{mood_name}\");",
        "\t\t\t  b can_self_destruct = true;",
        "\t\t\t\tcondition* = party_member_within_bounding_box(2.500000,0.450000,2.500000,\"on_every_enter\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = true;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
        "\t\t\t[*]",
        "\t\t\t{",
        f"\t\t\t\taction* = send_world_message(\"we_req_deactivate\",0x{sw_scid:08X},0f,\"default\",\"every\",0);",
        "\t\t\t  b can_self_destruct = true;",
        "\t\t\t\tcondition* = party_member_within_bounding_box(2.500000,1.000000,2.500000,\"on_every_last_leave\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = true;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
        "\t\t\t[*]",
        "\t\t\t{",
        f"\t\t\t\taction* = set_player_world_location(\"{world_location}\");",
        "\t\t\t  b can_self_destruct = true;",
        "\t\t\t\tcondition* = party_member_within_bounding_box(2.500000,0.450000,2.500000,\"on_every_enter\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = false;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
    ]
    if gate_mode == "qbit_clock":
        trig_entries.extend([
            "\t\t\t[*]",
            "\t\t\t{",
            f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{qset_scid:08X},0f,\"default\",\"every\",0);",
            "\t\t\t  b can_self_destruct = true;",
            "\t\t\t\tcondition* = party_member_within_bounding_box(2.500000,1.000000,2.500000,\"on_first_enter\");",
            "\t\t\t  f delay = 0.000000;",
            "\t\t\t  b flip_flop = false;",
            "\t\t\t  b multi_player = true;",
            "\t\t\t  b no_trig_bits = false;",
            "\t\t\t\toccupants_group = ;",
            "\t\t\t  f reset_duration = 0.000000;",
            "\t\t\t  b single_player = false;",
            "\t\t\t  b single_shot = true;",
            "\t\t\t  b start_active = true;",
            "\t\t\t}",
        ])
    trig_blk = "\r\n".join([
        f"[t:trigger_generic,n:0x{trig_scid:08X}]",
        "{",
        "\t[common]",
        "\t{",
        "\t\tdev_instance_text = \"DP PAD TRIGGER\";",
        "\t\t[instance_triggers]",
        "\t\t{",
        *trig_entries,
        "\t\t}",
        "\t}",
        "\t[placement]",
        "\t{",
        "\t  q orientation = 0,-0.382683,0,0.92388;",
        f"\t  p position = 0.0337372,0,0.0333037,0x{pit_guid:08x};",
        "\t}",
        "}",
    ])

    sw_blk = "\r\n".join([
        f"[t:trigger_generic,n:0x{sw_scid:08X}]",
        "{",
        "\t[common]",
        "\t{",
        "\t\tdev_instance_text = \"SWITCHER FOR DISP LIGHTS\";",
        "\t\t[instance_triggers]",
        "\t\t{",
        "\t\t\t[*]",
        "\t\t\t{",
        *[f"\t\t\t\taction* = send_world_message(\"we_req_activate\",0x{g:08X},0f,\"default\",\"every\",0);" for g in glow_scids],
        "\t\t\t  b can_self_destruct = true;",
        "\t\t\t\tcondition* = receive_world_message(\"we_req_activate\",0,\"on_every_message\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = true;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
        "\t\t\t[*]",
        "\t\t\t{",
        *[f"\t\t\t\taction* = send_world_message(\"we_req_deactivate\",0x{g:08X},0f,\"default\",\"every\",0);" for g in glow_scids],
        "\t\t\t  b can_self_destruct = true;",
        "\t\t\t\tcondition* = receive_world_message(\"we_req_deactivate\",0,\"on_every_message\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = true;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
        "\t\t}",
        "\t}",
        "\t[placement] { p position = 3.35143,0,1.49411,0x%08x; }" % pit_guid,
        "}",
    ])

    def _msg_trig(scid: int, label: str, msg: str, pos: str) -> str:
        return "\r\n".join([
            f"[t:trigger_generic,n:0x{scid:08X}]",
            "{",
            "\t[common]",
            "\t{",
            f"\t\tdev_instance_text = \"{label}\";",
            "\t\t[instance_triggers]",
            "\t\t{",
            "\t\t\t[*]",
            "\t\t\t{",
            *[f"\t\t\t\taction* = send_world_message(\"{msg}\",0x{p:08X},0f,\"default\",\"every\",0);" for p in part_scids],
            "\t\t\t  b can_self_destruct = true;",
            "\t\t\t\tcondition* = receive_world_message(\"we_req_activate\",0,\"on_every_message\");",
            "\t\t\t  f delay = 0.000000;",
            "\t\t\t  b flip_flop = false;",
            "\t\t\t  b multi_player = true;",
            "\t\t\t  b no_trig_bits = false;",
            "\t\t\t\toccupants_group = ;",
            "\t\t\t  f reset_duration = 0.000000;",
            "\t\t\t  b single_player = true;",
            "\t\t\t  b single_shot = false;",
            "\t\t\t  b start_active = true;",
            "\t\t\t}",
            "\t\t}",
            "\t}",
            f"\t[placement] {{ p position = {pos},0x{pit_guid:08x}; }}",
            "}",
        ])

    poff_blk = _msg_trig(poff_scid, "TURN OFF PARTICLE SHOOTERS", "we_req_deactivate", "-1.39585,0,-3.43884")
    pon_blk = _msg_trig(pon_scid, "TURN ON PARTICLE SHOOTERS", "we_req_activate", "1.56682,0,-3.44845")
    special_blocks.extend([trig_blk, sw_blk, poff_blk, pon_blk])

    emitter_blocks: List[str] = []
    for g_scid, (ori, pos, scale) in zip(glow_scids, _GLOW_SPECS):
        emitter_blocks.append("\r\n".join([
            f"[t:emt_glow_act,n:0x{g_scid:08X}]",
            "{",
            "\t[common] { dev_instance_text = \"GLOW BALL DP ACTIVE INDICATOR\"; }",
            "\t[glow_emitter_act] { f blue = 1; f green = 1; f red = 1; f scale = %s; }" % scale,
            f"\t[placement] {{ q orientation = {ori}; p position = {pos},0x{pit_guid:08x}; }}",
            "}",
        ]))
        snc_pairs.append((pit_guid, g_scid))

    for p_scid, (ori, pos, xacc, zacc) in zip(part_scids, _PARTICLE_SPECS):
        emitter_blocks.append("\r\n".join([
            f"[t:emt_particle_act,n:0x{p_scid:08X}]",
            "{",
            "\t[common] { dev_instance_text = \"PARTICLE SHOOTER FOR DP USE\"; }",
            "\t[particle_emitter_act]",
            "\t{",
            "\t  f blue = 1; i count = 30; f end_radius_max = 2; f end_radius_min = 2;",
            "\t  f fade = 0.5; f green = 1; f particle_size = 0.7; f red = 1;",
            f"\t  f start_radius_max = 1; f start_radius_min = 1; f xacc = {xacc}; f yacc = 0; f zacc = {zacc}; f zvel = 0;",
            "\t}",
            f"\t[placement] {{ q orientation = {ori}; p position = {pos},0x{pit_guid:08x}; }}",
            "}",
        ]))
        snc_pairs.append((pit_guid, p_scid))

    return {
        "elevator": elevator_blk,
        "interactive": lever_blk,
        "special": "\r\n".join(special_blocks),
        "emitter": "\r\n".join(emitter_blocks),
        "snc": snc_pairs,
    }


def _rewire_clockroom_elevator(txt: str) -> str:
    """Point Clockroom `0x0B8002A8` Displacer from Frozen Lake to Fortress Bay 2."""
    out = re.sub(
        r"(\bx\s+connect_node_location2\s*=\s*)0x[0-9a-fA-F]+(\s*;)",
        r"\g<1>0xB2F00007\2",
        txt,
    )
    out = re.sub(
        r"(\bx\s+e2_elevator_node\s*=\s*)0x[0-9a-fA-F]+(\s*;)",
        r"\g<1>0xB2F00008\2",
        out,
    )
    out = re.sub(
        r"(\bx\s+other_elevator\s*=\s*)0x[0-9a-fA-F]+(\s*;)",
        rf"\g<1>0x{BAY2_DP_SCID:08X}\2",
        out,
    )
    out = out.replace(
        "\"map_expansion_a1_r2_dp_down\"",
        "\"map_expansion_default_interior_dp_down\"",
    )
    return out


def _ramparts_door7_boulders() -> Tuple[str, List[Tuple[int, int]]]:
    """Return blocking rubble blocks & SNC pairs sealing Ramparts Door 7 (0xBD7F66F6)."""
    specs = (
        (0x0B000B00, "rubble_glb_01", "0,0.258819,0,0.965926", "0.375,-12,-2.0",  "1.15"),
        (0x0B000B01, "rock_grs_03",   "0,-0.382683,0,0.92388",  "-1.05,-12,-2.05", "1.2"),
        (0x0B000B02, "rock_grs_03",   "0,0.608761,0,0.793353",  "1.75,-12,-2.05",  "1.2"),
    )
    blocks = []
    snc = []
    for scid, tmpl, ori, pos, scale in specs:
        blocks.append("\r\n".join([
            f"[t:{tmpl},n:0x{scid:08x}]",
            "{",
            f"\t[aspect] {{ f scale_multiplier = {scale}; }}",
            f"\t[placement] {{ q orientation = {ori}; p position = {pos},0xbd7f66f6; }}",
            "}",
        ]))
        snc.append((0xBD7F66F6, scid))
    return "\r\n".join(blocks), snc


def _append_blocks(orig: Optional[str], extra: str) -> str:
    base = (orig or "").rstrip()
    return (base + "\r\n" + extra.rstrip() + "\r\n") if base else (extra.rstrip() + "\r\n")


def _build_donor_lnc_map(exp_tank: LazyTank, mp_tank: LazyTank) -> Dict[int, dict]:
    donors = [
        (exp_tank, "world/maps/map_expansion/regions/a1_r2_frozenlake/"),
        (mp_tank,  "world/maps/multiplayer_world/regions/oasis/"),
        (exp_tank, "world/maps/map_expansion/regions/a8_r2_fortress/"),
    ]
    out: Dict[int, dict] = {}
    for tk, pfx in donors:
        out.update(parse_lnc(tk.get(pfx + "terrain_nodes/siege_nodes.lnc")))
    return out


def _patch_oasis_files(
    files: Dict[str, bytes],
    mp_tank: LazyTank,
    sno_idx: SNOIndex,
    donor_lnc_map: Dict[int, dict],
) -> None:
    """Patch MpWorld oasis geometry and install Oasis <-> Fortress Bay 1 Displacer."""
    src_pfx = "world/maps/multiplayer_world/regions/oasis/"
    dst_pfx = "world/maps/multiplayer_world/regions/oasis/"

    geom = apply_oasis_displacer(
        mp_tank.get(src_pfx + "terrain_nodes/nodes.gas").decode("latin1", errors="replace"),
        mp_tank.get(src_pfx + "terrain_nodes/siege_nodes.lnc"),
        mp_tank.get(src_pfx + "editor/light_locks.gas").decode("latin1", errors="replace"),
        mp_tank.get(src_pfx + "editor/logical_flags.gas").decode("latin1", errors="replace"),
        mp_tank.get(src_pfx + "index/streamer_node_index.gas").decode("latin1", errors="replace"),
        sno_idx,
        donor_lnc_map,
    )
    files[dst_pfx + "terrain_nodes/nodes.gas"] = geom["nodes.gas"].encode("latin1")
    files[dst_pfx + "terrain_nodes/siege_nodes.lnc"] = geom["siege_nodes.lnc"]
    files[dst_pfx + "editor/light_locks.gas"] = geom["light_locks.gas"].encode("latin1")
    files[dst_pfx + "editor/logical_flags.gas"] = geom["logical_flags.gas"].encode("latin1")
    files[dst_pfx + "index/streamer_node_index.gas"] = geom["streamer_node_index.gas"].encode("latin1")

    rig = _format_displacer_rig(
        base_scid=OASIS_DP_SCID,
        pit_guid=0xB0760001,
        pad_guid=0xB0760002,
        region_guid=0x9EA4ABB9,
        sec_dec=0x51,
        is_shaft2=True,
        target_pit_guid=0xB2F00005,
        target_pad_guid=0xB2F00006,
        other_elevator_scid=BAY1_DP_SCID,
        loc1_down_actioninfo="multiplayer_world_oasis_teleport",
        loc2_down_actioninfo="map_expansion_default_interior_dp_down",
        world_location="endless_dunes",
        mood_name="multiplayer_world_dunes_1",
        lever_required_levels=(58, 85, 110),
        gate_mode="party_node",
    )

    for diff in DIFFS:
        for fn in ("elevator", "interactive", "special", "emitter"):
            rel = f"objects/{diff}/{fn}.gas"
            raw = mp_tank.get(src_pfx + rel)
            orig = raw.decode("latin1", errors="replace") if raw else ""
            files[dst_pfx + rel] = _append_blocks(orig, rig[fn]).encode("latin1")
        snc_rel = f"index/{diff}/streamer_node_content_index.gas"
        snc_txt = mp_tank.get(src_pfx + snc_rel).decode("latin1", errors="replace")
        files[dst_pfx + snc_rel] = _append_snc_entries(snc_txt, rig["snc"]).encode("latin1")


def _patch_fortress_region(
    files: Dict[str, bytes],
    exp_tank: LazyTank,
    sno_idx: SNOIndex,
    donor_lnc_map: Dict[int, dict],
) -> None:
    """Apply Fortress Foyer Annex geometry + Bay 1 & Bay 2 Displacer rigs."""
    src_pfx = "world/maps/map_expansion/regions/a8_r2_fortress/"
    dst_pfx = "world/maps/multiplayer_world/regions/a8_r2_fortress/"

    geom = apply_fortress_foyer_annex(
        exp_tank.get(src_pfx + "terrain_nodes/nodes.gas").decode("latin1", errors="replace"),
        exp_tank.get(src_pfx + "terrain_nodes/siege_nodes.lnc"),
        exp_tank.get(src_pfx + "index/node_mesh_index.gas").decode("latin1", errors="replace"),
        exp_tank.get(src_pfx + "editor/light_locks.gas").decode("latin1", errors="replace"),
        exp_tank.get(src_pfx + "editor/logical_flags.gas").decode("latin1", errors="replace"),
        exp_tank.get(src_pfx + "index/streamer_node_index.gas").decode("latin1", errors="replace"),
        exp_tank.get(src_pfx + "lights/lights.gas").decode("latin1", errors="replace"),
        sno_idx,
        donor_lnc_map,
    )
    files[dst_pfx + "terrain_nodes/nodes.gas"] = geom["nodes.gas"].encode("latin1")
    files[dst_pfx + "terrain_nodes/siege_nodes.lnc"] = geom["siege_nodes.lnc"]
    files[dst_pfx + "index/node_mesh_index.gas"] = geom["node_mesh_index.gas"].encode("latin1")
    files[dst_pfx + "editor/light_locks.gas"] = geom["light_locks.gas"].encode("latin1")
    files[dst_pfx + "editor/logical_flags.gas"] = geom["logical_flags.gas"].encode("latin1")
    files[dst_pfx + "index/streamer_node_index.gas"] = geom["streamer_node_index.gas"].encode("latin1")
    files[dst_pfx + "lights/lights.gas"] = geom["lights.gas"].encode("latin1")

    bay1 = _format_displacer_rig(
        base_scid=BAY1_DP_SCID,
        pit_guid=0xB2F00005,
        pad_guid=0xB2F00006,
        region_guid=0x000000B2,
        sec_dec=0x50,
        is_shaft2=False,
        target_pit_guid=0xB0760001,
        target_pad_guid=0xB0760002,
        other_elevator_scid=OASIS_DP_SCID,
        loc1_down_actioninfo="map_expansion_default_interior_dp_down",
        loc2_down_actioninfo="multiplayer_world_oasis_teleport",
        world_location="neener_emporium",
        mood_name="map_expansion_a8_r2_fortressgoblin",
        update_map_marker_scid=0x0B2004A5,
        gate_mode="party_node",
    )
    bay2 = _format_displacer_rig(
        base_scid=BAY2_DP_SCID,
        pit_guid=0xB2F00007,
        pad_guid=0xB2F00008,
        region_guid=178,
        sec_dec=0x51,
        is_shaft2=True,
        target_pit_guid=0x82AB747C,
        target_pad_guid=0x3E9D518D,
        other_elevator_scid=CLOCKROOM_DP_SCID,
        loc1_down_actioninfo="map_expansion_default_interior_dp_down",
        loc2_down_actioninfo="map_expansion_default_interior_dp_down",
        world_location="neener_emporium",
        mood_name="map_expansion_a8_r2_fortressgoblin",
        update_map_marker_scid=0x0B2004A5,
        gate_mode="qbit_clock",
    )

    extra_snc = bay1["snc"] + bay2["snc"]
    for diff in DIFFS:
        for fn in ("elevator", "interactive", "special", "emitter"):
            rel = f"objects/{diff}/{fn}.gas"
            cur = files.get(dst_pfx + rel, b"").decode("latin1", errors="replace")
            cur = _append_blocks(cur, bay1[fn])
            cur = _append_blocks(cur, bay2[fn])
            files[dst_pfx + rel] = cur.encode("latin1")
        snc_rel = f"index/{diff}/streamer_node_content_index.gas"
        snc_txt = files[dst_pfx + snc_rel].decode("latin1", errors="replace")
        files[dst_pfx + snc_rel] = _append_snc_entries(snc_txt, extra_snc).encode("latin1")


_LOA_LOC_UPDATES: Tuple[Tuple[str, int, str, Optional[Tuple[float, float, float]]], ...] = (
    # Ramparts <-> Secret Cave / Fortress
    ("a8_r1_ramparts",      0x0B000008, "a8_r1_ramparts",      None),
    ("a8_r1_ramparts",      0x0B000480, "a8_r1_ramparts",      (30.0, 2.5, 1.5)),
    # Secret Cave <-> Fortress <-> Inner Fortress <-> Tunnels
    ("a8_r2_fortress",      0x0B2004A3, "a8_r1b_secretcave",   (0.5, 2.0, 4.0)),
    ("a8_r2_fortress",      0x0B200164, "a8_r2_fortress",      (0.5, 2.0, 4.0)),
    ("a8_r2_fortress",      0x0B20034A, "neener_emporium",     (3.5, 2.5, 3.5)),
    ("a8_r2_fortress",      0x0B200189, "neener_emporium",     (0.5, 2.0, 4.0)),
    ("a8_r2_fortress",      0x0B200193, "a8_r2_fortress",      (0.5, 2.0, 4.0)),
    ("a8_r2_fortress",      0x0B200166, "a8_r1_ramparts",      (0.5, 2.5, 4.0)),
    ("a8_r2_fortress",      0x0B20018B, "a8_r2_fortress",      (0.5, 2.5, 4.0)),
    ("a8_r2_fortress",      0x0B200225, "a8_r2b_fortress",     (0.5, 2.0, 3.5)),
    ("a8_r2_fortress",      0x0B200226, "a8_r2_fortress",      (0.5, 2.0, 3.5)),
    ("a8_r2b_fortress",     0x0B300023, "a8_r2b_fortress",     (4.0, 2.0, 0.5)),
    ("a8_r2b_fortress",     0x0B300024, "a8_r3_tunnels",       (4.0, 2.0, 0.5)),
    ("a8_r3_tunnels",       0x0B400061, "a8_r3_tunnels",       (6.0, 2.0, 0.5)),
    ("a8_r3_tunnels",       0x0B4000B6, "a8_r3_tunnels",       (6.0, 2.0, 0.5)),
    ("a8_r3_tunnels",       0x0B400018, "a8_r3_tunnels",       (2.5, 2.0, 2.5)),
    # Great Clock Act 9 regions
    ("a9_r1_mesa",          0x0B500018, "a9_r1_mesa",          (2.5, 2.0, 2.5)),
    ("a9_r1_mesa",          0x0B500079, "a9_r1_mesa",          (3.0, 2.1, 0.25)),
    ("a9_r1_mesa",          0x0B5000AB, "a9_r2_cave",          (3.0, 2.1, 0.25)),
    ("a9_r2_cave",          0x0B600161, "a9_r2_cave",          (0.5, 2.5, 6.0)),
    ("a9_r2_cave",          0x0B60015D, "a9_r2a_clockdungeon", (0.5, 2.5, 6.0)),
    ("a9_r3_clockroom",     0x0B8002A0, "a9_r3_clockroom",     (2.5, 0.45, 2.5)),
)

# New trigger_generic blocks added at unguarded spawns / stitches / elevators
_LOA_LOC_ADDITIONS: Tuple[Tuple[str, int, int, Tuple[float, float, float], Tuple[float, float, float], str], ...] = (
    # Ramparts <-> Secret Cave doorway
    ("a8_r1_ramparts",      0x0B000F01, 0xAAFD4DB6, (0.071327, -12.0, -4.5), (3.0, 2.1, 1.5), "a8_r1_ramparts"),
    ("a8_r1b_secretcave",   0x0B100F01, 0x26933CA9, (0.38, -7.95, 0.5),      (3.0, 2.5, 1.5), "a8_r1b_secretcave"),
    # Secret Cave <-> z_a8_r1b_to_a9_r2a <-> Clockdungeon shortcut
    ("a8_r1b_secretcave",   0x0B100F02, 0xF9437252, (1.5, 0.0, 0.0),         (2.0, 2.5, 2.0), "a8_r1b_secretcave"),
    ("a9_r2a_clockdungeon", 0x0B700F01, 0x53698887, (1.5, 0.0, 0.0),         (2.0, 2.5, 2.0), "a9_r2a_clockdungeon"),
    # Clockdungeon <-> Clockroom walkway
    ("a9_r2a_clockdungeon", 0x0B700F02, 0x4E6019B5, (2.0, -2.0, 1.5),        (4.0, 3.0, 3.0), "a9_r2a_clockdungeon"),
    ("a9_r3_clockroom",     0x0B800F01, 0xF525F9FA, (0.0, 0.0, 0.0),         (2.0, 2.5, 2.0), "a9_r3_clockroom"),
    # Neener's Emporium Fortress spawn pads & vendor room coverage
    ("a8_r2_fortress",      0x0B200F01, 0xF9DD8D8E, (2.65, -2.0, 4.45),      (3.0, 3.0, 3.0), "neener_emporium"),
    ("a8_r2_fortress",      0x0B200F02, 0xBC135DDE, (2.34, -2.0, 0.0),       (3.0, 3.0, 3.0), "neener_emporium"),
    ("a8_r2_fortress",      0x0B200F03, 0x5D183CC1, (0.30, -2.0, -1.22),     (2.5, 3.0, 2.5), "neener_emporium"),
    ("a8_r2_fortress",      0x0B200F04, 0x24601E22, (1.62, -2.0, -0.12),     (2.5, 3.0, 2.5), "neener_emporium"),
    ("a8_r2_fortress",      0x0B200F05, 0x01584F8F, (-0.08, 0.30, -0.29),    (6.0, 4.0, 6.0), "neener_emporium"),
)


def _format_loc_subtrigger(world_location: str, box: Tuple[float, float, float]) -> str:
    """Format one multiplayer on_every_enter set_player_world_location sub-trigger."""
    bx, by, bz = box
    return "\r\n".join([
        "\t\t\t[*]",
        "\t\t\t{",
        f"\t\t\t\taction* = set_player_world_location(\"{world_location}\");",
        "\t\t\t  b can_self_destruct = true;",
        f"\t\t\t\tcondition* = party_member_within_bounding_box({bx:.6f},{by:.6f},{bz:.6f},\"on_every_enter\");",
        "\t\t\t  f delay = 0.000000;",
        "\t\t\t  b flip_flop = false;",
        "\t\t\t  b multi_player = true;",
        "\t\t\t  b no_trig_bits = false;",
        "\t\t\t\toccupants_group = ;",
        "\t\t\t  f reset_duration = 0.000000;",
        "\t\t\t  b single_player = false;",
        "\t\t\t  b single_shot = false;",
        "\t\t\t  b start_active = true;",
        "\t\t\t}",
    ])


def _upsert_loc_subtrigger(blk: str, world_location: str, box: Optional[Tuple[float, float, float]]) -> str:
    """Update an existing set_player_world_location sub-trigger or append one to [instance_triggers]."""
    if "set_player_world_location" in blk:
        sub_matches = extract_blocks(blk, r"\[\*\]")
        parts: List[str] = []
        pos = 0
        replaced = False
        for _, s_start, s_end, s_blk in sub_matches:
            parts.append(blk[pos:s_start])
            if "set_player_world_location" in s_blk:
                s2 = re.sub(
                    r'(set_player_world_location\s*\(\s*")[^"]*("\s*\))',
                    rf"\g<1>{world_location}\2",
                    s_blk,
                )
                if box is not None:
                    bx, by, bz = box
                    s2 = re.sub(
                        r"(party_member_within_bounding_box\s*\()[^)]+(,\"on_every_enter\"\s*\))",
                        rf"\g<1>{bx:.6f},{by:.6f},{bz:.6f}\2",
                        s2,
                    )
                parts.append(s2)
                replaced = True
            else:
                parts.append(s_blk)
            pos = s_end
        parts.append(blk[pos:])
        if not replaced:
            raise RuntimeError(f"Failed to replace set_player_world_location in block:\n{blk[:200]}")
        return "".join(parts)

    assert box is not None
    it_matches = extract_blocks(blk, r"\[instance_triggers\]")
    if len(it_matches) != 1:
        raise RuntimeError(f"Expected 1 [instance_triggers] in trigger block:\n{blk[:200]}")
    _, _, it_end, _ = it_matches[0]
    close_idx = it_end - 1
    payload = _format_loc_subtrigger(world_location, box) + "\r\n\t\t"
    prefix = blk[:close_idx]
    if not prefix.endswith("\n"):
        payload = "\r\n" + payload
    return prefix + payload + blk[close_idx:]


def _format_standalone_loc_trigger(
    scid: int,
    node_guid: int,
    pos: Tuple[float, float, float],
    box: Tuple[float, float, float],
    world_location: str,
) -> str:
    """Format a standalone [t:trigger_generic,n:SCID] location trigger."""
    px, py, pz = pos
    sub = _format_loc_subtrigger(world_location, box)
    return "\r\n".join([
        f"[t:trigger_generic,n:0x{scid:08X}]",
        "{",
        "\t[common]",
        "\t{",
        f"\t\tdev_instance_text = \"MP LOC {world_location.upper()}\";",
        "\t\t[instance_triggers]",
        "\t\t{",
        sub,
        "\t\t}",
        "\t}",
        "\t[placement]",
        "\t{",
        f"\t  p position = {px:g},{py:g},{pz:g},0x{node_guid:08x};",
        "\t}",
        "}",
    ])


def _patch_loa_location_triggers(files: Dict[str, bytes]) -> None:
    """Patch existing & add missing set_player_world_location triggers across all 3 difficulties."""
    updates_by_reg: Dict[str, List[Tuple[int, str, Optional[Tuple[float, float, float]]]]] = {}
    for reg, scid, loc, box in _LOA_LOC_UPDATES:
        updates_by_reg.setdefault(reg, []).append((scid, loc, box))

    adds_by_reg: Dict[str, List[Tuple[int, int, Tuple[float, float, float], Tuple[float, float, float], str]]] = {}
    for reg, scid, node_guid, pos, box, loc in _LOA_LOC_ADDITIONS:
        adds_by_reg.setdefault(reg, []).append((scid, node_guid, pos, box, loc))

    for reg in sorted(set(updates_by_reg) | set(adds_by_reg)):
        by_scid = {scid: (loc, box) for scid, loc, box in updates_by_reg.get(reg, ())}
        new_items = adds_by_reg.get(reg, [])
        for diff in DIFFS:
            sp_key = f"world/maps/multiplayer_world/regions/{reg}/objects/{diff}/special.gas"
            snc_key = f"world/maps/multiplayer_world/regions/{reg}/index/{diff}/streamer_node_content_index.gas"
            txt = files[sp_key].decode("latin1")
            found_scids: Set[int] = set()

            if by_scid:
                parts: List[str] = []
                pos = 0
                hdr_rx = r"\[t:trigger_[^\],]+,n:(0x[0-9a-fA-F]+)\]"
                for m, b_start, b_end, blk in extract_blocks(txt, hdr_rx):
                    sc = int(m.group(1), 16)
                    parts.append(txt[pos:b_start])
                    if sc in by_scid:
                        loc, box = by_scid[sc]
                        parts.append(_upsert_loc_subtrigger(blk, loc, box))
                        found_scids.add(sc)
                    else:
                        parts.append(blk)
                    pos = b_end
                parts.append(txt[pos:])
                txt = "".join(parts)

            missing = set(by_scid) - found_scids
            if missing:
                raise RuntimeError(
                    f"Missing expected location trigger SCID(s) in {reg}/{diff}: "
                    + ", ".join(f"0x{s:08X}" for s in sorted(missing))
                )

            if new_items:
                snc_entries: List[Tuple[int, int]] = []
                extra_blocks: List[str] = []
                for scid, node_guid, pos_xyz, box, loc in new_items:
                    if re.search(rf"\bn:0x{scid:08X}\b", txt, re.IGNORECASE):
                        raise RuntimeError(f"Location trigger SCID collision 0x{scid:08X} in {reg}/{diff}")
                    extra_blocks.append(_format_standalone_loc_trigger(scid, node_guid, pos_xyz, box, loc))
                    snc_entries.append((node_guid, scid))
                txt = _append_blocks(txt, "\r\n".join(extra_blocks))
                snc_txt = files[snc_key].decode("latin1")
                files[snc_key] = _append_snc_entries(snc_txt, snc_entries).encode("latin1")

            files[sp_key] = txt.encode("latin1")


def _rewire_clockroom_trigger(text: str) -> str:
    """Anchor Clockroom Displacer trigger `0x0B8002A0` on static pit `0x82AB747C` (not moving pad)."""
    parts: List[str] = []
    pos = 0
    found = False
    for _, start, end, blk in extract_blocks(text, r"\[t:trigger_generic,n:0x0B8002A0\]"):
        parts.append(text[pos:start])
        blk2, n_pos = re.subn(
            r"(p\s+position\s*=\s*)[^;]*\b0x3[eE]9[dD]518[dD](\s*;)",
            r"\g<1>0.0337372,0,0.0333037,0x82ab747c\2",
            blk,
        )
        if n_pos != 1:
            raise RuntimeError("Failed to re-anchor 0x0B8002A0 placement from 0x3e9d518d to 0x82ab747c")
        blk2 = re.sub(
            r'(party_member_within_bounding_box\s*\(\s*2\.500000\s*,\s*)1\.000000(\s*,\s*2\.500000\s*,\s*"on_every_enter"\s*\))',
            r"\g<1>0.450000\2",
            blk2,
        )
        parts.append(blk2)
        pos = end
        found = True
    parts.append(text[pos:])
    if not found:
        raise RuntimeError("Clockroom trigger 0x0B8002A0 not found in special.gas")
    return "".join(parts)


def generate_remapped_files(game_dir: str) -> Dict[str, bytes]:
    """Generate all remapped LoA10 region files plus patched Oasis & Fortress."""
    tanks, mp_tank, exp_tank = _open_tanks(game_dir)
    try:
        scids, _forbidden = _collect_loa10_scid_and_guid_sets(exp_tank)
        sno_idx = SNOIndex(tanks)
        donor_lnc_map = _build_donor_lnc_map(exp_tank, mp_tank)

        files: Dict[str, bytes] = {}
        boulder_txt, boulder_snc = _ramparts_door7_boulders()

        for reg in REGION_REMAP:
            src_pfx = f"world/maps/map_expansion/regions/{reg}/"
            dst_pfx = f"world/maps/multiplayer_world/regions/{reg}/"
            for k in exp_tank.keys():
                if not k.startswith(src_pfx):
                    continue
                rel = k[len(src_pfx):]
                if rel.endswith("dir.lqd20"):
                    # Omit precompiled LQD20 directory caches so remapped GAS loads cleanly.
                    continue

                raw = exp_tank.get(k)
                if not rel.endswith(".gas"):
                    files[dst_pfx + rel] = raw
                    continue

                txt = raw.decode("latin1", errors="replace")
                if rel == "main.gas":
                    txt = _remap_main_gas(txt, reg)
                elif rel == "editor/stitch_helper.gas":
                    if reg == "a8_r1_ramparts":
                        txt = strip_cityinterior_stitch_helper(txt)
                    else:
                        _, new_id = REGION_REMAP[reg]
                        txt = re.sub(
                            r"(source_region_guid\s*=\s*)0x[0-9a-fA-F]+(\s*;)",
                            rf"\g<1>0x00000{new_id:03X}\2",
                            txt,
                        )
                elif rel.startswith("index/") and rel.endswith("streamer_node_content_index.gas"):
                    txt = _remap_snc(txt, scids)
                    if reg == "a9_r3_clockroom":
                        txt, n_snc = re.subn(
                            r"(\bx\s+)0x3[eE]9[dD]518[dD](\s*=\s*0x0B8002A0\s*;)",
                            r"\g<1>0x82ab747c\2",
                            txt,
                        )
                        if n_snc != 1:
                            raise RuntimeError(f"Expected 0x3e9d518d = 0x0B8002A0 in {reg}/{rel}, got {n_snc}")
                    if reg == "a8_r1_ramparts":
                        txt = _append_snc_entries(txt, boulder_snc)
                elif rel.startswith("objects/"):
                    txt = _remap_templates_in_object_gas(txt)
                    txt = _remap_region_hex_str(txt)
                    txt = _remap_check_quest_bits(txt)
                    txt = _remap_scids_in_object_gas(txt, scids)
                    if reg == "a9_r3_clockroom" and rel.endswith("/elevator.gas"):
                        txt = _rewire_clockroom_elevator(txt)
                    if reg == "a9_r3_clockroom" and rel.endswith("/special.gas"):
                        txt = _rewire_clockroom_trigger(txt)
                    if reg == "a8_r1_ramparts" and rel.endswith("/non_interactive.gas"):
                        txt = _append_blocks(txt, boulder_txt)

                files[dst_pfx + rel] = txt.encode("latin1")

        _patch_fortress_region(files, exp_tank, sno_idx, donor_lnc_map)
        _patch_oasis_files(files, mp_tank, sno_idx, donor_lnc_map)
        _patch_loa_location_triggers(files)
        return files
    finally:
        for tk in tanks:
            tk.close()


def _self_test() -> None:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    game_dir = os.environ.get("DSLOA_GAME_DIR", root)

    tanks, mp_tank, exp_tank = _open_tanks(game_dir)
    try:
        old_scids, forbidden = _collect_loa10_scid_and_guid_sets(exp_tank)
    finally:
        for tk in tanks:
            tk.close()

    files = generate_remapped_files(game_dir)

    # 1. Verify no old LOA10 SCID remains in objects/conversations/snc
    leaked_scids = []
    leaked_tmpl = []
    unmapped_qbits = []
    for k, raw in files.items():
        if not k.endswith(".gas"):
            continue
        txt = raw.decode("latin1", errors="replace")
        if k.startswith("world/maps/multiplayer_world/regions/oasis/"):
            continue
        if "/objects/" in k or "/conversations/" in k:
            for m in re.finditer(r"\b0x[0-9a-fA-F]{8}\b", txt):
                h = m.group(0).lower()
                if h in old_scids:
                    leaked_scids.append((k, h))
        elif k.endswith("streamer_node_content_index.gas"):
            for m in re.finditer(r"x\s+0x[0-9a-fA-F]+\s*=\s*(0x[0-9a-fA-F]+)\s*;", txt):
                h = m.group(1).lower()
                if h in old_scids:
                    leaked_scids.append((k, h))
        if "/objects/" in k:
            for m in re.finditer(r"\[t:([^,\]]+),n:", txt):
                tl = m.group(1).lower()
                if tl in TEMPLATE_REMAP or re.sub(r"^(2w_|3w_)", "", tl) in TEMPLATE_REMAP:
                    leaked_tmpl.append((k, tl))
            for m in re.finditer(r'\bchild_template_name\s*=\s*"?([^"\s;]+)', txt, re.I):
                cl = m.group(1).lower()
                if cl in TEMPLATE_REMAP:
                    leaked_tmpl.append((k, "child:" + cl))
            for _, _, _, blk in extract_blocks(txt, r"\[check_quest_bits\]"):
                if "\"map_expansion\"" in blk:
                    unmapped_qbits.append((k, blk))

    assert not leaked_scids, f"Old SCIDs leaked: {leaked_scids[:10]}"
    assert not leaked_tmpl, f"Vanilla templates not remapped: {leaked_tmpl[:10]}"
    assert not unmapped_qbits, f"Unmapped check_quest_bits: {unmapped_qbits[:3]}"

    # 2. Verify all 3 Displacers cross-link symmetrically across all 3 difficulties
    for diff in DIFFS:
        o_elv = files[f"world/maps/multiplayer_world/regions/oasis/objects/{diff}/elevator.gas"].decode("latin1")
        f_elv = files[f"world/maps/multiplayer_world/regions/a8_r2_fortress/objects/{diff}/elevator.gas"].decode("latin1")
        c_elv = files[f"world/maps/multiplayer_world/regions/a9_r3_clockroom/objects/{diff}/elevator.gas"].decode("latin1")
        assert f"0x{BAY1_DP_SCID:08X}" in o_elv and f"0x{OASIS_DP_SCID:08X}" in f_elv
        assert f"0x{BAY2_DP_SCID:08X}" in c_elv and f"0x{CLOCKROOM_DP_SCID:08X}" in f_elv
        assert "multiplayer_world_oasis_teleport" in o_elv and "multiplayer_world_oasis_teleport" in f_elv
        assert "map_expansion_a1_r2_dp_down" not in o_elv and "map_expansion_a1_r2_dp_down" not in f_elv and "map_expansion_a1_r2_dp_down" not in c_elv
        o_int = files[f"world/maps/multiplayer_world/regions/oasis/objects/{diff}/interactive.gas"].decode("latin1")
        assert "required_level = 58;" in o_int and "required_level_2 = 85;" in o_int and "required_level_3 = 110;" in o_int

    # 3. Verify Ramparts stitch_helper stripped a7_r3_cityinterior and added Door 7 rocks
    sh = files["world/maps/multiplayer_world/regions/a8_r1_ramparts/editor/stitch_helper.gas"].decode("latin1")
    assert "a7_r3_cityinterior" not in sh and "0x000000B0" in sh
    rn = files["world/maps/multiplayer_world/regions/a8_r1_ramparts/objects/regular/non_interactive.gas"].decode("latin1")
    assert "rubble_glb_01,n:0x0b000b00" in rn

    # 4. Verify LoA per-region + Neener's Emporium location triggers across all 3 difficulties
    expected_locs = {
        "neener_emporium",
        "a8_r1_ramparts",
        "a8_r1b_secretcave",
        "a8_r2_fortress",
        "a8_r2b_fortress",
        "a8_r3_tunnels",
        "a9_r1_mesa",
        "a9_r2_cave",
        "a9_r2a_clockdungeon",
        "a9_r3_clockroom",
    }
    for diff in DIFFS:
        found_locs: Set[str] = set()
        for reg in REGION_REMAP:
            raw_sp = files.get(f"world/maps/multiplayer_world/regions/{reg}/objects/{diff}/special.gas")
            if not raw_sp:
                continue
            sp_txt = raw_sp.decode("latin1")
            stale = re.findall(r'set_player_world_location\s*\(\s*"(?:ramparts|stronghold|clock)"\s*\)', sp_txt)
            assert not stale, f"Stale LoA world_location in {reg}/{diff}: {stale}"
            for lm in re.finditer(r'set_player_world_location\s*\(\s*"([^"]+)"\s*\)', sp_txt):
                found_locs.add(lm.group(1))
        missing_locs = expected_locs - found_locs
        assert not missing_locs, f"Missing world_location triggers in {diff}: {sorted(missing_locs)}"
        f_sp = files[f"world/maps/multiplayer_world/regions/a8_r2_fortress/objects/{diff}/special.gas"].decode("latin1")
        for trig_scid in (BAY1_DP_TRIG, BAY2_DP_TRIG, 0x0B20034A, 0x0B200189, 0x0B200F01, 0x0B200F05):
            assert f"0x{trig_scid:08X}" in f_sp, f"Missing Neener trigger 0x{trig_scid:08X} in {diff}"
        for reg_n, trig_scid, pit_guid in (
            ("oasis", OASIS_DP_TRIG, 0xB0760001),
            ("a8_r2_fortress", BAY1_DP_TRIG, 0xB2F00005),
            ("a8_r2_fortress", BAY2_DP_TRIG, 0xB2F00007),
            ("a9_r3_clockroom", 0x0B8002A0, 0x82AB747C),
        ):
            sp_dp = files[f"world/maps/multiplayer_world/regions/{reg_n}/objects/{diff}/special.gas"].decode("latin1")
            dp_m = extract_blocks(sp_dp, rf"\[t:trigger_generic,n:0x{trig_scid:08x}\]")
            assert len(dp_m) == 1, f"Expected 1 DP trigger 0x{trig_scid:08X} in {reg_n}/{diff}"
            assert f",0x{pit_guid:08x};" in dp_m[0][3].lower(), f"DP trigger 0x{trig_scid:08X} not anchored on pit 0x{pit_guid:08x}"
            assert "0.450000" in dp_m[0][3], f"DP trigger 0x{trig_scid:08X} missing ground half_y 0.45"
            if reg_n == "oasis":
                dp_low = dp_m[0][3].lower()
                assert 'set_player_world_location("endless_dunes")' in dp_low, f"Oasis DP missing endless_dunes in {diff}"
                assert 'mood_change("multiplayer_world_dunes_1")' in dp_low, f"Oasis DP missing dunes_1 mood in {diff}"
                assert 'set_player_world_location("grescal")' not in dp_low, f"Oasis DP still has grescal in {diff}"
            snc_dp = files[f"world/maps/multiplayer_world/regions/{reg_n}/index/{diff}/streamer_node_content_index.gas"].decode("latin1")
            assert re.search(rf"\bx\s+0x{pit_guid:08x}\s*=\s*0x{trig_scid:08x}\s*;", snc_dp, re.I)
            assert not re.search(rf"\bx\s+0x(?!{pit_guid:08x})[0-9a-f]+\s*=\s*0x{trig_scid:08x}\s*;", snc_dp, re.I)

    print(
        f"OK: {len(files)} region files remapped/patched across 11 regions "
        f"(10 LoA + oasis), {len(old_scids)} SCIDs remapped, "
        f"{len(forbidden)} GUIDs preserved, 0 leaks."
    )


if __name__ == "__main__":
    _self_test()
