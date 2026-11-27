// The camp menu's Misc. list gets an "Options" row, which opens the same
// pages as the title menu's Options row (options_model.h): the categories,
// then one page of options per category. See
// config/hooks/dq9_arm9_ov002.toml for the guest code.
//
// The pages are shown by the Misc. list itself (camp state and window 0x15,
// step 0x02166168, with its description in window 0x16). While a page is
// open the hooks replace what the list is made of: its entries (0x02156080),
// their text, the window's title and the description. Choosing a row or
// pressing B then goes through the list's own code:
//   - a new page closes the list the way B does (0x02156E90) and sets the
//     camp state back to "open the Misc. list", so the window is opened
//     again at the new page's size, one frame later;
//   - a changed value only has the list window drawn again.
// Left and right step the value under the cursor.
//
// A page in parts (the keyboard's keys) shows one part per list: L and R,
// left and right on a row without a value, and the pen on the window's
// bottom corners turn the part, which opens the list again as a new page
// does, at the tallest part's size. The window shows the game's own page
// arrows and "n/N" (its page indicator and arrows flag, as the Zoom list
// sets them).
#include <cstdint>
#include <string>

#include "options_model.h"

namespace {

using namespace dq9::options;

// ---- Guest layout -------------------------------------------------------

// The camp menu object (r8 in the Misc. step).
constexpr uint32_t kCampState = 0x1BB8u;     // u32: state = top window id
constexpr uint32_t kCampSubState = 0x1BC0u;  // u32: 0 open, 1 run
constexpr uint32_t kCampMiscCursor = 0x1BFEu;  // s16
constexpr uint32_t kStateMisc = 0x15u;

// The window manager's "next window" fields (camp + 0xEC8), written by
// 0x0215BE00 from the layout table and copied into the window when it opens.
constexpr uint32_t kNextWidth = 0xA0u;   // s16, tiles
constexpr uint32_t kNextHeight = 0xA2u;  // s16, tiles: 3 + 2 per entry
constexpr uint32_t kNextLines = 0xAAu;   // s16: the title and the entries
constexpr uint32_t kLayoutLines = 2u;    // u8 in the layout table's entry
constexpr uint32_t kWindowMisc = 0x15u;

// The Misc. list holds entry codes 0..8 (text 4001 + code, description
// 4020 + code), at most 9 of them; the game itself lists 8 at most.
constexpr uint32_t kMaxEntries = 9u;
constexpr uint32_t kOptionsCode = 9u;   // ours, after the game's
constexpr uint32_t kPageCode = 0x100u;  // + the row, while a page is open

// The pad (0x02114E30): +0 held, +2 held on the previous frame.
constexpr uint32_t kPadHeld = 0x02114E30u;
constexpr uint32_t kPadPrevious = 0x02114E32u;
constexpr uint32_t kKeyRight = 0x010u, kKeyLeft = 0x020u;
constexpr uint32_t kKeyR = 0x100u, kKeyL = 0x200u;

// The window manager (camp + 0xEC8): +152 non-zero with windows up, +156
// the windows (224 bytes each), +179 how many, +180 the next one's slot.
constexpr uint32_t kWindowManager = 0xEC8u;
constexpr uint32_t kWindowSize = 224u;
// A window: s16 width, height, x, y in tiles; its id; flags (bit 3: draw the
// page arrows, with the text); the page indicator (+216 bit 0 on, bit 1
// redraw it), the page and the page count.
constexpr uint32_t kWinWidth = 168u, kWinHeight = 170u;
constexpr uint32_t kWinX = 172u, kWinY = 174u;
constexpr uint32_t kWinId = 196u;
constexpr uint32_t kWinFlags = 197u;
constexpr uint8_t kWinArrows = 0x08u;
constexpr uint32_t kWinIndicator = 216u;
constexpr uint32_t kWinPage = 217u;
constexpr uint32_t kWinPages = 218u;

// The touch manager: +85 a press began this frame; where, as 0x02012A94
// reads it.
constexpr uint32_t kTouch = 0x02114E54u;

// Free guest memory: the end of overlay slot 0 (0x021536E0 .. 0x021842A0),
// far past overlay 2's image and bss (they end at 0x0216D3C0).
constexpr uint32_t kRowText = 0x02183A00u;  // kMaxEntries strings
constexpr uint32_t kRowTextSize = 0x60u;
constexpr uint32_t kTitleText = kRowText + kMaxEntries * kRowTextSize;
constexpr uint32_t kTitleTextSize = 0x40u;
constexpr uint32_t kDescText = kTitleText + kTitleTextSize;
constexpr uint32_t kDescTextSize = 0x100u;

// A page's window is wider than the Misc. list's 12 tiles: its rows are
// "label: value". The description window opens to its right, 12 tiles wide.
constexpr uint32_t kPageWidth = 16u;

// ---- State --------------------------------------------------------------

struct Camp : Model {
    bool open = false;        // a page is shown instead of the Misc. entries
    uint32_t misc_count = 0;  // the Misc. list's entries, ours included
    int options_index = -1;   // our row in the Misc. list
    // Set by one hook for a later one in the Misc. step:
    bool close_list = false;    // next frame: close the list (as B does)
    bool keep_state = false;    // at the step's end: stay in the Misc. state
    bool reopen = false;        // at the step's end: open the list again
    bool reopening = false;     // the list is being opened by `reopen`
    bool redraw_list = false;   // draw the list window again
    bool redraw_arrows = false; // once, after a page in parts opens: its arrows
                                // are drawn only by a later draw of the window
    int cursor_on_open = -1;    // where the reopened list puts its cursor
    uint32_t polled = 0;        // frames, to look for changed rows now and then
};

Camp g_camp;

int16_t S16(uint32_t value) { return static_cast<int16_t>(value & 0xFFFFu); }

uint32_t Language() { return dq9::TextLanguage(); }

bool InMainRam(uint32_t address, uint32_t size) {
    return address >= 0x02000000u && address + size <= 0x02400000u;
}

// Window `id` of the camp menu, as 0x0215B918 finds it: an open one, else
// the slot the next window opens in. 0 for none.
uint32_t CampWindow(uint32_t camp, uint32_t id) {
    const uint32_t manager = camp + kWindowManager;
    const uint32_t windows = bus_read_u32(manager + 156u);
    if (!InMainRam(windows, kWindowSize)) return 0;
    if (bus_read_u32(manager + 152u)) {
        const uint32_t count = bus_read_u8(manager + 179u);
        const uint32_t next = bus_read_u8(manager + 180u);
        for (uint32_t i = 0; i < count && i != next; ++i) {
            const uint32_t window = windows + kWindowSize * i;
            if (bus_read_u8(window + kWinId) == id) return window;
        }
    }
    const uint32_t window = windows + kWindowSize * bus_read_u8(manager + 180u);
    return InMainRam(window, kWindowSize) ? window : 0;
}

// The page indicator and arrows of the list window, on for a page in parts
// (0x02050410, 0x0205043C and 0x02050450 natively).
// Left alone on a window that was never paged.
void SetPaging(uint32_t window, uint32_t page, uint32_t pages) {
    const bool on = pages > 1;
    const uint8_t flags = bus_read_u8(window + kWinFlags);
    if (!on && !(bus_read_u8(window + kWinIndicator) & 1u) &&
        !(flags & kWinArrows))
        return;
    // As 0x02050410 writes it: the page byte too.
    bus_write_u16(window + kWinIndicator, static_cast<uint16_t>(
        (bus_read_u8(window + kWinIndicator) & 0xFEu) | (on ? 1u : 0u) | 2u));
    bus_write_u8(window + kWinPage, static_cast<uint8_t>(on ? page : 0u));
    bus_write_u8(window + kWinPages, static_cast<uint8_t>(on ? pages : 0u));
    bus_write_u8(window + kWinFlags, on ? flags | kWinArrows : flags & ~kWinArrows);
}

// -1 or +1 when the pen was just put on the list window's left or right
// bottom corner (as 0x02161B98 tests the paged lists'), else 0.
int PenTurn(uint32_t camp) {
    if (!bus_read_u8(kTouch + 85u)) return 0;
    int x = 0, y = 0;
    if (bus_read_u16(kTouch + 36u)) {
        x = bus_read_u16(kTouch + 32u);
        y = bus_read_u16(kTouch + 34u);
    } else {
        x = static_cast<int>(bus_read_u32(kTouch + 56u));
        y = static_cast<int>(bus_read_u32(kTouch + 60u));
    }
    const uint32_t window = CampWindow(camp, kWindowMisc);
    if (!window || bus_read_u8(window + kWinId) != kWindowMisc) return 0;
    const int left = 8 * static_cast<int16_t>(bus_read_u16(window + kWinX));
    const int right = left + 8 * static_cast<int16_t>(bus_read_u16(window + kWinWidth));
    const int bottom = 8 * (static_cast<int16_t>(bus_read_u16(window + kWinY)) +
                            static_cast<int16_t>(bus_read_u16(window + kWinHeight)));
    if (y < bottom - 16 || y >= bottom) return 0;
    if (x >= left && x < left + 16) return -1;
    if (x >= right - 16 && x < right) return 1;
    return 0;
}

// The window text is markup: outside a <tag>, a comma and an apostrophe
// are written as the game's own text writes them. The description window
// reads "% s" as a format, so its % is doubled (`escape_percent`); the
// title and rows print a % as is.
std::string Markup(const std::string& text, bool escape_percent = false) {
    std::string out;
    bool tag = false;
    for (const char c : text) {
        if (c == '<') tag = true;
        if (!tag && c == ',') out += "<,>";
        else if (!tag && c == '\'') out += "<1>";
        else if (!tag && escape_percent && c == '%') out += "%%";
        else out += c;
        if (c == '>') tag = false;
    }
    return out;
}

// The rows of a part when a page has several: the game's own list limit. A
// window of 9 rows plus the arrows and "n/N" is taller than the screen allows
// (the arrows and the indicator fall off its bottom).
constexpr size_t kPartRows = 8u;

// No Back row. A page that fits shows all its rows (up to kMaxEntries); a
// longer one is shown in parts of kPartRows.
void BuildPage(Model& model) {
    BuildRows(model, kMaxEntries, false);
    if (model.parts > 1) BuildRows(model, kPartRows, false);
}

// B, or the pen outside the window, goes up one page.
void Rebuild() {
    BuildPage(g_camp);
    g_camp.signature = Signature(g_camp);
}

void ShowPage(const std::string& section) {
    g_camp.open = true;
    g_camp.section = section;
    Rebuild();
    uint32_t& cursor = g_camp.cursor[section];
    if (cursor >= g_camp.rows.size()) cursor = 0;
    g_camp.cursor_on_open = static_cast<int>(cursor);
}

// Up one page: from a category to the categories, from those to Misc.
void GoUp() {
    if (g_camp.section.empty()) {
        g_camp.open = false;
        g_camp.cursor_on_open = g_camp.options_index;
    } else {
        ShowPage(Parent(g_camp.section));
    }
}

// Shows the next (+1) or previous (-1) part of the page, the cursor on the
// same row or the last. False for a page with one part.
bool Turn(int direction, int cursor) {
    if (!TurnPart(g_camp, direction)) return false;
    Rebuild();
    uint32_t& at = g_camp.cursor[g_camp.section];
    at = cursor < 0 ? 0u : static_cast<uint32_t>(cursor);
    if (at >= g_camp.rows.size())
        at = g_camp.rows.empty() ? 0u : static_cast<uint32_t>(g_camp.rows.size() - 1);
    g_camp.cursor_on_open = static_cast<int>(at);
    return true;
}

}  // namespace

// 0x02156154, the end of the Misc. entry list builder: sp = its 9 s16
// codes, r7 = how many. Adds the Options row, or lists the open page's rows.
NDS_TITLE_HOOK void Dq9CampOptionsList(uint32_t* r7, uint32_t* sp) {
    if (g_camp.open) {
        const uint32_t count = static_cast<uint32_t>(g_camp.rows.size());
        for (uint32_t i = 0; i < count && i < kMaxEntries; ++i)
            bus_write_u16(*sp + 2u * i, static_cast<uint16_t>(kPageCode + i));
        *r7 = count < kMaxEntries ? count : kMaxEntries;
        return;
    }
    g_camp.options_index = -1;
    if (*r7 < kMaxEntries) {
        g_camp.options_index = static_cast<int>(*r7);
        bus_write_u16(*sp + 2u * *r7, static_cast<uint16_t>(kOptionsCode));
        *r7 += 1u;
    }
    g_camp.misc_count = *r7;
}

// 0x0215BED8 in 0x0215BE00, after the next window's size is set: r4 = the
// window manager, r6 = the layout entry. The Misc. window is sized for 8
// entries at most (19 tiles, 9 lines); a 9th needs one more of each. A
// page's window is wider.
NDS_TITLE_HOOK void Dq9CampOptionsSize(uint32_t* r4, uint32_t* r6) {
    if (bus_read_u8(*r6) != kWindowMisc) return;
    const uint32_t count = g_camp.open
        ? static_cast<uint32_t>(g_camp.rows.size()) : g_camp.misc_count;
    if (g_camp.open) bus_write_u16(*r4 + kNextWidth, kPageWidth);
    // A page's window is sized from its own rows, so a short one (the
    // categories) keeps the margin under its last row; a page in parts from
    // its tallest part, with room for the arrows and "n/N" under it.
    const bool paged = g_camp.open && g_camp.parts > 1;
    const uint32_t shown = paged ? g_camp.tallest : count;
    // The lines follow the tallest part too, so every part of a page lays its
    // title out like the first one (the text is centred by them).
    if (count > 8u || (g_camp.open && count > 0u)) {
        // One more tile on a page: the margin under the last row.
        bus_write_u16(*r4 + kNextHeight, static_cast<uint16_t>(
            3u + 2u * shown + (paged ? 2u : g_camp.open ? 1u : 0u)));
        bus_write_u16(*r4 + kNextLines, static_cast<uint16_t>(
            static_cast<int>(bus_read_u8(*r6 + kLayoutLines)) +
            static_cast<int>(shown) - 7));
    }
}

// 0x0215F53C in the list window's builder, r0 = its title ("Misc."), r10 =
// the camp menu: a page shows its own, and a page in parts the arrows and
// "n/N".
NDS_TITLE_HOOK void Dq9CampOptionsTitle(uint32_t* r0, uint32_t* r10) {
    if (const uint32_t window = CampWindow(*r10, kWindowMisc))
        SetPaging(window, g_camp.open ? g_camp.part[g_camp.section] : 0u,
                  g_camp.open ? g_camp.parts : 0u);
    if (!g_camp.open) return;
    const std::string title = g_camp.section.empty()
        ? std::string(dq9::Tr("main_menu", "options", Language()))
        : Text("cat_" + Slug(g_camp.section), PageName(g_camp.section));
    WriteString(kTitleText, kTitleTextSize, Markup(title));
    *r0 = kTitleText;
}

// 0x0215F5E4 in the list window's builder, r0 = the text of entry r7, whose
// code is r8 (null for ours: the game has no such text).
NDS_TITLE_HOOK void Dq9CampOptionsLabel(uint32_t* r0, uint32_t* r7,
                                        uint32_t* r8) {
    const uint32_t index = *r7;
    if (index >= kMaxEntries) return;
    std::string text;
    if (g_camp.open) {
        if (index < g_camp.rows.size())
            text = RowText(g_camp, g_camp.rows[index]);
    } else if (S16(*r8) == static_cast<int16_t>(kOptionsCode)) {
        text = dq9::Tr("main_menu", "options", Language());
    } else {
        return;
    }
    const uint32_t address = kRowText + index * kRowTextSize;
    WriteString(address, kRowTextSize, Markup(text));
    *r0 = address;
}

// 0x0215F690 in the description window's builder, r0 = the description of
// the entry under the cursor, r5 = the camp menu.
NDS_TITLE_HOOK void Dq9CampOptionsDescription(uint32_t* r0, uint32_t* r5) {
    const int cursor = S16(bus_read_u16(*r5 + kCampMiscCursor));
    std::string text;
    if (g_camp.open) {
        if (cursor >= 0 && static_cast<size_t>(cursor) < g_camp.rows.size())
            text = DescriptionText(g_camp, g_camp.rows[static_cast<size_t>(cursor)]);
    } else if (cursor >= 0 && cursor == g_camp.options_index) {
        text = dq9::Tr("camp_menu", "options_description", Language());
    } else {
        return;
    }
    WriteString(kDescText, kDescTextSize, Markup(text, true));
    *r0 = kDescText;
}

// The page indicator of the list window while a page in parts is open: the
// window list's step (0x0205D0F0) writes the list widget's own page and count
// (one page, as the Misc. list is) into it every frame; these two, on its
// calls, put the part and the number of parts in. See dq9_arm9.toml.
bool PartsWindow(uint32_t window) {
    return g_camp.open && g_camp.parts > 1 && InMainRam(window, kWindowSize) &&
           bus_read_u8(window + kWinId) == kWindowMisc;
}

NDS_TITLE_HOOK void Dq9CampOptionsIndicatorPage(uint32_t* window,
                                                uint32_t* page) {
    if (PartsWindow(*window)) *page = g_camp.part[g_camp.section];
}

NDS_TITLE_HOOK void Dq9CampOptionsIndicatorPages(uint32_t* window,
                                                 uint32_t* pages) {
    if (PartsWindow(*window)) *pages = g_camp.parts;
}

// 0x021661B0, the Misc. state opening its windows (sub-state 0), after it
// put the cursor on the first entry: r8 = the camp menu. Opened by the
// hooks, the cursor goes where the page (or Misc.) had it; opened by the
// game, the list is the Misc. list again.
NDS_TITLE_HOOK void Dq9CampOptionsOpen(uint32_t* r8) {
    if (!g_camp.reopening) {
        g_camp = Camp{};
        return;
    }
    g_camp.reopening = false;
    g_camp.redraw_arrows = g_camp.open && g_camp.parts > 1;
    if (g_camp.cursor_on_open >= 0)
        bus_write_u16(*r8 + kCampMiscCursor,
                      static_cast<uint16_t>(g_camp.cursor_on_open));
    g_camp.cursor_on_open = -1;
}

// 0x021661E0, the start of the Misc. state's every-frame part (sub-state
// 1): r8 = the camp menu. True: jump to the code that closes the list
// (0x02166588), for a new page. Otherwise left and right step the value
// under the cursor.
NDS_TITLE_HOOK bool Dq9CampOptionsFrame(uint32_t* r8) {
    if (g_camp.close_list) {
        g_camp.close_list = false;
        g_camp.reopen = true;
        return true;
    }
    if (!g_camp.open) return false;
    if (g_camp.redraw_arrows) {
        g_camp.redraw_arrows = false;
        g_camp.redraw_list = true;
    }
    // Rows the frontend changed on its own (a key row waiting for its key,
    // then bound) draw the list again; a few times a second is enough.
    if (++g_camp.polled % 10 == 0) {
        Camp probe = g_camp;
        BuildPage(probe);
        if (Signature(probe) != g_camp.signature &&
            probe.rows.size() == g_camp.rows.size()) {
            Rebuild();
            g_camp.redraw_list = true;
        }
    }
    const uint32_t held = bus_read_u16(kPadHeld);
    const uint32_t pressed = held & ~bus_read_u16(kPadPrevious);
    const int direction = (pressed & kKeyLeft) ? -1
                        : (pressed & kKeyRight) ? 1 : 0;
    const int cursor = S16(bus_read_u16(*r8 + kCampMiscCursor));
    // A turn of the part opens the list again (Dq9CampOptionsExit).
    if (g_camp.parts > 1) {
        int turn = (pressed & kKeyL) ? -1 : (pressed & kKeyR) ? 1 : PenTurn(*r8);
        if (turn == 0 && direction != 0 &&
            (cursor < 0 || static_cast<size_t>(cursor) >= g_camp.rows.size() ||
             !Steps(g_camp.rows[static_cast<size_t>(cursor)])))
            turn = direction;
        if (turn != 0 && Turn(turn, cursor)) {
            g_camp.reopen = true;
            return true;
        }
    }
    if (direction == 0 || cursor < 0 ||
        static_cast<size_t>(cursor) >= g_camp.rows.size())
        return false;
    const Row row = g_camp.rows[static_cast<size_t>(cursor)];
    if (row.kind == Row::Kind::Item && Change(g_camp, row.item, direction)) {
        Rebuild();
        g_camp.redraw_list = true;
    }
    return false;
}

// 0x0216627C, `cmp r0, #0`: r0 != 0 draws the list window again.
NDS_TITLE_HOOK void Dq9CampOptionsRedraw(uint32_t* r0) {
    if (!g_camp.redraw_list) return;
    g_camp.redraw_list = false;
    *r0 = 1u;
}

// 0x02166344, `cmp r0, #8` on the code of the chosen entry (A or the pen).
// True for our entries: jump to 0x021663B4, which plays the choice sound
// and sets a camp state that Dq9CampOptionsExit then puts back.
NDS_TITLE_HOOK bool Dq9CampOptionsChoose(uint32_t* r0) {
    const int code = S16(*r0);
    if (!g_camp.open) {
        if (code != static_cast<int>(kOptionsCode)) return false;
        g_camp.pending.clear();
        ShowPage("");
        g_camp.close_list = true;
        g_camp.keep_state = true;
        return true;
    }
    const int index = code - static_cast<int>(kPageCode);
    if (index < 0 || static_cast<size_t>(index) >= g_camp.rows.size())
        return false;
    g_camp.cursor[g_camp.section] = static_cast<uint32_t>(index);
    const Row row = g_camp.rows[static_cast<size_t>(index)];
    switch (row.kind) {
    case Row::Kind::Category:
        ShowPage(row.section);
        g_camp.close_list = true;
        break;
    case Row::Kind::Item:
        if (Change(g_camp, row.item, 0)) {
            Rebuild();
            g_camp.redraw_list = true;
        }
        break;
    default:
        GoUp();
        g_camp.close_list = true;
        break;
    }
    g_camp.keep_state = true;
    return true;
}

// 0x02166580, `cmp r0, #0`: r0 != 0 when B (or the pen's cancel) closes the
// list. On a page that goes up one page: the list closes as usual and
// Dq9CampOptionsExit has it opened again.
NDS_TITLE_HOOK void Dq9CampOptionsBack(uint32_t* r0, uint32_t* r8) {
    if (!g_camp.open || *r0 == 0u) return;
    g_camp.cursor[g_camp.section] = static_cast<uint32_t>(
        S16(bus_read_u16(*r8 + kCampMiscCursor)) & 0x7FFF);
    GoUp();
    g_camp.reopen = true;
}

// 0x0216659C, the end of the Misc. step: r8 = the camp menu.
NDS_TITLE_HOOK void Dq9CampOptionsExit(uint32_t* r8) {
    if (g_camp.keep_state) {
        g_camp.keep_state = false;
        bus_write_u32(*r8 + kCampState, kStateMisc);
        bus_write_u32(*r8 + kCampSubState, 1u);
    }
    if (g_camp.reopen) {
        g_camp.reopen = false;
        g_camp.reopening = true;
        bus_write_u32(*r8 + kCampState, kStateMisc);
        bus_write_u32(*r8 + kCampSubState, 0u);
    }
}
