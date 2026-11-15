"""Fast, string/comment-aware Dungeon Siege .gas block parser and text rewriter."""

from __future__ import annotations

import re
from typing import Callable, List, Tuple


def _find_block_end(txt: str, open_brace_idx: int) -> int:
    """Return index right after the matching '}' for '{' at open_brace_idx."""
    n = len(txt)
    depth = 1
    i = open_brace_idx + 1
    in_str = False
    while i < n and depth > 0:
        ch = txt[i]
        if in_str:
            if ch == "\\" and i + 1 < n:
                i += 2
                continue
            if ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            i += 1
            continue
        if ch == "/" and i + 1 < n:
            nxt = txt[i + 1]
            if nxt == "/":
                nl = txt.find("\n", i + 2)
                i = n if nl < 0 else nl + 1
                continue
            if nxt == "*":
                endc = txt.find("*/", i + 2)
                i = n if endc < 0 else endc + 2
                continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        i += 1
    return i


def extract_blocks(txt: str, header_rx: str) -> List[Tuple[re.Match, int, int, str]]:
    """Find top-matched blocks '[... ] { ... }' safely without quadratic scanning."""
    out: List[Tuple[re.Match, int, int, str]] = []
    for m in re.finditer(header_rx, txt, re.I | re.M):
        open_idx = txt.find("{", m.end())
        if open_idx < 0:
            continue
        # Ensure only whitespace/comments sit between header and opening '{'
        between = txt[m.end() : open_idx]
        if re.search(r"[^\s]", re.sub(r"//[^\r\n]*|/\*.*?\*/", "", between, flags=re.S)):
            continue
        end_idx = _find_block_end(txt, open_idx)
        out.append((m, m.start(), end_idx, txt[m.start() : end_idx]))
    return out


def replace_blocks(
    txt: str, header_rx: str, transform: Callable[[re.Match, str], str]
) -> str:
    """Replace matched blocks using transform(match, full_block_text) -> replacement."""
    blocks = extract_blocks(txt, header_rx)
    if not blocks:
        return txt
    parts: List[str] = []
    cur = 0
    for m, st, en, blk in blocks:
        if st < cur:
            continue
        parts.append(txt[cur:st])
        parts.append(transform(m, blk))
        cur = en
    parts.append(txt[cur:])
    return "".join(parts)
