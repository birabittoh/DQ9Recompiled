// l5_archive.h — Level-5's compression and the archives DQ9 keeps its files
// in (GPC2, NARC, the named-table pac), as scripts/l5_archive.py reads them.
// Used by the Japanese release's converter (ja_convert.cpp) and font
// (ja_font.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace l5 {

using Bytes = std::vector<uint8_t>;

inline uint16_t U16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | d[1] << 8); }
inline uint32_t U32(const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 |
           static_cast<uint32_t>(d[2]) << 16 | static_cast<uint32_t>(d[3]) << 24;
}

// ---- Level-5 compression -------------------------------------------------
// A block: u32 size << 3 | method; 0 raw, 1 LZ10, 2 Huffman over nibbles,
// 3 Huffman over bytes, 4 RLE (the GBA BIOS formats without their own
// headers). scripts/l5_archive.py is the reference.

inline bool Lz10(const Bytes& d, size_t pos, size_t size, Bytes* out) {
    out->clear();
    out->reserve(size);
    while (out->size() < size) {
        if (pos >= d.size()) return false;
        const uint8_t flags = d[pos++];
        for (int bit = 0; bit < 8 && out->size() < size; ++bit) {
            if (flags & (0x80 >> bit)) {
                if (pos + 2 > d.size()) return false;
                const uint8_t b0 = d[pos], b1 = d[pos + 1];
                pos += 2;
                const size_t dist = ((b0 & 0x0Fu) << 8 | b1) + 1;
                if (dist > out->size()) return false;
                for (int i = 0; i < (b0 >> 4) + 3; ++i)
                    out->push_back((*out)[out->size() - dist]);
            } else {
                if (pos >= d.size()) return false;
                out->push_back(d[pos++]);
            }
        }
    }
    out->resize(size);
    return true;
}

inline bool Huffman(const Bytes& d, size_t pos, size_t size, int bits, Bytes* out) {
    out->clear();
    out->reserve(size);
    if (pos >= d.size()) return false;
    const size_t tree = pos;
    pos = tree + (static_cast<size_t>(d[tree]) + 1) * 2;
    const size_t root = tree + 1;
    size_t node = root;
    int low = -1;
    while (out->size() < size) {
        if (pos + 4 > d.size()) return false;
        const uint32_t word = U32(&d[pos]);
        pos += 4;
        for (int i = 0; i < 32 && out->size() < size; ++i) {
            if (node >= d.size()) return false;
            const uint8_t value = d[node];
            const size_t child = (node & ~size_t{1}) + (value & 0x3Fu) * 2 + 2;
            bool leaf;
            if ((word >> (31 - i)) & 1) {
                leaf = value & 0x40;
                node = child + 1;
            } else {
                leaf = value & 0x80;
                node = child;
            }
            if (!leaf) continue;
            if (node >= d.size()) return false;
            const uint8_t symbol = d[node];
            node = root;
            if (bits == 8) {
                out->push_back(symbol);
            } else if (low < 0) {
                low = symbol & 0x0F;
            } else {
                out->push_back(static_cast<uint8_t>(low | (symbol & 0x0F) << 4));
                low = -1;
            }
        }
    }
    out->resize(size);
    return true;
}

inline bool Rle(const Bytes& d, size_t pos, size_t size, Bytes* out) {
    out->clear();
    out->reserve(size);
    while (out->size() < size) {
        if (pos >= d.size()) return false;
        const uint8_t flag = d[pos++];
        if (flag & 0x80) {
            if (pos >= d.size()) return false;
            out->insert(out->end(), (flag & 0x7Fu) + 3, d[pos++]);
        } else {
            const size_t n = flag + 1u;
            if (pos + n > d.size()) return false;
            out->insert(out->end(), d.begin() + pos, d.begin() + pos + n);
            pos += n;
        }
    }
    out->resize(size);
    return true;
}

// The block at `pos`; *end is the position after it (raw blocks only:
// the compressed ones are not walked past by callers that need it).
inline bool Decompress(const Bytes& d, size_t pos, Bytes* out, size_t* end) {
    if (pos + 4 > d.size()) return false;
    const uint32_t head = U32(&d[pos]);
    const size_t size = head >> 3;
    pos += 4;
    switch (head & 7) {
    case 0:
        if (pos + size > d.size()) return false;
        out->assign(d.begin() + pos, d.begin() + pos + size);
        if (end) *end = pos + size;
        return true;
    case 1: return Lz10(d, pos, size, out);
    case 2: return Huffman(d, pos, size, 4, out);
    case 3: return Huffman(d, pos, size, 8, out);
    case 4: return Rle(d, pos, size, out);
    default: return false;
    }
}

// ---- Archives --------------------------------------------------------------

struct Member {
    size_t offset = 0;
    size_t size = 0;     // the bytes stored at `offset`
    bool block = false;  // they are a compressed block
};

struct Archive {
    Bytes data;
    std::unordered_map<std::string, Member> members;
};

// GPC2: see scripts/l5_archive.py. The table and the names are blocks; the
// positions after them come from the header (u16 @8 names / 4, @10 data / 4).
inline bool ReadGpc2(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x18) return false;
    Bytes table, names;
    if (!Decompress(d, 0x14, &table, nullptr)) return false;
    const size_t names_at = size_t{U16(&d[8])} * 4;
    const size_t base = size_t{U16(&d[10])} * 4;
    if (!Decompress(d, names_at, &names, nullptr)) return false;
    const bool raw = U32(&d[0x10]) & 0x10000000u;
    for (size_t i = 0; i + 12 <= table.size(); i += 12) {
        const uint32_t w2 = U32(&table[i + 4]), w3 = U32(&table[i + 8]);
        const size_t name_off = (w3 >> 24) << 8 | (w2 >> 24);
        if (name_off >= names.size()) return false;
        const char* name = reinterpret_cast<const char*>(&names[name_off]);
        const size_t len = strnlen(name, names.size() - name_off);
        Member m;
        m.offset = base + 4 * size_t{w2 & 0xFFFFFFu};
        m.size = w3 & 0xFFFFFFu;
        m.block = !raw;
        if (m.offset + m.size > d.size()) return false;
        a->members.emplace(std::string(name, len), m);
    }
    return true;
}

// The Japanese release's menu layouts (bin/menu/bgmake_*.pac): one string
// table, named NUL-terminated in the first 0x20 bytes; u32 @0x40 is where
// it starts and u32 @0x48 where it ends (scripts/l5_archive.py).
inline bool ReadNamedTablePac(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x50) return false;
    size_t len = 0;
    while (len < 0x20 && d[len]) {
        if (d[len] <= 0x20 || d[len] >= 0x7F) return false;
        ++len;
    }
    const std::string name(reinterpret_cast<const char*>(d.data()), len);
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".bin")) return false;
    const size_t start = U32(&d[0x40]), end = U32(&d[0x48]);
    if (start < 0x50 || end <= start || end > d.size()) return false;
    Member m;
    m.offset = start;
    m.size = end - start;
    a->members.emplace(name, m);
    return true;
}

// NARC: names as ndspy gives them ("dir/name"), or the file index as
// "%04d" when the archive has no names.
inline bool ReadNarc(Archive* a) {
    const Bytes& d = a->data;
    if (d.size() < 0x10) return false;
    size_t pos = U16(&d[0x0C]);
    size_t fat = 0, fnt = 0, fnt_size = 0, img = 0;
    for (int chunk = 0; chunk < 3; ++chunk) {
        if (pos + 8 > d.size()) return false;
        const uint32_t size = U32(&d[pos + 4]);
        if (!std::memcmp(&d[pos], "BTAF", 4)) fat = pos + 8;
        else if (!std::memcmp(&d[pos], "BTNF", 4)) { fnt = pos + 8; fnt_size = size - 8; }
        else if (!std::memcmp(&d[pos], "GMIF", 4)) img = pos + 8;
        else return false;
        pos += size;
    }
    if (!fat || !fnt || !img || fat + 4 > d.size()) return false;
    const size_t count = U16(&d[fat]);
    if (fat + 4 + count * 8 > d.size()) return false;
    auto file = [&](size_t id, Member* m) {
        if (id >= count) return false;
        const size_t start = U32(&d[fat + 4 + id * 8]), end = U32(&d[fat + 8 + id * 8]);
        if (end < start || img + end > d.size()) return false;
        m->offset = img + start;
        m->size = end - start;
        return true;
    };
    bool named = false;
    // Walk the name table from the root directory (0xF000).
    struct Dir { size_t index; std::string prefix; };
    std::vector<Dir> todo = {{0, ""}};
    while (!todo.empty()) {
        const Dir dir = todo.back();
        todo.pop_back();
        const size_t entry = fnt + dir.index * 8;
        if (entry + 8 > fnt + fnt_size || entry + 8 > d.size()) return false;
        size_t p = fnt + U32(&d[entry]);
        size_t id = U16(&d[entry + 4]);
        while (p < d.size()) {
            const uint8_t len = d[p++];
            if (!len) break;
            const size_t n = len & 0x7F;
            if (p + n > d.size()) return false;
            std::string name(reinterpret_cast<const char*>(&d[p]), n);
            p += n;
            named = true;
            if (len & 0x80) {
                if (p + 2 > d.size()) return false;
                todo.push_back({U16(&d[p]) & 0x0FFFu, dir.prefix + name + "/"});
                p += 2;
            } else {
                Member m;
                if (!file(id++, &m)) return false;
                a->members.emplace(dir.prefix + name, m);
            }
        }
    }
    if (!named) {
        for (size_t id = 0; id < count; ++id) {
            Member m;
            if (!file(id, &m)) return false;
            char name[16];
            std::snprintf(name, sizeof name, "%04zu", id);
            a->members.emplace(name, m);
        }
    }
    return true;
}

}  // namespace l5
