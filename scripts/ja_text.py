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

The values a line shows come from YDQJ's code in its own placeholders
(<str_N>, [数値N], %s, %d, [ACTER], [TARGET], [VAL]...) and from YDQP's
code in the English line's (<DEF_ART_ACTOR>, <SGL_M_NAME>, <str_N>,
<val_N>, %d...). `Binding` maps the first onto the second: a placeholder
the English line has too stays, a named one ([ACTER]) takes the English
line's tag for it, and the others take the English line's remaining
ones of their type (a string or a number) in order. Names lose their
articles and plurals (<DEF_ART_PLR_M_NAME> -> <SGL_M_NAME>): Japanese has
neither.

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


# YDQP's value placeholders: a name tag (role = what it names, its
# article and number dropped), <LEADER>/<HERO>, a treasure map name's
# part, <str_N>, <val_N>, or a printf conversion (each one its own value).
PAL_ARG = re.compile(
    rb"<(?:DEF_ART_|INDEF_ART_)?(?:SGL_|PLR_)?"
    rb"(ACTOR|TARGET|ACTION|REFLEX|[MI]_NAME(?:S)?\d?)>"
    rb"|<(LEADER|HERO)>"
    rb"|<(tmap_sec\d)>"
    rb"|<(str_\d+|val_\d+)>"
    rb"|(%[-+ 0#]*\d*[sdcuxX])")
PRINTF = rb"%[-+ 0#]*\d*[sdcuxX]"
NUMBER_TYPES = b"dcuxX"
# YDQJ's named placeholders (actmsg): their role (None: a number).
JP_NAMED = {
    b"ACTER": b"ACTOR", b"TARGET": b"TARGET", b"ACTION": b"ACTION",
    b"REFLEX": b"REFLEX", b"SEL": b"I_NAME", b"ITEM": b"I_NAME",
    b"WPN": b"I_NAME", b"VAL": None,
}
NAMED_ROLES = (b"ACTOR", b"TARGET", b"ACTION", b"REFLEX", b"I_NAME")


def _role_tag(role):
    """The YDQP tag that shows `role` (from PAL_ARG) bare."""
    if role.startswith((b"M_NAME", b"I_NAME")):
        return b"<SGL_" + role.replace(b"NAMES", b"NAME") + b">"
    return b"<" + role + b">"


def pal_roles(pal):
    """The English line's values, in order: (key, type, YDQP text).
    Types: "s" a string, "d" a number, "m" a treasure map name's part
    (YDQP orders them for English: they are listed by number)."""
    roles, seen, n_conv = [], set(), 0
    for m in PAL_ARG.finditer(pal or b""):
        if m.group(1):
            key, kind, out = m.group(1).replace(b"NAMES", b"NAME"), "s", _role_tag(m.group(1))
        elif m.group(2):
            key, kind, out = m.group(2), "s", m.group()
        elif m.group(3):
            key, kind, out = m.group(3), "m", m.group()
        elif m.group(4):
            key, out = m.group(4), m.group()
            kind = "d" if key.startswith(b"val_") else "s"
        else:
            n_conv += 1
            key, out = b"%" + str(n_conv).encode(), m.group(5)
            kind = "d" if m.group(5)[-1:] in NUMBER_TYPES else "s"
        if key not in seen:
            seen.add(key)
            roles.append((key, kind, out))
    maps = sorted(r for r in roles if r[1] == "m")
    return [maps.pop(0) if r[1] == "m" else r for r in roles]


class Binding:
    """Which YDQP placeholder each YDQJ one of a line becomes: `get`, and
    `pad` (printf conversions to spend before it: YDQP's line has more of
    them, the ones the Japanese line skips come first)."""

    def __init__(self, tokens, pal, has=()):
        self.map, self.pad, self.unbound = {}, {}, []
        # The names the Japanese line writes itself (<LEADER>) are no
        # value of its to bind.
        free = [r for r in pal_roles(pal) if r[0] not in has]
        if not free:
            return
        printf_roles = [r for r in free if r[0].startswith(b"%")]

        def take(pred):
            r = next((r for r in free if pred(r)), None)
            if r is not None:
                free.remove(r)
            return r

        # A treasure map's name: the run of adjacent placeholders as long
        # as the English line's parts.
        maps = [r for r in free if r[1] == "m"]
        if maps:
            for k in range(len(tokens) - len(maps) + 1):
                run = tokens[k:k + len(maps)]
                if all(t[1] == "s" and t[4] for t in run[1:]) and \
                        all(t[1] == "s" for t in run):
                    for t, r in zip(run, maps):
                        self.map[t[0]] = r[2]
                        free.remove(r)
                    break
        rest = []
        for t in tokens:
            tok, kind, key = t[0], t[1], t[2]
            if tok in self.map or any(x[0] == tok for x in rest):
                continue
            hit = None
            if key is not None:
                hit = take(lambda r: r[0] == key)
                if hit is None and key == b"ACTION":
                    hit = take(lambda r: r[0].startswith(b"I_NAME"))
                if hit is None and key in NAMED_ROLES:
                    self.map[tok] = _role_tag(key)
                    continue
            if hit is not None:
                self.map[tok] = hit[2]
            else:
                rest.append(t)
        # printf ones inside tags (<PLTT=[数値3]>): when the English line
        # has more conversions, the Japanese ones are its last.
        in_tags = [t for t in rest if t[3]]
        skip = max(0, len([r for r in free if r[0].startswith(b"%")]) - len(
            [t for t in rest if t[3] or t[0].startswith(b"%")]))
        if in_tags and skip:
            self.pad[in_tags[0][0]] = skip
            for _ in range(skip):
                take(lambda r: r[0].startswith(b"%"))
        used = {}
        for t in rest:
            tok, kind, in_tag = t[0], t[1], t[3]
            hit = take(lambda r: r[1] == kind and (not in_tag or r[0].startswith(b"%")))
            if hit is None and tok.startswith(b"%"):
                # YDQJ's printf takes a value again for each use: reuse
                # the line's values of the type, in turn.
                same = [r for r in pal_roles(pal) if r[1] == kind and r[0] not in has]
                if same:
                    hit = same[used.get(kind, 0) % len(same)]
                    used[kind] = used.get(kind, 0) + 1
            if hit is None:
                self.unbound.append(tok)
            else:
                self.map[tok] = hit[2]

    def get(self, tok):
        return self.map.get(tok)


def _jp_placeholder(text):
    """(type, role key) of a YDQJ placeholder's text, or None."""
    m = re.fullmatch(r"\[(数値|文字列)(\d+)\]", text)
    if m:
        return ("d" if m.group(1) == "数値" else "s",
                (b"val_" if m.group(1) == "数値" else b"str_") + m.group(2).encode())
    m = re.fullmatch(r"<(str|val)_(\d+)>", text)
    if m:
        return ("d" if m.group(1) == "val" else "s", text[1:-1].encode())
    m = re.fullmatch(r"\[([A-Z]+)\]", text)
    if m and m.group(1).encode() in JP_NAMED:
        role = JP_NAMED[m.group(1).encode()]
        return ("d", None) if role is None else ("s", role)
    m = re.fullmatch(r"%[-+ 0#]*\d*([sdcuxX])", text)
    if m:
        return ("d" if m.group(1) in "dcuxX" else "s", None)
    return None


JP_TOKEN = re.compile(r"\[[^\[\]/]{1,16}\]|<(?:str|val)_\d+>|%[-+ 0#]*\d*[sdcuxX]")


def jp_tokens(jp):
    """YDQJ's value placeholders in `jp`, in order: (text, type, role key,
    inside a tag, right after the previous one). A printf conversion is
    its own value each time (text + "#n")."""
    text = b"".join(chars(jp)).decode("cp932", "replace")
    out = []
    depth = n_conv = 0
    last_end = -1
    i = 0
    while i < len(text):
        m = JP_TOKEN.match(text, i)
        if m and not (text[i] == "<" and depth):
            t = _jp_placeholder(m.group())
            if t is not None:
                tok = m.group().encode("cp932")
                if tok.startswith(b"%"):
                    n_conv += 1
                    tok = tok + b"#" + str(n_conv).encode()
                out.append((tok, t[0], t[1], depth > 0, m.start() == last_end))
                last_end = m.end()
            i = m.end()
            continue
        if text[i] == "<":
            depth += 1
        elif text[i] == ">" and depth:
            depth -= 1
        i += 1
    return out


def names_written(jp):
    """The YDQP names `jp` writes itself (its ○ＰＣ○○ ones and tags)."""
    text = jp.decode("cp932", "replace")
    out = set()
    for k, tag in NAMES.items():
        if k in text or tag.decode() in text:
            out.add(tag[1:-1])
    return out


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
    binding = Binding(jp_tokens(jp), pal, names_written(jp)) if pal else None
    n_conv = [0]

    def bound(tok):
        return binding.get(tok) if binding else None

    def conversion(m):
        """A printf conversion of YDQJ's: its binding, or itself."""
        n_conv[0] += 1
        return bound(m.group() + b"#" + str(n_conv[0]).encode())

    def in_tag(name):
        """A tag's name with the placeholders in it bound (printf ones
        only: a tag cannot hold one), and how many times to write it
        before (Binding.pad)."""
        pad = [0]

        def sub(m):
            tok = m.group()
            if tok.startswith(b"%"):
                b = conversion(m)
                tok = tok + b"#" + str(n_conv[0]).encode()
            else:
                b = bound(tok)
            if binding:
                pad[0] = max(pad[0], binding.pad.get(tok, 0))
            return b if b is not None and not b.startswith(b"<") else m.group()
        return re.sub(PRINTF + rb"|\[[^\[\]/<>]{1,16}\]", sub, name), pad[0]

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
            bound_name, pad = in_tag(name) if j < len(cs) else (name, 0)
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
            elif re.fullmatch(rb"(str|val)_\d+", name) and bound(b"<" + name + b">"):
                new(bound(b"<" + name + b">"))
            elif bound_name != name:
                new((b"<" + bound_name + b">") * (pad + 1))
            else:
                keep(i, end)
            i = end
            continue
        if c == b"%":
            m = re.compile(rb"%[-+ 0#]*\d*[sdcuxX]").match(b"".join(cs[i:i + 12]))
            if m:
                n = len(m.group())
                b = conversion(m)
                if b is None or b == m.group():
                    keep(i, i + n)
                else:
                    new(b)
                i += n
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
                b = bound(b"[" + b"".join(inner) + b"]")
                if b is not None:
                    new(b)
                    i = j + 1
                    continue
                m = re.fullmatch(r"(数値|文字列)(\d+)", text)
                if m:
                    new(b"<" + PLACEHOLDERS[m.group(1)] + m.group(2).encode() + b">")
                    i = j + 1
                    continue
        keep(i, i + 1)
        i += 1
    return out
