"""DQ9's text tables: the record format of the scenario, event and most
menu string files, the same in every release.

    0x00  u32 record count
    0x04  u32 offset of the string pool
    0x08  u32 size of the string pool
    0x0C  u32 (varies)
    0x10  records: u16 kind, u8 argument count, u8 flags, then the
          arguments (u32 each); some are offsets into the pool
    pool  NUL-terminated strings (PAL: tagged ASCII; JP: Shift-JIS with
          the same control tags)

Which arguments point into the pool depends on the record kind; callers
find them by comparing two languages of one table (the arguments that
differ are string offsets: `string_args`).
"""
import struct


class Table:
    def __init__(self, data):
        self.data = bytes(data)
        count, self.pool, self.pool_size, self.extra = struct.unpack_from("<IIII", data, 0)
        if self.pool > len(data) or self.pool < 16:
            raise ValueError("pool out of range")
        self.records = []  # (kind, flags, [args])
        p = 16
        for _ in range(count):
            if p + 4 > self.pool:
                raise ValueError("records run into the pool")
            kind, argc, flags = struct.unpack_from("<HBB", data, p)
            if p + 4 + 4 * argc > self.pool:
                raise ValueError("record runs into the pool")
            self.records.append((kind, flags, list(struct.unpack_from("<%dI" % argc, data, p + 4))))
            p += 4 + 4 * argc
        self.records_end = p

    def string(self, offset):
        start = self.pool + offset
        if offset == 0xFFFFFFFF or start >= len(self.data):
            return None
        end = self.data.find(b"\0", start)
        return self.data[start:end if end >= 0 else len(self.data)]


def parse(data):
    """A Table, or None when `data` is not one."""
    if len(data) < 16:
        return None
    try:
        return Table(data)
    except (ValueError, struct.error):
        return None


class Nat:
    """A .nat table (menu strings, item names, action messages...):

        0x00  u32  record count (low 12 bits) | string pool size << 12
        0x04  records, all the same size (8 bytes in the menu string
              tables: u32 id, u32 string offset; 16 in itemname)
        pool  NUL-terminated strings, offsets relative to the pool

    The game reads it with 0x020E53CC / 0x020E52B0 (itemname: binary
    search on the u16 id at record +0x0C). The record layout depends on
    the table; `records` are the raw bytes of each, as u32 words.
    """

    def __init__(self, data):
        self.data = bytes(data)
        head = struct.unpack_from("<I", data, 0)[0]
        count, self.pool_size = head & 0xFFF, head >> 12
        body = len(data) - 4 - self.pool_size
        if count == 0 or body <= 0 or body % count or (body // count) % 4:
            raise ValueError("not a .nat table")
        self.record_size = body // count
        self.pool = 4 + body
        words = self.record_size // 4
        self.records = [list(struct.unpack_from("<%dI" % words, data, 4 + i * self.record_size))
                        for i in range(count)]

    def string(self, offset):
        start = self.pool + offset
        if start >= len(self.data):
            return None
        end = self.data.find(b"\0", start)
        return self.data[start:end if end >= 0 else len(self.data)]


def parse_nat(data):
    if len(data) < 8:
        return None
    try:
        return Nat(data)
    except (ValueError, struct.error):
        return None


def string_args(a, b):
    """For two languages of one table: per record, the indices of the
    arguments that are string offsets (they differ between the two, or
    are offset 0 and point at a string in both), or None if the two do
    not have the same records."""
    if len(a.records) != len(b.records):
        return None
    out = []
    for (ka, fa, xa), (kb, fb, xb) in zip(a.records, b.records):
        if ka != kb or len(xa) != len(xb):
            return None
        out.append([i for i, (u, v) in enumerate(zip(xa, xb)) if u != v])
    return out
