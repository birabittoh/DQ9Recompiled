// Screen layouts per game mode: some modes force both screens stacked at the
// same size, whatever Display > screen_layout says (the single-screen layout
// is the one that changes; the others already show both).
//
// The title screen (dq9::TitleScreen: the boot logos, the menu and the
// fade-out after Continue) uses both screens, so it is shown as on the DS,
// the top screen above the touchscreen. So are the story movies, which play
// on both screens too (dq9::MoviePlaying). Add a mode to Wanted() to give
// it the same layout.
//
// In battle the second screen stays below the main one in a portrait window,
// where the overworld and the other scenes put it above.
//
// Display > 2nd screen pos.: Over draws the second screen over the main
// one instead, see-through, in its bottom-left corner: in the field the map
// (the top LCD) at a third of the main screen's size, in battle the commands
// and the party (the bottom LCD) at half its size, over the 3D. Its fill is
// see-through, its white frames and letters solid. Elsewhere (the title, the
// pages that move the 3D to the top LCD) the second screen stays beside.
//
// With Over, the battle and the overworld (the camp menu too, drawn over
// the field) also fill the window at any shape: the main screen widens to
// it (title_settings.h, nds_title_set_wide_query): wider or, with the HD
// renderer, taller. The 2D stays at its size in the middle, the second screen in the window's bottom-left. The
// full-screen pages, the title and the movies keep the DS's 4:3.
//
// In battle the commands screen is cut into the party's panels, in two
// columns of two in the bottom corners, in the party's order (down the left
// column, then the right), without the wood around them: whole
// while someone chooses commands, only their names and HP/MP while the
// actions play. A submenu across the panels (spells, items) goes between the
// columns on its own, the panels keeping what is left of it (one piece with
// them when it covers their names too); a window with no panels (the results) goes at the top in
// the middle. Only what is inside the panels' and windows' white frames is
// drawn (OutsideFrames).
//
// The panels come from the battle's panel manager, ctrl+14176 (ov000
// 0x02160F08), ctrl being *(*(*(0x020F33D8) + 0x3718) + 1708) (as ov000
// 0x021700DC finds it):
// - +108: s8[4], the panels' order on screen (indices into the records);
// - +2392: four 1096-byte panel records: +76 s32 the party member (-1: no
//   panel), +68 / +72 s32 x / y in pixels, +36 bit 2 the tall panel, the
//   one choosing commands. 0x02174B14 lays them out: x 0, from y 0 down
//   when one is tall, else from y 32, 72 rows tall or 40;
// - +644: seven arm9 windows (224 bytes, owner at +392): +168 / +170 w / h,
//   +172 / +174 x / y, s16 tiles, +197 bit 0 open.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "title_hooks.h"
#include "title_settings.h"

#include "dq9_scene.h"

namespace {

constexpr uint32_t kMainMode = 0x020F33D8 + 4;
constexpr uint8_t kTitleMode = 6;

// The title screen is judged at G3_SwapBuffers, where the overlay slots are
// consistent: the frontend thread reading them mid-load flickered between
// layouts during the fade-out. The mode byte is a single byte, safe to read
// at any time, so the boot and the menu follow it at once.
std::atomic<bool> g_title_at_swap{false};
std::atomic<bool> g_battle_at_swap{false};
// The game proper, not one of its full-screen pages: where the second screen
// may go over the main one. Battles have no such pages, and the page tests
// (overlay slots, BG and blend registers) can match a battle's effects for a
// frame, which flipped the layout back and forth: a battle is never one.
std::atomic<bool> g_over_at_swap{false};
// The battle or the field, not a full-screen page: the scenes the main
// screen widens in. Debounced: each change resizes the 3D, and a test that
// matches for a frame or two would flicker.
std::atomic<bool> g_wide_at_swap{false};
constexpr int kWideSettleSwaps = 6;

// Called once per presented frame on the frontend thread. The movie is read
// here, not at the swap: it may draw no 3D, so no swap would see it.
bool Wanted() {
    return bus_read_u8(kMainMode) == kTitleMode ||
           g_title_at_swap.load(std::memory_order_relaxed) ||
           dq9::MoviePlaying();
}

// The story movies keep the two screens a little apart; the title screen
// keeps them together, as on the DS.
constexpr int kMovieGap = 16;  // LCD pixels
int StackedGap() { return dq9::MoviePlaying() ? kMovieGap : 0; }

// Called once per presented frame on the frontend thread. Judged at the swap
// like the title, since the overlay slots are inconsistent mid-load.
bool InsetBelow() { return g_battle_at_swap.load(std::memory_order_relaxed); }

constexpr int kBeside = 0;
constexpr int kOver = 1;
constexpr const char* kPlaceChoices[] = {"Beside", "Over"};
constexpr int kPlaceValues[] = {kBeside, kOver};

constexpr NdsTitleSetting kSecondScreenOver = {
    "second_screen_over",
    "2nd screen pos.",
    "Over draws the other screen see-through in a corner: the map in the "
    "field, the commands in battle.",
    0, 1, 1,
    kPlaceChoices, sizeof(kPlaceChoices) / sizeof(kPlaceChoices[0]),
    kPlaceValues,
    kOver,
};

const int kSecondScreenOverId = nds_title_setting_register(&kSecondScreenOver);

constexpr int kMapPercent = 33;      // the field's map: a third
constexpr int kBattlePercent = 50;   // the battle's commands: half
constexpr uint32_t kMapFill = 128;   // how solid the fill is, of 255
constexpr uint32_t kBattleFill = 179;

// `fill` for the fill, rising to solid for the near-white frames and letters
// (the darkest channel from 80% to 92% white). A black screen (a fade, the
// battle's way in) is left out whole.
void SeeThrough(const uint32_t* pixels, int width, uint32_t* out,
                uint32_t fill) {
    const size_t count = size_t(width) * 192;
    bool blank = true;
    for (size_t i = 0; i < count && blank; ++i)
        blank = (pixels[i] & 0xFFFFFFu) == 0;
    for (size_t i = 0; i < count; ++i) {
        const uint32_t rgb = pixels[i] & 0xFFFFFFu;
        uint32_t alpha = 0;
        if (!blank) {
            const uint32_t low = std::min({rgb >> 16, (rgb >> 8) & 0xFFu,
                                           rgb & 0xFFu});
            constexpr uint32_t kFrom = 204, kTo = 235;
            const uint32_t t = low <= kFrom ? 0
                : low >= kTo ? 256 : (low - kFrom) * 256 / (kTo - kFrom);
            const uint32_t smooth = t * t * (768 - 2 * t) / 65536;  // 0..256
            alpha = fill + (255 - fill) * smooth / 256;
        }
        out[i] = alpha << 24 | rgb;
    }
}

// The battle's panels and windows are each framed by a near-white line
// (grey while dimmed behind a submenu) around a dark fill; the wood and the
// panels' surround outside them are not drawn. Everything reachable from the
// screen's edges without crossing a frame or a fill pixel gets alpha 0. The
// fill stops it too: the frames have gaps (at their rounded corners) that let
// it into the boxes, which then lost their fill.
bool FramePixel(uint32_t rgb) {
    const uint32_t r = rgb >> 16 & 0xFFu, g = rgb >> 8 & 0xFFu, b = rgb & 0xFFu;
    const uint32_t low = std::min({r, g, b}), high = std::max({r, g, b});
    return low >= 140 && high - low <= 40;
}

// The boxes' dark fill (and the letters' shadows), darker than any of the
// wood, gold or light green around them.
bool FillPixel(uint32_t rgb) {
    const uint32_t r = rgb >> 16 & 0xFFu, g = rgb >> 8 & 0xFFu, b = rgb & 0xFFu;
    return std::max({r, g, b}) <= 96;
}

// The holes in the letters (b, B, A, 6...) are closed off by the white
// strokes, so the flood never reaches them even when it clears all around
// them: they stayed as dark specks. A small region the flood missed takes
// what is just across its strokes: cleared when most of that was. `seen` is
// the flood's (1: cleared); this marks the regions it looks at with 2.
void ClearedHoles(const uint32_t* pixels, int width, uint32_t* out,
                  std::vector<uint8_t>& seen) {
    constexpr size_t kMaxHole = 24;  // pixels: a letter's hole, not a box
    const int count = width * 192;
    static std::vector<int> hole, stack;
    const auto frame = [&](int i) { return FramePixel(pixels[i] & 0xFFFFFFu); };
    const auto neighbours = [&](int i, auto&& visit) {
        const int x = i % width;
        if (x > 0) visit(i - 1);
        if (x + 1 < width) visit(i + 1);
        if (i >= width) visit(i - width);
        if (i + width < count) visit(i + width);
    };
    for (int start = 0; start < count; ++start) {
        if (seen[size_t(start)] || frame(start)) continue;
        // The region: the pixels the flood missed, joined to this one.
        hole.clear();
        stack.assign(1, start);
        seen[size_t(start)] = 2;
        bool small = true;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            if (hole.size() < kMaxHole + 1) hole.push_back(i);
            else small = false;
            neighbours(i, [&](int j) {
                if (seen[size_t(j)] || frame(j)) return;
                seen[size_t(j)] = 2;
                stack.push_back(j);
            });
        }
        if (!small || hole.size() > kMaxHole) continue;
        // Across its strokes: straight out from each of its pixels, past up
        // to 3 frame pixels, to the first pixel that is not one.
        int cleared = 0, kept = 0;
        constexpr int kDx[] = {-1, 1, 0, 0}, kDy[] = {0, 0, -1, 1};
        for (const int i : hole) {
            for (int d = 0; d < 4; ++d) {
                int x = i % width, y = i / width, steps = 0;
                do {
                    x += kDx[d];
                    y += kDy[d];
                    ++steps;
                } while (x >= 0 && x < width && y >= 0 && y < 192 &&
                         steps <= 3 && frame(y * width + x));
                if (steps == 1 || steps > 3 || x < 0 || x >= width || y < 0 ||
                    y >= 192)
                    continue;  // inside the region, or no stroke out there
                const int across = y * width + x;
                if (seen[size_t(across)] == 1) ++cleared;
                else if (std::find(hole.begin(), hole.end(), across) ==
                         hole.end())
                    ++kept;
            }
        }
        if (cleared > kept)
            for (const int i : hole) out[i] &= 0x00FFFFFFu;
    }
}

void OutsideFrames(const uint32_t* pixels, int width, uint32_t* out) {
    const int count = width * 192;
    static std::vector<uint8_t> seen;
    static std::vector<int> stack;
    seen.assign(size_t(count), 0);
    stack.clear();
    const auto push = [&](int i) {
        const uint32_t rgb = pixels[i] & 0xFFFFFFu;
        if (seen[size_t(i)] || FramePixel(rgb) || FillPixel(rgb)) return;
        seen[size_t(i)] = 1;
        stack.push_back(i);
    };
    for (int x = 0; x < width; ++x) {
        push(x);
        push(191 * width + x);
    }
    for (int y = 0; y < 192; ++y) {
        push(y * width);
        push(y * width + width - 1);
    }
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        out[i] &= 0x00FFFFFFu;
        const int x = i % width;
        if (x > 0) push(i - 1);
        if (x + 1 < width) push(i + 1);
        if (i >= width) push(i - width);
        if (i + width < count) push(i + width);
    }
    ClearedHoles(pixels, width, out, seen);
}

// The battle's panels and windows on the commands screen, read at the swap
// (SampleScreenLayout) and cut up on the frontend thread.
struct BattleHud {
    bool valid = false;
    int panels = 0;
    int panel_x[4]{}, panel_y[4]{}, panel_h[4]{};
    bool tall = false;      // someone chooses commands
    bool across = false;    // a window across the panels (spells, items)
    bool windows = false;   // any window open
    int win_x0 = 0, win_y0 = 0, win_x1 = 0, win_y1 = 0;  // their bounds
    bool panel_tall[4]{};
    // With a window across the panels, the submenu: the windows reaching as
    // far right as the across ones (its list and its description).
    int sub_x0 = 0, sub_y0 = 0, sub_x1 = 0, sub_y1 = 0;
    int subs = 0;
    int sub_rect[7][4]{};  // its windows: x0, y0, x1, y1
};

std::mutex g_hud_mutex;
BattleHud g_hud;

bool InMainRam(uint32_t addr, uint32_t size) {
    return addr >= 0x02000000u && addr + size <= 0x02400000u;
}

BattleHud ReadBattleHud() {
    BattleHud hud;
    constexpr uint32_t kGame = 0x020F33D8;
    constexpr uint32_t kPanelManager = 14176;
    constexpr uint32_t kRecordSize = 1096;
    const uint32_t game = bus_read_u32(kGame);
    if (!InMainRam(game, 0x3720)) return hud;
    const uint32_t battle = bus_read_u32(game + 0x3718);
    if (!InMainRam(battle, 1712)) return hud;
    const uint32_t ctrl = bus_read_u32(battle + 1708);
    if (!InMainRam(ctrl, kPanelManager + 2392 + 4 * kRecordSize)) return hud;
    const uint32_t m = ctrl + kPanelManager;
    for (uint32_t i = 0; i < 4; ++i) {
        const int index = static_cast<int8_t>(bus_read_u8(m + 108 + i));
        if (index < 0 || index > 3) continue;
        const uint32_t record = m + 2392 + uint32_t(index) * kRecordSize;
        const int32_t member = static_cast<int32_t>(bus_read_u32(record + 76));
        if (member < 0 || member > 3) continue;
        const int32_t x = static_cast<int32_t>(bus_read_u32(record + 68));
        const int32_t y = static_cast<int32_t>(bus_read_u32(record + 72));
        const bool tall = (bus_read_u8(record + 36) & 4) != 0;
        const int h = tall ? 72 : 40;
        if (x < -256 || x > 256 || y < 0 || y + h > 192) return hud;
        hud.panel_x[hud.panels] = x;
        hud.panel_y[hud.panels] = y;
        hud.panel_h[hud.panels] = h;
        hud.panel_tall[hud.panels] = tall;
        hud.tall = hud.tall || tall;
        ++hud.panels;
    }
    struct Rect {
        int x0, y0, x1, y1;
        bool across;
    };
    Rect rects[7]{};
    int opened = 0;
    for (uint32_t i = 0; i < 7; ++i) {
        const uint32_t window = m + 644 + 224 * i;
        if ((bus_read_u8(window + 197) & 1) == 0) continue;
        const int w = static_cast<int16_t>(bus_read_u16(window + 168)) * 8;
        const int h = static_cast<int16_t>(bus_read_u16(window + 170)) * 8;
        const int x = static_cast<int16_t>(bus_read_u16(window + 172)) * 8;
        const int y = static_cast<int16_t>(bus_read_u16(window + 174)) * 8;
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > 256 || y + h > 192)
            continue;
        // Inside one panel (the commands in the tall one) it is that panel's.
        bool inside = false;
        for (int p = 0; p < hud.panels && !inside; ++p)
            inside = y >= hud.panel_y[p] &&
                     y + h <= hud.panel_y[p] + hud.panel_h[p];
        if (!hud.windows) {
            hud.win_x0 = x;
            hud.win_y0 = y;
            hud.win_x1 = x + w;
            hud.win_y1 = y + h;
        } else {
            hud.win_x0 = std::min(hud.win_x0, x);
            hud.win_y0 = std::min(hud.win_y0, y);
            hud.win_x1 = std::max(hud.win_x1, x + w);
            hud.win_y1 = std::max(hud.win_y1, y + h);
        }
        hud.windows = true;
        hud.across = hud.across || !inside;
        rects[opened++] = {x, y, x + w, y + h, !inside};
    }
    if (hud.across) {
        // Its windows reach as far right as the across ones (the spells' or
        // items' list starts left of the description, the items' at 112);
        // the commands window under them stops short (at 136 or 208).
        int right = 0;
        for (int i = 0; i < opened; ++i)
            if (rects[i].across) right = std::max(right, rects[i].x1);
        bool first = true;
        for (int i = 0; i < opened; ++i) {
            const Rect& r = rects[i];
            if (r.x1 < right - 8) continue;
            hud.sub_x0 = first ? r.x0 : std::min(hud.sub_x0, r.x0);
            hud.sub_y0 = first ? r.y0 : std::min(hud.sub_y0, r.y0);
            hud.sub_x1 = first ? r.x1 : std::max(hud.sub_x1, r.x1);
            hud.sub_y1 = first ? r.y1 : std::max(hud.sub_y1, r.y1);
            first = false;
            int* out = hud.sub_rect[hud.subs++];
            out[0] = r.x0;
            out[1] = r.y0;
            out[2] = r.x1;
            out[3] = r.y1;
        }
    }
    hud.valid = hud.panels > 0 || hud.windows;
    return hud;
}

// Pieces in the main screen's pixels: half size, from its bottom left (of a
// widened or taller view too), a 2-pixel gap under and beside each.
constexpr float kHalf = 0.5f;
constexpr float kGap = 2.0f;
constexpr float kLeft = 2.0f;
// A party of one stands a panel higher, easier to see.
constexpr float kAloneRaise = 40 * kHalf + kGap;
// A submenu starting left of this covers the panels' names: kept whole with
// them.
constexpr int kMinSubmenuX = 64;

int CutBattleHud(const BattleHud& hud, NdsTitleInsetPiece* pieces) {
    int n = 0;
    const auto add = [&](float sx, float sy, float sw, float sh, float dx,
                         float dy, float scale) {
        if (n < kNdsTitleMaxInsetPieces)
            pieces[n++] = {sx, sy, sw, sh, dx, dy, sw * scale, sh * scale};
    };
    if (hud.panels == 0) {
        // A window alone (the results): big, at the top in the middle.
        const float x0 = float(std::max(hud.win_x0 - 3, 0));
        const float y0 = float(std::max(hud.win_y0 - 3, 0));
        const float w = float(std::min(hud.win_x1 + 3, 256)) - x0;
        const float h = float(std::min(hud.win_y1 + 3, 192)) - y0;
        const float scale = std::min({1.0f, 200.0f / w, 110.0f / h});
        add(x0, y0, w, h, (nds_title_wide_width() - w * scale) / 2, 6,
            scale);
        return n;
    }
    const float raise = hud.panels == 1 ? kAloneRaise : 0.0f;
    // A submenu opens only while someone chooses commands; during the
    // actions a window outside the panels is the action's line (below a
    // party of one's panel).
    const bool submenu = hud.across && hud.tall && hud.sub_x0 >= kMinSubmenuX;
    if (hud.across && !submenu) {
        // A window across the panels that is not a submenu, or covers their
        // names too: all of it in one piece, so its windows stay whole.
        int top = hud.panel_y[0], bottom = hud.panel_y[0] + hud.panel_h[0];
        for (int i = 1; i < hud.panels; ++i) {
            top = std::min(top, hud.panel_y[i]);
            bottom = std::max(bottom, hud.panel_y[i] + hud.panel_h[i]);
        }
        top = std::min(top, hud.win_y0);
        bottom = std::max(bottom, hud.win_y1);
        const float h = float(bottom - top);
        add(0, float(top), 256, h, kLeft,
            float(nds_title_wide_height()) - h * kHalf - kGap - raise, kHalf);
        return n;
    }
    // Two columns of up to two panels each, one in each bottom corner, the
    // party's order down the left column then the right, each column
    // standing on the bottom edge.
    // A submenu (spells, items) is cut out on its own, between the columns,
    // and the panels it covered keep only what is left of it: the chooser's
    // name and commands, the others' names and HP/MP.
    const auto panel_w = [&](int i) {
        if (!submenu) return hud.tall ? 256 : 119 + 15;
        // Up to the submenu's windows beside it (the items' list reaches
        // further left than its description).
        int cut = 256;
        for (int s = 0; s < hud.subs; ++s)
            if (hud.sub_rect[s][1] < hud.panel_y[i] + hud.panel_h[i] &&
                hud.sub_rect[s][3] > hud.panel_y[i])
                cut = std::min(cut, hud.sub_rect[s][0]);
        return hud.panel_tall[i] ? cut : std::min(cut, 119);
    };
    for (int column = 0; column * 2 < hud.panels; ++column) {
        const int first = column * 2;
        const int last = std::min(first + 2, hud.panels);
        float total = raise;
        int widest = 0;
        for (int i = first; i < last; ++i) {
            total += hud.panel_h[i] * kHalf + kGap;
            widest = std::max(widest, panel_w(i));
        }
        float y = float(nds_title_wide_height()) - total;
        const float dx = column == 0
            ? kLeft
            : float(nds_title_wide_width()) - kLeft - widest * kHalf;
        for (int i = first; i < last; ++i) {
            const float sx = float(hud.panel_x[i]);
            const float sy = float(hud.panel_y[i]);
            const float sh = float(hud.panel_h[i]);
            // Whole rows: OutsideFrames leaves only what is inside the frames.
            if (submenu) {
                add(sx, sy, float(panel_w(i)), sh, dx, y, kHalf);
            } else if (hud.tall) {
                // Choosing commands: the whole panels.
                add(sx, sy, 256, sh, dx, y, kHalf);
            } else {
                // The actions: each panel's name and HP/MP, then its right
                // edge.
                add(sx, sy, 119, sh, dx, y, kHalf);
                add(sx + 241, sy, 15, sh, dx + 119 * kHalf, y, kHalf);
            }
            y += sh * kHalf + kGap;
        }
    }
    if (submenu) {
        // Window by window, as they stand together: what is between them
        // and their bounds (the panels' edges) stays with the panels.
        const float w = float(hud.sub_x1 - hud.sub_x0);
        const float h = float(hud.sub_y1 - hud.sub_y0);
        const float x = (nds_title_wide_width() - w * kHalf) / 2;
        const float top = float(nds_title_wide_height()) - h * kHalf - kGap;
        for (int s = 0; s < hud.subs; ++s) {
            const int* r = hud.sub_rect[s];
            add(float(r[0]), float(r[1]), float(r[2] - r[0]),
                float(r[3] - r[1]), x + (r[0] - hud.sub_x0) * kHalf,
                top + (r[1] - hud.sub_y0) * kHalf, kHalf);
        }
    }
    return n;
}

// screen: the LCD the inset shows. The field puts the 3D on the bottom LCD,
// so its inset is the top one; the battle the other way round.
int InsetOverlay(int screen, const uint32_t* pixels, int width,
                 uint32_t* out, NdsTitleInsetPiece* pieces,
                 int* piece_count) {
    if (nds_title_setting_value(kSecondScreenOverId) != kOver ||
        !g_over_at_swap.load(std::memory_order_relaxed))
        return 0;
    if (screen == 0) {
        SeeThrough(pixels, width, out, kMapFill);
        return kMapPercent;
    }
    if (!g_battle_at_swap.load(std::memory_order_relaxed)) return 0;
    SeeThrough(pixels, width, out, kBattleFill);
    OutsideFrames(pixels, width, out);
    BattleHud hud;
    {
        std::lock_guard<std::mutex> lock(g_hud_mutex);
        hud = g_hud;
    }
    // Until the panels are up, the whole screen.
    if (!hud.valid) return kBattlePercent;
    *piece_count = CutBattleHud(hud, pieces);
    return 100;
}

// Called once per presented frame on the frontend thread.
bool Wide() {
    return nds_title_setting_value(kSecondScreenOverId) == kOver &&
           g_wide_at_swap.load(std::memory_order_relaxed) &&
           !dq9::MoviePlaying();
}

const bool kRegistered = (nds_title_set_stacked_layout_query(Wanted),
                          nds_title_set_wide_query(Wide),
                          nds_title_set_stacked_gap_query(StackedGap),
                          nds_title_set_inset_below_query(InsetBelow),
                          nds_title_set_inset_overlay_query(InsetOverlay),
                          true);

}  // namespace

namespace dq9 {

void SampleScreenLayout() {
    g_title_at_swap.store(TitleScreen(), std::memory_order_relaxed);
    const bool battle = InBattle();
    g_battle_at_swap.store(battle, std::memory_order_relaxed);
    const bool running = GameModeRunning();
    const bool page = !battle && FullScreenPage();
    g_over_at_swap.store(running && !page, std::memory_order_relaxed);
    const bool wide = running && !page && (battle || FieldLoaded());
    static int changed_swaps = 0;
    if (wide == g_wide_at_swap.load(std::memory_order_relaxed)) {
        changed_swaps = 0;
    } else if (++changed_swaps >= kWideSettleSwaps) {
        changed_swaps = 0;
        g_wide_at_swap.store(wide, std::memory_order_relaxed);
    }
    const BattleHud hud = battle ? ReadBattleHud() : BattleHud{};
    std::lock_guard<std::mutex> lock(g_hud_mutex);
    g_hud = hud;
}

}  // namespace dq9
