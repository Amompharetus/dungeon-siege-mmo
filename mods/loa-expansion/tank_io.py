"""Lazy Tank (.dsres / .dsmap) reader and Priority-aware Tank builder wrapper."""

from __future__ import annotations

import mmap
import os
import struct
import sys
import zlib
from typing import Dict, Iterable, Optional

TOOLS_DIR = os.environ.get(
    "TANK_TOOLS_DIR",
    os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "tools")),
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import tank_build


def _nstr_tank(buf: bytes | memoryview, off: int) -> tuple[str, int]:
    """Decode Dungeon Siege NString matching tank_read.nstr (mandatory NUL + 4B align)."""
    ln = struct.unpack_from("<H", buf, off)[0]
    raw = bytes(buf[off + 2 : off + 2 + ln]).decode("latin1")
    return raw, off + ((2 + ln + 1 + 3) & ~3)


class LazyTank:
    """Memory-mapped DSigTank reader that decompresses files only on demand."""

    def __init__(self, path: str):
        self.path = path
        self._fp = open(path, "rb")
        self._mm = mmap.mmap(self._fp.fileno(), 0, access=mmap.ACCESS_READ)
        d = self._mm
        if d[:8] != b"DSigTank":
            raise ValueError(f"Not a DSigTank file: {path}")
        ds, fs = struct.unpack_from("<II", d, 12)
        self.dataoff = struct.unpack_from("<I", d, 24)[0]
        self.priority = struct.unpack_from("<I", d, 52)[0]
        dc = struct.unpack_from("<I", d, ds)[0]
        dirs: dict[int, tuple[int, str]] = {}
        for o in struct.unpack_from(f"<{dc}I", d, ds + 4):
            b = ds + o
            parent, _ = struct.unpack_from("<II", d, b)
            name, _ = _nstr_tank(d, b + 16)
            dirs[o] = (parent, name)

        dir_cache: dict[int, str] = {}

        def dpath(o: int) -> str:
            if o in dir_cache:
                return dir_cache[o]
            parts = []
            seen = set()
            cur = o
            while cur and cur not in seen:
                seen.add(cur)
                p, n = dirs.get(cur, (0, ""))
                if n:
                    parts.append(n)
                cur = p
            res = "/".join(reversed(parts))
            dir_cache[o] = res
            return res

        fc = struct.unpack_from("<I", d, fs)[0]
        self.entries: dict[str, tuple[int, int, int, int, int]] = {}
        for o in struct.unpack_from(f"<{fc}I", d, fs + 4):
            b = fs + o
            parent, size, doff, _crc = struct.unpack_from("<IIII", d, b)
            fmt, _flags = struct.unpack_from("<HH", d, b + 24)
            name, after = _nstr_tank(d, b + 28)
            dp = dpath(parent)
            fpath = f"{dp}/{name}" if dp else name
            self.entries[fpath.lower()] = (size, doff, fmt, after, _crc)

    def close(self) -> None:
        if getattr(self, "_mm", None) is not None:
            self._mm.close()
            self._mm = None
        if getattr(self, "_fp", None) is not None:
            self._fp.close()
            self._fp = None

    def __contains__(self, key: str) -> bool:
        return key.lower() in self.entries

    def __len__(self) -> int:
        return len(self.entries)

    def keys(self):
        return self.entries.keys()

    def get(self, key: str, default: Optional[bytes] = None) -> Optional[bytes]:
        k = key.lower()
        if k not in self.entries:
            return default
        return self[k]

    def __getitem__(self, key: str) -> bytes:
        k = key.lower()
        size, doff, fmt, after, _crc = self.entries[k]
        d = self._mm
        base = self.dataoff + doff
        if fmt == 0:
            return bytes(d[base : base + size])
        csize, chunk = struct.unpack_from("<II", d, after)
        if chunk == 0:
            return zlib.decompress(d[base : base + csize])
        nch = (size + chunk - 1) // chunk
        buf = bytearray()
        hp = after + 8
        for _ in range(nch):
            ucs, cs, extra, coff = struct.unpack_from("<IIII", d, hp)
            hp += 16
            seg = d[base + coff : base + coff + cs]
            if ucs == cs:
                buf.extend(seg)
            else:
                buf.extend(zlib.decompress(seg))
                if extra:
                    buf.extend(d[base + coff + cs : base + coff + cs + extra])
        return bytes(buf)

    def items_matching(self, predicate) -> Iterable[tuple[str, bytes]]:
        for k in self.entries:
            if predicate(k):
                yield k, self[k]


def build_tank_with_priority(
    files: Dict[str, bytes],
    out_path: str,
    title: str,
    author: str,
    description: str,
    priority: int = 0x5000,
) -> tuple[int, int]:
    """Build a DSigTank v1.2 file and set Priority at header offset 52."""
    nfiles, nbytes = tank_build.build_tank(
        files, out_path, title=title, author=author, description=description
    )
    if priority != 0x5000:
        with open(out_path, "r+b") as f:
            f.seek(52)
            f.write(struct.pack("<I", priority))
    return nfiles, nbytes
