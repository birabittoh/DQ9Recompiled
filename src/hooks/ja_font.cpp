// Japanese characters for the Japanese release. The Europe build's two fonts
// (data/pack_lv5/fi_<f>.bin + fd_<f>.bin, f = s7 and me) are Latin only: an
// index of byte sequences ("A", "<'e>") with a width and a column each, and
// one 1bpp strip 12 rows tall holding every glyph side by side. They are
// loaded into a boot heap of 37 KB (0x02114EC4, size 0x9478) that has no
// room for more.
//
// A folder converted from the Japanese ROM carries the Japanese release's
// fonts at the end of data/pack/font.gp2 (scripts/ja-convert.py, font_ops),
// which the Europe build never reads from. Once the game has loaded its
// fonts, this builds a copy of each with the Japanese glyphs added after the
// Latin ones, keyed by their 2-byte Shift JIS codes (the converted text is
// Shift JIS), maps the copies into the ARM9's slot-2 ROM range
// (nds_bus_map_title_rom) and points the game's font table at them. The
// game's own code then lays out and draws them like any other glyph:
//
//   * me (message windows, menus): the 12 px glyphs (f12.mes, font12.mes,
//     the per-scene kanji sets), columns 1-11 of their 12 (column 0 is the
//     spacing), so with the game's 1 px gap they advance 12 px, as the
//     Japanese release lays its lines out;
//   * s7 (the narrow font): the 10 px ones, columns 1-9, a row down, where
//     there is one; else the 12 px one.
//
// The game keeps a glyph's index in a byte only for the kerning search
// (0x020425F4 takes prev & 0xFF and cur & 0xFF), so an index whose low byte
// is a glyph of the kerning table would pick up a Latin pair's adjustment.
// Those indices get filler entries ("\xFF\xFF", which no text holds).
//
// The glyph searches (0x020424F4 index, 0x0204255C entry) are linear scans
// with memcmp; with some 1800 glyphs they are done natively here
// (Dq9GlyphIndex, Dq9GlyphEntry), with the same result for every font.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "io.h"
#include "title_hooks.h"

#include "l5_archive.h"

namespace {

using l5::Bytes;

constexpr uint32_t kIndexTable = 0x0210782Cu;   // fi_<f> per font: s7, me
constexpr uint32_t kBitmapTable = 0x02107834u;  // fd_<f> per font
constexpr int kFonts = 2;
constexpr uint32_t kRomBase = 0x09000000u;
constexpr int kRows = 12;
constexpr char kFontFile[] = "data/pack/font.gp2";
constexpr char kTrailer[8] = {'D', 'Q', '9', 'J', 'F', 'O', 'N', 'T'};

// A Japanese glyph: rows of `width` pixels, bit c of row r = pixel
// (first + c, r) of the source.
struct Glyph {
    int width = 0;
    int top = 0;  // the strip row of its first row
    std::vector<uint16_t> rows;
};

using GlyphSet = std::map<uint16_t, Glyph>;  // by Shift JIS code

// A .mes font: u8 bytes per glyph, u8 0, u8 width, height, ?, u8 0, u16
// count, u32 offset of the codes (big-endian u16), u32 offset of the
// bitmaps (row-major, LSB first, rows not padded). Adds the glyphs of
// `size` x `size` fonts not in `out` yet, columns 1..size-1.
void AddMes(const Bytes& d, int size, int top, GlyphSet* out) {
    if (d.size() < 16) return;
    const size_t bpg = d[0];
    if (d[2] != size || d[3] != size || bpg * 8 < size_t(size * size)) return;
    const size_t count = l5::U16(&d[6]);
    const size_t codes = l5::U32(&d[8]), bitmaps = l5::U32(&d[12]);
    if (codes + count * 2 > d.size() || bitmaps + count * bpg > d.size()) return;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t code = static_cast<uint16_t>(d[codes + 2 * i] << 8 | d[codes + 2 * i + 1]);
        if (!code || out->count(code)) continue;
        const uint8_t* b = &d[bitmaps + i * bpg];
        Glyph g;
        g.width = size - 1;
        g.top = top;
        for (int r = 0; r < size; ++r) {
            uint16_t row = 0;
            for (int c = 1; c < size; ++c) {
                const int bit = r * size + c;
                if (b[bit >> 3] >> (bit & 7) & 1) row |= uint16_t(1u << (c - 1));
            }
            g.rows.push_back(row);
        }
        (*out)[code] = std::move(g);
    }
}

// The members of a GPC2 or NARC, decoded, in name order.
std::vector<std::pair<std::string, Bytes>> Members(const Bytes& data) {
    l5::Archive a;
    a.data = data;
    std::vector<std::pair<std::string, Bytes>> out;
    const bool gpc2 = data.size() >= 4 && !std::memcmp(data.data(), "GPC2", 4);
    const bool narc = data.size() >= 4 && !std::memcmp(data.data(), "NARC", 4);
    if (!(gpc2 ? l5::ReadGpc2(&a) : narc && l5::ReadNarc(&a))) return out;
    for (const auto& [name, m] : a.members) {
        Bytes b;
        if (m.block) {
            if (!l5::Decompress(a.data, m.offset, &b, nullptr)) continue;
        } else {
            b.assign(a.data.begin() + m.offset, a.data.begin() + m.offset + m.size);
        }
        out.emplace_back(name, std::move(b));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    return out;
}

bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// The `size` px glyphs of the Japanese release: its base fonts first
// (`base` names, in order), then the rest of font_lv5.gp2 (loose .mes,
// then those in its .pac NARCs), then font.gp2's per-scene sets. Where two
// fonts draw a code differently (a few icons) the first wins.
void Collect(const std::vector<std::pair<std::string, Bytes>>& lv5,
             const std::vector<std::pair<std::string, Bytes>>& scenes,
             int size, int top, std::initializer_list<const char*> base,
             GlyphSet* out) {
    for (const char* name : base)
        for (const auto& [n, d] : lv5)
            if (n == name) AddMes(d, size, top, out);
    for (const auto& [n, d] : lv5)
        if (EndsWith(n, ".mes")) AddMes(d, size, top, out);
    for (const auto& [n, d] : lv5) {
        if (!EndsWith(n, ".pac")) continue;
        for (const auto& [mn, md] : Members(d))
            if (EndsWith(mn, ".mes")) AddMes(md, size, top, out);
    }
    for (const auto& [n, d] : scenes)
        if (EndsWith(n, ".mes")) AddMes(d, size, top, out);
}

// The fonts as built, and per font the glyph indices by first byte of their
// sequence (in index order), for the native searches.
struct Built {
    uint32_t index = 0;   // guest address of the fi copy
    uint32_t bitmap = 0;  // and of the fd copy
    uint32_t count = 0;
    uint32_t entries = 0;
    std::array<std::vector<uint16_t>, 256> by_first;
};

Bytes g_rom;
Built g_built[kFonts];

void Put32(Bytes* b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) (*b)[at + i] = uint8_t(v >> (8 * i));
}

void Put16(Bytes* b, size_t at, uint32_t v) {
    (*b)[at] = uint8_t(v);
    (*b)[at + 1] = uint8_t(v >> 8);
}

size_t Align(Bytes* b, size_t n) {
    b->resize((b->size() + n - 1) / n * n);
    return b->size();
}

// Appends to `rom` font `f` as the game loaded it plus `glyphs`, and
// records it in `built`. False (rom unchanged) if it does not fit.
bool BuildFont(int f, const GlyphSet& glyphs, Bytes* rom, Built* built) {
    const uint32_t fi = bus_read_u32(kIndexTable + 4u * f);
    const uint32_t fd = bus_read_u32(kBitmapTable + 4u * f);
    if (!fi || !fd) return false;
    const uint32_t count = bus_read_u32(fi + 4u);
    const uint32_t kerns = bus_read_u32(fi + 8u);
    const uint32_t kern_table = bus_read_u32(fi + 12u);
    const uint32_t entries = bus_read_u32(fi + 16u);
    const uint32_t width = bus_read_u16(fd + 4u);
    const uint32_t height = bus_read_u16(fd + 6u);
    const uint32_t bitmap = bus_read_u32(fd + 12u);
    if (height != kRows || (width & 7u) || count > 0x1000u || kerns > 0x1000u) return false;

    bool kerned[256] = {};
    for (uint32_t i = 0; i < kerns; ++i) {
        kerned[bus_read_u8(kern_table + 4u * i)] = true;
        kerned[bus_read_u8(kern_table + 4u * i + 1u)] = true;
    }

    // Glyph entries: the game's, then the Japanese ones (fillers between).
    struct Entry {
        Bytes seq;
        int8_t width;
        uint8_t len_flags;
        uint32_t column;
    };
    std::vector<Entry> list;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = entries + 8u * i;
        Entry x;
        const uint32_t seq = bus_read_u32(e);
        x.width = static_cast<int8_t>(bus_read_u8(e + 4u));
        x.len_flags = bus_read_u8(e + 5u);
        x.column = bus_read_u16(e + 6u);
        for (uint32_t k = 0; k < (x.len_flags & 0x3Fu); ++k) x.seq.push_back(bus_read_u8(seq + k));
        list.push_back(std::move(x));
    }
    uint32_t column = width;
    std::vector<std::pair<uint32_t, const Glyph*>> placed;
    for (const auto& [code, g] : glyphs) {
        while (kerned[list.size() & 0xFFu])
            list.push_back({{0xFF, 0xFF}, 0, 2, 0});
        list.push_back({{uint8_t(code >> 8), uint8_t(code)}, int8_t(g.width), 2, column});
        placed.emplace_back(column, &g);
        column += g.width;
    }
    const uint32_t new_width = (column + 7u) & ~7u;
    if (new_width > 0xFFFFu || list.size() > 0xFFFFu) return false;

    Bytes out = *rom;
    const auto guest = [&](size_t at) { return kRomBase + uint32_t(at); };
    // fi: header, kerning pairs, entries, sequences.
    const size_t at_fi = Align(&out, 4);
    out.resize(at_fi + 24);
    for (int i = 0; i < 4; ++i) out[at_fi + i] = bus_read_u8(fi + i);
    Put32(&out, at_fi + 4, uint32_t(list.size()));
    Put32(&out, at_fi + 8, kerns);
    const size_t at_kern = out.size();
    for (uint32_t i = 0; i < kerns * 4; ++i) out.push_back(bus_read_u8(kern_table + i));
    const size_t at_entries = Align(&out, 4);
    out.resize(at_entries + 8 * list.size());
    const size_t at_pool = out.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const size_t e = at_entries + 8 * i;
        Put32(&out, e, guest(out.size()));
        out[e + 4] = uint8_t(list[i].width);
        out[e + 5] = list[i].len_flags;
        Put16(&out, e + 6, list[i].column);
        out.insert(out.end(), list[i].seq.begin(), list[i].seq.end());
    }
    Put32(&out, at_fi + 12, guest(at_kern));
    Put32(&out, at_fi + 16, guest(at_entries));
    Put32(&out, at_fi + 20, guest(at_pool));

    // fd: header, the strip (the game's columns, then the new ones).
    const size_t at_fd = Align(&out, 4);
    const uint32_t stride = width / 8u, new_stride = new_width / 8u;
    out.resize(at_fd + 16 + size_t{new_stride} * kRows);
    for (int i = 0; i < 4; ++i) out[at_fd + i] = bus_read_u8(fd + i);
    Put16(&out, at_fd + 4, new_width);
    Put16(&out, at_fd + 6, kRows);
    Put32(&out, at_fd + 8, new_stride * kRows);
    Put32(&out, at_fd + 12, guest(at_fd + 16));
    uint8_t* strip = &out[at_fd + 16];
    for (uint32_t r = 0; r < kRows; ++r)
        for (uint32_t x = 0; x < stride; ++x)
            strip[r * new_stride + x] = bus_read_u8(bitmap + r * stride + x);
    for (const auto& [col, g] : placed) {
        for (size_t r = 0; r < g->rows.size(); ++r) {
            const size_t y = size_t(g->top) + r;
            if (y >= kRows) break;
            for (int c = 0; c < g->width; ++c) {
                if (!(g->rows[r] >> c & 1)) continue;
                const uint32_t x = col + uint32_t(c);
                strip[y * new_stride + x / 8] |= uint8_t(0x80u >> (x & 7u));
            }
        }
    }

    *rom = std::move(out);
    built->index = guest(at_fi);
    built->bitmap = guest(at_fd);
    built->count = uint32_t(list.size());
    built->entries = guest(at_entries);
    for (auto& v : built->by_first) v.clear();
    for (size_t i = 0; i < list.size(); ++i) {
        const Bytes& s = list[i].seq;
        if (s.empty()) {
            for (auto& v : built->by_first) v.push_back(uint16_t(i));
        } else {
            built->by_first[s[0]].push_back(uint16_t(i));
        }
    }
    return true;
}

const Built* BuiltFor(uint32_t index) {
    if (index < kRomBase) return nullptr;
    for (const Built& b : g_built)
        if (b.index && b.index == index) return &b;
    return nullptr;
}

// memcmp(seq, guest str, len) == 0, as 0x02001AEC.
bool SameGuest(uint32_t seq, uint32_t str, uint32_t len) {
    for (uint32_t k = 0; k < len; ++k)
        if (bus_read_u8(seq + k) != bus_read_u8(str + k)) return false;
    return true;
}

// The index of the first glyph of font `font` whose sequence starts the
// text at `str`, or -1, as 0x020424F4 finds it.
int32_t FindGlyph(uint32_t str, uint32_t font) {
    const uint32_t index = bus_read_u32(kIndexTable + 4u * font);
    const uint32_t entries = bus_read_u32(index + 16u);
    if (const Built* b = BuiltFor(index)) {
        const uint8_t first = bus_read_u8(str);
        for (const uint16_t i : b->by_first[first]) {
            const uint32_t e = b->entries + 8u * i;
            if (SameGuest(bus_read_u32(e), str, bus_read_u8(e + 5u) & 0x3Fu)) return i;
        }
        return -1;
    }
    const uint32_t count = bus_read_u32(index + 4u);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = entries + 8u * i;
        if (SameGuest(bus_read_u32(e), str, bus_read_u8(e + 5u) & 0x3Fu))
            return static_cast<int32_t>(i);
    }
    return -1;
}

}  // namespace

// After the game loads its fonts (0x02042954, before its last call): adds
// the Japanese glyphs when the cartridge carries them.
NDS_TITLE_HOOK void Dq9JaFont() {
    for (Built& b : g_built) b = Built{};
    nds_bus_map_title_rom(0, nullptr, 0);
    const uint8_t* file = nullptr;
    uint32_t size = 0;
    if (!nds_io_cartridge_file(kFontFile, &file, &size) || size < 16 ||
        std::memcmp(file + size - 16, kTrailer, 8) != 0)
        return;
    const uint32_t at = l5::U32(file + size - 8), base_size = l5::U32(file + size - 4);
    if (at > size - 16 || base_size > size - 16 - at) return;
    const Bytes scenes_file(file, file + at);
    const Bytes base_file(file + at, file + at + base_size);
    const auto scenes = Members(scenes_file);
    const auto base = Members(base_file);

    GlyphSet f12, f10;
    Collect(base, scenes, 12, 0, {"f12.mes", "font12.mes"}, &f12);
    Collect(base, scenes, 10, 1, {"f10.mes", "font10.mes"}, &f10);
    // The full-width space: no font draws it.
    Glyph space;
    space.width = 11;
    f12.emplace(uint16_t(0x8140), space);
    // The narrow font: the 10 px glyph where there is one.
    GlyphSet narrow = f12;
    for (const auto& [code, g] : f10) narrow[code] = g;
    narrow[0x8140].width = 9;

    Bytes rom;
    const GlyphSet* sets[kFonts] = {&narrow, &f12};
    Built built[kFonts];
    for (int f = 0; f < kFonts; ++f) {
        if (!BuildFont(f, *sets[f], &rom, &built[f])) {
            std::fprintf(stderr, "[ja-font] font %d: cannot add the glyphs\n", f);
            return;
        }
    }
    g_rom = std::move(rom);
    if (!nds_bus_map_title_rom(kRomBase, g_rom.data(), uint32_t(g_rom.size()))) return;
    for (int f = 0; f < kFonts; ++f) {
        g_built[f] = std::move(built[f]);
        bus_write_u32(kIndexTable + 4u * f, g_built[f].index);
        bus_write_u32(kBitmapTable + 4u * f, g_built[f].bitmap);
    }
    std::fprintf(stderr, "[ja-font] %zu + %zu glyphs, %zu bytes at 0x%08X\n",
                 f12.size(), f10.size(), g_rom.size(), kRomBase);
}

// At the entry of 0x020424F4(str = r0, font = r1): r0 = the glyph's index
// or -1 (none, or a null str). Returns true (the bank then does `bx lr`);
// r1-r3, r12 and the flags are scratch to its callers.
NDS_TITLE_HOOK bool Dq9GlyphIndex(uint32_t* r0, uint32_t* r1) {
    *r0 = *r0 ? static_cast<uint32_t>(FindGlyph(*r0, *r1)) : 0xFFFFFFFFu;
    return true;
}

// At the entry of 0x0204255C(str = r0, font = r1): r0 = the glyph's entry
// (8 bytes in the font's table) or 0. As Dq9GlyphIndex.
NDS_TITLE_HOOK bool Dq9GlyphEntry(uint32_t* r0, uint32_t* r1) {
    if (!*r0) return true;
    const int32_t i = FindGlyph(*r0, *r1);
    if (i < 0) {
        *r0 = 0;
        return true;
    }
    const uint32_t index = bus_read_u32(kIndexTable + 4u * *r1);
    *r0 = bus_read_u32(index + 16u) + 8u * static_cast<uint32_t>(i);
    return true;
}
