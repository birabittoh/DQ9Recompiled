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
// In battle the commands screen is cut into the party's panels, stacked in
// the bottom left in the party's order, without the wood around them: whole
// while someone chooses commands, only their names and HP/MP while the
// actions play. A window across the panels (spells, items) keeps them in one
// piece with it; a window with no panels (the results) goes at the top in
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
// (grey while dimmed behind a submenu); the wood and the panels' surround
// outside those frames are not drawn. Everything reachable from the
// screen's edges without crossing a frame pixel gets alpha 0.
bool FramePixel(uint32_t rgb) {
    const uint32_t r = rgb >> 16 & 0xFFu, g = rgb >> 8 & 0xFFu, b = rgb & 0xFFu;
    const uint32_t low = std::min({r, g, b}), high = std::max({r, g, b});
    return low >= 140 && high - low <= 40;
}

void OutsideFrames(const uint32_t* pixels, int width, uint32_t* out) {
    const int count = width * 192;
    static std::vector<uint8_t> seen;
    static std::vector<int> stack;
    seen.assign(size_t(count), 0);
    stack.clear();
    const auto push = [&](int i) {
        if (seen[size_t(i)] || FramePixel(pixels[i] & 0xFFFFFFu)) return;
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
        hud.tall = hud.tall || tall;
        ++hud.panels;
    }
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
    }
    hud.valid = hud.panels > 0 || hud.windows;
    return hud;
}

// Pieces in the main screen's pixels: half size, stacked up from its bottom
// left, a 2-pixel gap under each.
constexpr float kHalf = 0.5f;
constexpr float kGap = 2.0f;
constexpr float kLeft = 2.0f;
// A party of one stands a panel higher, easier to see.
constexpr float kAloneRaise = 40 * kHalf + kGap;

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
        add(x0, y0, w, h, (256 - w * scale) / 2, 6, scale);
        return n;
    }
    const float raise = hud.panels == 1 ? kAloneRaise : 0.0f;
    if (hud.across) {
        // A window across the panels: all of it in one piece, so its
        // windows stay whole.
        int top = hud.panel_y[0], bottom = hud.panel_y[0] + hud.panel_h[0];
        for (int i = 1; i < hud.panels; ++i) {
            top = std::min(top, hud.panel_y[i]);
            bottom = std::max(bottom, hud.panel_y[i] + hud.panel_h[i]);
        }
        top = std::min(top, hud.win_y0);
        bottom = std::max(bottom, hud.win_y1);
        const float h = float(bottom - top);
        add(0, float(top), 256, h, kLeft, 192 - h * kHalf - kGap - raise,
            kHalf);
        return n;
    }
    float total = raise;
    for (int i = 0; i < hud.panels; ++i)
        total += hud.panel_h[i] * kHalf + kGap;
    float y = 192 - total;
    for (int i = 0; i < hud.panels; ++i) {
        const float sx = float(hud.panel_x[i]);
        const float sy = float(hud.panel_y[i]);
        const float sh = float(hud.panel_h[i]);
        // Whole rows: OutsideFrames leaves only what is inside the frames.
        if (hud.tall) {
            // Choosing commands: the whole panels.
            add(sx, sy, 256, sh, kLeft, y, kHalf);
        } else {
            // The actions: each panel's name and HP/MP, then its right
            // edge.
            add(sx, sy, 119, sh, kLeft, y, kHalf);
            add(sx + 241, sy, 15, sh, kLeft + 119 * kHalf, y, kHalf);
        }
        y += sh * kHalf + kGap;
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

const bool kRegistered = (nds_title_set_stacked_layout_query(Wanted),
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
    g_over_at_swap.store(GameModeRunning() && (battle || !FullScreenPage()),
                         std::memory_order_relaxed);
    const BattleHud hud = battle ? ReadBattleHud() : BattleHud{};
    std::lock_guard<std::mutex> lock(g_hud_mutex);
    g_hud = hud;
}

}  // namespace dq9
