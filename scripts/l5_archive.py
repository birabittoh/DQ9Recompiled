"""Level-5's compression and GPC2 archives, as DQ9 uses them.

A compressed block starts with a u32, size << 3 | method; method 0 is raw,
1 LZ10, 2 Huffman over nibbles, 3 Huffman over bytes, 4 RLE (the GBA BIOS
formats, without their own headers).

A GPC2 archive (.gp2):

    0x00  "GPC2"
    0x04  u16  entry count (low 12 bits) and flags
    0x06  u16 x 5            not needed to read it
    0x10  u32  bit 28: the entries are stored raw (no block header,
               compressed size = their size); the rest not needed
    0x14  block  the entry table, 12 bytes per entry:
                 u32 name hash
                 u32 name offset low byte << 24 | data offset / 4
                 u32 name offset high byte << 24 | compressed size
          block  (4-aligned) the NUL-separated names
          data   (4-aligned) each entry, a block of its own, at
                 base + 4 * data offset

NARC archives (the JP release keeps most of its text in them) are read with
ndspy.narc.
"""
import struct


def _lz10(data, pos, size):
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b0, b1 = data[pos], data[pos + 1]
                pos += 2
                dist = ((b0 & 0x0F) << 8 | b1) + 1
                if dist > len(out):
                    raise ValueError("LZ10: back-reference before the start")
                for _ in range((b0 >> 4) + 3):
                    out.append(out[-dist])
            else:
                out.append(data[pos])
                pos += 1
    return bytes(out[:size]), pos


def _huffman(data, pos, size, bits):
    tree = pos
    pos = tree + (data[tree] + 1) * 2
    root = tree + 1
    out = bytearray()
    low = None
    node = root
    while len(out) < size:
        word = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        for i in range(32):
            value = data[node]
            child = (node & ~1) + (value & 0x3F) * 2 + 2
            if (word >> (31 - i)) & 1:
                leaf, node = value & 0x40, child + 1
            else:
                leaf, node = value & 0x80, child
            if not leaf:
                continue
            symbol = data[node]
            node = root
            if bits == 8:
                out.append(symbol)
            elif low is None:
                low = symbol & 0x0F
            else:
                out.append(low | (symbol & 0x0F) << 4)
                low = None
            if len(out) >= size:
                break
    return bytes(out[:size]), pos


def _rle(data, pos, size):
    out = bytearray()
    while len(out) < size:
        flag = data[pos]
        pos += 1
        if flag & 0x80:
            out += bytes([data[pos]]) * ((flag & 0x7F) + 3)
            pos += 1
        else:
            out += data[pos:pos + flag + 1]
            pos += flag + 1
    return bytes(out[:size]), pos


def decompress(data, pos=0):
    """The block at `pos`: (its bytes, the position after it)."""
    head = struct.unpack_from("<I", data, pos)[0]
    method, size = head & 7, head >> 3
    pos += 4
    if method == 0:
        return bytes(data[pos:pos + size]), pos + size
    if method == 1:
        return _lz10(data, pos, size)
    if method in (2, 3):
        return _huffman(data, pos, size, 4 if method == 2 else 8)
    if method == 4:
        return _rle(data, pos, size)
    raise ValueError(f"unknown compression method {method}")


def is_gpc2(data):
    return data[:4] == b"GPC2"


def read_gpc2(data):
    """The entries of a GPC2 archive, decompressed: [(name, bytes)] in the
    order of the entry table (sorted by name hash)."""
    if not is_gpc2(data):
        raise ValueError("not a GPC2 archive")
    table, pos = decompress(data, 0x14)
    names, pos = decompress(data, (pos + 3) & ~3)
    base = (pos + 3) & ~3
    raw = struct.unpack_from("<I", data, 0x10)[0] & 0x10000000
    out = []
    for i in range(len(table) // 12):
        _, w2, w3 = struct.unpack_from("<III", table, 12 * i)
        name_off = (w3 >> 24) << 8 | (w2 >> 24)
        name = names[name_off:names.index(b"\0", name_off)].decode("ascii")
        at = base + 4 * (w2 & 0xFFFFFF)
        if raw:
            out.append((name, bytes(data[at:at + (w3 & 0xFFFFFF)])))
        else:
            out.append((name, decompress(data, at)[0]))
    return out


def is_narc(data):
    return data[:4] == b"NARC"


def read_narc(data):
    """The files of a NARC: [(name, bytes)], unnamed ones as their index."""
    import ndspy.narc
    narc = ndspy.narc.NARC(bytes(data))
    out = []

    def walk(folder, prefix):
        for i, name in enumerate(folder.files):
            out.append((prefix + name, narc.files[folder.firstID + i]))
        for name, sub in folder.folders:
            walk(sub, prefix + name + "/")

    if narc.filenames and (narc.filenames.files or narc.filenames.folders):
        walk(narc.filenames, "")
    else:
        out = [(f"{i:04d}", d) for i, d in enumerate(narc.files)]
    return out


def is_named_table_pac(data):
    """The JP menu layouts (bin/menu/bgmake_*.pac): the name of the string
    table they carry, NUL-terminated in the first 0x20 bytes, then (at 0x40) u32
    offsets: the table's start, ..., the table's end at +8."""
    if len(data) < 0x50:
        return False
    name = data[:0x20].split(b"\0")[0]
    if not name.endswith(b".bin") or not all(0x20 < c < 0x7F for c in name):
        return False
    start, _, end = struct.unpack_from("<III", data, 0x40)
    return 0x50 <= start < end <= len(data)


def read_named_table_pac(data):
    name = data[:0x20].split(b"\0")[0].decode("ascii")
    start, _, end = struct.unpack_from("<III", data, 0x40)
    return [(name, bytes(data[start:end]))]


def read_archive(data):
    """GPC2, NARC or named-table entries, or None for anything else."""
    if is_gpc2(data):
        return read_gpc2(data)
    if is_narc(data):
        return read_narc(data)
    if is_named_table_pac(data):
        return read_named_table_pac(data)
    return None


def read_gpc2_stored(data):
    """The entries of a GPC2 as stored ({name: bytes}): each a compressed
    block with its header, or the raw bytes in a raw-entry archive."""
    if not is_gpc2(data):
        raise ValueError("not a GPC2 archive")
    table, pos = decompress(data, 0x14)
    names, pos = decompress(data, (pos + 3) & ~3)
    base = (pos + 3) & ~3
    out = {}
    for i in range(len(table) // 12):
        _, w2, w3 = struct.unpack_from("<III", table, 12 * i)
        name_off = (w3 >> 24) << 8 | (w2 >> 24)
        name = names[name_off:names.index(b"\0", name_off)].decode("ascii")
        at = base + 4 * (w2 & 0xFFFFFF)
        out[name] = bytes(data[at:at + (w3 & 0xFFFFFF)])
    return out
