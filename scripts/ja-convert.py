#!/usr/bin/env python3
"""The data plan that turns the Japanese release's files (YDQJ) into the
Europe release's (YDQP), with Japanese text, for players who own only the
Japanese ROM.

    usage: ja-convert.py plan [--pal assets/YDQP] [--jp assets/YDQJ] OUT
           ja-convert.py fill PLAN [--pal assets/YDQP] OUT
           ja-convert.py apply PLAN [--jp assets/YDQJ] OUT_DIR

`plan` needs both releases unpacked and writes a plan with no bytes of
YDQP in it (they are references, "P" below), fit for version control.
`fill` puts YDQP's bytes in from the Europe release, as the build does.
`apply` runs a filled plan on the Japanese release and writes YDQP's
data files (their folder paths) under OUT_DIR, each checked against the
SHA-1 the plan gives it; the runner does the same in C++.

What each YDQP data file becomes:
  * a file YDQJ has too, byte for byte: YDQJ's file;
  * a text table of one language (an archive member or loose file named
    `_<lg>`): YDQP's English table with the strings of the matching
    Japanese records (scripts/ja_text.py converts them); records with no
    Japanese counterpart keep their English. Every language's name gets
    the same table, so the game shows Japanese whatever language it
    picks;
  * a per-language image: YDQJ's counterpart as it is (scripts/ja-map.py
    finds it); any other per-language file keeps YDQP's English one;
  * anything else not per language: YDQJ's counterpart where the two
    agree, YDQP's bytes elsewhere; the movies YDQJ's own;
  * pack/font.gp2 (which YDQP never reads from): YDQJ's, followed by
    YDQJ's base fonts (FONT_FILE below), for the runner's Japanese font.

Plan format (little-endian; LEB128 numbers in the ops):
    "DQ9J", u32 version, YDQJ's image SHA-1, YDQP's image SHA-1
    u32 n, n sources: u16 length + "path" or "path::member" of YDQJ
                      (a member of a GPC2, NARC or named-table pac,
                      decompressed)
    u32 n, n refs:    the same for YDQP (what the P ops read; only
                      `fill` reads these)
    u32 n, n files:   u16 length + path, u32 size, SHA-1 of the result,
                      u32 op bytes, ops
    u32 n, n literal bytes (0 in a blank plan): YDQP's bytes, in the
                      order the P ops read them
An op is one byte then its arguments:
    0 J src off n    n bytes of source `src` from `off`
    1 P ref off n    n bytes of YDQP's `ref` from `off`: the next n
                     literal bytes
    2 R n bytes      n bytes the plan carries (structure, converted text)
    3 Z n            n zero bytes
"""
import argparse
import collections
import hashlib
import importlib.util
import math
import os
import re
import struct
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "ndsrecomp", "tools"))
import l5_archive as l5  # noqa: E402
import dq9_text_table as tt  # noqa: E402
import ja_text  # noqa: E402

_spec = importlib.util.spec_from_file_location("ja_map", os.path.join(ROOT, "scripts", "ja-map.py"))
jm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(jm)

try:
    import tomllib
except ImportError:  # Python < 3.11
    import tomli as tomllib

MAGIC = b"DQ9J"
VERSION = 1
J, P, R, Z = range(4)
LANGS = jm.LANGS


# ---------------------------------------------------------------------------
# Ops

class Ops:
    """A file's ops; adjacent ones of a kind are merged."""

    def __init__(self, stored=False):
        self.ops = []
        self.size = 0
        # An archive member given as stored (a compressed block with its
        # header), not as its contents.
        self.stored = stored

    def j(self, src, off, n):
        if n <= 0:
            return
        last = self.ops[-1] if self.ops else None
        if last and last[0] == J and last[1] == src and last[2] + last[3] == off:
            self.ops[-1] = (J, src, last[2], last[3] + n)
        else:
            self.ops.append((J, src, off, n))
        self.size += n

    def p(self, ref, off, n):
        if n <= 0:
            return
        last = self.ops[-1] if self.ops else None
        if last and last[0] == P and last[1] == ref and last[2] + last[3] == off:
            self.ops[-1] = (P, ref, last[2], last[3] + n)
        else:
            self.ops.append((P, ref, off, n))
        self.size += n

    def r(self, data):
        if not data:
            return
        if not any(data):
            return self.z(len(data))
        if self.ops and self.ops[-1][0] == R:
            self.ops[-1] = (R, self.ops[-1][1] + bytes(data))
        else:
            self.ops.append((R, bytes(data)))
        self.size += len(data)

    def z(self, n):
        if n <= 0:
            return
        if self.ops and self.ops[-1][0] == Z:
            self.ops[-1] = (Z, self.ops[-1][1] + n)
        else:
            self.ops.append((Z, n))
        self.size += n

    def extend(self, other):
        for op in other.ops:
            if op[0] == J:
                self.j(*op[1:])
            elif op[0] == P:
                self.p(*op[1:])
            elif op[0] == R:
                self.r(op[1])
            else:
                self.z(op[1])

    def pad(self, align):
        self.z(-self.size % align)


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def read_leb(data, p):
    n = shift = 0
    while True:
        b = data[p]
        p += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return n, p


# ---------------------------------------------------------------------------
# Releases

class Release(jm.Release):
    def __init__(self, folder, cache):
        super().__init__(folder)
        self.idx = self.index(cache)
        self.sha = {}
        for path, e in self.idx.items():
            self.sha.setdefault(e["sha1"], path)
            for name, _, msha in e["members"] or []:
                self.sha.setdefault(msha, f"{path}::{name}")

    def fat(self):
        """The data files of the image, as folder paths (data/...)."""
        with open(os.path.join(self.folder, "rom.toml"), "rb") as f:
            manifest = tomllib.load(f)
        return [e[0] for e in manifest["files"]["fat"] if e[0].startswith("data/")]

    def image_sha1(self):
        with open(os.path.join(self.folder, "rom.toml"), "rb") as f:
            return bytes.fromhex(tomllib.load(f)["rom"]["sha1"])


def folder_ref(ref):
    """ja-map's refs are relative to data/; the plan's to the folder."""
    return "data/" + ref


# ---------------------------------------------------------------------------
# Text tables

def string_starts(data, pool, size):
    """Offsets (relative to the pool) where a string starts."""
    out = {0}
    end = pool + size
    i = data.find(b"\0", pool)
    while 0 <= i < end - 1:
        out.add(i + 1 - pool)
        i = data.find(b"\0", i + 1)
    return out


# A string argument that names a file (data/ani/bg_cm_sx.gp2,
# bg_cm_sx_<LG>.pac): the code loads it, so it stays PAL's even where the
# Japanese table has its own (YDQJ's file names are not in YDQP's layout).
FILE_NAME = re.compile(rb"[A-Za-z0-9_/<>%.\-]+\.[A-Za-z0-9]{2,5}")
# Likewise a model id (mon_data's z000a, the slime's model): the Japanese
# table has one string per monster, its name, where YDQP's has the name,
# the model and the plural.
MODEL_ID = re.compile(rb"[a-z][0-9]{3}[a-z]?")

# Records whose text stays PAL's, by unit and record id (the first
# argument): the name entry's rows (0x4268..0x4274, the Japanese release's
# kana grid, which YDQP's code does not read; keyboard_*.bin below is the
# layout) and its placeholders (0x4A38..). The random default names
# (0x4E20..) are YDQJ's: names are stored one byte per glyph, and the
# runner's Japanese font gives the kana indices below 256
# (src/hooks/ja_font.cpp).
KEEP_PAL_RECORDS = {
    "data/bin/menu/str_cm.gp2::str_cm.nat":
        lambda rid: 0x4268 <= rid <= 0x4274 or 0x4A38 <= rid < 0x4E20,
}

# Japanese lines for units YDQJ has no file for (its code writes them):
# {unit: {record id: Shift-JIS line in YDQP's form}}. A record's entry can
# instead bind placeholders of YDQJ's own line ({"<str_1>": "<TARGET>"}),
# where YDQP's code gives the value another way.
JA_STRINGS_FILE = os.path.join(ROOT, "scripts", "ja-strings.toml")


def load_ja_strings(path=JA_STRINGS_FILE):
    with open(path, "rb") as f:
        units = tomllib.load(f)

    def value(v):
        if isinstance(v, dict):
            return {a.encode("cp932"): b.encode("cp932") for a, b in v.items()}
        return v.encode("cp932")
    return {unit: {int(k, 0): value(v) for k, v in lines.items()}
            for unit, lines in units.items()}


class Text:
    """A PAL text unit rebuilt with Japanese strings."""

    def __init__(self, plan, unit, langs, ref):
        self.plan = plan
        self.unit = unit
        self.langs = langs  # {lang: PAL ref}
        self.ref = ref      # the JP counterpart (ja-map ref) or None

    def jp_strings(self, tables, en):
        """Per PAL record (in order), the JP strings of its string
        arguments, each (bytes, plan source, offset in it), or None where
        it has no Japanese counterpart."""
        jp = self.plan.jp
        ref = self.ref

        def loc(t, r, o):
            s = t.string(o)
            if s is None:
                return None
            base = t.nat.pool if isinstance(t, jm.NatView) else t.pool
            return (s, folder_ref(r), base + o)

        jt = jm.parse_any(jp.get(self.ref)) if self.ref else None
        spos = jm.string_positions(tables)
        n = len(en.records)
        if jt is None:
            return spos, [None] * n
        if isinstance(en, jm.NatView) and isinstance(jt, jm.NatView) and \
                en.nat.record_size != jt.nat.record_size:
            return spos, self.restructured(en, jt, spos, lambda o: loc(jt, ref, o))
        if isinstance(en, jm.NatView) and en.nat.record_size == 8 and \
                not isinstance(jt, jm.NatView):
            by_id = {}
            for _, _, args in jt.records:
                if len(args) >= 2 and jt.string(args[-1]) is not None:
                    by_id.setdefault(args[0], loc(jt, ref, args[-1]))
            return spos, [[by_id[r[0]]] if r[0] in by_id else None
                          for r in en.nat.records]
        own = dict(jm.record_keys(jt, spos))
        chain = None
        out = []
        for key, strings in jm.record_keys(en, spos):
            if not strings:
                out.append(None)
                continue
            hit = (jt, ref, own.get(key))
            if hit[2] is None:
                if chain is None:
                    chain = []
                    for r in self.plan.sources.fallbacks(ref):
                        t = jm.parse_any(jp.get(r))
                        if t is not None:
                            chain.append((t, r, dict(jm.record_keys(t, spos))))
                first = key[:-1] + (1,)
                for t, r, keys in chain:
                    if key in keys:
                        hit = (t, r, keys[key])
                        break
                else:
                    for t, r, keys in [(jt, ref, own)] + chain:
                        if first in keys:
                            hit = (t, r, keys[first])
                            break
            t, r, offs = hit
            out.append(None if offs is None else [loc(t, r, o) for o in offs])
        return spos, out

    @staticmethod
    def restructured(en, jt, spos, loc):
        best = (0, None, None)
        for pi in range(en.nat.record_size // 4):
            if any(pi in v for v in spos.values()):
                continue
            pv = {r[pi] & 0xFFFF for r in en.nat.records}
            for ji in range(jt.nat.record_size // 4):
                overlap = len(pv & {r[ji] & 0xFFFF for r in jt.nat.records})
                if overlap > best[0]:
                    best = (overlap, pi, ji)
        _, pi, ji = best
        if pi is None:
            return [None] * len(en.records)
        jstr = [i for i in range(jt.nat.record_size // 4) if i != ji]
        by_id = {}
        for r in jt.nat.records:
            for i in jstr:
                s = loc(r[i])
                if s is not None:
                    by_id.setdefault(r[ji] & 0xFFFF, s)
                    break
        out = []
        for r in en.nat.records:
            s = by_id.get(r[pi] & 0xFFFF)
            out.append(None if s is None else [s])
        return out

    def build(self):
        """The unit's table (Ops), or None if it is not a text table."""
        pal = self.plan.pal
        en_ref = folder_ref(self.langs["en"])
        en_data = pal.get(self.langs["en"])
        tables = {lg: jm.parse_any(pal.get(r)) for lg, r in self.langs.items()}
        en = tables["en"]
        if en is None:
            return None
        spos, jps = self.jp_strings(tables, en)
        if isinstance(en, jm.NatView):
            pool, pool_size = en.nat.pool, en.nat.pool_size
        else:
            pool, pool_size = en.pool, en.pool_size
        valid = string_starts(en_data, pool, pool_size)

        # The pool: each string once, in the order the records use them.
        # Arguments that are the same in every language are not known to be
        # strings, but may be (str_op's build dates): the pool keeps PAL's
        # strings up to the last one such an argument points at in every
        # language (the same string in each), and the Japanese ones follow.
        others = [t for lg, t in tables.items() if lg != "en" and t is not None]

        def same_string(v):
            s = en.string(v)
            return all(t.string(v) == s for t in others)

        keep = 0
        for kind, _, args in en.records:
            idx = spos.get((kind, len(args)), set())
            for k, v in enumerate(args):
                if k not in idx and v in valid and same_string(v):
                    end = en_data.find(b"\0", pool + v)
                    keep = max(keep, (end if end >= 0 else pool + pool_size) + 1 - pool)
        keep = min(keep, pool_size)
        kept_strings = en_data[pool:pool + keep].count(b"\0")
        # The rest: Japanese strings as their bytes in YDQJ's tables (and
        # what ja_text replaces), English ones as YDQP's: the plan carries
        # neither release's text.
        pool_ops = Ops()
        offsets = {}

        def add(text, pieces):
            if text not in offsets:
                offsets[text] = keep + pool_ops.size
                for piece in pieces:
                    if piece[0] == J:
                        pool_ops.j(*piece[1:])
                    elif piece[0] == P:
                        pool_ops.p(*piece[1:])
                    else:
                        pool_ops.r(piece[1])
                pool_ops.z(1)
            return offsets[text]

        stats = self.plan.stats
        record_words = []  # (record position, [(arg index, new offset)])
        positions = self.record_positions(en)
        keep_pal = KEEP_PAL_RECORDS.get(self.unit)
        own_lines = self.plan.ja_strings.get(self.unit, {})
        for i, (kind, _, args) in enumerate(en.records):
            idx = sorted(spos.get((kind, len(args)), ()))
            idx = [k for k in idx if args[k] != 0xFFFFFFFF and args[k] in valid]
            if not idx:
                continue
            js = jps[i]
            fixes = []
            for n, k in enumerate(idx):
                english = en.string(args[k]) or b""
                found = js[min(n, len(js) - 1)] if js is not None else None
                if found is not None and FILE_NAME.fullmatch(english):
                    found = None
                    stats["strings: file names"] += 1
                if found is not None and MODEL_ID.fullmatch(english):
                    found = None
                    stats["strings: model ids"] += 1
                if found is not None and keep_pal and keep_pal(args[0]):
                    found = None
                    stats["strings: kept PAL"] += 1
                # A whole line is the record's first string; bindings
                # apply to each.
                own = own_lines.get(args[0]) if args else None
                forced = own if isinstance(own, dict) else None
                if own is not None and forced is None and n == 0:
                    converted = own
                    pieces = [(R, converted)]
                    stats["strings: ja-strings.toml"] += 1
                elif found is not None:
                    text, src, at = found
                    spans = ja_text.convert_spans(text, english, forced)
                    if forced:
                        stats["strings: ja-strings.toml bindings"] += 1
                    pieces = [(J, src, at + a, b - a) if kind == "keep" else (R, a)
                              for kind, a, b in spans]
                    converted = b"".join(text[a:b] if kind == "keep" else a
                                         for kind, a, b in spans)
                    stats["strings: Japanese"] += 1
                else:
                    converted = english
                    pieces = [(P, en_ref, pool + args[k], len(english))]
                    stats["strings: English"] += 1
                fixes.append((k, add(converted, pieces)))
            record_words.append((positions[i], fixes))

        out = Ops()
        if isinstance(en, jm.NatView):
            count = len(en.records)
            out.r(struct.pack("<I", count | (keep + pool_ops.size) << 12))
            body_end = en.nat.pool
        else:
            body_end = en.pool
            out.r(struct.pack("<IIII", len(en.records), en.pool, keep + pool_ops.size,
                              kept_strings + len(offsets)))
        # Records (and whatever lies between them and the pool): PAL's,
        # string offsets replaced.
        at = 4 if isinstance(en, jm.NatView) else 16
        for start, fixes in record_words:
            for k, value in fixes:
                word = start + 4 * k
                out.p(en_ref, at, word - at)
                out.r(struct.pack("<I", value))
                at = word + 4
        out.p(en_ref, at, body_end - at)
        out.p(en_ref, pool, keep)
        out.extend(pool_ops)
        if not isinstance(en, jm.NatView):
            out.pad(16)
        return out

    @staticmethod
    def record_positions(en):
        """Where each record's first argument is."""
        if isinstance(en, jm.NatView):
            return [4 + i * en.nat.record_size for i in range(len(en.records))]
        return en.positions


# ---------------------------------------------------------------------------
# Archives

def gpc2_ops(pal_ref, pal_data, contents):
    """A GPC2 with `pal_data`'s entries (names, order, flags) and the
    contents given: {name: Ops}; identical contents are stored once."""
    table, pos = l5.decompress(pal_data, 0x14)
    names, _ = l5.decompress(pal_data, (pos + 3) & ~3)
    head4, = struct.unpack_from("<H", pal_data, 4)
    flags = struct.unpack_from("<I", pal_data, 0x10)[0] >> 24 & ~0x10
    count = len(table) // 12
    entries = []
    for i in range(count):
        h, w2, w3 = struct.unpack_from("<III", table, 12 * i)
        name_off = (w3 >> 24) << 8 | (w2 >> 24)
        name = names[name_off:names.index(b"\0", name_off)].decode("ascii")
        entries.append((h, name_off, name))
    blocks, where = [], {}
    data = Ops()
    for h, name_off, name in entries:
        c = contents[name]
        key = (c.stored, tuple(c.ops))
        if key not in where:
            if c.stored:
                where[key] = (data.size, c.size)
            else:
                where[key] = (data.size, 4 + c.size)
                data.r(struct.pack("<I", c.size << 3))
            data.extend(c)
            data.pad(4)
        blocks.append(where[key])
    new_table = bytearray()
    for (h, name_off, _), (off, size) in zip(entries, blocks):
        new_table += struct.pack("<III", h, (name_off & 0xFF) << 24 | off // 4,
                                 (name_off >> 8) << 24 | size)
    table_block = struct.pack("<I", len(new_table) << 3) + new_table
    names_padded = names + b"\0" * (-len(names) % 4)
    names_block = struct.pack("<I", len(names_padded) << 3) + names_padded
    names_at = 0x14 + len(table_block)
    names_at += -names_at % 4
    base = names_at + len(names_block)
    base += -base % 4
    head = struct.pack("<4sHHHHHHI", b"GPC2", head4, 0x14 // 4, names_at // 4,
                       base // 4, len(new_table) // 4, len(names_padded) // 4,
                       data.size // 4 | flags << 24)
    # The names and their hashes are YDQP's ("path##table" / "path##names":
    # its decompressed entry table and name block); the plan carries only
    # where the entries are.
    out = Ops()
    out.r(head + table_block[:4])
    for i in range(count):
        out.p(pal_ref + "##table", 12 * i, 4)
        out.r(new_table[12 * i + 4:12 * i + 12])
    out.pad(4)
    out.r(names_block[:4])
    out.p(pal_ref + "##names", 0, len(names))
    out.z(len(names_padded) - len(names))
    out.pad(4)
    out.extend(data)
    return out


# ---------------------------------------------------------------------------
# Other files

# Taken from JP as they are when they differ: the movies, re-encoded for
# the Japanese release (their own subtitles and credits).
JP_AS_IT_IS = {".mods"}

# The Japanese fonts: FONT_FILE gets FONT_BASE appended, then 16 bytes:
# FONT_TRAILER, u32 offset and u32 size of FONT_BASE's copy.
FONT_FILE = "data/pack/font.gp2"
FONT_BASE = "data/pack_lv5/font_lv5.gp2"
FONT_TRAILER = b"DQ9JFONT"

# Per-language units that are not text tables are taken from YDQJ only when
# they are images (their text drawn in Japanese). Anything else keeps
# YDQP's English one: YDQJ's counterparts of the rest are other formats
# (itemdt_*.nat are itemdata_*3.cn there; the equipment setup overran the
# heap on them).
JP_IMAGES = {".pac", ".spr", ".ncgr", ".lia"}


def diff_ops(ref, x, jref, y, k=16):
    """`x` (PAL's bytes, ref `ref`) from `y` (JP's, `jref`): runs of at
    least `k` bytes that `y` has too are J ops, the rest P."""
    index = {}
    for j in range(0, len(y) - k + 1, k):
        index.setdefault(y[j:j + k], j)
    out = Ops()
    i = lit = 0  # lit: start of the pending PAL bytes
    while i < len(x):
        j = index.get(x[i:i + k])
        if j is None:
            i += 1
            continue
        back = 0
        while back < i - lit and j - back > 0 and x[i - back - 1] == y[j - back - 1]:
            back += 1
        i, j = i - back, j - back
        n = back + k
        step = 4096
        while i + n < len(x) and j + n < len(y):
            m = min(step, len(x) - i - n, len(y) - j - n)
            if x[i + n:i + n + m] == y[j + n:j + n + m]:
                n += m
                continue
            if step > 1:
                step //= 16
                continue
            break
        out.p(ref, lit, i - lit)
        out.j(jref, j, n)
        i += n
        lit = i
    out.p(ref, lit, len(x) - lit)
    return out


# ---------------------------------------------------------------------------
# The name keyboard

# YDQP's character creation types names on a keyboard laid out by
# data/bin/keyboard_cm.bin (the hero) and keyboard_cs.bin (companions),
# drawn by the pages of bg_cm_nm_<lg>.pac. For the Japanese release both
# become its 50-sound kana grid: hiragana on page 1, katakana on page 2
# (the shifted strings), and the control column of the grid (カナ/かな,
# ゜, ゛, back, 自動 = a random name, おわる). The runner voices the last
# kana for ゛ and ゜ (Dq9NameKeyboardVoice, src/hooks/ja_font.cpp).
KEYBOARD_FILES = {"data/bin/keyboard_cm.bin", "data/bin/keyboard_cs.bin"}
KEYBOARD_HIRAGANA = [
    "あいうえお", "はひふへほ", "かきくけこ", "まみむめも", "さしすせそ", "やゆよわん",
    "たちつてと", "らりるれろ", "なにぬねの", "ゃゅょっを", "ぁぃぅぇぉ", "ー．…？！"]
KEYBOARD_KATAKANA = [
    "アイウエオ", "ハヒフヘホ", "カキクケコ", "マミムメモ", "サシスセソ", "ヤユヨワン",
    "タチツテト", "ラリルレロ", "ナニヌネノ", "ャュョッヲ", "ァィゥェォ", "ー．…？！"]


def keyboard_bytes():
    """The kana layout, in the keyboard files' format: a header (u32
    record count, u32 pool offset, u32 pool size, u32 YDQP's own value,
    left out here), then records: u16 kind, u8 argument count, 2-bit
    argument types (0 a pool offset, -1 none; 1 an integer), LSB first,
    padded with 0xFF to 4 bytes, then the u32 arguments. Kinds: 0x64 (the
    most keys), 0x65 a key (id, x, y, string, shifted string, pages, action,
    size), 0x66 (the number of grids), 0x67 a grid for the D-pad (pages,
    columns, rows, key ids by row, -1 none). Actions: 0 type the string,
    1 switch pages 1 and 2, 5 back, 6 done, 8 a random name. Sizes: 16, 24,
    32 px wide (0, 1, 2), 16 tall."""
    pool = bytearray()
    where = {}

    def string(s):
        if s is None:
            return None
        if s not in where:
            where[s] = len(pool)
            pool.extend(s.encode("cp932") + b"\0")
        return where[s]

    S, I = 0, 1
    records = [(0x64, [(I, 144)])]
    grid = []
    for r in range(6):
        row = []
        for c in range(10):
            block, col = divmod(c, 5)
            kid = 1 + r * 10 + c
            x = (24, 112)[block] + 16 * col
            y = 56 + 16 * r
            lo = KEYBOARD_HIRAGANA[2 * r + block][col]
            hi = KEYBOARD_KATAKANA[2 * r + block][col]
            records.append((0x65, [(I, kid), (I, x), (I, y), (S, string(lo)), (S, string(hi)),
                                   (I, 3), (I, 0), (I, 0)]))
            row.append(kid)
        grid.append(row)
    controls = [  # (string, pages, action, size)
        (None, 15, 1, 1),   # カナ / かな
        ("゜", 3, 0, 1),
        ("゛", 3, 0, 1),
        (None, 15, 5, 1),   # back
        (None, 15, 8, 1),   # 自動
        (None, 15, 6, 2),   # おわる
    ]
    for r, (s, pages, action, size) in enumerate(controls):
        kid = 61 + r
        records.append((0x65, [(I, kid), (I, 208), (I, 56 + 16 * r), (S, string(s)),
                               (S, string(s)), (I, pages), (I, action), (I, size)]))
        grid[r].append(kid)
    records.append((0x66, [(I, 1)]))
    records.append((0x67, [(I, 3), (I, 11), (I, 6)] + [(I, k) for row in grid for k in row]))

    body = bytearray()
    for kind, args in records:
        head = bytearray(struct.pack("<HB", kind, len(args)))
        for i in range(0, len(args), 4):
            b = 0
            for k, (t, _) in enumerate(args[i:i + 4]):
                b |= t << (2 * k)
            head.append(b)
        head.extend(b"\xff" * (-len(head) % 4))
        body += head
        for _, v in args:
            body += struct.pack("<i", -1 if v is None else v)
    at = 16 + len(body)
    at += -at % 16
    body.extend(b"\0" * (at - 16 - len(body)))
    pool.extend(b"\xff" * (-len(pool) % 4))
    return struct.pack("<III", len(records), at, len(pool)), bytes(body + pool)


def pac_members(data):
    """The members of an animation pac (bg_*.pac): a 0x50-byte header each
    (the name, then at 0x40 u32 data start, u32 data size, u32 next member,
    relative to the header), then a header of zeros. [(offset, name,
    start, size, next)]"""
    out, p = [], 0
    while p + 0x50 <= len(data):
        start, size, nxt = struct.unpack_from("<III", data, p + 0x40)
        if not nxt:
            break
        out.append((p, data[p:p + 0x20].split(b"\0")[0].decode("ascii"), start, size, nxt))
        p += nxt
    return out, p


# bg_cm_nm_<lg>.pac's members (the game picks them by position): the four
# keyboard pages, their tiles and palettes from YDQJ's bg_cm_name.pac (the
# companions' grid, which has 自動), the windows YDQP's own.
NAME_PAC_UNIT = "data/ani/bg_cm_nm.gp2::bg_cm_nm.pac"
NAME_PAC_MEMBERS = {
    "cm_name_chara.bncg": "cm_name_chara.bncg",
    "cm_name_small.bnsc": "cm_name_hiragana_npc.bnsc",
    "cm_name_capital.bnsc": "cm_name_katakana_npc.bnsc",
    "cm_name_europe.bnsc": "cm_name_hiragana_npc.bnsc",
    "cm_name_symbol.bnsc": "cm_name_katakana_npc.bnsc",
    "cm_name_npc_pltt.bncl": "cm_name_npc_pltt.bncl",
    "cm_name_pltt.bncl": "cm_name_pltt.bncl",
}
# YDQP's 4x2-tile "?" key for its random-name key, drawn over the page
# where the grid has its ？ and ！ keys: those (tiles 20..23 of rows 17
# and 18 of the page).
NAME_PAC_QUE = "cm_name_que.bnsc"


def name_pac_ops(pal_ref, pal_data, jp_ref, jp_data):
    jm_, _ = pac_members(jp_data)
    theirs = {name: (off, start, size) for off, name, start, size, _ in jm_}
    members, end = pac_members(pal_data)
    out = Ops()
    for off, name, start, size, nxt in members:
        if name in NAME_PAC_MEMBERS:
            joff, jstart, jsize = theirs[NAME_PAC_MEMBERS[name]]
            new_next = 0x50 + jsize
            new_next += -new_next % 16
            out.p(pal_ref, off, 0x40)
            out.r(struct.pack("<III", 0x50, jsize, new_next))
            out.p(pal_ref, off + 0x4C, 4)
            out.j(jp_ref, joff + jstart, jsize)
            out.z(new_next - 0x50 - jsize)
        elif name == NAME_PAC_QUE:
            page_off, page_start, _ = theirs[NAME_PAC_MEMBERS["cm_name_small.bnsc"]]
            entries = page_off + page_start + 0x10
            out.p(pal_ref, off, start + 0x10)
            for row in (17, 18):
                out.j(jp_ref, entries + 2 * (row * 32 + 20), 8)
            out.z(nxt - start - 0x20)
        else:
            out.p(pal_ref, off, nxt)
    out.p(pal_ref, end, len(pal_data) - end)
    return out


# ---------------------------------------------------------------------------
# The plan

class Planner:
    def __init__(self, pal_dir, jp_dir, cache):
        self.pal = Release(pal_dir, cache)
        self.jp = Release(jp_dir, cache)
        with open(jm.RENAMES, "rb") as f:
            files = tomllib.load(f)
        self.sources = jm.Sources(self.jp.idx, files["renames"],
                                  files.get("pal_only", {}).get("prefixes", []))
        self.stats = collections.Counter()
        self.ja_strings = load_ja_strings()
        jp_sha = set(self.jp.sha)
        self.units, _ = jm.pal_units(self.pal.idx, jp_sha)
        # PAL ref (relative to data/) of every language -> its unit
        self.unit_of = {}
        for unit, langs in self.units.items():
            for lg, r in langs.items():
                self.unit_of[r] = unit
        self.built = {}
        self._stored = None

    def jp_whole(self, ref):
        """Ops copying a JP file or member (ja-map ref) whole."""
        out = Ops()
        out.j(folder_ref(ref), 0, len(self.jp.get(ref)))
        return out

    def pal_whole(self, ref):
        out = Ops()
        out.p(folder_ref(ref), 0, len(self.pal.get(ref)))
        return out

    def same(self, ref):
        """A PAL file or member that is not per language: JP's when JP has
        it byte for byte, else PAL's bytes made from its JP counterpart
        where they agree (diff_ops)."""
        data = self.pal.get(ref)
        jref = self.jp.sha.get(hashlib.sha1(data).hexdigest())
        if jref:
            self.stats["pieces: from JP"] += 1
            return self.jp_whole(jref)
        cp = jm.piece_counterpart(ref, self.sources)
        theirs = self.jp.get(cp) if cp else None
        if theirs is not None and os.path.splitext(ref)[1] in JP_AS_IT_IS:
            self.stats["pieces: JP's own"] += 1
            return self.jp_whole(cp)
        if theirs is None:
            self.stats["pieces: PAL bytes"] += 1
            self.stats["pieces: PAL bytes, size"] += len(data)
            return self.pal_whole(ref)
        out = diff_ops(folder_ref(ref), data, folder_ref(cp), theirs)
        self.stats["pieces: from JP and PAL"] += 1
        self.stats["pieces: from JP and PAL, PAL bytes"] += sum(
            op[3] for op in out.ops if op[0] == P)
        return out

    def stored_index(self):
        """{SHA-1 of a stored block: "path#member"} over YDQJ's GPC2s."""
        if self._stored is None:
            self._stored = {}
            for path, e in self.jp.idx.items():
                if e["members"] is None:
                    continue
                data = self.jp.read(path)
                if not l5.is_gpc2(data):
                    continue
                for name, block in l5.read_gpc2_stored(data).items():
                    self._stored.setdefault(hashlib.sha1(block).hexdigest(), f"{path}#{name}")
        return self._stored

    def stored_member(self, path, name, pal_stored):
        """A member of a PAL GPC2 that is not per language, as PAL stores
        it: YDQJ's identical block, else PAL's block made from its YDQJ
        counterpart's (diff_ops), so its size and compression stay
        PAL's. The game sizes buffers by the stored size (the party's
        models in chara_pc.gp2 overran one when stored uncompressed)."""
        block = pal_stored[name]
        out = Ops(stored=True)
        jref = self.stored_index().get(hashlib.sha1(block).hexdigest())
        if jref:
            self.stats["pieces: JP's block"] += 1
            out.j(folder_ref(jref), 0, len(block))
            return out
        ref = f"{path}#{name}"
        cp = jm.piece_counterpart(f"{path}::{name}", self.sources)
        theirs = None
        if cp and "::" in cp:
            jpath, _, jname = cp.partition("::")
            jdata = self.jp.read(jpath)
            if l5.is_gpc2(jdata):
                theirs = l5.read_gpc2_stored(jdata).get(jname)
                cp = f"{jpath}#{jname}"
        if theirs is None:
            self.stats["pieces: PAL block"] += 1
            self.stats["pieces: PAL block, size"] += len(block)
            out.p(folder_ref(ref), 0, len(block))
            return out
        diff = diff_ops(folder_ref(ref), block, folder_ref(cp), theirs)
        self.stats["pieces: PAL block from JP's"] += 1
        self.stats["pieces: PAL block from JP's, PAL bytes"] += sum(
            op[3] for op in diff.ops if op[0] == P)
        out.extend(diff)
        return out

    def unit_ops(self, unit):
        if unit in self.built:
            return self.built[unit]
        langs = self.units[unit]
        ref, how = self.sources.find(unit)
        if ref is not None and self.jp.get(ref) is None:
            ref = None
        out = None
        if unit == NAME_PAC_UNIT and ref is not None:
            out = name_pac_ops(folder_ref(langs["en"]), self.pal.get(langs["en"]),
                               folder_ref(ref), self.jp.get(ref))
            self.stats["units: name keyboard"] += 1
        if out is None and "en" in langs and                 jm.parse_any(self.pal.get(langs["en"])) is not None:
            out = Text(self, unit, langs, ref).build()
            self.stats["units: text"] += 1
        if out is None and ref is not None and                 os.path.splitext(ref)[1].lower() in JP_IMAGES:
            out = self.jp_whole(ref)
            self.stats["units: JP as it is"] += 1
        if out is None:
            self.stats["units: PAL English"] += 1
            out = self.pal_whole(langs.get("en") or next(iter(langs.values())))
        self.built[unit] = out
        return out

    def font_ops(self, path):
        """pack/font.gp2 (the same in both releases; YDQP reads none of its
        members) followed by YDQJ's base fonts, pack_lv5/font_lv5.gp2
        (YDQP's is a stub), which the runner's Japanese font is made from
        (src/hooks/ja_font.cpp). FONT_TRAILER says where they are."""
        out = Ops()
        out.extend(self.jp_whole(path))
        out.pad(16)
        at = out.size
        out.extend(self.jp_whole(FONT_BASE))
        size = out.size - at
        out.pad(16)
        out.r(FONT_TRAILER + struct.pack("<II", at, size))
        self.stats["files: fonts"] += 1
        return out

    def file_ops(self, path):
        """Ops for a PAL data file (relative to data/)."""
        if path == FONT_FILE:
            return self.font_ops(path)
        if path in KEYBOARD_FILES:
            head, body = keyboard_bytes()
            out = Ops()
            out.r(head)
            out.p(folder_ref(path), 12, 4)
            out.r(body)
            self.stats["files: name keyboard"] += 1
            return out
        e = self.pal.idx[path]
        jpath = self.jp.sha.get(e["sha1"])
        if jpath:
            self.stats["files: from JP"] += 1
            return self.jp_whole(jpath)
        if path in self.unit_of:
            self.stats["files: loose text"] += 1
            return self.unit_ops(self.unit_of[path])
        data = self.pal.read(path)
        if e["members"] is not None and l5.is_gpc2(data):
            # A per-language archive (iteminfo_fr.gp2) is built from the
            # English one, its members named the same.
            d, name = os.path.split(path)
            stripped, lang = jm.strip_lang(name)
            src = path
            if lang:
                src = f"{d}/{os.path.splitext(stripped)[0]}_en{os.path.splitext(stripped)[1]}"
                data = self.pal.read(src)
            raw = struct.unpack_from("<I", data, 0x10)[0] & 0x10000000
            stored = None if raw else l5.read_gpc2_stored(data)
            contents = {}
            for mname, _, _ in self.pal.idx[src]["members"]:
                mref = f"{src}::{mname}"
                unit = self.unit_of.get(mref)
                if unit:
                    contents[mname] = self.unit_ops(unit)
                elif stored is not None:
                    contents[mname] = self.stored_member(src, mname, stored)
                else:
                    contents[mname] = self.same(mref)
            self.stats["files: archives rebuilt"] += 1
            return gpc2_ops(folder_ref(src), data, contents)
        return self.same(path)

    def plan(self):
        files = self.layout()
        for fpath in self.pal.fat():
            files.append((fpath, self.file_ops(fpath[len("data/"):])))
        return files

    def layout(self):
        """The rest of an unpacked ROM in YDQP's layout, with YDQJ's code:
        the folder YDQJ.recipe (ndsrecomp tools/release_recipe.py) then
        rebuilds YDQP's code from. YDQJ's ARM9 and overlays go in as they
        are (compressed; the two ARM9 footers are the same), so the header
        gives YDQJ's ARM9 size and the footer sits where YDQJ has it."""
        pal, jp = self.pal.folder, self.jp.folder

        def read(folder, name):
            with open(os.path.join(folder, name), "rb") as f:
                return f.read()

        def manifest(folder):
            with open(os.path.join(folder, "rom.toml"), "rb") as f:
                return tomllib.load(f)

        pm, jmf = manifest(pal), manifest(jp)
        files = []

        def jp_file(name, as_name=None):
            out = Ops()
            out.j(name, 0, len(read(jp, name)))
            files.append((as_name or name, out))

        def pal_file(name):
            out = Ops()
            out.p(name, 0, len(read(pal, name)))
            files.append((name, out))

        pal_header, jp_header = read(pal, "header.bin"), read(jp, "header.bin")
        jp_arm9_end = struct.unpack_from("<I", jp_header, 0x20)[0] + \
            struct.unpack_from("<I", jp_header, 0x2C)[0]
        pal_arm9_end = struct.unpack_from("<I", pal_header, 0x20)[0] + \
            struct.unpack_from("<I", pal_header, 0x2C)[0]
        footer = next(n for n, o in jmf["extra"].items() if o == jp_arm9_end)
        pal_footer = next(n for n, o in pm["extra"].items() if o == pal_arm9_end)
        if read(jp, footer) != read(pal, pal_footer):
            raise SystemExit("the ARM9 footers differ")

        out = Ops()
        out.p("header.bin", 0, 0x2C)
        out.r(jp_header[0x2C:0x30])
        out.p("header.bin", 0x30, len(pal_header) - 0x30)
        files.append(("header.bin", out))

        # The overlay table: YDQP's, with YDQJ's compressed sizes.
        y9 = read(pal, "y9.bin")
        jy9 = read(jp, "y9.bin")
        if len(y9) != len(jy9):
            raise SystemExit("the overlay tables differ in length")
        out = Ops()
        for at in range(0, len(y9), 32):
            oid = struct.unpack_from("<I", y9, at)[0]
            if oid != struct.unpack_from("<I", jy9, at)[0] or y9[at + 0x1F] != jy9[at + 0x1F]:
                raise SystemExit(f"overlay table entry {at // 32}: ids or flags differ")
            out.p("y9.bin", at, 0x1C)
            out.r(jy9[at + 0x1C:at + 0x1F])
            out.p("y9.bin", at + 0x1F, 1)
        files.append(("y9.bin", out))

        for name in ("fnt.bin", "banner.bin"):
            pal_file(name)
        for name in pm["regions"]:
            if name in ("arm9.bin", "arm7.bin"):
                jp_file(name)
        for name, _, _ in pm["files"]["fat"]:
            if name.startswith("overlay/"):
                jp_file(name)
        for name in pm["extra"]:
            if name != pal_footer:
                pal_file(name)
        jp_file(footer)

        # rom.toml: YDQP's, naming YDQJ's image and its footer.
        text = read(pal, "rom.toml")
        pal_sha = pm["rom"]["sha1"].encode()
        edits = [(pal_sha, jmf["rom"]["sha1"].encode()),
                 (f'"{pal_footer}" = 0x{pal_arm9_end:X}'.encode(),
                  f'"{footer}" = 0x{jp_arm9_end:X}'.encode())]
        spans = sorted((text.index(a), a, b) for a, b in edits)
        out = Ops()
        at = 0
        for pos, a, b in spans:
            out.p("rom.toml", at, pos - at)
            out.r(b)
            at = pos + len(a)
        out.p("rom.toml", at, len(text) - at)
        files.append(("rom.toml", out))
        return files


def encode(jp_sha, pal_sha, files, literals=b""):
    """The plan's bytes. `files`: [(path, Ops, sha1)]."""
    sources, src_id = [], {}
    refs, ref_id = [], {}

    def intern(table, ids, name):
        if name not in ids:
            ids[name] = len(table)
            table.append(name)
        return ids[name]

    body = bytearray()
    for path, ops, digest in files:
        enc = bytearray()
        for op in ops.ops:
            if op[0] == J:
                enc += bytes([J]) + leb(intern(sources, src_id, op[1])) + leb(op[2]) + leb(op[3])
            elif op[0] == P:
                enc += bytes([P]) + leb(intern(refs, ref_id, op[1])) + leb(op[2]) + leb(op[3])
            elif op[0] == R:
                enc += bytes([R]) + leb(len(op[1])) + op[1]
            else:
                enc += bytes([Z]) + leb(op[1])
        name = path.encode()
        body += struct.pack("<H", len(name)) + name
        body += struct.pack("<I", ops.size) + digest
        body += struct.pack("<I", len(enc)) + enc
    out = bytearray(MAGIC + struct.pack("<I", VERSION) + jp_sha + pal_sha)
    for table in (sources, refs):
        out += struct.pack("<I", len(table))
        for s in table:
            b = s.encode()
            out += struct.pack("<H", len(b)) + b
    out += struct.pack("<I", len(files)) + body
    out += struct.pack("<I", len(literals)) + literals
    return bytes(out)


def decode(blob):
    if blob[:4] != MAGIC:
        raise SystemExit("not a DQ9 Japanese data plan")
    version, = struct.unpack_from("<I", blob, 4)
    if version != VERSION:
        raise SystemExit(f"plan version {version}, expected {VERSION}")
    jp_sha, pal_sha = blob[8:28], blob[28:48]
    p = 48

    def string():
        nonlocal p
        n, = struct.unpack_from("<H", blob, p)
        s = blob[p + 2:p + 2 + n].decode()
        p += 2 + n
        return s

    tables = []
    for _ in range(2):
        n, = struct.unpack_from("<I", blob, p)
        p += 4
        tables.append([string() for _ in range(n)])
    n, = struct.unpack_from("<I", blob, p)
    p += 4
    files = []
    for _ in range(n):
        path = string()
        size, = struct.unpack_from("<I", blob, p)
        digest = blob[p + 4:p + 24]
        olen, = struct.unpack_from("<I", blob, p + 24)
        files.append((path, size, digest, blob[p + 28:p + 28 + olen]))
        p += 28 + olen
    n, = struct.unpack_from("<I", blob, p)
    return dict(jp_sha=jp_sha, pal_sha=pal_sha, sources=tables[0], refs=tables[1],
                files=files, literals_at=p, literals=blob[p + 4:p + 4 + n])


def p_ops(enc):
    """The (ref, off, n) of a file's P ops, in order."""
    out = []
    p = 0
    while p < len(enc):
        kind = enc[p]
        p += 1
        if kind in (J, P):
            a, p = read_leb(enc, p)
            off, p = read_leb(enc, p)
            n, p = read_leb(enc, p)
            if kind == P:
                out.append((a, off, n))
        else:
            n, p = read_leb(enc, p)
            if kind == R:
                p += n
    return out


def run_ops(enc, size, sources, literals, lit_at, get):
    """A file from its ops; (bytes, literals read)."""
    out = bytearray()
    p = 0
    while p < len(enc):
        kind = enc[p]
        p += 1
        if kind == J:
            src, p = read_leb(enc, p)
            off, p = read_leb(enc, p)
            n, p = read_leb(enc, p)
            data = get(sources[src])
            if off + n > len(data):
                raise ValueError(f"{sources[src]}: {off:#x}+{n:#x} past its end")
            out += data[off:off + n]
        elif kind == P:
            _, p = read_leb(enc, p)
            _, p = read_leb(enc, p)
            n, p = read_leb(enc, p)
            if lit_at + n > len(literals):
                raise ValueError("the plan's literal bytes run out (is it filled?)")
            out += literals[lit_at:lit_at + n]
            lit_at += n
        elif kind == R:
            n, p = read_leb(enc, p)
            out += enc[p:p + n]
            p += n
        elif kind == Z:
            n, p = read_leb(enc, p)
            out += bytes(n)
        else:
            raise ValueError(f"bad op {kind}")
    if len(out) != size:
        raise ValueError(f"ops give {len(out):#x} bytes, expected {size:#x}")
    return bytes(out), lit_at


class Getter:
    """Decoded sources of a release folder ("path" or "path::member")."""

    def __init__(self, folder):
        self.folder = folder
        self.files = collections.OrderedDict()
        self.members = {}

    def file(self, path):
        if path not in self.files:
            if len(self.files) > 16:
                self.files.popitem(last=False)
            with open(os.path.join(self.folder, path), "rb") as f:
                self.files[path] = f.read()
        return self.files[path]

    def __call__(self, ref):
        if "##" in ref:  # a GPC2's decompressed entry table or name block
            if ref not in self.members:
                path, _, part = ref.partition("##")
                data = self.file(path)
                table, pos = l5.decompress(data, 0x14)
                self.members.clear()
                self.members[ref] = table if part == "table" else \
                    l5.decompress(data, (pos + 3) & ~3)[0]
            return self.members[ref]
        if "#" in ref:  # a GPC2 member as stored
            path, _, member = ref.partition("#")
            key = path + "#"
            if key not in self.members:
                self.members.clear()
                self.members[key] = l5.read_gpc2_stored(self.file(path))
            return self.members[key][member]
        path, _, member = ref.partition("::")
        if not member:
            return self.file(path)
        if path not in self.members:
            self.members.clear()
            self.members[path] = dict(l5.read_archive(self.file(path)) or [])
        return self.members[path][member]


# ---------------------------------------------------------------------------

def cmd_plan(args):
    planner = Planner(args.pal, args.jp, args.cache)
    print("ja-convert: planning", flush=True)
    files = planner.plan()
    # Check each file against the reference applier, and name its SHA-1.
    pal_get, jp_get = Getter(args.pal), Getter(args.jp)
    done = []
    literal_bytes = 0
    for path, ops in files:
        data = bytearray()
        for op in ops.ops:
            if op[0] == J:
                data += jp_get(op[1])[op[2]:op[2] + op[3]]
            elif op[0] == P:
                data += pal_get(op[1])[op[2]:op[2] + op[3]]
                literal_bytes += op[3]
            elif op[0] == R:
                data += op[1]
            else:
                data += bytes(op[1])
        assert len(data) == ops.size, path
        done.append((path, ops, hashlib.sha1(data).digest()))
    blob = encode(planner.jp.image_sha1(), planner.pal.image_sha1(), done)
    with open(args.out, "wb") as f:
        f.write(blob)
    for k, v in sorted(planner.stats.items()):
        print(f"  {k}: {v}")
    print(f"{args.out}: {len(blob)} bytes, {len(done)} files, "
          f"{literal_bytes} literal bytes", flush=True)


def cmd_fill(args):
    with open(args.plan, "rb") as f:
        blob = f.read()
    plan = decode(blob)
    if plan["literals"]:
        raise SystemExit(f"{args.plan} is filled already")
    get = Getter(args.pal)
    lit = bytearray()
    for _, _, _, enc in plan["files"]:
        for ref, off, n in p_ops(enc):
            data = get(plan["refs"][ref])
            if off + n > len(data):
                raise SystemExit(f"{plan['refs'][ref]}: {off:#x}+{n:#x} past its end")
            lit += data[off:off + n]
    out = blob[:plan["literals_at"]] + struct.pack("<I", len(lit)) + bytes(lit)
    with open(args.out, "wb") as f:
        f.write(out)
    print(f"{args.out}: {len(lit)} literal bytes filled in")


def cmd_apply(args):
    with open(args.plan, "rb") as f:
        plan = decode(f.read())
    get = Getter(args.jp)
    lit_at = 0
    for path, size, digest, enc in plan["files"]:
        data, lit_at = run_ops(enc, size, plan["sources"], plan["literals"], lit_at, get)
        if hashlib.sha1(data).digest() != digest:
            raise SystemExit(f"{path}: the result's SHA-1 is not the plan's")
        dest = os.path.join(args.out, path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(data)
    print(f"{args.out}: {len(plan['files'])} files written")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("plan")
    a.add_argument("--pal", default=os.path.join(ROOT, "assets", "YDQP"))
    a.add_argument("--jp", default=os.path.join(ROOT, "assets", "YDQJ"))
    a.add_argument("--cache", default=os.path.join(ROOT, "out", "ja-map"))
    a.add_argument("out")
    a = sub.add_parser("fill")
    a.add_argument("plan")
    a.add_argument("--pal", default=os.path.join(ROOT, "assets", "YDQP"))
    a.add_argument("out")
    a = sub.add_parser("apply")
    a.add_argument("plan")
    a.add_argument("--jp", default=os.path.join(ROOT, "assets", "YDQJ"))
    a.add_argument("out")
    args = ap.parse_args(argv)
    {"plan": cmd_plan, "fill": cmd_fill, "apply": cmd_apply}[args.cmd](args)


if __name__ == "__main__":
    main(sys.argv[1:])
