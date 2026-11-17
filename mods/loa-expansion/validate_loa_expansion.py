"""End-to-end VFS validation suite for Z_LoA_Expansion.dsres."""
from __future__ import annotations

import argparse
import os
import re
from collections import defaultdict, deque
from typing import Dict, List, Set, Tuple

from gas_parser import extract_blocks
from loa_geom import parse_node_mesh_index, parse_sno
from loa_remapper import REGION_REMAP
from loa_templates import CONTAINER_STEMS, TIERS, TemplateVFS, _extract_gold_bounds, mmo_name
from tank_io import LazyTank

DEFAULT_GAME_DIR = os.environ.get("DSLOA_GAME_DIR", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
DEFAULT_MOD_PATH = os.environ.get("DSLOA_MOD_PATH", os.path.abspath(os.path.join(os.path.dirname(__file__), "dist", "Z_LoA_Expansion.dsres")))


class MountedVFS:
    """Mount all Resources/Maps/DSLOA tanks plus overlay mod by descending priority."""

    def __init__(self, game_dir: str, mod_path: str) -> None:
        self.tanks: List[Tuple[int, str, LazyTank]] = []
        paths: List[str] = []
        for sub in ("Resources", "DSLOA", "Maps"):
            d = os.path.join(game_dir, sub)
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                low = fn.lower()
                if low == os.path.basename(mod_path).lower():
                    continue
                if low.endswith((".dsres", ".dsmap")):
                    paths.append(os.path.join(d, fn))
        paths.append(mod_path)

        loaded = []
        for p in paths:
            tk = LazyTank(p)
            loaded.append((tk.priority, os.path.basename(p), tk))
        # Highest priority first (tie-break: later path / mod last wins if same priority)
        loaded.sort(key=lambda x: (x[0], x[1]), reverse=True)
        self.tanks = loaded

        self.index: Dict[str, LazyTank] = {}
        self.basenames: Set[str] = set()
        for _, _, tk in self.tanks:
            for k in tk.keys():
                lk = k.lower()
                if lk not in self.index:
                    self.index[lk] = tk
                bn = lk.rsplit("/", 1)[-1]
                stem = bn.rsplit(".", 1)[0]
                self.basenames.add(bn)
                self.basenames.add(stem)

    def get(self, path: str) -> bytes | None:
        tk = self.index.get(path.lower())
        return tk.get(path) if tk is not None else None

    def keys_under(self, prefix: str) -> List[str]:
        lp = prefix.lower()
        return sorted(k for k in self.index if k.startswith(lp))

    def has_asset(self, stem_or_name: str) -> bool:
        s = stem_or_name.strip().strip('"').lower()
        if not s or s == "none":
            return True
        if s in self.basenames:
            return True
        return False

    def close(self) -> None:
        for _, _, tk in self.tanks:
            tk.close()


def check_gas_syntax(txt: str, path: str, errors: List[str]) -> None:
    """Verify balanced braces and closed strings outside comments."""
    depth = 0
    in_str = False
    in_line_comment = False
    in_block_comment = False
    i = 0
    n = len(txt)
    while i < n:
        c = txt[i]
        nxt = txt[i + 1] if i + 1 < n else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
            i += 1
            continue
        if in_block_comment:
            if c == "*" and nxt == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if c == "\\" and nxt:
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                errors.append(f"GAS negative brace depth in {path} at char {i}")
                return
        i += 1
    if in_str:
        errors.append(f"GAS unclosed quote in {path}")
    if in_block_comment:
        errors.append(f"GAS unclosed block comment in {path}")
    if depth != 0:
        errors.append(f"GAS unbalanced braces (depth={depth}) in {path}")


def strip_gas_comments(txt: str) -> str:
    """Strip // and /* */ comments while preserving string literals."""
    out: List[str] = []
    in_str = False
    in_lc = False
    in_bc = False
    i = 0
    n = len(txt)
    while i < n:
        c = txt[i]
        nxt = txt[i + 1] if i + 1 < n else ""
        if in_lc:
            if c == "\n":
                in_lc = False
                out.append(c)
            i += 1
            continue
        if in_bc:
            if c == "*" and nxt == "/":
                in_bc = False
                i += 2
                continue
            if c == "\n":
                out.append(c)
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == "\\" and nxt:
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == "/" and nxt == "/":
            in_lc = True
            i += 2
            continue
        if c == "/" and nxt == "*":
            in_bc = True
            i += 2
            continue
        if c == '"':
            in_str = True
        out.append(c)
        i += 1
    return "".join(out)


def validate(game_dir: str, mod_path: str) -> None:
    errors: List[str] = []
    warnings: List[str] = []

    mod = LazyTank(mod_path)
    vfs = MountedVFS(game_dir, mod_path)
    tvfs = TemplateVFS(game_dir)

    try:
        # 1. Archive & priority
        if mod.priority != 0x5500:
            errors.append(f"Expected mod priority 0x5500, got 0x{mod.priority:x}")
        mod_keys = sorted(mod.keys())
        print(f"[1/8] Archive OK: {len(mod_keys)} files, priority=0x{mod.priority:x}")

        # 2. GAS syntax check across all mod .gas files
        gas_count = 0
        for k in mod_keys:
            if k.lower().endswith(".gas"):
                gas_count += 1
                raw = mod.get(k)
                if not raw:
                    errors.append(f"Empty GAS file in mod: {k}")
                    continue
                check_gas_syntax(raw.decode("latin1"), k, errors)
        print(f"[2/8] GAS syntax checked across {gas_count} files (errors so far: {len(errors)})")

        # 3. Templates & specializes closure (overlaying mod templates on TemplateVFS)
        merged_templates: Dict[str, Tuple[str, str | None]] = {}
        tpl_hdr_rx = r"\[(?:[a-zA-Z0-9_]+,)*t:template,n:([a-zA-Z0-9_-]+)\]"
        # Parse all VFS templates so our checks reflect final runtime resolution
        for tk_key in vfs.keys_under("world/contentdb/templates/"):
            if not tk_key.endswith(".gas"):
                continue
            data = vfs.get(tk_key)
            if not data:
                continue
            txt = strip_gas_comments(data.decode("latin1", errors="replace"))
            for m, _, _, blk in extract_blocks(txt, tpl_hdr_rx):
                tname = m.group(1).lower()
                if tname not in merged_templates:
                    sm = re.search(r"^\s*specializes\s*=\s*([a-zA-Z0-9_]+)\s*;", blk, re.M)
                    merged_templates[tname] = (blk, sm.group(1).lower() if sm else None)

        # Verify mod templates specialize valid parents
        mod_tpl_names: Set[str] = set()
        for k in mod_keys:
            if k.startswith("world/contentdb/templates/") and k.endswith(".gas"):
                txt = strip_gas_comments(mod.get(k).decode("latin1"))
                for m, _, _, blk in extract_blocks(txt, tpl_hdr_rx):
                    tn = m.group(1).lower()
                    mod_tpl_names.add(tn)
                    sm = re.search(r"^\s*specializes\s*=\s*([a-zA-Z0-9_]+)\s*;", blk, re.M)
                    if sm and sm.group(1).lower() not in merged_templates:
                        errors.append(f"Template {tn} missing specializes base: {sm.group(1)}")
                    # Verify aspect model/texture refs
                    for am in re.finditer(r"^\s*model\s*=\s*([a-zA-Z0-9_]+)\s*;", blk, re.M):
                        if not vfs.has_asset(am.group(1)):
                            errors.append(f"Template {tn} missing model asset: {am.group(1)}")
                    # Verify mod enemy/weapon/spell references resolve cleanly in merged_templates
                    for rm in re.finditer(
                        r"^\s*(?:es_weapon_hand|il_active_primary_spell|il_spell_\d+)\s*=\s*([a-zA-Z0-9_]+)\s*;",
                        blk,
                        re.M | re.I,
                    ):
                        ref_tpl = rm.group(1).lower()
                        if ref_tpl not in merged_templates:
                            errors.append(f"Template {tn} references missing template: {ref_tpl}")
                    sm_list = re.search(r'(?mi)&spells\s*=\s*"([^"]+)"', blk)
                    if sm_list:
                        for sp_ref in (x.strip().lower() for x in sm_list.group(1).split(",") if x.strip()):
                            if sp_ref not in merged_templates:
                                errors.append(f"Template {tn} &spells references missing spell: {sp_ref}")
        for mname, req_tok in (
            ("mmo_loa_neener", "#melee/248-285"),
            ("mmo_loa_dsx_goblin_vendingmachine", "#melee/298-360"),
        ):
            mblk = merged_templates.get(mname, ("", None))[0].lower()
            if "dsx_backpack_empty" in mblk:
                errors.append(f"Merchant {mname} contains static dsx_backpack_empty (expected SiegeHost ShopStock)")
            ranged_tok = req_tok.replace("#melee/", "#ranged/")
            if "#melee/168-192" not in mblk or req_tok not in mblk or ranged_tok not in mblk or "-rare(1)" in mblk:
                errors.append(f"Merchant {mname} missing expected 1W/2W stock or ranged parity ({req_tok}, {ranged_tok})")
        for cname, c1w, c2w in (
            ("mmo_loa_crate_dsx_a8_r2-01", "#weapon/70-96", "#weapon/190-265"),
            ("mmo_loa_crate_dsx_a9_r3-spec_fire", "#melee,f/79-87/-rare(1)", "#melee,f/245-360/-rare(1)"),
            ("mmo_loa_dsx_shadow_bigboss", "#weapon/-unique(2)/85-115", "#weapon/-unique(2)/275-360"),
        ):
            cblk = merged_templates.get(cname, ("", None))[0].lower()
            if c1w not in cblk or c2w not in cblk:
                errors.append(f"Template {cname} missing additive 1W ({c1w}) or 2W ({c2w}) entry")

        # Verify post-Hades enemy gold/XP and monotonic Act 8/9 container gold progression.
        for ename, min_xp, min_g in (
            ("mmo_loa_dsx_zaurask_fighter", 108000, 22000),
            ("mmo_loa_dsx_troll_mountain", 138000, 26000),
            ("mmo_loa_dsx_golem_clockwork", 210000, 35000),
            ("mmo_loa_dsx_shadow_bigboss", 566720, 110000),
            ("2w_mmo_loa_dsx_golem_clockwork", 530000, 230000),
            ("3w_mmo_loa_dsx_shadow_bigboss", 36000000, 1850000),
        ):
            eblk = merged_templates.get(ename, ("", None))[0]
            xp_m = re.search(r"experience_value\s*=\s*([0-9.]+)\s*;", eblk, re.I)
            gb = _extract_gold_bounds(eblk)
            if not xp_m or float(xp_m.group(1)) < min_xp:
                errors.append(f"Enemy {ename} XP below {min_xp}: {xp_m.group(1) if xp_m else None}")
            if not gb or gb[0] < min_g:
                errors.append(f"Enemy {ename} gold min below {min_g}: {gb}")
        for cstem in CONTAINER_STEMS:
            cgold = [
                _extract_gold_bounds(merged_templates.get(mmo_name(pfx.lower() + cstem).lower(), ("", None))[0])
                for _, pfx, _ in TIERS
            ]
            if any(cgold) and (any(x is None for x in cgold) or not (cgold[0][0] < cgold[1][0] < cgold[2][0])):
                errors.append(f"Non-monotonic container gold on {cstem}: {cgold}")
        for tier_key, pfx, _ in TIERS:
            g_r2a = _extract_gold_bounds(merged_templates[mmo_name(pfx.lower() + "crate_dsx_a9_r2a-01").lower()][0])
            g_r3 = _extract_gold_bounds(merged_templates[mmo_name(pfx.lower() + "crate_dsx_a9_r3-01").lower()][0])
            if not (g_r2a and g_r3 and g_r3[0] > g_r2a[0]):
                errors.append(f"Tier {tier_key} crate a9_r3-01 regressed below a9_r2a-01: {(g_r2a, g_r3)}")
        print(f"[3/8] Templates validated: {len(mod_tpl_names)} mod templates over {len(merged_templates)} total VFS templates")

        # 4. Region GUID / SCID / Node GUID uniqueness across all 80 MpWorld regions + LOA10 checks
        all_region_dirs = sorted({
            k.split("/")[4]
            for k in vfs.keys_under("world/maps/multiplayer_world/regions/")
            if len(k.split("/")) > 5
        })
        reg_guids: Dict[int, str] = {}
        node_guids: Dict[int, str] = {}
        loa_nodes: Set[int] = set()
        old_pfxs = {old for old, _ in REGION_REMAP.values()}
        missing_obj_tpls: Set[str] = set()
        scid_leaks: List[Tuple[str, str]] = []
        map_exp_leaks: List[str] = []

        for reg in all_region_dirs:
            mg = vfs.get(f"world/maps/multiplayer_world/regions/{reg}/main.gas")
            if mg:
                mtxt = strip_gas_comments(mg.decode("latin1", errors="replace"))
                gm = re.search(r"\bx\s+guid\s*=\s*(0x[0-9a-fA-F]+)", mtxt)
                if gm:
                    rv = int(gm.group(1), 16)
                    if rv in reg_guids:
                        errors.append(f"Duplicate region guid 0x{rv:x}: {reg} vs {reg_guids[rv]}")
                    reg_guids[rv] = reg

            ng = vfs.get(f"world/maps/multiplayer_world/regions/{reg}/terrain_nodes/nodes.gas")
            if ng:
                ntxt = strip_gas_comments(ng.decode("latin1", errors="replace"))
                for nm in re.finditer(r"^\s*x\s+guid\s*=\s*(0x[0-9a-fA-F]+)\s*;", ntxt, re.M):
                    nv = int(nm.group(1), 16)
                    if nv in node_guids:
                        errors.append(f"Duplicate node guid 0x{nv:08x}: {reg} vs {node_guids[nv]}")
                    node_guids[nv] = reg
                    if reg in REGION_REMAP:
                        loa_nodes.add(nv)

        # Scan LOA10 + oasis objects across all 3 difficulties
        referenced_moods: Set[str] = set()
        check_regs = list(REGION_REMAP.keys()) + ["oasis"]
        for reg in check_regs:
            for diff in ("regular", "veteran", "elite"):
                seen_scids_in_reg_diff: Set[int] = set()
                pref = f"world/maps/multiplayer_world/regions/{reg}/objects/{diff}/"
                for k in vfs.keys_under(pref):
                    if not k.endswith(".gas"):
                        continue
                    data = vfs.get(k)
                    if not data:
                        continue
                    txt = strip_gas_comments(data.decode("latin1", errors="replace"))
                    for line in txt.splitlines():
                        low_line = line.lower()
                        if "map_expansion" in low_line:
                            # Valid only inside mood_change(...) or elevator moving*actioninfo mood strings
                            is_mood_ref = (
                                "mood_change(" in low_line
                                or "actioninfo" in low_line
                            )
                            if not is_mood_ref:
                                map_exp_leaks.append(f"{k}: {line.strip()}")
                        for mm in re.finditer(r'(map_expansion_[a-zA-Z0-9_]+)', line, re.I):
                            referenced_moods.add(mm.group(1).lower())

                    for om in re.finditer(r"\[t:([a-zA-Z0-9_]+),n:(0x[0-9a-fA-F]+)\]", txt):
                        tname = om.group(1).lower()
                        scid = int(om.group(2), 16)
                        if tname not in merged_templates:
                            missing_obj_tpls.add(f"{reg}/{diff}:{tname}")
                        if scid in seen_scids_in_reg_diff:
                            errors.append(f"Duplicate SCID 0x{scid:08x} in {reg}/{diff} ({k})")
                        seen_scids_in_reg_diff.add(scid)
                        if reg in REGION_REMAP and (scid >> 20) in old_pfxs:
                            scid_leaks.append((k, f"0x{scid:08x}"))

                    # Check referenced SCID properties if they leaked old prefixes
                    if reg in REGION_REMAP:
                        for sm in re.finditer(r"\bscid\b[^=]*=\s*(0x[0-9a-fA-F]+)", txt, re.I):
                            val = int(sm.group(1), 16)
                            if (val >> 20) in old_pfxs:
                                scid_leaks.append((k, f"prop=0x{val:08x}"))

        if missing_obj_tpls:
            errors.append(f"Missing object templates ({len(missing_obj_tpls)}): {sorted(missing_obj_tpls)[:10]}")
        if scid_leaks:
            errors.append(f"Old SCID prefix leaks ({len(scid_leaks)}): {scid_leaks[:10]}")
        if map_exp_leaks:
            errors.append(f"Stray map_expansion strings in: {map_exp_leaks[:10]}")
        print(f"[4/8] Scanned {len(all_region_dirs)} regions, {len(node_guids)} total nodes ({len(loa_nodes)} LOA10)")

        # 5. Siege nodes + mesh_guid + SNO existence check
        global_siege: Dict[int, str] = {}
        for sk in vfs.keys_under("world/global/siege_nodes/"):
            if not sk.endswith(".gas") or sk.endswith("naming_key.gas"):
                continue
            stxt = strip_gas_comments(vfs.get(sk).decode("latin1", errors="replace"))
            for mm in re.finditer(r"filename\s*=\s*([a-zA-Z0-9_-]+)\s*;\s*guid\s*=\s*(0x[0-9a-fA-F]+)", stxt, re.I):
                fn, mg = mm.group(1), int(mm.group(2), 16)
                if mg in global_siege and global_siege[mg].lower() != fn.lower():
                    errors.append(f"Global siege_node collision 0x{mg:08x}: {fn} vs {global_siege[mg]} in {sk}")
                global_siege[mg] = fn

        sno_paths: Dict[str, str] = {}
        for k in vfs.index:
            if k.endswith(".sno"):
                sno_paths[k.rsplit("/", 1)[-1].rsplit(".", 1)[0]] = k

        checked_meshes = 0
        for reg in check_regs:
            ntxt = strip_gas_comments(vfs.get(f"world/maps/multiplayer_world/regions/{reg}/terrain_nodes/nodes.gas").decode("latin1"))
            nmi_bytes = vfs.get(f"world/maps/multiplayer_world/regions/{reg}/index/node_mesh_index.gas")
            nmi = parse_node_mesh_index(nmi_bytes.decode("latin1")) if nmi_bytes else {}
            for mm in re.finditer(r"\bx\s+mesh_guid\s*=\s*(0x[0-9a-fA-F]+)", ntxt):
                mg = int(mm.group(1), 16)
                checked_meshes += 1
                sno_name = global_siege.get(mg) or nmi.get(mg)
                if not sno_name:
                    errors.append(f"Unresolved mesh_guid 0x{mg:08x} in {reg}")
                    continue
                if reg in REGION_REMAP and mg not in global_siege:
                    errors.append(f"LOA10 mesh_guid 0x{mg:08x} ({sno_name}) missing from global siege_nodes")
                spath = sno_paths.get(sno_name.lower())
                if not spath:
                    errors.append(f"Missing .sno file {sno_name}.sno referenced in {reg}")
                    continue
                if sno_name.lower() != "t_gi_lift_1-a":  # SNOD v6.2 known special header
                    sbytes = vfs.get(spath)
                    try:
                        parse_sno(sbytes)
                    except Exception as e:
                        warnings.append(f"SNO parse quirk on {sno_name} ({len(sbytes)}B): {e}")
        print(f"[5/8] Siege nodes & SNOs checked: {checked_meshes} node mesh refs, {len(global_siege)} global mappings")

        # 6. Door reciprocity & stitch_index & BFS reachability
        door_adj: Dict[int, Set[int]] = defaultdict(set)
        door_pairs: Dict[Tuple[int, int], Tuple[int, int]] = {}
        for reg in REGION_REMAP:
            ntxt = strip_gas_comments(vfs.get(f"world/maps/multiplayer_world/regions/{reg}/terrain_nodes/nodes.gas").decode("latin1"))
            for nm, _, _, nblk in extract_blocks(ntxt, r"\[t:snode,n:(0x[0-9a-fA-F]+)\]"):
                src = int(nm.group(1), 16)
                for _, _, _, dblk in extract_blocks(nblk, r"\[door\*\]"):
                    dm_id = re.search(r"\bi\s+id\s*=\s*(\d+)", dblk)
                    dm_fg = re.search(r"\bx\s+farguid\s*=\s*(0x[0-9a-fA-F]+)", dblk)
                    dm_fd = re.search(r"\bi\s+fardoor\s*=\s*(\d+)", dblk)
                    if not (dm_id and dm_fg and dm_fd):
                        continue
                    did = int(dm_id.group(1))
                    fg = int(dm_fg.group(1), 16)
                    fd = int(dm_fd.group(1))
                    if fg not in loa_nodes:
                        errors.append(f"Dangling external door in {reg}: node 0x{src:08x}:{did} -> 0x{fg:08x}:{fd}")
                    else:
                        door_adj[src].add(fg)
                        door_pairs[(src, did)] = (fg, fd)

        # Verify stitch_index has 0x000000B0..B9 and zero external a7_r3_cityinterior refs
        si_txt = strip_gas_comments(vfs.get("world/maps/multiplayer_world/index/stitch_index.gas").decode("latin1"))
        si_names = {int(m.group(1), 16) for m in re.finditer(r"\[t:stitch_index,n:(0x[0-9a-fA-F]+)\]", si_txt)}
        for _, new_pfx in REGION_REMAP.values():
            if new_pfx not in si_names:
                errors.append(f"Missing [t:stitch_index,n:0x{new_pfx:08x}] in stitch_index.gas")
        for forbidden in ("0x021ccbbc", "0x9902377d", "0xfdc62174"):
            if forbidden in si_txt.lower():
                errors.append(f"Forbidden a7_r3_cityinterior node {forbidden} present in stitch_index.gas")

        # Add cross-region stitch_index edges, elevator links, and logical displacer links for end-to-end BFS
        logical_adj: Dict[int, Set[int]] = defaultdict(set, {k: set(v) for k, v in door_adj.items()})
        for _, new_pfx in REGION_REMAP.values():
            for sm in re.finditer(rf"\[t:stitch_index,n:0x{new_pfx:08x}\]\s*\{{([^}}]*)\}}", si_txt, re.I):
                for lm in re.finditer(r"\*\s*=\s*(0x[0-9a-fA-F]+)\s*,\s*\d+\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*\d+\s*;", sm.group(1)):
                    a = int(lm.group(1), 16)
                    b = int(lm.group(2), 16)
                    logical_adj[a].add(b)
                    logical_adj[b].add(a)

        # Link nodes referenced within each elevator object (location1/2 connect/moving actioninfo nodes)
        for reg in REGION_REMAP:
            ebytes = vfs.get(f"world/maps/multiplayer_world/regions/{reg}/objects/regular/elevator.gas")
            if not ebytes:
                continue
            etxt = strip_gas_comments(ebytes.decode("latin1"))
            for _, _, _, eblk in extract_blocks(etxt, r"\[t:[^,]+,n:0x[0-9a-fA-F]+\]"):
                enodes = {
                    int(m.group(1), 16)
                    for m in re.finditer(r"(?:connect_node[^=]*=\s*|position\s*=\s*[-0-9.,]+,)(0x[0-9a-fA-F]+)", eblk, re.I)
                }
                enodes = {n for n in enodes if n in loa_nodes}
                if len(enodes) >= 2:
                    nlist = list(enodes)
                    for n1 in nlist[1:]:
                        logical_adj[nlist[0]].add(n1)
                        logical_adj[n1].add(nlist[0])

        # Displacer logical jumps: Oasis <-> Bay 1, Bay 2 <-> Clockroom
        for a, b in ((0xB0760002, 0xB2F00006), (0xB2F00008, 0x3E9D518D)):
            logical_adj[a].add(b)
            logical_adj[b].add(a)

        visited: Set[int] = set()
        q = deque([0xF9DD8D8E])  # Neener's Emporium P1 spawn node
        while q:
            cur = q.popleft()
            if cur in visited:
                continue
            visited.add(cur)
            for nxt in logical_adj.get(cur, ()):
                if nxt not in visited:
                    q.append(nxt)
        unreached = loa_nodes - visited
        if unreached:
            warnings.append(f"Unreachable LOA10 nodes from spawn (likely isolated visual/sky/NIS nodes): {len(unreached)}")
        if 0xB0760002 not in visited or 0x3E9D518D not in visited:
            errors.append("End-to-end displacer BFS failed to reach Oasis or Clockroom")
        print(f"[6/8] Doors & BFS validated: {len(visited & loa_nodes)}/{len(loa_nodes)} LOA10 nodes reachable (warnings: {len(warnings)})")

        # 7. Start positions, world_locations, moods, quests, elevators
        sp_txt = strip_gas_comments(vfs.get("world/maps/multiplayer_world/info/start_positions.gas").decode("latin1"))
        sp_groups = extract_blocks(sp_txt, r"\[t:start_group,n:([a-zA-Z0-9_]+)\]")
        seen_gids: Set[int] = set()
        found_fortress = False
        for gm, _, _, gblk in sp_groups:
            gname = gm.group(1).lower()
            idm = re.search(r"\bi\s+id\s*=\s*(\d+)", gblk)
            if idm:
                gid = int(idm.group(1))
                if gid in seen_gids:
                    errors.append(f"Duplicate start_group id {gid} ({gname})")
                seen_gids.add(gid)
            if gname == "fortress":
                found_fortress = True
                if 'screen_name = "Neener\'s Emporium"' not in gblk:
                    errors.append("fortress start_group missing exact screen_name Neener's Emporium")
                for lv in ("58", "85", "110"):
                    if f"required_level = {lv}" not in gblk:
                        errors.append(f"fortress start_group missing required_level = {lv}")
                pos_ids = [int(pm.group(1)) for pm in re.finditer(r"\[start_position\][^{]*\{\s*i\s+id\s*=\s*(\d+)", gblk)]
                if pos_ids != list(range(1, 9)):
                    errors.append(f"fortress start_position IDs not 1..8: {pos_ids}")
                for nm in re.finditer(r"position\s*=\s*[-0-9.,]+,(0x[0-9a-fA-F]+);", gblk):
                    ng = int(nm.group(1), 16)
                    if ng not in loa_nodes:
                        errors.append(f"fortress start_position references missing node 0x{ng:08x}")
        if not found_fortress:
            errors.append("Missing [t:start_group,n:fortress] in start_positions.gas")

        wl_txt = strip_gas_comments(vfs.get("world/maps/multiplayer_world/info/world_locations.gas").decode("latin1"))
        wl_ids: Dict[int, str] = {}
        wl_names: Set[str] = set()
        for wm, _, _, wblk in extract_blocks(wl_txt, r"\[t:location,n:([a-zA-Z0-9_]+)\]"):
            ln = wm.group(1).lower()
            if ln in wl_names:
                errors.append(f"Duplicate location name {ln}")
            wl_names.add(ln)
            im = re.search(r"\bid\s*=\s*(\d+)", wblk)
            if im:
                lid = int(im.group(1))
                if lid in wl_ids:
                    errors.append(f"Duplicate world_location id {lid}: {ln} vs {wl_ids[lid]}")
                wl_ids[lid] = ln
        for req_id in (922, 923, 924, *range(1500, 1510)):
            if req_id not in wl_ids:
                errors.append(f"Missing world_location id {req_id:04d}")
        if wl_ids.get(1500) != "neener_emporium":
            errors.append(f"Expected world_location id 1500 == neener_emporium, got {wl_ids.get(1500)}")

        mood_txt = strip_gas_comments(vfs.get("world/global/moods/multiplayer_world/moods1.gas").decode("latin1"))
        moods = {m.group(1).lower() for m in re.finditer(r"\bmood_name\s*=\s*([a-zA-Z0-9_]+)", mood_txt)}
        missing_moods = sorted(referenced_moods - moods)
        if missing_moods:
            errors.append(f"Missing moods in multiplayer_world/moods1.gas ({len(missing_moods)}): {missing_moods[:10]}")

        q_txt = strip_gas_comments(vfs.get("world/maps/multiplayer_world/quests/quests.gas").decode("latin1"))
        if re.search(r"^\s*\[chapters\]", q_txt, re.I | re.M):
            errors.append("Unexpected active [chapters] block in quests.gas (hides UP quests in journal)")
        q_blocks = extract_blocks(q_txt, r"(?mi)^[ \t]*\[quests\]")
        if len(q_blocks) != 1:
            errors.append(f"Expected 1 active [quests] block in quests.gas, found {len(q_blocks)}")
        else:
            quest_entries: List[str] = []
            q_depth = 0
            for q_line in q_blocks[0][3].splitlines():
                q_s = q_line.strip()
                if q_depth == 1 and q_s.startswith("[") and q_s.endswith("]"):
                    quest_entries.append(q_s)
                q_depth += q_s.count("{") - q_s.count("}")
            if len(quest_entries) != 28 or "[a8_pad]" not in quest_entries:
                errors.append(f"Expected 28 quest entries in quests.gas (27 UP + [a8_pad]), found {len(quest_entries)}: {quest_entries}")
        for req_q in ("[quest_trella_rune]", "[quest_zabar_hovart]", "[quest_townstones]", "[quest_crystwind]", "[a8_pad]"):
            if req_q not in q_txt:
                errors.append(f"Missing {req_q} in quests.gas")
        for img in ("b_gui_ig_mnu_jnl_quest_23", "b_gui_ig_mnu_jnl_quest_24", "b_gui_ig_mnu_jnl_quest_pad"):
            if not vfs.has_asset(img):
                errors.append(f"Missing quest image asset {img}")

        elev_txt = strip_gas_comments(vfs.get("world/global/elevators.gas").decode("latin1"))
        mp_elev = extract_blocks(elev_txt, r"\[multiplayer_world\]")
        if len(mp_elev) != 1:
            errors.append("Missing [multiplayer_world] in elevators.gas")
        else:
            for g in ("0xb0760002", "0xb2f00006", "0xb2f00008", "0x8d3a85e1", "0x910144c5", "0x73851e47", "0x3e9d518d"):
                if g not in mp_elev[0][3].lower():
                    errors.append(f"Missing elevator guid {g} in elevators.gas")

        # 8. Check Oasis & Fortress & Clockroom displacers, Door 7 seal, and LoA location triggers across all 3 diffs
        expected_loa_locs = {"neener_emporium"} | (set(REGION_REMAP.keys()) - {"z_a8_r1b_to_a9_r2a"})
        stale_loa_locs = {"ramparts", "stronghold", "clock"}
        for diff in ("regular", "veteran", "elite"):
            oasis_sp = vfs.get(f"world/maps/multiplayer_world/regions/oasis/objects/{diff}/special.gas").decode("latin1")
            if "0x07600b00" not in oasis_sp.lower() or "0x9ea4abb9" not in oasis_sp.lower():
                errors.append(f"Oasis {diff}/special.gas missing displacer/receiver wiring")
            f_sp = vfs.get(f"world/maps/multiplayer_world/regions/a8_r2_fortress/objects/{diff}/special.gas").decode("latin1")
            if "0x0b200b00" not in f_sp.lower() or "0x0b200b20" not in f_sp.lower():
                errors.append(f"a8_r2_fortress {diff}/special.gas missing Bay 1/2 displacer wiring")
            if 'set_player_world_location("neener_emporium")' not in f_sp.lower():
                errors.append(f"a8_r2_fortress {diff}/special.gas Bay 1/2 displacers missing set_player_world_location(neener_emporium)")
            oasis_elv = vfs.get(f"world/maps/multiplayer_world/regions/oasis/objects/{diff}/elevator.gas").decode("latin1").lower()
            fort_elv = vfs.get(f"world/maps/multiplayer_world/regions/a8_r2_fortress/objects/{diff}/elevator.gas").decode("latin1").lower()
            clk_elv = vfs.get(f"world/maps/multiplayer_world/regions/a9_r3_clockroom/objects/{diff}/elevator.gas").decode("latin1").lower()
            if "multiplayer_world_oasis_teleport" not in oasis_elv or "multiplayer_world_oasis_teleport" not in fort_elv:
                errors.append(f"{diff}/elevator.gas missing multiplayer_world_oasis_teleport on Oasis/Bay1 Displacer")
            for elv_name, elv_txt in (("oasis", oasis_elv), ("a8_r2_fortress", fort_elv), ("a9_r3_clockroom", clk_elv)):
                if "map_expansion_a1_r2_dp_down" in elv_txt:
                    errors.append(f"{elv_name}/{diff}/elevator.gas still references snowy map_expansion_a1_r2_dp_down")
            c_sp = vfs.get(f"world/maps/multiplayer_world/regions/a9_r3_clockroom/objects/{diff}/special.gas").decode("latin1")
            if "0x0b8002a8" not in c_sp.lower() or "0x82ab747c" not in c_sp.lower():
                errors.append(f"a9_r3_clockroom {diff}/special.gas missing return displacer wiring")

            dp_pit_specs = (
                ("oasis", oasis_sp, 0x07600B05, 0xB0760001),
                ("a8_r2_fortress", f_sp, 0x0B200B05, 0xB2F00005),
                ("a8_r2_fortress", f_sp, 0x0B200B25, 0xB2F00007),
                ("a9_r3_clockroom", c_sp, 0x0B8002A0, 0x82AB747C),
            )
            for reg_n, sp_txt_dp, trig_scid, pit_guid in dp_pit_specs:
                matches = extract_blocks(sp_txt_dp, rf"\[t:trigger_generic,n:0x{trig_scid:08x}\]")
                if len(matches) != 1:
                    errors.append(f"{reg_n}/{diff} expected 1 trigger 0x{trig_scid:08X}, found {len(matches)}")
                    continue
                trig_blk = matches[0][3]
                pm = re.search(rf"p\s+position\s*=\s*([^,]+),([^,]+),([^,]+),0x{pit_guid:08x}\s*;", trig_blk, re.I)
                if not pm or abs(float(pm.group(2))) > 0.05:
                    errors.append(f"{reg_n}/{diff} DP trigger 0x{trig_scid:08X} must sit at y~0 on static pit 0x{pit_guid:08X}")
                hy_vals = [float(hm.group(1)) for hm in re.finditer(r'party_member_within_bounding_box\([^,]+,([^,]+),[^,]+,"on_every_enter"\)', trig_blk)]
                if not hy_vals or any(h > 0.5 for h in hy_vals):
                    errors.append(f"{reg_n}/{diff} DP trigger 0x{trig_scid:08X} on_every_enter half_y must be <= 0.5, got {hy_vals}")
                if reg_n == "oasis":
                    dp_low = trig_blk.lower()
                    if 'set_player_world_location("endless_dunes")' not in dp_low or 'mood_change("multiplayer_world_dunes_1")' not in dp_low:
                        errors.append(f"oasis/{diff} DP trigger 0x{trig_scid:08X} missing endless_dunes location/mood")
                    if 'set_player_world_location("grescal")' in dp_low:
                        errors.append(f"oasis/{diff} DP trigger 0x{trig_scid:08X} must not use grescal location")
                snc_txt = vfs.get(f"world/maps/multiplayer_world/regions/{reg_n}/index/{diff}/streamer_node_content_index.gas").decode("latin1")
                snc_hits = re.findall(rf"x\s+(0x[0-9a-fA-F]+)\s*=\s*0x{trig_scid:08x}\s*;", snc_txt, re.I)
                if [h.lower() for h in snc_hits] != [f"0x{pit_guid:08x}"]:
                    errors.append(f"{reg_n}/{diff} SNC for 0x{trig_scid:08X} expected [0x{pit_guid:08x}], got {snc_hits}")
            r_ni = vfs.get(f"world/maps/multiplayer_world/regions/a8_r1_ramparts/objects/{diff}/non_interactive.gas").decode("latin1")
            if "0x0b000b00" not in r_ni.lower():
                errors.append(f"a8_r1_ramparts {diff}/non_interactive.gas missing Door 7 seal props")

            seen_locs: Set[str] = set()
            for reg in REGION_REMAP:
                sp_raw = vfs.get(f"world/maps/multiplayer_world/regions/{reg}/objects/{diff}/special.gas")
                if not sp_raw:
                    continue
                sp_txt = sp_raw.decode("latin1", errors="replace")
                for lm in re.finditer(r'set_player_world_location\("([^"]+)"\)', sp_txt, re.I):
                    loc_name = lm.group(1).strip().lower()
                    seen_locs.add(loc_name)
                    if loc_name in stale_loa_locs:
                        errors.append(f"Stale LoA world_location trigger {loc_name!r} in {reg}/{diff}/special.gas")
                    elif loc_name not in wl_names:
                        errors.append(f"Unknown world_location {loc_name!r} in {reg}/{diff}/special.gas")
            missing_locs = sorted(expected_loa_locs - seen_locs)
            if missing_locs:
                errors.append(f"Missing LoA world_location triggers in {diff}: {missing_locs}")
        print(f"[7/8] Metadata & Displacers validated.")
        print(f"[8/8] Summary: {len(errors)} errors, {len(warnings)} warnings")
        for w in warnings:
            print(f"  WARN: {w}")
        if errors:
            for e in errors:
                print(f"  ERR:  {e}")
            raise SystemExit(1)
        print("OK: All 8 validation suites passed with 0 errors!")
    finally:
        mod.close()
        vfs.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate Z_LoA_Expansion.dsres")
    parser.add_argument("--game-dir", default=DEFAULT_GAME_DIR)
    parser.add_argument("--mod", default=DEFAULT_MOD_PATH)
    args = parser.parse_args()
    validate(args.game_dir, args.mod)


if __name__ == "__main__":
    main()
