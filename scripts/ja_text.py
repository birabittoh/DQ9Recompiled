"""Japanese (YDQJ) text in the form the Europe build reads.

YDQJ and YDQP share their text format: NUL-terminated strings with
<TAG> controls. YDQJ's strings are Shift-JIS, and they spell a few
things their own way, which `convert` rewrites as YDQP does:

    ○ＰＣ○○ ○ホ主○○ ...   name placeholders     -> <LEADER>, <HERO>, ...
    [数値N] [文字列N]       values and strings    -> <val_N>, <str_N>
    [漢字/かな]             furigana              -> 漢字 (dropped)
    <IF_OTOKO> ...          the hero is male      -> <IF_HERO_MALE> ...
    <N_EAST> ...            arrows                -> <ar-ur> ...

The male/female condition is <IF_OTOKO> in every YDQJ line, where YDQP
names whose sex it asks about (the hero, the speaker's addressee, ...):
`convert` takes the condition the English line of the same record uses,
when it has one.

Shift-JIS is read a character at a time: a trail byte can be '[', '\\'
or ']'.
"""
import re

NAMES = {
    "○ＰＣ○○": b"<LEADER>",
    "○先頭○○": b"<LEADER>",
    "○ホ主○○": b"<HERO>",
    "○自主○○": b"<ADDRESSEE>",
    "○ホスト○": b"<HOST>",
}
ARROWS = {
    b"N_EAST": b"ar-ur",
    b"N_WEST": b"ar-ul",
    b"S_EAST": b"ar-dr",
    b"S_WEST": b"ar-dl",
}
PLACEHOLDERS = {"数値": b"val_", "文字列": b"str_"}

# YDQP's male/female conditions: IF_<X>, ELSE_<NOT X>, ENDIF_<X>.
CONDITIONS = {
    b"HERO_MALE": (b"IF_HERO_MALE", b"ELSE_HERO_NOT_MALE", b"ENDIF_HERO_MALE"),
    b"MALE": (b"IF_MALE", b"ELSE_NOT_MALE", b"ENDIF_MALE"),
    b"ADDRESSEE_MALE": (b"IF_ADDRESSEE_MALE", b"ELSE_ADDRESSEE_NOT_MALE",
                        b"ENDIF_ADDRESSEE_MALE"),
}
DEFAULT_CONDITION = b"HERO_MALE"
OTOKO = {b"IF_OTOKO": 0, b"ELSE_OTOKO": 1, b"END_OTOKO": 2,
         b"IF_OTOKO_HOST": 0, b"ELSE_OTOKO_HOST": 1, b"END_OTOKO_HOST": 2}
PAL_CONDITION = re.compile(rb"<IF_(HERO_MALE|MALE|ADDRESSEE_MALE)>")


def is_lead(c):
    return 0x81 <= c <= 0x9F or 0xE0 <= c <= 0xFC


def chars(data):
    """The characters of a Shift-JIS byte string, as byte strings."""
    i = 0
    while i < len(data):
        n = 2 if is_lead(data[i]) and i + 1 < len(data) else 1
        yield data[i:i + n]
        i += n


def conditions_of(pal):
    """The male/female conditions of an English line, in order."""
    return [m.group(1) for m in PAL_CONDITION.finditer(pal or b"")]


def convert(jp, pal=None):
    """`jp` (a YDQJ string, no NUL) as YDQP writes it. `pal`, the English
    line of the same record if there is one, picks the conditions."""
    return b"".join(jp[a:b] if kind == "keep" else a
                    for kind, a, b in convert_spans(jp, pal))


def convert_spans(jp, pal=None):
    """`convert` as pieces: ("keep", start, end) for bytes of `jp` kept as
    they are, ("new", bytes, None) for what replaces the rest."""
    cs = list(chars(jp))
    pos = [0]
    for c in cs:
        pos.append(pos[-1] + len(c))
    wanted = conditions_of(pal)
    out = []

    def keep(i0, i1):
        if i1 <= i0:
            return
        if out and out[-1][0] == "keep" and out[-1][2] == pos[i0]:
            out[-1] = ("keep", out[-1][1], pos[i1])
        else:
            out.append(("keep", pos[i0], pos[i1]))

    def new(data):
        if out and out[-1][0] == "new":
            out[-1] = ("new", out[-1][1] + data, None)
        else:
            out.append(("new", data, None))

    open_conditions = []  # the YDQP condition of each open <IF_OTOKO>
    used = 0
    i = 0
    while i < len(cs):
        c = cs[i]
        if c == b"<":
            j = i + 1
            while j < len(cs) and cs[j] != b">":
                j += 1
            name = b"".join(cs[i + 1:j])
            end = min(j + 1, len(cs))
            part = OTOKO.get(name)
            if part is not None:
                if part == 0:
                    cond = wanted[used] if used < len(wanted) else DEFAULT_CONDITION
                    used += 1
                    open_conditions.append(cond)
                cond = open_conditions[-1] if open_conditions else DEFAULT_CONDITION
                if part == 2 and open_conditions:
                    open_conditions.pop()
                new(b"<" + CONDITIONS[cond][part] + b">")
            elif name in ARROWS:
                new(b"<" + ARROWS[name] + (b">" if j < len(cs) else b""))
            else:
                keep(i, end)
            i = end
            continue
        if c == "○".encode("cp932"):
            for text, tag in NAMES.items():
                n = len(text)
                if "".join(x.decode("cp932", "replace") for x in cs[i:i + n]) == text:
                    new(tag)
                    i += n
                    break
            else:
                keep(i, i + 1)
                i += 1
            continue
        if c == b"[":
            j = i + 1
            while j < len(cs) and cs[j] not in (b"]", b"[") and j - i < 64:
                j += 1
            if j < len(cs) and cs[j] == b"]":
                inner = cs[i + 1:j]
                if b"/" in inner:  # furigana: keep the kanji
                    keep(i + 1, i + 1 + inner.index(b"/"))
                    i = j + 1
                    continue
                text = b"".join(inner).decode("cp932", "replace")
                m = re.fullmatch(r"(数値|文字列)(\d+)", text)
                if m:
                    new(b"<" + PLACEHOLDERS[m.group(1)] + m.group(2).encode() + b">")
                    i = j + 1
                    continue
        keep(i, i + 1)
        i += 1
    return out
