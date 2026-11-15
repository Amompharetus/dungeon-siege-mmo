"""LNC parser/serializer, SNO full decoder/BFS/AABB checker, and Displacer/Annex builders."""

from __future__ import annotations

import math
import re
import struct
from collections import deque
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from gas_parser import extract_blocks

FLIP_Y = np.diag([-1.0, 1.0, -1.0])


def _skip_bsp(data: bytes, pos: int) -> int:
    nt = struct.unpack_from("<H", data, pos + 25)[0]
    pos += 27 + nt * 2
    nc = data[pos]
    pos += 1
    for _ in range(nc):
        pos = _skip_bsp(data, pos)
    return pos


def parse_sno(data: bytes) -> dict[str, Any]:
    """Decode full .sno binary geometry, doors, vertices, and logical-node leaves."""
    magic, ver, bver, nd, ns, nvert, ntri, nsurf = struct.unpack_from("<8I", data, 0)
    mn = np.array(struct.unpack_from("<3f", data, 32), dtype=np.float64)
    mx = np.array(struct.unpack_from("<3f", data, 44), dtype=np.float64)
    pos = 88

    doors: dict[int, tuple[np.ndarray, np.ndarray]] = {}
    for _ in range(nd):
        did = struct.unpack_from("<I", data, pos)[0]
        dp = np.array(struct.unpack_from("<3f", data, pos + 4), dtype=np.float64)
        dr = np.array(struct.unpack_from("<9f", data, pos + 16), dtype=np.float64).reshape(3, 3)
        nhv = struct.unpack_from("<I", data, pos + 52)[0]
        pos += 56 + nhv * 4
        doors[did] = (dp, dr)

    spots: list[tuple[np.ndarray, np.ndarray, str]] = []
    for _ in range(ns):
        rot_p = np.array(struct.unpack_from("<9f", data, pos), dtype=np.float64).reshape(3, 3)
        pos_p = np.array(struct.unpack_from("<3f", data, pos + 36), dtype=np.float64)
        end = data.index(0, pos + 48)
        name = data[pos + 48 : end].decode("latin1")
        pos = end + 1
        spots.append((pos_p, rot_p, name))

    v_raw = np.frombuffer(data, dtype="<f4", count=nvert * 9, offset=pos).reshape(nvert, 9)
    vpos = v_raw[:, 0:3].astype(np.float64)
    vnorm = v_raw[:, 3:6].astype(np.float64)
    pos += nvert * 36

    for _ in range(nsurf):
        pos = data.index(0, pos) + 1
        _, _, ni = struct.unpack_from("<3I", data, pos)
        pos += 12 + ni * 2

    n_lns = struct.unpack_from("<I", data, pos)[0]
    pos += 4
    lns: dict[int, dict[str, Any]] = {}
    for _ in range(n_lns):
        ln_id = data[pos]
        ln_mn = np.array(struct.unpack_from("<3f", data, pos + 1), dtype=np.float64)
        ln_mx = np.array(struct.unpack_from("<3f", data, pos + 13), dtype=np.float64)
        flags, n_leaves = struct.unpack_from("<2I", data, pos + 25)
        pos += 33
        leaves = []
        for _ in range(n_leaves):
            lid = struct.unpack_from("<H", data, pos)[0]
            lmn = np.array(struct.unpack_from("<3f", data, pos + 2), dtype=np.float64)
            lmx = np.array(struct.unpack_from("<3f", data, pos + 14), dtype=np.float64)
            lc = np.array(struct.unpack_from("<3f", data, pos + 26), dtype=np.float64)
            pos += 38
            n1 = struct.unpack_from("<H", data, pos)[0]
            pos += 2 + n1 * 2
            n2 = struct.unpack_from("<H", data, pos)[0]
            pos += 2 + n2 * 2 + 2
            leaves.append((lid, lmn, lmx, lc))
        n_ll = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        for _ in range(n_ll):
            n_p = struct.unpack_from("<I", data, pos + 1)[0]
            pos += 5 + n_p * 4
        nt2 = struct.unpack_from("<I", data, pos)[0]
        pos += 4 + nt2 * 48
        pos = _skip_bsp(data, pos)
        lns[ln_id] = {"id": ln_id, "mn": ln_mn, "mx": ln_mx, "flags": flags, "leaves": leaves}

    return {
        "ver": ver,
        "bver": bver,
        "mn": mn,
        "mx": mx,
        "doors": doors,
        "nvert": nvert,
        "vpos": vpos,
        "vnorm": vnorm,
        "lns": lns,
        "parsed_end": pos,
        "raw_len": len(data),
    }


class SNOIndex:
    """Resolve global mesh GUIDs + region node_mesh_index.gas mappings to parsed SNOs."""

    def __init__(self, tanks: list[Any]):
        self.global_guids: dict[int, str] = {}
        self.sno_locations: dict[str, tuple[Any, str]] = {}
        self.cache: dict[str, dict[str, Any]] = {}
        for tk in tanks:
            for k in tk.keys():
                kl = k.lower()
                if kl.endswith(".sno"):
                    name = kl.rsplit("/", 1)[-1][:-4]
                    self.sno_locations.setdefault(name, (tk, k))
                elif kl.startswith("world/global/siege_nodes/") and kl.endswith(".gas"):
                    txt = tk.get(k).decode("latin1")
                    for _, _, _, blk in extract_blocks(txt, r"\[mesh_file\*\]"):
                        mg = re.search(r"guid\s*=\s*(0x[0-9a-fA-F]+)", blk)
                        fn = re.search(r"filename\s*=\s*([^\s;\}]+)", blk)
                        if mg and fn:
                            self.global_guids[int(mg.group(1), 16)] = fn.group(1).lower()

    def get_sno(self, mesh_name: str) -> dict[str, Any]:
        key = mesh_name.lower()
        if key not in self.cache:
            tk, path = self.sno_locations[key]
            self.cache[key] = parse_sno(tk.get(path))
        return self.cache[key]

    def resolve_mesh(self, mesh_guid: int, local_nmi: dict[int, str] | None = None) -> str:
        if local_nmi and mesh_guid in local_nmi:
            return local_nmi[mesh_guid]
        return self.global_guids[mesh_guid]


def parse_node_mesh_index(txt: str) -> dict[int, str]:
    out: dict[int, str] = {}
    for line in txt.splitlines():
        if "=" in line and "{" not in line and "}" not in line:
            lhs, rhs = line.split("=", 1)
            lhs = lhs.strip()
            if lhs.lower().startswith("0x"):
                out[int(lhs, 16)] = rhs.strip().rstrip(";").strip().lower()
    return out


def format_node_mesh_index(nmi: dict[int, str]) -> str:
    lines = ["[node_mesh_index]", "{"]
    for mg in sorted(nmi):
        lines.append(f"\t0x{mg:08X} = {nmi[mg]};")
    lines.append("}")
    return "\r\n".join(lines) + "\r\n"


@dataclass
class SNode:
    guid: int
    mesh_guid: int
    bounds_camera: bool = False
    camera_fade: bool = False
    nodelevel: int = 0xFFFFFFFF
    nodeobject: int = 0xFFFFFFFF
    nodesection: int = 0xFFFFFFFF
    occludes_camera: bool = True
    occludes_light: bool = True
    texsetabbr: str = "xxx"
    doors: dict[int, tuple[int, int]] = field(default_factory=dict)  # id -> (farguid, fardoor)
    extra_lines: list[str] = field(default_factory=list)


def parse_nodes_gas(txt: str) -> tuple[list[str], int, dict[int, SNode]]:
    """Parse terrain_nodes/nodes.gas into (header_lines, targetnode, nodes_dict)."""
    body_start = txt.index("{") + 1
    first_node = txt.find("[t:snode")
    hdr_end = first_node if first_node != -1 else txt.rindex("}")
    header_raw = txt[body_start:hdr_end]
    header_lines = [ln.rstrip("\r") for ln in header_raw.splitlines() if ln.strip()]
    tn_m = re.search(r"targetnode\s*=\s*(0x[0-9a-fA-F]+)", txt)
    targetnode = int(tn_m.group(1), 16) if tn_m else 0

    nodes: dict[int, SNode] = {}
    for _, _, _, blk in extract_blocks(txt, r"\[t:snode,n:[^\]]+\]"):
        g = int(re.search(r"x\s+guid\s*=\s*(0x[0-9a-fA-F]+)", blk).group(1), 16)
        mg = int(re.search(r"x\s+mesh_guid\s*=\s*(0x[0-9a-fA-F]+)", blk).group(1), 16)

        def _b(name: str, default: bool) -> bool:
            m = re.search(rf"\bb\s+{name}\s*=\s*(true|false)", blk)
            return (m.group(1) == "true") if m else default

        def _x(name: str, default: int) -> int:
            m = re.search(rf"\bx\s+{name}\s*=\s*(0x[0-9a-fA-F]+)", blk)
            return int(m.group(1), 16) if m else default

        tm = re.search(r"\btexsetabbr\s*=\s*([^\s;]+)", blk)
        doors: dict[int, tuple[int, int]] = {}
        for _, _, _, dblk in extract_blocks(blk, r"\[door\*\]"):
            did = int(re.search(r"\bi\s+id\s*=\s*(\d+)", dblk).group(1))
            fg = int(re.search(r"\bx\s+farguid\s*=\s*(0x[0-9a-fA-F]+)", dblk).group(1), 16)
            fd = int(re.search(r"\bi\s+fardoor\s*=\s*(\d+)", dblk).group(1))
            doors[did] = (fg, fd)
        nodes[g] = SNode(
            guid=g,
            mesh_guid=mg,
            bounds_camera=_b("bounds_camera", False),
            camera_fade=_b("camera_fade", False),
            nodelevel=_x("nodelevel", 0xFFFFFFFF),
            nodeobject=_x("nodeobject", 0xFFFFFFFF),
            nodesection=_x("nodesection", 0xFFFFFFFF),
            occludes_camera=_b("occludes_camera", True),
            occludes_light=_b("occludes_light", True),
            texsetabbr=tm.group(1) if tm else "xxx",
            doors=doors,
        )
    return header_lines, targetnode, nodes


def format_nodes_gas(header_lines: list[str], targetnode: int, nodes: dict[int, SNode]) -> str:
    lines = ["[t:terrain_nodes,n:siege_node_list]", "{"]
    has_tn = False
    for ln in header_lines:
        if re.search(r"\btargetnode\b", ln):
            lines.append(f"  x targetnode = 0x{targetnode:08X};")
            has_tn = True
        else:
            lines.append(ln)
    if not has_tn and targetnode:
        lines.append(f"  x targetnode = 0x{targetnode:08X};")
    for g, n in nodes.items():
        bstr = lambda v: "true" if v else "false"
        lines.extend([
            f"\t[t:snode,n:0x{g:08x}]",
            "\t{",
            f"\t  b bounds_camera = {bstr(n.bounds_camera)};",
            f"\t  b camera_fade = {bstr(n.camera_fade)};",
            f"\t  x guid = 0x{n.guid:08X};",
            f"\t  x mesh_guid = 0x{n.mesh_guid:08X};",
            f"\t  x nodelevel = 0x{n.nodelevel:08X};",
            f"\t  x nodeobject = 0x{n.nodeobject:08X};",
            f"\t  x nodesection = 0x{n.nodesection:08X};",
            f"\t  b occludes_camera = {bstr(n.occludes_camera)};",
            f"\t  b occludes_light = {bstr(n.occludes_light)};",
            f"\t\ttexsetabbr = {n.texsetabbr};",
        ])
        for did, (fg, fd) in sorted(n.doors.items()):
            lines.extend([
                "\t\t[door*]",
                "\t\t{",
                f"\t\t  i fardoor = {fd};",
                f"\t\t  x farguid = 0x{fg:08X};",
                f"\t\t  i id = {did};",
                "\t\t}",
            ])
        lines.append("\t}")
    lines.append("}")
    return "\r\n".join(lines) + "\r\n"


def stitch_transform(Ru: np.ndarray, tu: np.ndarray, pA: np.ndarray, rA: np.ndarray, pB: np.ndarray, rB: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Compute world (Rv, tv) for node B stitched to node A (Ru, tu) via door A -> door B."""
    Rv = Ru @ rA.T @ FLIP_Y @ rB
    tv = (tu + Ru @ pA) - Rv @ pB
    return Rv, tv


def bfs_region_xforms(
    targetnode: int,
    nodes: dict[int, SNode],
    sno_idx: SNOIndex,
    local_nmi: dict[int, str] | None = None,
) -> tuple[dict[int, tuple[np.ndarray, np.ndarray]], list[tuple[int, int, int, int, float]], list[int]]:
    """Compute world transforms for all nodes reachable from targetnode and check door residuals."""
    xforms: dict[int, tuple[np.ndarray, np.ndarray]] = {targetnode: (np.eye(3), np.zeros(3))}
    q = deque([targetnode])
    while q:
        u = q.popleft()
        Ru, tu = xforms[u]
        su = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[u].mesh_guid, local_nmi))
        for did, (v, fd) in nodes[u].doors.items():
            if v not in nodes or v in xforms:
                continue
            sv = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[v].mesh_guid, local_nmi))
            pA, rA = su["doors"][did]
            pB, rB = sv["doors"][fd]
            xforms[v] = stitch_transform(Ru, tu, pA, rA, pB, rB)
            q.append(v)

    residuals: list[tuple[int, int, int, int, float]] = []
    for u, (Ru, tu) in xforms.items():
        su = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[u].mesh_guid, local_nmi))
        for did, (v, fd) in nodes[u].doors.items():
            if v not in xforms:
                continue
            Rv, tv = xforms[v]
            sv = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[v].mesh_guid, local_nmi))
            wpA = tu + Ru @ su["doors"][did][0]
            wpB = tv + Rv @ sv["doors"][fd][0]
            res = float(np.linalg.norm(wpA - wpB))
            residuals.append((u, did, v, fd, res))
    unreachable = [g for g in nodes if g not in xforms]
    return xforms, residuals, unreachable


def world_aabb(R: np.ndarray, t: np.ndarray, mn: np.ndarray, mx: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    c = (mn + mx) * 0.5
    h = (mx - mn) * 0.5
    wc = t + R @ c
    wh = np.abs(R) @ h
    return wc - wh, wc + wh


def parse_lnc(data: bytes) -> dict[int, dict[str, Any]]:
    cnt = struct.unpack_from("<I", data, 0)[0]
    entries: dict[int, dict[str, Any]] = {}
    for i in range(cnt):
        g, o, s = struct.unpack_from("<3I", data, 4 + 12 * i)
        b = data[o : o + s]
        ver, chk = struct.unpack_from("<2I", b, 0)
        bbox = list(struct.unpack_from("<6f", b, 8))
        n_lns = struct.unpack_from("<I", b, 32)[0]
        q = 36
        lns = []
        for _ in range(n_lns):
            ln_id = b[q]
            flags, nf = struct.unpack_from("<IH", b, q + 1)
            q += 7
            fars = []
            for _ in range(nf):
                fg = struct.unpack_from(">I", b, q)[0]
                fln = b[q + 4]
                npairs = struct.unpack_from("<I", b, q + 5)[0]
                q += 9
                pairs = [struct.unpack_from("<2H", b, q + 4 * j) for j in range(npairs)]
                q += 4 * npairs
                fars.append({"fg": fg, "fln": fln, "pairs": pairs})
            lns.append({"ln_id": ln_id, "flags": flags, "fars": fars})
        nv, nl = struct.unpack_from("<2I", b, q)
        q += 8
        lights = []
        for _ in range(nl):
            lg = struct.unpack_from("<I", b, q)[0]
            hdr60 = b[q + 4 : q + 64]
            has_vals = b[q + 60] != 0
            vlen = nv * 4 if has_vals else 0
            vals = b[q + 64 : q + 64 + vlen]
            lights.append({"guid": lg, "hdr60": hdr60, "vals": vals})
            q += 64 + vlen
        rem = b[q:]
        entries[g] = {
            "ver": ver,
            "chk": chk,
            "bbox": bbox,
            "lns": lns,
            "nv": nv,
            "lights": lights,
            "rem": rem,
        }
    return entries


def build_lnc(entries: dict[int, dict[str, Any]]) -> bytes:
    guids = list(entries.keys())
    blobs: list[bytes] = []
    for g in guids:
        e = entries[g]
        buf = bytearray()
        buf += struct.pack("<2I6fI", e["ver"], e["chk"], *e["bbox"], len(e["lns"]))
        for ln in e["lns"]:
            buf += struct.pack("<BIH", ln["ln_id"], ln["flags"], len(ln["fars"]))
            for far in ln["fars"]:
                pairs = far["pairs"]
                buf += struct.pack(">I", far["fg"])
                buf += struct.pack("<BI", far["fln"], len(pairs))
                for p0, p1 in pairs:
                    buf += struct.pack("<2H", p0, p1)
        buf += struct.pack("<2I", e["nv"], len(e["lights"]))
        for lt in e["lights"]:
            buf += struct.pack("<I", lt["guid"])
            buf += lt["hdr60"]
            buf += lt["vals"]
        buf += e["rem"]
        blobs.append(bytes(buf))

    out = bytearray(struct.pack("<I", len(guids)))
    off = 4 + len(guids) * 12
    for g, blob in zip(guids, blobs):
        out += struct.pack("<3I", g, off, len(blob))
        off += len(blob)
    for blob in blobs:
        out += blob
    return bytes(out)


def compute_leaf_pairs(
    Ru: np.ndarray,
    tu: np.ndarray,
    su: dict[str, Any],
    ln_u: int,
    Rv: np.ndarray,
    tv: np.ndarray,
    sv: dict[str, Any],
    ln_v: int,
    eps: float = 0.05,
) -> list[tuple[int, int]]:
    """Match overlapping world-space AABB leaf IDs between two logical nodes."""
    u_leaves = su["lns"][ln_u]["leaves"]
    v_leaves = sv["lns"][ln_v]["leaves"]
    if not u_leaves or not v_leaves:
        return []
    u_ids = [x[0] for x in u_leaves]
    u_mn = np.array([world_aabb(Ru, tu, x[1], x[2])[0] for x in u_leaves]) - eps
    u_mx = np.array([world_aabb(Ru, tu, x[1], x[2])[1] for x in u_leaves]) + eps
    pairs: list[tuple[int, int]] = []
    for vid, v_lmn, v_lmx, _ in v_leaves:
        wmn, wmx = world_aabb(Rv, tv, v_lmn, v_lmx)
        hit = np.all((u_mx >= wmn) & (u_mn <= wmx), axis=1)
        for idx in np.nonzero(hit)[0]:
            pairs.append((int(u_ids[idx]), int(vid)))
    return pairs


def make_lnc_entry_from_sno(
    sno: dict[str, Any],
    R: np.ndarray,
    t: np.ndarray,
    nbr_pairs: dict[int, list[tuple[int, int, list[tuple[int, int]]]]],
    lights: list[dict[str, Any]] | None = None,
    chk: int = 0,
    ver: int = 10,
    rem: bytes = b"\x01\x00\x00\x00\x01\x00\x00\x00\x00\x00\x80?\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00",
    ln_flags_override: dict[int, int] | None = None,
) -> dict[str, Any]:
    lns = []
    for ln_id, ldata in sorted(sno["lns"].items()):
        flags = (ln_flags_override or {}).get(ln_id, ldata["flags"])
        if flags & 0x40000001 == 0x40000001 and (flags & 0xF) == 1:
            flags = 0x4000000E  # human + computer + dirt walkable flag matching DS maps
        fars = []
        for fg, fln, pairs in sorted(nbr_pairs.get(ln_id, []), key=lambda x: (f"{x[0]:08x}", x[1])):
            if pairs:
                fars.append({"fg": fg, "fln": fln, "pairs": pairs})
        lns.append({"ln_id": ln_id, "flags": flags, "fars": fars})
    return {
        "ver": ver,
        "chk": chk,
        "bbox": [*sno["mn"].tolist(), *sno["mx"].tolist()],
        "lns": lns,
        "nv": sno["nvert"],
        "lights": lights or [],
        "rem": rem,
    }


def make_directional_lights_for_node(
    sno: dict[str, Any],
    R: np.ndarray,
    t: np.ndarray,
    ref_lights: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Adapt oasis directional light headers (first 6 floats = local origin & dir) + vertex dot intensities."""
    out = []
    norms = sno["vnorm"]
    for lt in ref_lights:
        old_pos = struct.unpack_from("<3f", lt["hdr60"], 0)
        old_dir = np.array(struct.unpack_from("<3f", lt["hdr60"], 12), dtype=np.float64)
        # Keep direction in node local space; compute Lambertian dot >= 0
        dots = np.clip(norms @ old_dir, 0.0, 1.0).astype("<f4")
        hdr = bytearray(lt["hdr60"])
        struct.pack_into("<3f", hdr, 0, *(R.T @ (-t)).tolist())
        out.append({"guid": lt["guid"], "hdr60": bytes(hdr), "vals": dots.tobytes()})
    return out


def make_point_light_lnc(
    sno: dict[str, Any],
    R: np.ndarray,
    t: np.ndarray,
    light_guid: int,
    wpos: np.ndarray,
    color_argb: int = 0xFFFFB050,
    inner_r: float = 2.0,
    outer_r: float = 12.0,
    intensity: float = 1.0,
) -> dict[str, Any]:
    """Synthesize a local LNC point-light contribution header + per-vertex intensities."""
    lpos = R.T @ (wpos - t)
    dvec = lpos - sno["vpos"]
    dist = np.linalg.norm(dvec, axis=1)
    safe_d = np.where(dist > 1e-5, dist, 1.0)
    ndotl = np.clip(np.sum(sno["vnorm"] * (dvec / safe_d[:, None]), axis=1), 0.0, 1.0)
    atten = np.clip((outer_r - dist) / max(outer_r - inner_r, 0.1), 0.0, 1.0)
    vals = (ndotl * atten).astype("<f4")
    hdr = bytearray(60)
    # hdr60 offsets: 0:3f local_pos, 12:3f dir(0), 24:0,0,color(I), 36:I=1, 40:inner(f), 44:outer(f), 48:intensity(f), 52:I, 56:I
    struct.pack_into(
        "<3f3f2I2I3f2I",
        hdr,
        0,
        *lpos.tolist(),
        0.0,
        0.0,
        0.0,
        0,
        0,
        color_argb,
        1,
        inner_r,
        outer_r,
        intensity,
        0x01000000,
        1,
    )
    return {"guid": light_guid, "hdr60": bytes(hdr), "vals": vals.tobytes()}


def rebuild_lnc_edges_for_nodes(
    lnc: dict[int, dict[str, Any]],
    affected_nodes: set[int],
    nodes: dict[int, SNode],
    xforms: dict[int, tuple[np.ndarray, np.ndarray]],
    sno_idx: SNOIndex,
    local_nmi: dict[int, str] | None = None,
) -> None:
    """Recompute LNC leaf `pairs` symmetrically for every edge touching affected_nodes."""
    for u in list(lnc.keys()):
        Ru, tu = xforms[u]
        su = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[u].mesh_guid, local_nmi))
        if u in affected_nodes:
            nbrs = sorted({fg for fg, _ in nodes[u].doors.values()} & set(xforms))
            for ln in lnc[u]["lns"]:
                ln_u = ln["ln_id"]
                new_fars = []
                for v in nbrs:
                    Rv, tv = xforms[v]
                    sv = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[v].mesh_guid, local_nmi))
                    for ln_v_entry in lnc[v]["lns"]:
                        ln_v = ln_v_entry["ln_id"]
                        pairs = compute_leaf_pairs(Ru, tu, su, ln_u, Rv, tv, sv, ln_v)
                        if pairs:
                            new_fars.append({"fg": v, "fln": ln_v, "pairs": pairs})
                ln["fars"] = sorted(new_fars, key=lambda x: (f"{x['fg']:08x}", x["fln"]))
        else:
            # Update only far references pointing into affected_nodes
            for ln in lnc[u]["lns"]:
                ln_u = ln["ln_id"]
                kept = [f for f in ln["fars"] if f["fg"] not in affected_nodes]
                for v in sorted(affected_nodes & {fg for fg, _ in nodes[u].doors.values()}):
                    if v not in lnc:
                        continue
                    Rv, tv = xforms[v]
                    sv = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[v].mesh_guid, local_nmi))
                    for ln_v_entry in lnc[v]["lns"]:
                        ln_v = ln_v_entry["ln_id"]
                        pairs = compute_leaf_pairs(Ru, tu, su, ln_u, Rv, tv, sv, ln_v)
                        if pairs:
                            kept.append({"fg": v, "fln": ln_v, "pairs": pairs})
                ln["fars"] = sorted(kept, key=lambda x: (f"{x['fg']:08x}", x["fln"]))


def _update_light_locks(txt: str, remove_guids: set[int], add_guids: list[int]) -> str:
    lines = []
    for ln in txt.splitlines():
        m = re.search(r"b\s+(0x[0-9a-fA-F]+)\s*=", ln)
        if m and int(m.group(1), 16) in remove_guids:
            continue
        lines.append(ln)
    idx = len(lines) - 1
    while idx >= 0 and "}" not in lines[idx]:
        idx -= 1
    inserts = [f"  b 0x{g:08x} = false;" for g in add_guids]
    lines[idx:idx] = inserts
    return "\r\n".join(lines) + "\r\n"


def _update_streamer_index(txt: str, remove_guids: set[int], add_guids: list[int]) -> str:
    lines = []
    for ln in txt.splitlines():
        m = re.search(r"x\s+\*\s*=\s*(0x[0-9a-fA-F]+)", ln)
        if m and int(m.group(1), 16) in remove_guids:
            continue
        lines.append(ln)
    idx = len(lines) - 1
    while idx >= 0 and "}" not in lines[idx]:
        idx -= 1
    inserts = [f"  x * = 0x{g:08X};" for g in add_guids]
    lines[idx:idx] = inserts
    return "\r\n".join(lines) + "\r\n"


def _make_lf_block(guid: int, sno: dict[str, Any]) -> str:
    lines = [f"\t[t:lf_snode,n:0x{guid:08x}]", "\t{"]
    for ln_id, ldata in sorted(sno["lns"].items()):
        lines.append(f"\t\t[t:lf_lnode,n:{ln_id}]")
        lines.append("\t\t{")
        if ldata["flags"] & 0x40000001 == 0x40000001:
            lines.extend([
                "\t\t\t* = lf_human_player;",
                "\t\t\t* = lf_computer_player;",
                "\t\t\t* = lf_dirt;",
            ])
        lines.append("\t\t}")
    lines.append("\t}")
    return "\r\n".join(lines)


def _update_logical_flags(txt: str, remove_guids: set[int], add_entries: list[tuple[int, dict[str, Any]]]) -> str:
    out_parts: list[str] = []
    last = 0
    for _, s, e, blk in extract_blocks(txt, r"\[t:lf_snode,n:[^\]]+\]"):
        m = re.search(r"n:(0x[0-9a-fA-F]+)", blk)
        if m and int(m.group(1), 16) in remove_guids:
            out_parts.append(txt[last:s].rstrip("\t "))
            last = e
    out_parts.append(txt[last:])
    merged = "".join(out_parts)
    tail = merged.rindex("}")
    add_txt = "\r\n".join(_make_lf_block(g, s) for g, s in add_entries)
    return merged[:tail] + add_txt + "\r\n}\r\n"


def _match_doors_world(
    su: dict[str, Any],
    Ru: np.ndarray,
    tu: np.ndarray,
    sv: dict[str, Any],
    Rv: np.ndarray,
    tv: np.ndarray,
    tol: float = 0.05,
) -> list[tuple[int, int]]:
    """Find all door pairs (du, dv) whose world positions coincide within tol and normals oppose."""
    pairs: list[tuple[int, int]] = []
    for du, (pu, ru) in su["doors"].items():
        wpu = tu + Ru @ pu
        for dv, (pv, rv) in sv["doors"].items():
            wpv = tv + Rv @ pv
            if np.linalg.norm(wpu - wpv) <= tol:
                pairs.append((du, dv))
    return pairs


def apply_oasis_displacer(
    nodes_txt: str,
    lnc_bytes: bytes,
    light_locks_txt: str,
    logical_flags_txt: str,
    streamer_txt: str,
    sno_idx: SNOIndex,
    donor_lnc_map: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    """Replace Oasis Crest 0x45251CA5 with t_dc01_dunes-32x32-teleporter-a + Displacer 0xB0760001 + Pad 0xB0760002."""
    NODE_PIT = 0x45251CA5
    NODE_DISP = 0xB0760001
    NODE_PAD = 0xB0760002
    MESH_TELEPORTER = 0xAA001027
    MESH_DISP = 0xBEEF8001  # registered in world/global/siege_nodes/loa_expansion/loa_expansion_nodes.gas
    MESH_PAD = 0xBEEF8002

    # Inject dynamic filenames into sno_idx global lookup for BFS/LNC calculations
    sno_idx.global_guids[MESH_DISP] = "t_xxx_wal_displacer-a"
    sno_idx.global_guids[MESH_PAD] = "t_xxx_wal_displacer_pad"

    hdr, tn, nodes = parse_nodes_gas(nodes_txt)
    lnc = parse_lnc(lnc_bytes)

    # Swap 0x45251CA5 mesh_guid; keep outer 4 doors identical to neighbors
    nodes[NODE_PIT].mesh_guid = MESH_TELEPORTER
    xforms, _, _ = bfs_region_xforms(tn, nodes, sno_idx)
    Ru, tu = xforms[NODE_PIT]
    su = sno_idx.get_sno("t_dc01_dunes-32x32-teleporter-a")
    sd = sno_idx.get_sno("t_xxx_wal_displacer-a")
    sp = sno_idx.get_sno("t_xxx_wal_displacer_pad")

    # Solve Displacer world transform via primary door pair: teleporter d5 <-> displacer-a d7
    Rd, td = stitch_transform(Ru, tu, *su["doors"][5], *sd["doors"][7])
    # Stitch ALL matching perimeter pit doors (5..12 <-> 1..8)
    pit_pairs = _match_doors_world(su, Ru, tu, sd, Rd, td, tol=0.05)
    assert len(pit_pairs) == 8, f"Expected 8 pit door matches, got {pit_pairs}"

    # Solve Pad world transform via displacer-a d9 <-> pad d1
    Rp, tp = stitch_transform(Rd, td, *sd["doors"][9], *sp["doors"][1])
    pad_pairs = _match_doors_world(sd, Rd, td, sp, Rp, tp, tol=0.05)
    assert pad_pairs == [(9, 1), (12, 2)], f"Unexpected pad pairs: {pad_pairs}"

    disp_doors = {dv: (NODE_PIT, du) for du, dv in pit_pairs}
    for d_disp, d_pad in pad_pairs:
        disp_doors[d_disp] = (NODE_PAD, d_pad)
    for du, dv in pit_pairs:
        nodes[NODE_PIT].doors[du] = (NODE_DISP, dv)

    nodes[NODE_DISP] = SNode(
        guid=NODE_DISP,
        mesh_guid=MESH_DISP,
        bounds_camera=False,
        camera_fade=False,
        nodelevel=0x00000001,
        nodeobject=0x00000002,
        nodesection=0x00000051,
        occludes_camera=True,
        occludes_light=True,
        texsetabbr="xxx",
        doors=disp_doors,
    )
    nodes[NODE_PAD] = SNode(
        guid=NODE_PAD,
        mesh_guid=MESH_PAD,
        bounds_camera=True,
        camera_fade=False,
        nodelevel=0x00000001,
        nodeobject=0x00000001,
        nodesection=0x00000051,
        occludes_camera=True,
        occludes_light=True,
        texsetabbr="xxx",
        doors={dp: (NODE_DISP, dd) for dd, dp in pad_pairs},
    )

    xforms[NODE_DISP] = (Rd, td)
    xforms[NODE_PAD] = (Rp, tp)

    # Rebuild LNC entries for 0x45251CA5, 0xB0760001, 0xB0760002 (and edge updates on the 4 neighbors)
    ref_lights_oasis = lnc[NODE_PIT]["lights"]
    lnc[NODE_PIT] = make_lnc_entry_from_sno(
        su,
        Ru,
        tu,
        {},
        lights=make_directional_lights_for_node(su, Ru, tu, ref_lights_oasis),
        chk=lnc[NODE_PIT]["chk"],
        rem=lnc[NODE_PIT]["rem"],
    )
    lnc[NODE_DISP] = make_lnc_entry_from_sno(
        sd,
        Rd,
        td,
        {},
        lights=make_directional_lights_for_node(sd, Rd, td, ref_lights_oasis),
        chk=donor_lnc_map[0x0B9BF568]["chk"],
        rem=donor_lnc_map[0x0B9BF568]["rem"],
    )
    lnc[NODE_PAD] = make_lnc_entry_from_sno(
        sp,
        Rp,
        tp,
        {},
        lights=make_directional_lights_for_node(sp, Rp, tp, ref_lights_oasis),
        chk=donor_lnc_map[0x44C4537B]["chk"],
        rem=donor_lnc_map[0x44C4537B]["rem"],
    )
    rebuild_lnc_edges_for_nodes(lnc, {NODE_PIT, NODE_DISP, NODE_PAD}, nodes, xforms, sno_idx)

    return {
        "nodes.gas": format_nodes_gas(hdr, tn, nodes),
        "siege_nodes.lnc": build_lnc(lnc),
        "light_locks.gas": _update_light_locks(light_locks_txt, set(), [NODE_DISP, NODE_PAD]),
        "logical_flags.gas": _update_logical_flags(logical_flags_txt, set(), [(NODE_DISP, sd), (NODE_PAD, sp)]),
        "streamer_node_index.gas": _update_streamer_index(streamer_txt, set(), [NODE_DISP, NODE_PAD]),
        "xforms": xforms,
    }


def apply_fortress_foyer_annex(
    nodes_txt: str,
    lnc_bytes: bytes,
    nmi_txt: str,
    light_locks_txt: str,
    logical_flags_txt: str,
    streamer_txt: str,
    lights_txt: str,
    sno_idx: SNOIndex,
    donor_lnc_map: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    """Attach symmetric South Foyer hallway + wing foyer + twin Displacer bays off 0x87144D12."""
    nmi = parse_node_mesh_index(nmi_txt)
    free_mg = max(nmi) + 1
    mg_floor4x8 = next(k for k, v in nmi.items() if v == "t_dgn02_flr_04x08-v0")
    mg_floor8x8 = next(k for k, v in nmi.items() if v == "t_dgn02_flr_08x08-v0")
    mg_wall8 = next(k for k, v in nmi.items() if v == "t_dgn02_side_wall-thick-support-a")
    mg_cncv = next(k for k, v in nmi.items() if v == "t_dgn02_side_cnr-cncv-a")
    mg_cnvx = next(k for k, v in nmi.items() if v == "t_dgn02_side_cnr-cnvx-b")
    mg_wall4 = free_mg
    mg_disp = free_mg + 1
    mg_pad = free_mg + 2
    nmi[mg_wall4] = "t_dgn02_side_wall-thin-a"
    nmi[mg_disp] = "t_xxx_wal_displacer-a"
    nmi[mg_pad] = "t_xxx_wal_displacer_pad"

    hdr, tn, nodes = parse_nodes_gas(nodes_txt)
    lnc = parse_lnc(lnc_bytes)

    removed = {0xF336140D, 0x9CD9DE67, 0xEF7BBD1F}
    for g in removed:
        nodes.pop(g, None)
        lnc.pop(g, None)
    for g, n in nodes.items():
        n.doors = {d: (fg, fd) for d, (fg, fd) in n.doors.items() if fg not in removed}

    xforms, _, _ = bfs_region_xforms(tn, nodes, sno_idx, nmi)

    annex_defs: list[tuple[int, int, str]] = []

    def place(guid: int, mg: int, mesh: str, anchor_g: int, anchor_d: int, self_d: int, **kw: Any) -> tuple[np.ndarray, np.ndarray]:
        R_a, t_a = xforms[anchor_g]
        s_a = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[anchor_g].mesh_guid, nmi))
        s_b = sno_idx.get_sno(mesh)
        R_b, t_b = stitch_transform(R_a, t_a, *s_a["doors"][anchor_d], *s_b["doors"][self_d])
        nodes[guid] = SNode(
            guid=guid,
            mesh_guid=mg,
            texsetabbr=kw.get("texsetabbr", "abc01"),
            bounds_camera=kw.get("bounds_camera", False),
            nodelevel=kw.get("nodelevel", 0xFFFFFFFF),
            nodeobject=kw.get("nodeobject", 0xFFFFFFFF),
            nodesection=kw.get("nodesection", 0x00000002),
            doors={},
        )
        xforms[guid] = (R_b, t_b)
        annex_defs.append((guid, mg, mesh))
        return R_b, t_b

    base = 0xB2F00000
    G_ENTRY   = base + 1   # 8x8  X[-32.75..-24.75], Z[-110..-102]
    G_HUB     = base + 2   # 8x8  X[-32.75..-24.75], Z[-102..-94]
    G_WING_W  = base + 3   # 4x8  X[-36.75..-32.75], Z[-102..-94]
    G_WING_E  = base + 4   # 4x8  X[-24.75..-20.75], Z[-102..-94]
    G_BAY1    = base + 5   # 8x8  X[-44.75..-36.75], Z[-102..-94]  (Oasis Displacer)
    G_PAD1    = base + 6   # pad inside BAY1
    G_BAY2    = base + 7   # 8x8  X[-20.75..-12.75], Z[-102..-94]  (Clock Displacer)
    G_PAD2    = base + 8   # pad inside BAY2
    G_W4_EW   = base + 9   # 4m wall X[-36.75..-32.75], Z[-110..-106]
    G_W4_EE   = base + 10  # 4m wall X[-24.75..-20.75], Z[-110..-106]
    G_CNVX_W  = base + 11  # 4x4 corner X[-36.75..-32.75], Z[-106..-102]
    G_CNVX_E  = base + 12  # 4x4 corner X[-24.75..-20.75], Z[-106..-102]
    G_W4_WN   = base + 13  # 4m wall north of WING_W X[-36.75..-32.75], Z[-94..-90]
    G_W4_EN   = base + 14  # 4m wall north of WING_E X[-24.75..-20.75], Z[-94..-90]
    G_W8_HN   = base + 15  # 8m wall north of HUB    X[-32.75..-24.75], Z[-94..-90]
    G_W8_B1S  = base + 16  # 8m wall south of BAY1   X[-44.75..-36.75], Z[-106..-102]
    G_W8_B1W  = base + 17  # 8m wall west of BAY1    X[-48.75..-44.75], Z[-102..-94]
    G_W8_B1N  = base + 18  # 8m wall north of BAY1   X[-44.75..-36.75], Z[-94..-90]
    G_W8_B2S  = base + 19  # 8m wall south of BAY2   X[-20.75..-12.75], Z[-106..-102]
    G_W8_B2E  = base + 20  # 8m wall east of BAY2    X[-12.75..-8.75],  Z[-102..-94]
    G_W8_B2N  = base + 21  # 8m wall north of BAY2   X[-20.75..-12.75], Z[-94..-90]
    G_C_B1SW  = base + 22  # 4x4 cncv X[-48.75..-44.75], Z[-106..-102]
    G_C_B1NW  = base + 23  # 4x4 cncv X[-48.75..-44.75], Z[-94..-90]
    G_C_B2SE  = base + 24  # 4x4 cncv X[-12.75..-8.75],  Z[-106..-102]
    G_C_B2NE  = base + 25  # 4x4 cncv X[-12.75..-8.75],  Z[-94..-90]

    # Floor layout (all at Y = -2)
    # G_ENTRY: 0x87144D12:5 (-30.75,-2,-110) <-> s_08x08:2 (1.25,-2,-2)
    place(G_ENTRY, mg_floor8x8, "t_dgn02_flr_08x08-v0", 0x87144D12, 5, 2)
    # G_HUB north of G_ENTRY: G_ENTRY:5 (-30.75,-2,-102) <-> s_08x08:2
    place(G_HUB, mg_floor8x8, "t_dgn02_flr_08x08-v0", G_ENTRY, 5, 2)
    # G_WING_W west of G_HUB: G_HUB:3 (-32.75,-2,-100) & G_HUB:4 (-32.75,-2,-96).
    # In t_dgn02_flr_04x08-v0 (4m X x 8m Z): East doors 6=(3.25,-2,0), 5=(3.25,-2,4), West doors 2=(-0.75,-2,0), 3=(-0.75,-2,4).
    # Stitch G_HUB:3 <-> WING_W:6 (places WING_W X[-36.75..-32.75], Z[-102..-94], doors 1=S, 4=N)
    place(G_WING_W, mg_floor4x8, "t_dgn02_flr_04x08-v0", G_HUB, 3, 6)
    # G_WING_E east of G_HUB: G_HUB:7 (-24.75,-2,-96) & G_HUB:8 (-24.75,-2,-100) -> WING_E:2=(-0.75,-2,0) & 3=(-0.75,-2,4)
    # Stitch G_HUB:8 <-> WING_E:2 (places WING_E X[-24.75..-20.75], Z[-102..-94], doors 1=S, 4=N)
    place(G_WING_E, mg_floor4x8, "t_dgn02_flr_04x08-v0", G_HUB, 8, 2)

    # BAY1 (West of G_WING_W): G_WING_W:2 (-36.75,-2,-100) & 3 (-36.75,-2,-96) <-> s_disp:6 & 5 (East doors!)
    place(G_BAY1, mg_disp, "t_xxx_wal_displacer-a", G_WING_W, 2, 6, texsetabbr="xxx", nodelevel=1, nodeobject=2, nodesection=0x50)
    place(G_PAD1, mg_pad, "t_xxx_wal_displacer_pad", G_BAY1, 9, 1, texsetabbr="xxx", bounds_camera=True, nodelevel=1, nodeobject=1, nodesection=0x50)
    # BAY2 (East of G_WING_E): G_WING_E:6 (-20.75,-2,-96) & 5 (-20.75,-2,-100) <-> s_disp:1 & 2 (West doors!)
    place(G_BAY2, mg_disp, "t_xxx_wal_displacer-a", G_WING_E, 6, 1, texsetabbr="xxx", nodelevel=1, nodeobject=2, nodesection=0x51)
    place(G_PAD2, mg_pad, "t_xxx_wal_displacer_pad", G_BAY2, 9, 1, texsetabbr="xxx", bounds_camera=True, nodelevel=1, nodeobject=1, nodesection=0x51)

    # 4m walls along G_ENTRY South half (Z[-110..-106]): G_ENTRY:3 (-32.75,-2,-108) <-> wall4:1; G_ENTRY:8 (-24.75,-2,-108) <-> wall4:1
    place(G_W4_EW, mg_wall4, "t_dgn02_side_wall-thin-a", G_ENTRY, 3, 1)
    place(G_W4_EE, mg_wall4, "t_dgn02_side_wall-thin-a", G_ENTRY, 8, 1)
    # 4x4 convex corners at G_ENTRY:4 (-32.75,-2,-104) & G_WING_W:1 (-34.75,-2,-102)
    # For CNVX_W: stitch G_WING_W:1 (South face, -34.75,-2,-102, faces -Z) <-> cnr-cnvx-b:4 (+X in local, 2,-2,0) -> +90deg CCW!
    place(G_CNVX_W, mg_cnvx, "t_dgn02_side_cnr-cnvx-b", G_WING_W, 1, 4)
    # For CNVX_E: stitch G_ENTRY:7 (East face, -24.75,-2,-104, faces +X) <-> cnr-cnvx-b:4 (or :1):
    # Wait: cnr-cnvx-b d1=(0,-2,-2) [-Z], d4=(2,-2,0) [+X]. To occupy X[-24.75..-20.75], Z[-106..-102] with floor doors on West & North:
    # We need 180deg rotation: d1 (local -Z -> world +Z North = -102!) stitches G_WING_E:1! Check!
    place(G_CNVX_E, mg_cnvx, "t_dgn02_side_cnr-cnvx-b", G_WING_E, 1, 1)

    # North 4m walls on WING_W & WING_E, and 8m wall on HUB:
    place(G_W4_WN, mg_wall4, "t_dgn02_side_wall-thin-a", G_WING_W, 4, 1)
    place(G_W4_EN, mg_wall4, "t_dgn02_side_wall-thin-a", G_WING_E, 4, 1)
    place(G_W8_HN, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_HUB, 5, 2)

    # Thick 8m enclosing walls around BAY1 (S, W, N) and BAY2 (S, E, N)
    # Note s_wall8 d2=(-2,-2,-2) is left side when facing wall; d1=(2,-2,-2) is right side.
    place(G_W8_B1S, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY1, 8, 1)
    place(G_W8_B1W, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY1, 2, 1)
    place(G_W8_B1N, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY1, 3, 2)
    place(G_W8_B2S, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY2, 7, 2)
    place(G_W8_B2E, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY2, 5, 2)
    place(G_W8_B2N, mg_wall8, "t_dgn02_side_wall-thick-support-a", G_BAY2, 3, 2)

    # 4 outer concave corners (cncv-a d1=(0,0,-1) faces -Z, d2=(1,0,0) faces +X -> fits 90deg outer corner between two walls!)
    place(G_C_B1SW, mg_cncv, "t_dgn02_side_cnr-cncv-a", G_W8_B1W, 3, 2)
    place(G_C_B1NW, mg_cncv, "t_dgn02_side_cnr-cncv-a", G_W8_B1W, 4, 1)
    place(G_C_B2SE, mg_cncv, "t_dgn02_side_cnr-cncv-a", G_W8_B2E, 4, 1)
    place(G_C_B2NE, mg_cncv, "t_dgn02_side_cnr-cncv-a", G_W8_B2E, 3, 2)

    # Auto-stitch EVERY coincident door pair among annex nodes & their boundary anchors!
    stitch_pool = [0x87144D12, 0xB18E9C00, 0x59996A3E] + [g for g, _, _ in annex_defs]
    for i, ga in enumerate(stitch_pool):
        Ra, ta = xforms[ga]
        sa = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[ga].mesh_guid, nmi))
        for gb in stitch_pool[i + 1 :]:
            Rb, tb = xforms[gb]
            sb = sno_idx.get_sno(sno_idx.resolve_mesh(nodes[gb].mesh_guid, nmi))
            for da, db in _match_doors_world(sa, Ra, ta, sb, Rb, tb, tol=0.05):
                if da not in nodes[ga].doors and db not in nodes[gb].doors:
                    nodes[ga].doors[da] = (gb, db)
                    nodes[gb].doors[db] = (ga, da)

    new_guids = {g for g, _, _ in annex_defs}
    hub_w = xforms[G_HUB][1] + np.array([3.25, 1.5, 2.0])
    bay1_w = xforms[G_BAY1][1] + np.array([0.0, 2.5, 0.0])
    bay2_w = xforms[G_BAY2][1] + np.array([0.0, 2.5, 0.0])
    pt_lights_info = [
        (0xB2F10001, hub_w, 0xFFFFB050, 2.0, 14.0, 1.0, G_HUB, np.array([3.25, 1.5, 2.0])),
        (0xB2F10002, bay1_w, 0xFF70B8FF, 2.0, 12.0, 1.0, G_BAY1, np.array([0.0, 2.5, 0.0])),
        (0xB2F10003, bay2_w, 0xFF70FFAA, 2.0, 12.0, 1.0, G_BAY2, np.array([0.0, 2.5, 0.0])),
    ]

    for g, mg, mesh in annex_defs:
        s = sno_idx.get_sno(mesh)
        Rg, tg = xforms[g]
        donor_chk = 0x1EFFFDD8
        if mesh == "t_xxx_wal_displacer-a":
            donor_chk = donor_lnc_map[0x0B9BF568]["chk"]
        elif mesh == "t_xxx_wal_displacer_pad":
            donor_chk = donor_lnc_map[0x44C4537B]["chk"]
        lts = [make_point_light_lnc(s, Rg, tg, lg, wpos, col, ir, or_, inten) for lg, wpos, col, ir, or_, inten, _, _ in pt_lights_info]
        lnc[g] = make_lnc_entry_from_sno(s, Rg, tg, {}, lights=lts, chk=donor_chk)

    rebuild_lnc_edges_for_nodes(lnc, new_guids, nodes, xforms, sno_idx, nmi)

    lt_blocks = []
    for lg, _, col, ir, or_, inten, ln_g, lpos in pt_lights_info:
        lt_blocks.append(
            f"\t[t:point,n:light_0x{lg:08x}]\r\n\t{{\r\n"
            f"\t  b active = true;\r\n\t  b affects_actors = true;\r\n\t  b affects_items = true;\r\n\t  b affects_terrain = true;\r\n"
            f"\t  x color = 0x{col:08X};\r\n\t  b draw_shadow = false;\r\n"
            f"\t  f inner_radius = {ir:.6f};\r\n\t  f intensity = {inten:.6f};\r\n"
            f"\t  b occlude_geometry = true;\r\n\t  b on_timer = false;\r\n\t  f outer_radius = {or_:.6f};\r\n"
            f"\t\t[position]\r\n\t\t{{\r\n\t\t  x node = 0x{ln_g:08X};\r\n"
            f"\t\t  f x = {lpos[0]:.6f};\r\n\t\t  f y = {lpos[1]:.6f};\r\n\t\t  f z = {lpos[2]:.6f};\r\n"
            f"\t\t}}\r\n\t}}"
        )
    lt_tail = lights_txt.rindex("}")
    new_lights_txt = lights_txt[:lt_tail] + "\r\n".join(lt_blocks) + "\r\n}\r\n"

    lf_entries = [(g, sno_idx.get_sno(m)) for g, _, m in annex_defs]
    add_g = [g for g, _, _ in annex_defs]
    return {
        "nodes.gas": format_nodes_gas(hdr, tn, nodes),
        "siege_nodes.lnc": build_lnc(lnc),
        "node_mesh_index.gas": format_node_mesh_index(nmi),
        "light_locks.gas": _update_light_locks(light_locks_txt, removed, add_g),
        "logical_flags.gas": _update_logical_flags(logical_flags_txt, removed, lf_entries),
        "streamer_node_index.gas": _update_streamer_index(streamer_txt, removed, add_g),
        "lights.gas": new_lights_txt,
        "xforms": xforms,
        "nodes": nodes,
        "nmi": nmi,
        "annex_guids": add_g,
    }
