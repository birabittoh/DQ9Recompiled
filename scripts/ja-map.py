#!/usr/bin/env python3
"""Maps the Europe release (YDQP) onto the Japanese one (YDQJ): which JP
file each PAL file comes from, and how much of PAL's text has a Japanese
counterpart. Groundwork for running the Europe build from a JP ROM.

    usage: ja-map.py [--pal assets/YDQP] [--jp assets/YDQJ] [--out out/ja-map]

Both releases unpacked (ndsrecomp/tools/nds_folder.py); a jp.nds in the
repo root is unpacked into assets/YDQJ if that folder is missing. Writes
to --out:

    report.md      the summary
    units.tsv      every PAL per-language unit: its JP source, how it was
                   found, records matched / unmatched
    unmatched.tsv  the PAL text records with no JP counterpart (English)
    pieces.tsv     every PAL file or archive member that is not per
                   language and differs from JP: its JP counterpart and
                   the bytes a recipe would have to carry

Matching:
  * Units: a PAL file or archive member named <base>_<de|en|es|fr|it>.
    Its JP source is, in order: scripts/ja-files.toml [renames]; a member
    <base> of the JP archive with the PAL archive's stem (C01B0.gp2 ->
    C01B0.msg::004.bin); a JP file or member of the same name.
  * Records (scripts/dq9_text_table.py): the arguments that differ
    between PAL's languages are string offsets, the rest the record's
    key. A key missing from the JP table is looked for in the same table
    of the map's other story phases (PAL copies C01Q0's later lines into
    C01B0..C01O0), then anywhere, then as its first occurrence. A .nat
    whose records changed size is matched on the column the two share.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "ndsrecomp", "tools"))
import l5_archive as l5  # noqa: E402
import dq9_text_table as tt  # noqa: E402

try:
    import tomllib
except ImportError:  # Python < 3.11
    import tomli as tomllib

LANGS = ("en", "fr", "de", "it", "es")
LANG_NAME = re.compile(r"^(.*)_(de|en|es|fr|it)(\.[^./]+)?$")
RENAMES = os.path.join(ROOT, "scripts", "ja-files.toml")


# ---------------------------------------------------------------------------
# Releases

class Release:
    """An unpacked release: its files under data/, archives opened lazily."""

    def __init__(self, folder):
        self.folder = folder
        self.base = os.path.join(folder, "data")
        self._open = collections.OrderedDict()

    def read(self, path):
        with open(os.path.join(self.base, path), "rb") as f:
            return f.read()

    def members(self, path):
        if path not in self._open:
            if len(self._open) > 32:
                self._open.popitem(last=False)
            self._open[path] = dict(l5.read_archive(self.read(path)) or [])
        return self._open[path]

    def get(self, ref):
        """A file ("data/x.bin") or archive member ("data/x.gp2::y.bin")."""
        path, _, member = ref.partition("::")
        if not member:
            return self.read(path)
        return self.members(path).get(member)

    def index(self, cache_dir):
        """{path: {"sha1", "size", "members": [[name, size, sha1]] or None}},
        cached next to the report by the folder's ROM SHA-1."""
        rom_sha = re.search(r'sha1\s*=\s*"([0-9a-f]+)"',
                            open(os.path.join(self.folder, "rom.toml")).read()).group(1)
        cache = os.path.join(cache_dir, f"index-{rom_sha}.json")
        if os.path.exists(cache):
            with open(cache) as f:
                return json.load(f)
        out = {}
        for dirpath, _, names in os.walk(self.base):
            for name in names:
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, self.base).replace("\\", "/")
                data = open(full, "rb").read()
                members = l5.read_archive(data)
                out[rel] = {
                    "sha1": hashlib.sha1(data).hexdigest(),
                    "size": len(data),
                    "members": None if members is None else
                    [[n, len(m), hashlib.sha1(m).hexdigest()] for n, m in members],
                }
        os.makedirs(cache_dir, exist_ok=True)
        with open(cache, "w") as f:
            json.dump(out, f)
        return out


def strip_lang(name):
    m = LANG_NAME.match(name)
    return (m.group(1) + (m.group(3) or ""), m.group(2)) if m else (name, None)


def unit_ref(unit, lang):
    """The PAL file or member of `unit` in `lang`."""
    container, _, member = unit.partition("::")
    if member:
        cdir, cname = os.path.split(container)
        cbase, cext = os.path.splitext(cname)
        if cbase in LANG_CONTAINERS.get(cdir, ()):
            container = f"{cdir}/{cbase}_{lang}{cext}"
            return f"{container}::{member}"
        base, ext = os.path.splitext(member)
        return f"{container}::{base}_{lang}{ext}"
    d, name = os.path.split(unit)
    base, ext = os.path.splitext(name)
    return f"{d}/{base}_{lang}{ext}"


LANG_CONTAINERS = collections.defaultdict(set)  # dir -> archive stems named _<lang>


def pal_units(pal_index, jp_sha):
    """Per-language units ({unit: {lang: ref}}), and the PAL pieces that
    are not per language and differ from every JP file and member."""
    units = collections.defaultdict(dict)
    pieces = []
    for path, e in sorted(pal_index.items()):
        d, name = os.path.split(path)
        stripped, lang = strip_lang(name)
        if e["members"] is None:
            if e["sha1"] in jp_sha:
                continue
            if lang:
                units[f"{d}/{stripped}"][lang] = path
            else:
                pieces.append(path)
            continue
        if e["sha1"] in jp_sha:
            continue
        container = path
        if lang:  # iteminfo_en.gp2: one archive per language
            container = f"{d}/{stripped}"
            LANG_CONTAINERS[d].add(os.path.splitext(stripped)[0])
        for mname, _, msha in e["members"]:
            mstripped, mlang = strip_lang(mname)
            if lang or mlang:
                units[f"{container}::{mstripped}"][lang or mlang] = f"{path}::{mname}"
            elif msha not in jp_sha:
                pieces.append(f"{path}::{mname}")
    return units, pieces


# ---------------------------------------------------------------------------
# Finding JP sources

class Sources:
    def __init__(self, jp_index, renames, pal_only=()):
        self.jp = jp_index
        self.renames = renames
        self.pal_only = tuple(pal_only)
        self.by_stem = collections.defaultdict(list)
        self.by_name = collections.defaultdict(list)
        self.by_member = collections.defaultdict(list)
        for path, e in jp_index.items():
            self.by_stem[os.path.splitext(path)[0]].append(path)
            self.by_name[os.path.basename(path)].append(path)
            for n, *_ in e["members"] or []:
                self.by_name[n].append(f"{path}::{n}")
                self.by_member[n].append(path)

    def find(self, unit):
        """(JP ref or None, how it was found)."""
        if self.pal_only and unit.startswith(self.pal_only):
            return None, "PAL only"
        if unit in self.renames:
            target = self.renames[unit]
            return (target, "rename") if target else (None, "PAL only")
        container, _, member = unit.partition("::")
        name = member or os.path.basename(unit)
        if member:
            for jpath in self.by_stem.get(os.path.splitext(container)[0], []):
                for n, *_ in self.jp[jpath]["members"] or []:
                    if n == name or os.path.splitext(n)[0] == os.path.splitext(name)[0]:
                        return f"{jpath}::{n}", "same archive"
        hits = self.by_name.get(name)
        if hits:
            return hits[0], "same name"
        return None, "not found"

    def fallbacks(self, ref):
        """JP tables to try for keys `ref` lacks: the same member in the
        map's other story phases (latest first), then anywhere."""
        path, _, member = ref.partition("::")
        if not member:
            return []
        stem = os.path.basename(path)[:3]
        others = [p for p in self.by_member.get(member, []) if p != path]
        same_map = sorted((p for p in others if os.path.basename(p)[:3] == stem), reverse=True)
        return [f"{p}::{member}" for p in same_map + [p for p in others if p not in same_map]]


# ---------------------------------------------------------------------------
# Records

class NatView:
    """A .nat with the Table interface: kind 0, the record words as args."""

    def __init__(self, nat):
        self.nat = nat
        self.records = [(0, 0, r) for r in nat.records]

    def string(self, offset):
        return self.nat.string(offset)


def parse_any(data):
    if data is None:
        return None
    table = tt.parse(data)
    if table is not None:
        return table
    nat = tt.parse_nat(data)
    return NatView(nat) if nat is not None else None


def string_positions(tables):
    """Per (kind, argc): the argument indices that are string offsets,
    from the PAL languages of one table: the arguments typed as pool
    offsets (a table whose only string is at offset 0 in every language
    has no other sign of it), and those that differ between languages
    (ttldata's offsets are typed as integers)."""
    en = tables["en"]
    out = collections.defaultdict(set)
    if isinstance(en, NatView) and en.nat.record_size == 8:
        out[(0, 2)].add(1)  # (id, string offset), even when every offset is 0
    if not isinstance(en, NatView):
        for i, (kind, _, args) in enumerate(en.records):
            out[(kind, len(args))].update(en.string_args(i))
    for lang, t in tables.items():
        if lang == "en" or t is None:
            continue
        diff = tt.string_args(en, t)
        if diff is None:
            continue
        for (kind, _, args), idx in zip(en.records, diff):
            out[(kind, len(args))].update(idx)
    return out


def record_keys(table, spos):
    """[(key, string offsets)] in order; the key ends with the occurrence."""
    seen = collections.Counter()
    out = []
    for kind, _, args in table.records:
        strings = spos.get((kind, len(args)), set())
        key = (kind, tuple(v for i, v in enumerate(args) if i not in strings))
        seen[key] += 1
        out.append((key + (seen[key],), [args[i] for i in sorted(strings)]))
    return out


class Matcher:
    def __init__(self, jp, sources):
        self.jp = jp
        self.sources = sources
        self._keys = {}

    def keys(self, ref, spos):
        k = (ref, tuple(sorted((a, tuple(sorted(b))) for a, b in spos.items())))
        if k not in self._keys:
            table = parse_any(self.jp.get(ref))
            self._keys[k] = dict(record_keys(table, spos)) if table else {}
        return self._keys[k]

    def match(self, pal_tables, ref):
        """(matched, unmatched records, [(key, English text)], how)."""
        en = pal_tables["en"]
        jt = parse_any(self.jp.get(ref))
        if jt is None:
            return None
        spos = string_positions(pal_tables)
        if isinstance(en, NatView) and isinstance(jt, NatView) and \
                en.nat.record_size != jt.nat.record_size:
            return self.match_restructured(en, jt, spos)
        if isinstance(en, NatView) and en.nat.record_size == 8 and not isinstance(jt, NatView):
            return self.match_nat_to_table(en, jt)
        own = dict(record_keys(jt, spos))
        chain = None
        matched, missing = 0, []
        for key, strings in record_keys(en, spos):
            if not strings:
                continue
            if key in own:
                matched += 1
                continue
            if chain is None:
                chain = [self.keys(r, spos) for r in self.sources.fallbacks(ref)]
            first = key[:-1] + (1,)
            if any(key in c for c in chain) or any(first in c for c in [own] + chain):
                matched += 1
            else:
                text = en.string(strings[-1]) or b""
                missing.append((key, text.decode("latin-1")))
        return matched, len(missing), missing, "records"

    @staticmethod
    def match_nat_to_table(en, jt):
        """A PAL (id, string) .nat for a JP record table (bstr*.nat ->
        bstr.pac::bstr*.bin): JP records whose first argument is the id
        and whose last points at a string."""
        jids = {args[0] for _, _, args in jt.records
                if len(args) >= 2 and jt.string(args[-1])}
        missing = [(("id", r[0]), (en.string(r[1]) or b"").decode("latin-1"))
                   for r in en.nat.records if r[0] not in jids]
        return len(en.records) - len(missing), len(missing), missing, ".nat -> record table, by id"

    @staticmethod
    def match_restructured(en, jt, spos):
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
            return 0, len(en.records), [], "restructured, no shared column"
        jids = {r[ji] & 0xFFFF for r in jt.nat.records}
        missing = [(("id", r[pi] & 0xFFFF), "") for r in en.nat.records
                   if (r[pi] & 0xFFFF) not in jids]
        how = f"restructured: PAL word {pi} = JP word {ji}, " \
              f"{en.nat.record_size} -> {jt.nat.record_size}-byte records"
        return len(en.records) - len(missing), len(missing), missing, how


# ---------------------------------------------------------------------------
# Recipe cost of differing pieces

def literal_cost(x, y, k=16):
    """Bytes of `x` not covered by runs of `y` at least `k` long."""
    index = {}
    for j in range(0, len(y) - k + 1, k):
        index.setdefault(y[j:j + k], j)
    i = miss = 0
    while i < len(x):
        j = index.get(x[i:i + k])
        if j is None:
            miss += 1
            i += 1
            continue
        back = 0
        while back < miss and j - back > 0 and x[i - back - 1] == y[j - back - 1]:
            back += 1
        miss -= back
        n = k
        while i + n < len(x) and j + n < len(y) and x[i + n] == y[j + n]:
            n += 1
        i += n
    return miss


def piece_counterpart(ref, sources):
    path, _, member = ref.partition("::")
    name = member or os.path.basename(path)
    for jpath in sources.by_stem.get(os.path.splitext(path)[0], []):
        for n, *_ in sources.jp[jpath]["members"] or []:
            if n == name:
                return f"{jpath}::{n}"
    if not member and path in sources.jp:
        return path
    hits = sources.by_name.get(name)
    return hits[0] if hits else None


# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pal", default=os.path.join(ROOT, "assets", "YDQP"))
    ap.add_argument("--jp", default=os.path.join(ROOT, "assets", "YDQJ"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "ja-map"))
    args = ap.parse_args(argv)

    if not os.path.exists(os.path.join(args.jp, "rom.toml")):
        jp_rom = os.path.join(ROOT, "jp.nds")
        if not os.path.exists(jp_rom):
            sys.exit(f"ja-map: no {args.jp} and no {jp_rom} to unpack")
        import nds_folder
        from pathlib import Path
        print(f"ja-map: unpacking {jp_rom} into {args.jp}", flush=True)
        nds_folder.unpack(open(jp_rom, "rb").read(), Path(args.jp))

    pal, jp = Release(args.pal), Release(args.jp)
    print("ja-map: indexing (cached after the first run)", flush=True)
    pal_index, jp_index = pal.index(args.out), jp.index(args.out)
    jp_sha = set()
    for e in jp_index.values():
        jp_sha.add(e["sha1"])
        jp_sha.update(m[2] for m in e["members"] or [])

    with open(RENAMES, "rb") as f:
        files = tomllib.load(f)
    renames = files["renames"]
    sources = Sources(jp_index, renames, files.get("pal_only", {}).get("prefixes", []))
    matcher = Matcher(jp, sources)
    units, pieces = pal_units(pal_index, jp_sha)
    unused_renames = set(renames) - set(units)

    stats = collections.Counter()
    unit_rows, unmatched_rows = [], []
    for unit in sorted(units):
        langs = units[unit]
        ref, how = sources.find(unit)
        row = [unit, ref or "", how, "", "", ""]
        stats[f"units: {how}"] += 1
        if ref is not None and jp.get(ref) is None:
            row[2] = how = "missing in JP"
            stats["units: rename target missing"] += 1
            ref = None
        if ref is not None and "en" in langs:
            tables = {lg: parse_any(pal.get(r)) for lg, r in langs.items()}
            if tables.get("en") is not None:
                result = matcher.match(tables, ref)
                if result is None:
                    row[5] = "JP side is not a table"
                    stats["tables: JP side is not a table"] += 1
                else:
                    m, u, missing, kind = result
                    row[3], row[4], row[5] = str(m), str(u), kind
                    stats["records matched"] += m
                    stats["records unmatched"] += u
                    stats["tables"] += 1
                    for key, text in missing:
                        unmatched_rows.append([unit, repr(key), text[:120]])
            else:
                row[5] = "not a text table"
                stats["units: not a text table"] += 1
        unit_rows.append(row)

    piece_rows = []
    for ref in pieces:
        if "/.svn" in ref or ".svn-base" in ref or ref.endswith("/entries"):
            continue  # Subversion leftovers on the cartridge
        x = pal.get(ref)
        cp = piece_counterpart(ref, sources)
        cost = literal_cost(x, jp.get(cp)) if cp else len(x)
        piece_rows.append([str(cost), str(len(x)), ref, cp or ""])
        stats["pieces"] += 1
        stats["pieces: bytes"] += len(x)
        stats["pieces: literal bytes"] += cost
    piece_rows.sort(key=lambda r: -int(r[0]))

    os.makedirs(args.out, exist_ok=True)

    def tsv(name, header, rows):
        with open(os.path.join(args.out, name), "w", encoding="utf8", newline="\n") as f:
            f.write("\t".join(header) + "\n")
            for r in rows:
                f.write("\t".join(r) + "\n")

    tsv("units.tsv", ["unit", "jp", "found", "matched", "unmatched", "notes"], unit_rows)
    tsv("unmatched.tsv", ["unit", "key", "english"], unmatched_rows)
    tsv("pieces.tsv", ["literal", "size", "pal", "jp"], piece_rows)

    m, u = stats["records matched"], stats["records unmatched"]
    by_ext = collections.Counter()
    for cost, _, ref, _ in piece_rows:
        by_ext[os.path.splitext(ref)[1] or "(none)"] += int(cost)
    lines = [
        "# PAL -> JP map",
        "",
        f"PAL `{args.pal}`, JP `{args.jp}`.",
        "",
        "## Text units",
        "",
        f"- {len(units)} per-language units (PAL files or archive members named `_<lang>`).",
    ]
    lines += [f"- {k[len('units: '):]}: {v}" for k, v in sorted(stats.items()) if k.startswith("units: ")]
    lines += [
        f"- {stats['tables']} text tables compared: {m} records matched, {u} unmatched"
        f" ({100.0 * m / max(m + u, 1):.2f}% covered); the unmatched are in unmatched.tsv.",
    ]
    if unused_renames:
        lines += ["", "Renames in ja-files.toml with no PAL unit:", ""]
        lines += [f"- `{r}`" for r in sorted(unused_renames)]
    lines += [
        "",
        "## Other pieces",
        "",
        f"- {stats['pieces']} PAL files or archive members that are not per language and"
        f" differ from JP: {stats['pieces: bytes']} bytes, of which"
        f" {stats['pieces: literal bytes']} not found in their JP counterpart"
        " (what a recipe would carry).",
        "- Literal bytes by extension: " + ", ".join(f"{e} {c}" for e, c in by_ext.most_common(12)),
    ]
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1:])
