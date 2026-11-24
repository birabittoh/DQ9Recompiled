// Mouse-friendly menus (Game > Mouse-friendly menus, [title] mouse_friendly
// in settings.toml): the menu entry under the mouse is highlighted without
// a click, and one click chooses it. On the DS the first tap highlights an
// entry and the second chooses it. Off leaves the game untouched.
//
// Each menu type has its own pen code, so each is hooked on its own. So far:
// the grid menus of overlay 23 (title, file select; see
// config/hooks/dq9_arm9_ov023.toml) and the window lists of the main ARM9
// (camp menu, priest...; "part 2" in config/hooks/dq9_arm9.toml, and their
// green look, part 3, in the overlay manifests) and the simple choice lists
// (Yes/No; "part 4" in config/hooks/dq9_arm9.toml), the layout menus (part
// 5), the name keyboard (part 7, dq9_arm9_ov003.toml) and the equipment page
// (part 8, dq9_arm9_ov005.toml).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "battle_multipliers.h"
#include "dq9_scene.h"
#include "title_hooks.h"
#include "title_settings.h"

namespace {

constexpr const char* kChoices[] = {"Off", "On"};

constexpr NdsTitleSetting kMouseFriendly = {
    "mouse_friendly",
    "Mouse-friendly menus",
    "Highlights the menu entry under the mouse and chooses it with one "
    "click. Off, the first click highlights and the second chooses, as "
    "with the stylus on the DS.",
    0, 1, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    0,
};

const int kMouseFriendlyId = nds_title_setting_register(&kMouseFriendly);

constexpr uint32_t kCpsrZ = 1u << 30;

bool Enabled() { return nds_title_setting_value(kMouseFriendlyId) != 0; }

// The mouse position when it has moved since the last call for the same
// menu (`key`) that returned one. A resting mouse leaves the cursor to the
// keypad. Each menu keeps its own last position, so two menus stepped in
// the same frame both see a move.
bool HoverMoved(uint32_t key, int* x, int* y) {
    struct Last { uint32_t key; int x, y; };
    static Last last[8] = {};
    static unsigned next = 0;
    if (!nds_title_pointer_hover(x, y)) {
        for (Last& l : last) l = {};
        return false;
    }
    Last* slot = nullptr;
    for (Last& l : last)
        if (l.key == key) slot = &l;
    if (slot == nullptr) {
        slot = &last[next++ % 8];
        *slot = {key, -1, -1};
    }
    if (*x == slot->x && *y == slot->y) return false;
    slot->x = *x;
    slot->y = *y;
    return true;
}

// The grid menu's pen step is running on the mouse position, not on a press.
// Set by Dq9MouseMenuGridPress, which every pass through that code runs
// first.
bool g_grid_hover = false;
int g_grid_hover_x = 0, g_grid_hover_y = 0;

// The same for the window lists: the widget whose pen step is running on
// the mouse position (0 when none), set by Dq9MouseMenuListPress.
uint32_t g_list_hover = 0;
int g_list_hover_x = 0, g_list_hover_y = 0;

// When a choice last ran its pen step (a simple choice list, or a window list
// such as the priest's): see Dq9ChoiceListActive.
std::chrono::steady_clock::time_point g_choice_seen{};
}  // namespace

// Before `cmp r1, #0` at 0x021F9384: r1 is the press edge. A moved mouse
// with no button held enters the pen code too.
NDS_TITLE_HOOK void Dq9MouseMenuGridPress(uint32_t* press) {
    g_grid_hover = false;
    if (!Enabled() || *press != 0u) return;
    if (!HoverMoved(0u, &g_grid_hover_x, &g_grid_hover_y)) return;
    g_grid_hover = true;
    *press = 1u;
}

// After the position getter, at 0x021F9398: x is at sp+0x34, y at sp+0x30.
NDS_TITLE_HOOK void Dq9MouseMenuGridPosition(uint32_t* sp) {
    if (!g_grid_hover) return;
    bus_write_u32(*sp + 0x34u, static_cast<uint32_t>(g_grid_hover_x));
    bus_write_u32(*sp + 0x30u, static_cast<uint32_t>(g_grid_hover_y));
}

// Before `movs r6, r0` at 0x021F93AC: r0 is the page arrow under the pen
// (-1, 0 or 1). The mouse turns no pages by passing over an arrow.
NDS_TITLE_HOOK void Dq9MouseMenuGridArrows(uint32_t* arrow) {
    if (g_grid_hover) *arrow = 0u;
}

// Before `b 0x021F9638` at 0x021F9608: the press has just selected the
// cell under the pen (it turned green). A click chooses it as well, through
// the flag at sp+0x28; the cell becomes the cursor on the way.
NDS_TITLE_HOOK void Dq9MouseMenuGridSelected(uint32_t* sp) {
    if (Enabled() && !g_grid_hover) bus_write_u32(*sp + 0x28u, 1u);
}

// Before `moveq r0, #1` at 0x021F961C (plain cells, and selected ones): Z
// is set when the cell under the pen is the cursor, and the press then
// chooses it. A click chooses
// any cell; the mouse passing over chooses none.
NDS_TITLE_HOOK void Dq9MouseMenuGridChoose(uint32_t* cpsr) {
    if (!Enabled()) return;
    if (g_grid_hover)
        *cpsr &= ~kCpsrZ;
    else
        *cpsr |= kCpsrZ;
}

// Window lists (camp menu...): see "part 2" in config/hooks/dq9_arm9.toml.

// Before `cmp r0, #0` at 0x0205BC70 (in 0x0205BC3C): r0 is the press edge,
// r4 the widget. A moved mouse with no button held enters the pen code too.
// The list's step (0x0205C5C0) runs this every frame the list takes input.
NDS_TITLE_HOOK void Dq9MouseMenuListPress(uint32_t* press, uint32_t* widget) {
    g_list_hover = 0;
    g_choice_seen = std::chrono::steady_clock::now();
    if (!Enabled() || *press != 0u) return;
    if (!HoverMoved(*widget, &g_list_hover_x, &g_list_hover_y)) return;
    g_list_hover = *widget;
    *press = 1u;
}

// At 0x0205BC8C, before the position is loaded: x is at sp+4, y at sp+0.
NDS_TITLE_HOOK void Dq9MouseMenuListPosition(uint32_t* sp) {
    if (g_list_hover == 0) return;
    bus_write_u32(*sp + 4u, static_cast<uint32_t>(g_list_hover_x));
    bus_write_u32(*sp + 0u, static_cast<uint32_t>(g_list_hover_y));
}

// Before `movne r0, #0` at 0x0205BCC4: Z is set when the entry under the pen
// is the one touched before, and only then may the press choose it. A click
// may always choose; the scene still wants it on the cursor entry, and the
// mouse has put the cursor there.
NDS_TITLE_HOOK void Dq9MouseMenuListSame(uint32_t* cpsr) {
    if (Enabled() && g_list_hover == 0) *cpsr |= kCpsrZ;
}

// Before the `beq` that skips a list's green look (part 3, see
// config/hooks/dq9_arm9_ov002.toml): Z is set when the pen is not down. The
// scene only gets here when the list's pen step moved the cursor, and with
// no button held only the mouse hover can have done that, so the entry it
// moved to turns green as it does under the pen.
NDS_TITLE_HOOK void Dq9MouseMenuListGreen(uint32_t* cpsr) {
    if (!Enabled()) return;
    int x = 0, y = 0;
    if (nds_title_pointer_hover(&x, &y)) *cpsr &= ~kCpsrZ;
}

// Before `b 0x0205C6C4` at 0x0205C65C: the pass on the mouse position has
// moved the cursor. Clear the press state it left (w+0x4C..0x4E) so the next
// real press is handled as a fresh one; w+0x30 keeps the entry, which makes
// it "touched before" for the click.
NDS_TITLE_HOOK void Dq9MouseMenuListDone(uint32_t* owner) {
    if (g_list_hover == 0 || g_list_hover != *owner + 4u) return;
    g_list_hover = 0;
    for (uint32_t off = 0x4C; off <= 0x4E; ++off)
        bus_write_u8(*owner + 4u + off, 0u);
}

// Simple choice lists (Yes/No...): see "part 4" in config/hooks/dq9_arm9.toml.

namespace {
// The list (m+0x10) whose hit-test is running on the mouse position (0 when
// none), set by Dq9MouseMenuChoicePress, and whether a real press is being
// handled with the setting on.
uint32_t g_choice_hover = 0;
bool g_choice_click = false;
int g_choice_hover_x = 0, g_choice_hover_y = 0;
}  // namespace

// True while a simple choice list (Yes/No...) or a window list (priest, camp
// menu...) takes input: their pen steps stamp g_choice_seen every frame. The
// message box under it waits in its own states, so a right click or B that
// advances the box (an advance flag, or an injected A) would answer the list
// with its first entry. Used by text_instant.cpp and Dq9MessageTopClick.
// The setting, for the battle's target choice (battle_targets.cpp), whose
// hover follows it too.
bool Dq9MouseFriendlyMenus() { return Enabled(); }

bool Dq9ChoiceListActive() {
    return std::chrono::steady_clock::now() - g_choice_seen <
           std::chrono::milliseconds(150);
}

// Before `cmp r1, #0` at 0x020E1E70 (in 0x020E1E5C): r1 is the press edge,
// r4 the list's rects. A moved mouse with no button held enters the pen
// code too.
NDS_TITLE_HOOK void Dq9MouseMenuChoicePress(uint32_t* press, uint32_t* rects) {
    g_choice_hover = 0;
    g_choice_click = false;
    g_choice_seen = std::chrono::steady_clock::now();
    if (!Enabled()) return;
    if (*press != 0u) {
        g_choice_click = true;
        return;
    }
    if (!HoverMoved(*rects, &g_choice_hover_x, &g_choice_hover_y)) return;
    g_choice_hover = *rects;
    *press = 1u;
}

// At 0x020E1E84, after the position is read: x is at sp+4, y at sp+0.
NDS_TITLE_HOOK void Dq9MouseMenuChoicePosition(uint32_t* sp) {
    if (g_choice_hover == 0) return;
    bus_write_u32(*sp + 4u, static_cast<uint32_t>(g_choice_hover_x));
    bus_write_u32(*sp + 0u, static_cast<uint32_t>(g_choice_hover_y));
}

// Before `cmp r2, r0` at 0x020E26CC: r2 is the cursor, r0 the entry under
// the pen, and only when they match may the press choose (the cursor). The
// hover pass must not choose, so it never matches; a click chooses the
// entry under it even when the cursor is elsewhere (the mouse may have
// rested there since the list opened).
NDS_TITLE_HOOK void Dq9MouseMenuChoiceSame(uint32_t* entry, uint32_t* cursor) {
    if (g_choice_hover != 0)
        *cursor = *entry + 1u;
    else if (g_choice_click)
        *cursor = *entry;
}

// Before `cmp r1, #0` at 0x020E26D8: r1 is m+0x17, "touched before". A
// click may choose without it.
NDS_TITLE_HOOK void Dq9MouseMenuChoiceTouched(uint32_t* touched) {
    if (g_choice_click) *touched = 1u;
}

// Layout menus (shop, Patty's party planner, Krak Pot...): see "part 5" in
// config/hooks/dq9_arm9_ov003.toml.

namespace {
// What the next run of the tap routine 0x02080DE4 is for, set by the scene's
// press hook: 0 = leave it alone, 1 = the mouse hover, 2 = a click.
int g_tap_mode = 0;
int g_tap_hover_x = 0, g_tap_hover_y = 0;
// The last press hook ran on the mouse hover (g_tap_mode is reset by the tap
// routine; the scene's code after it may need to know).
bool g_tap_was_hover = false;
// The Defeated Monster List: the hover moved its cursor, so the next frame
// redraws the other screen (Dq9MouseMenuRecordsRedraw).
bool g_records_redraw = false;
}  // namespace

// Before `cmp r1, #0` after the scene reads the press edge (r1). `sp` tells
// the scenes apart for HoverMoved. A moved mouse with no button held enters
// the pen code too.
// It runs every frame the menu takes input (Pavo's multiplayer choice, over a
// box waiting in state 3), so it marks a choice as up (Dq9ChoiceListActive).
NDS_TITLE_HOOK void Dq9MouseMenuTapPress(uint32_t* press, uint32_t* sp) {
    g_choice_seen = std::chrono::steady_clock::now();
    g_tap_mode = 0;
    g_tap_was_hover = false;
    if (!Enabled()) return;
    if (*press != 0u) {
        g_tap_mode = 2;
        return;
    }
    if (!HoverMoved(*sp ^ 0x80000000u, &g_tap_hover_x, &g_tap_hover_y))
        return;
    g_tap_mode = 1;
    g_tap_was_hover = true;
    *press = 1u;
}

// At the entry of 0x02080DE4: r2 / r3 are the pen position.
NDS_TITLE_HOOK void Dq9MouseMenuTapPosition(uint32_t* x, uint32_t* y) {
    if (g_tap_mode != 1) return;
    *x = static_cast<uint32_t>(g_tap_hover_x);
    *y = static_cast<uint32_t>(g_tap_hover_y);
}

// Before `tst r0, #4` at 0x02080E28: bit 2 of the item's flags means it is
// green already, and only then does the tap choose it. The hover never
// chooses; a click always does.
NDS_TITLE_HOOK void Dq9MouseMenuTapChoose(uint32_t* flags) {
    if (g_tap_mode == 1)
        *flags &= ~4u;
    else if (g_tap_mode == 2)
        *flags |= 4u;
    g_tap_mode = 0;
}

// The Defeated Monster List (battle records): see
// config/hooks/dq9_arm9_ov014.toml. At 0x02186FC4 the pen step has moved the
// cursor; on the hover, the next frame redraws the info as the keypad does.
NDS_TITLE_HOOK void Dq9MouseMenuRecordsMoved() {
    if (g_tap_was_hover) g_records_redraw = true;
}

// Before `cmps rN, #0` on s+0x160 (what the keypad step did this frame): non
// zero redraws the other screen (no page bits, 0x10 / 0x20, are added).
NDS_TITLE_HOOK void Dq9MouseMenuRecordsRedraw(uint32_t* flags) {
    if (!g_records_redraw) return;
    g_records_redraw = false;
    if (*flags == 0u) *flags = 1u;
}

// Character creation: see config/hooks/dq9_arm9_ov009.toml.
//
// Off for now: the hover applies the entry under the mouse at once (gender,
// looks), as the keypad cursor does, which may feel too eager, so the screen
// stays vanilla. Set kCharHover to true to bring it back.

namespace {
constexpr bool kCharHover = false;
bool g_char_hover = false;
constexpr uint32_t kCharPage = 0xC58u;  // u8, page of the menu
}  // namespace

// Before `cmp r1, #0` at 0x02188F24: r1 is the press edge, r4 the menu, and
// the pen position is already at sp+4 / sp+0. A moved mouse with no button
// held takes the press path on the mouse position.
NDS_TITLE_HOOK void Dq9MouseMenuCharPress(uint32_t* press, uint32_t* cc,
                                          uint32_t* sp) {
    g_char_hover = false;
    if (!kCharHover || !Enabled() || *press != 0u) return;
    if (bus_read_u8(*cc + kCharPage) == 8u) return;
    int x = 0, y = 0;
    if (!HoverMoved(*cc, &x, &y)) return;
    bus_write_u32(*sp + 4u, static_cast<uint32_t>(x));
    bus_write_u32(*sp + 0u, static_cast<uint32_t>(y));
    g_char_hover = true;
    *press = 1u;
}

// Before `add r0, r6, #3` at 0x02188F44: r6 is what the pen hit. The hover
// keeps entries (0..100) and drops the tabs and the buttons.
NDS_TITLE_HOOK void Dq9MouseMenuCharHit(uint32_t* hit) {
    if (!g_char_hover) return;
    g_char_hover = false;
    const int32_t h = static_cast<int32_t>(*hit);
    if (h < 0 || h > 100) *hit = static_cast<uint32_t>(-1);
}

// Name keyboard: see "part 7" in config/hooks/dq9_arm9_ov003.toml.

namespace {
bool g_key_hover = false;
int g_key_hover_x = 0, g_key_hover_y = 0;
}  // namespace

// Before `cmp r1, #0` at 0x0215F350: r1 is the press edge, r10 the keyboard.
// A moved mouse with no button held enters the key search too.
NDS_TITLE_HOOK void Dq9MouseMenuKeyPress(uint32_t* press, uint32_t* kb) {
    g_key_hover = false;
    if (!Enabled() || *press != 0u) return;
    if (!HoverMoved(*kb, &g_key_hover_x, &g_key_hover_y)) return;
    g_key_hover = true;
    *press = 1u;
}

// At 0x0215F364, after the pen position is read: x at sp+8, y at sp+0xC.
NDS_TITLE_HOOK void Dq9MouseMenuKeyPosition(uint32_t* sp) {
    if (!g_key_hover) return;
    bus_write_u32(*sp + 8u, static_cast<uint32_t>(g_key_hover_x));
    bus_write_u32(*sp + 12u, static_cast<uint32_t>(g_key_hover_y));
}

// Before `cmp r7, #0` at 0x0215F3F0: r7 is the key found (0 for none). On
// the hover it becomes the cursor (*kb) and r7 is cleared, so nothing is
// typed.
NDS_TITLE_HOOK void Dq9MouseMenuKeyFound(uint32_t* key, uint32_t* kb) {
    if (!g_key_hover) return;
    g_key_hover = false;
    if (*key != 0u) bus_write_u32(*kb, *key);
    *key = 0u;
}

// Equipment page: see "part 8" in config/hooks/dq9_arm9_ov005.toml.

namespace {
constexpr uint32_t kEquipHeld = 0x3D78u;   // s16, item being dragged, -1 none
constexpr uint32_t kEquipState = 0x3DB8u;  // u8: 2 tabs, 3 box, 4 grid
constexpr uint32_t kEquipCursor = 0x3DBBu; // u8, 8..23 grid cell
constexpr uint32_t kEquipDrag = 0x3DC0u;   // u8, 0xFF = no drag
constexpr uint8_t kEquipBox = 3, kEquipGrid = 4;

bool g_equip_hover = false;
int g_equip_hover_x = 0, g_equip_hover_y = 0;

// What the pen step does with a press at (x, y): the grid cell (8..23) or
// the box (0) it moves the cursor to, or -1. The grid's rects are those of
// 0x02155670, the box's those of 0x0215690C.
int EquipTarget(int x, int y) {
    for (int i = 0; i < 16; ++i) {
        const int cx = 141 + 26 * (i % 4), cy = 52 + 26 * (i / 4);
        if (x >= cx && x <= cx + 24 && y >= cy && y <= cy + 24) return i + 8;
    }
    if (x > 132 && x < 157 && y > 22 && y < 45) return 0;
    return -1;
}
}  // namespace

// Before `cmp rN, #0` after the page step reads the press edge: s is the
// page. When the mouse has moved onto a cell or the box that is not the
// cursor, take the pen step this frame (Dq9MouseMenuEquipPress makes it a
// press at the mouse position).
NDS_TITLE_HOOK void Dq9MouseMenuEquipStep(uint32_t* press, uint32_t* s) {
    g_equip_hover = false;
    if (!Enabled() || *press != 0u) return;
    int x = 0, y = 0;
    if (!HoverMoved(*s, &x, &y)) return;
    const uint8_t state = bus_read_u8(*s + kEquipState);
    if (state < 2 || state > kEquipGrid) return;
    if (bus_read_u8(*s + kEquipDrag) != 0xFFu) return;
    if (static_cast<int16_t>(bus_read_u16(*s + kEquipHeld)) != -1) return;
    const int target = EquipTarget(x, y);
    if (target < 0) return;
    if (target == 0 && state == kEquipBox) return;
    if (target >= 8 && state == kEquipGrid &&
        bus_read_u8(*s + kEquipCursor) == target)
        return;
    g_equip_hover = true;
    g_equip_hover_x = x;
    g_equip_hover_y = y;
    *press = 1u;
}

// Before `cmp r1, #0` at 0x021562EC (the pen step): r1 is the press edge,
// the pen position is at sp+4 / sp+0.
NDS_TITLE_HOOK void Dq9MouseMenuEquipPress(uint32_t* press, uint32_t* sp) {
    if (!g_equip_hover) return;
    *press = 1u;
    bus_write_u32(*sp + 4u, static_cast<uint32_t>(g_equip_hover_x));
    bus_write_u32(*sp + 0u, static_cast<uint32_t>(g_equip_hover_y));
}

// Before `beq` at 0x02156338: Z clear means the page is closing. The hover
// returns from the pen step instead of buzzing.
NDS_TITLE_HOOK bool Dq9MouseMenuEquipClosing(uint32_t* cpsr) {
    if (!g_equip_hover || (*cpsr & kCpsrZ) != 0u) return false;
    g_equip_hover = false;
    return true;
}

// Before `cmp r3, r1` at 0x02156828: r3 is the cell's item, -1 when empty.
NDS_TITLE_HOOK void Dq9MouseMenuEquipCell(uint32_t* item) {
    if (g_equip_hover) *item = 0u;
}

// At 0x02156880: the cursor is on the cell. The hover returns before the
// drag starts (the flag stays set for Dq9MouseMenuEquipSkipModel).
NDS_TITLE_HOOK bool Dq9MouseMenuEquipPicked(uint32_t* s) {
    if (!g_equip_hover) return false;
    bus_write_u16(*s + kEquipHeld, 0xFFFFu);
    return true;
}

// Before `bl 0x0215690C` at 0x021563E0: the hover skips the model hit-test.
NDS_TITLE_HOOK bool Dq9MouseMenuEquipSkipModel() { return g_equip_hover; }

// At 0x0215B3C8, after the category list's keypad step: when the mouse has
// moved onto a row (x > 128, 24 px rows from y 0, as the press loop in
// 0x0215B2BC), put the list widget's cursor there, as 0x0205BB14 does:
// w+20 page = n / (w+8 * w+12), then the rest split by the column count
// **(w+36) into **(w+40) row and **(w+44) column.
NDS_TITLE_HOOK void Dq9MouseMenuEquipCategory(uint32_t* s) {
    if (!Enabled()) return;
    int x = 0, y = 0;
    if (!HoverMoved(*s + 0x19F4u, &x, &y)) return;
    if (x <= 128 || y <= 0) return;
    const int n = y / 24;
    if (n >= 8 || y == 24 * n) return;  // the press loop wants 24n < y
    const uint32_t w = *s + 0x19F4u;
    if (n >= static_cast<int>(bus_read_u32(w))) return;
    const uint32_t per = bus_read_u32(w + 8u) * bus_read_u32(w + 12u);
    const uint32_t cols = bus_read_u32(bus_read_u32(w + 36u));
    if (per == 0u || cols == 0u) return;
    const uint32_t rest = static_cast<uint32_t>(n) % per;
    bus_write_u32(w + 20u, static_cast<uint32_t>(n) / per);
    bus_write_u32(bus_read_u32(w + 40u), rest / cols);
    bus_write_u32(bus_read_u32(w + 44u), rest % cols);
}

// The top screen (see config/hooks/dq9_arm9_ov017.toml).
//
// A click on one of the party's names in the row at the bottom of the top
// screen (the overworld) opens that member's equipment page, as choosing
// Equipment and that member in the camp menu does (a member the camp menu does
// not list there, see HasEquipmentPage, ignores the click): the camp menu calls the
// field's 0x021A120C(field, 3, slot), which sets the page scene's kind and
// member and queues it. The hook is on the entry of the field step
// (0x0218CBD4(field)) and jumps to that function with r1 = 3, r2 = slot, so
// the step is skipped for one frame and the call returns to its caller.

namespace {

constexpr uint32_t kCtx = 0x020F33D8u;          // main loop context
constexpr uint32_t kCtxPartyEntity = kCtx + 8u; // u32 per slot 0..3
constexpr uint32_t kCtxLeader = 14716u;         // u8, slot of the leader
constexpr uint32_t kFieldQueue = 14076u;        // scene queue, head at +0
constexpr uint32_t kMessageOwner = 0x020421BCu; // literal -> +28 = messages
constexpr uint32_t kEntityBusy = 620u;          // u8
constexpr uint32_t kEntityActing = 196u;        // u16, low 15 bits

// The row of names at the bottom of the top screen: one 64x64 tab per party
// member, at x = 64 * position (0x02027110 draws them), y 176 at rest (only
// its top rows on screen). Clicks on the names were at y 176 to 188; the
// row's hot zone starts a little higher.
constexpr int kRowTop = 172, kTabWidth = 64;

// Each tab's state, per party position i, at 0x020FDC60 + 20 * i, stepped
// every frame by 0x0201FA3C: +0 mode (1 down, 3 rising, 2 up, 4 lowering),
// +1 request (3 = rise, taken in mode 1; 4 = lower, taken in mode 2 when +2
// is 0), +2 hold flags, +4 u16 time it stays up (0 = until lowered), +16
// float y (176.0 down, 136.0 up). Raised, the tab shows the member's level,
// class, HP and MP with their bars: the game draws those sprites from this
// state. 0x0201FBBC raises a tab for good (request 3, time 0), as the camp
// menu and a hit in a battle do; the hover does the same.
constexpr uint32_t kTabState = 0x020FDC60u, kTabStateSize = 20u;
constexpr uint8_t kTabDown = 1, kTabUp = 2, kTabRising = 3, kTabLowering = 4;

// The raised tab under the mouse (x of its left edge, -1 if none, and the
// top of its sprite): a click on it opens the equipment page too.
int g_raised_x = -1, g_raised_top = 0;

// Why the field is not idle, as a bit mask (0 = idle): for the log.
unsigned FieldBusy(uint32_t field) {
    unsigned why = 0;
    if (field == 0) return 1;
    if (!dq9::GameModeRunning()) why |= 2;
    if (dq9::TitleScreen()) why |= 4;
    if (dq9::InBattle()) why |= 8;
    // A scene (camp menu, shop, battle...) is queued or running.
    // The field's own scene (kind 0, seen in the log; 1 is 0x021ACDF4) is at
    // the head when nothing else runs.
    const uint32_t head = bus_read_u32(field + kFieldQueue);
    if (head != 0 && bus_read_u8(head) > 1) why |= 16;
    const uint32_t leader = bus_read_u8(kCtx + kCtxLeader);
    const uint32_t player =
        leader < 4 ? bus_read_u32(kCtxPartyEntity + 4u * leader) : 0u;
    if (player == 0) {
        why |= 32;
    } else {
        if (bus_read_u8(player + kEntityBusy) != 0) why |= 64;
        if ((bus_read_u16(player + kEntityActing) & 0x7FFFu) != 0) why |= 128;
    }
    const uint32_t messages = bus_read_u32(bus_read_u32(kMessageOwner) + 28u);
    if (messages != 0 && bus_read_u32(messages + 2456u) != 0) why |= 256;
    return why;
}

// The names row's own list, on the top-screen object (field+14032, which the
// field step hands to 0x02022DD8): the character id shown at each position
// (u8s at +1880, count at +1884; 0x02027110 draws position i at x = 64 * i),
// and a mask of ids whose tab is not drawn (+1885). Alone it is the party
// order; in multiplayer the players' ids (0 = the host's hero), with this
// machine's own anywhere in it.
constexpr uint32_t kFieldTop = 14032u;
constexpr uint32_t kTopTabIds = 1880u, kTopTabCount = 1884u, kTopTabHidden = 1885u;

// The id at each position, -1 for a tab not drawn; returns the count.
int TabRow(uint32_t field, int ids[4]) {
    const uint32_t top = field != 0 ? bus_read_u32(field + kFieldTop) : 0u;
    if (top < 0x02000000u || top >= 0x02400000u) return 0;
    int count = bus_read_u8(top + kTopTabCount);
    if (count > 4) count = 4;
    const uint8_t hidden = bus_read_u8(top + kTopTabHidden);
    for (int i = 0; i < count; ++i) {
        const int id = bus_read_u8(top + kTopTabIds + static_cast<uint32_t>(i));
        ids[i] = id < 8 && (hidden & (1u << id)) != 0 ? -1 : id;
    }
    return count;
}

// This machine's player's id (0x020100B8): 0 alone or hosting, a guest's own
// index in someone else's world.
constexpr uint32_t kCtxOwnId = 940u;

// Whether the member with this character id has an equipment page. The camp
// menu (overlay 2, 0x02155458) lists for Equipment only this machine's party,
// the ids 0x020114A4 copies from ctx+22296 (count at ctx+22300; a host's
// guests are not in it), that 0x0200FE00(ctx, id) finds (the u32 at
// ctx+8+4*id, with bit 0x800 of its first u16) and whose u32 at +0x1C4 is 0.
bool HasEquipmentPage(int id) {
    if (id < 0 || id >= 0xE9) return false;
    int count = bus_read_u8(kCtx + 22300u);
    if (count > 4) count = 4;
    bool in_party = false;
    for (int i = 0; i < count; ++i)
        if (bus_read_u8(kCtx + 22296u + static_cast<uint32_t>(i)) == id) in_party = true;
    if (!in_party) return false;
    const uint32_t member = bus_read_u32(kCtxPartyEntity + 4u * static_cast<uint32_t>(id));
    if (member == 0 || (bus_read_u16(member) & 0x800u) == 0) return false;
    return bus_read_u32(member + 0x1C4u) == 0;
}

// Raises the tab under the mouse and lowers it once the mouse leaves, through
// the tab's own state, so it moves (and shows the status) as when the camp
// menu raises it. Only the tabs the hover raised are lowered by it; while a
// scene runs (the camp menu, a shop...) the tabs are left to the game.
void HoverTabs(unsigned busy, const int ids[4], int count) {
    static bool raised_by_hover[4] = {false, false, false, false};
    const bool blocked = (busy & (2u | 4u | 8u | 16u)) != 0;
    int px = 0, py = 0;
    const bool over = nds_title_top_pointer(&px, &py);
    g_raised_x = -1;
    for (int i = 0; i < 4; ++i) {
        const uint32_t state = kTabState + kTabStateSize * static_cast<uint32_t>(i);
        if (blocked || i >= count || ids[i] < 0) {
            raised_by_hover[i] = false;
            continue;
        }
        const uint8_t mode = bus_read_u8(state);
        const uint32_t ybits = bus_read_u32(state + 16u);
        float yf = 0.0f;
        std::memcpy(&yf, &ybits, sizeof yf);
        const int top = static_cast<int>(yf);
        // Stay hot over the whole raised sprite (y top..191), or it would
        // drop as it rises past the pointer.
        const int x = kTabWidth * i;
        const bool hot = over && px >= x && px < x + kTabWidth &&
                         py >= (top < kRowTop ? top : kRowTop);
        if (hot) {
            raised_by_hover[i] = true;
            if (mode == kTabDown || mode == kTabLowering) bus_write_u8(state + 1u, 3);
            bus_write_u16(state + 4u, 0);  // stays up while hovered
            if (mode == kTabUp || mode == kTabRising) {
                g_raised_x = x;
                g_raised_top = top;
            }
        } else if (raised_by_hover[i]) {
            if (mode == kTabUp || mode == kTabRising) bus_write_u8(state + 1u, 4);
            else if (mode == kTabDown) raised_by_hover[i] = false;
        }
    }
}

}  // namespace

// Entry of 0x0218CBD4 (the field step, r0 = field), every field frame: the
// hover on the names row, then a click on it, which jumps to 0x021A120C with
// r1 = 3 (the equipment page) and r2 = the member's character id. Only the
// members the camp menu would list open it (HasEquipmentPage): a host's own
// party, not the guests; a guest, whose party stays home, only its own tab.
NDS_TITLE_HOOK bool Dq9TopScreenClick(uint32_t* field, uint32_t* kind,
                                      uint32_t* slot) {
    const unsigned busy = FieldBusy(*field);
    int ids[4] = {-1, -1, -1, -1};
    const int count = TabRow(*field, ids);
    HoverTabs(busy, ids, count);
    static unsigned seen = 0;
    int x = 0, y = 0;
    const unsigned clicks = nds_title_top_click(&x, &y);
    if (clicks == seen) return false;
    seen = clicks;
    const bool idle = busy == 0;
    int hit = -1;
    const bool on_raised = g_raised_x >= 0 && x >= g_raised_x &&
                           x < g_raised_x + kTabWidth && y >= g_raised_top;
    if (idle && (y >= kRowTop || on_raised) && x >= 0 && x / kTabWidth < count)
        hit = x / kTabWidth;
    if (hit < 0 || ids[hit] < 0 || !HasEquipmentPage(ids[hit])) return false;
    if (dq9::VisitingAnotherWorld() &&
        static_cast<uint32_t>(ids[hit]) != bus_read_u32(kCtx + kCtxOwnId))
        return false;
    *kind = 3u;
    *slot = static_cast<uint32_t>(ids[hit]);
    return true;
}

// ---- Top-screen map: drag to scroll ------------------------------------------
// The field map on the top screen (mode 0 of the top-screen step 0x020235D8,
// drawn by 0x02024528(map)) follows the player: map+1452/+1456 is its centre
// in world units, moved only when the player walks out of a dead zone, and
// from it the step works out the centre in map pixels, stored at map+68/+72.
// Everything shown is placed from those two: the map pieces (8x8 tiles stamped
// into engine B's BG0 around it), the BG's fine scroll (0x02027448), the icons
// and the party markers (screen point (128, 96) is the centre). The hook, where
// the step stores them, adds an offset the mouse drags, as an image on a
// phone: it follows the pointer while the left button is held after a press
// on the map, keeps the release's speed for a moment, stops at the map's
// edges, and eases back to the game's own centre once the player walks.
namespace {

constexpr uint32_t kMapZoom = 56;              // fx32 world -> map scale
constexpr uint32_t kMapOriginX = 24, kMapOriginY = 28;  // s32, in tiles
constexpr uint32_t kMapPieces = 32, kMapPieceCount = 36;  // 36-byte entries
constexpr uint32_t kMapPlaced = 44, kMapPlacedCount = 48; // 16-byte entries
constexpr uint32_t kMapGrottoLayout = 108;     // non-zero: a 32x32 map
// 0x0201B5C0: map ids 0x9C41..0x9CA0 draw one 32x32-tile map at the origin
// (when map+108 is set; probably the grottos), the others draw the placed
// pieces.
constexpr int kGrottoFirst = 0x9C41, kGrottoLast = 0x9C41 + 95;
// Where the centre is on screen, and the map's visible rows (the name tabs
// cover it from y 176).
constexpr int kMapCentreX = 128, kMapCentreY = 96, kMapBottom = 176;

struct MapPan {
    float x = 0.0f, y = 0.0f;    // offset of the shown centre, map pixels
    float vx = 0.0f, vy = 0.0f;  // per frame, after a release
    bool dragging = false;       // a drag that started on the map
    bool ignored = false;        // a drag that started elsewhere
    int last_x = 0, last_y = 0;
    uint32_t key[6] = {};        // the map shown (a change resets the pan)
    uint32_t player_x = 0, player_z = 0;
    std::chrono::steady_clock::time_point seen;
};
MapPan g_pan;

// The map's extent in map pixels (the centre's space): the union of what
// 0x02024528 stamps. False if it draws nothing.
bool MapBounds(uint32_t map, bool grotto, int* l, int* t, int* r, int* b) {
    const int ox = static_cast<int>(bus_read_u32(map + kMapOriginX));
    const int oy = static_cast<int>(bus_read_u32(map + kMapOriginY));
    if (grotto) {
        if (bus_read_u32(map + kMapGrottoLayout) == 0) return false;
        *l = 8 * ox;
        *t = 8 * oy;
        *r = *l + 256;
        *b = *t + 256;
        return true;
    }
    bool any = false;
    const uint32_t placed = bus_read_u32(map + kMapPlaced);
    const int count = static_cast<int>(bus_read_u32(map + kMapPlacedCount));
    const int pieces = static_cast<int>(bus_read_u32(map + kMapPieceCount));
    for (int i = 0; i < count && i < 256; ++i) {
        const uint32_t e = placed + 16u * static_cast<uint32_t>(i);
        const int id = static_cast<int>(bus_read_u32(e + 4u));
        if (id < 0 || id >= pieces) continue;  // as 0x02027888
        const uint32_t piece =
            bus_read_u32(map + kMapPieces) + 36u * static_cast<uint32_t>(id);
        const int x0 = 8 * (ox + static_cast<int>(bus_read_u32(e + 8u)));
        const int y0 = 8 * (oy + static_cast<int>(bus_read_u32(e + 12u)));
        const int x1 = x0 + 8 * bus_read_u8(piece + 8u);
        const int y1 = y0 + 8 * bus_read_u8(piece + 9u);
        if (!any || x0 < *l) *l = x0;
        if (!any || y0 < *t) *t = y0;
        if (!any || x1 > *r) *r = x1;
        if (!any || y1 > *b) *b = y1;
        any = true;
    }
    return any;
}

// The shown centre may go as far as puts the map's edge at the screen's edge
// (or stays in the middle of a map smaller than the screen), and always
// where the game itself puts it.
float ClampAxis(float pan, int game, int lo_edge, int hi_edge, int before,
                int after, float* vel) {
    int lo = lo_edge + before, hi = hi_edge - after;
    if (lo > hi) lo = hi = (lo + hi) / 2;
    if (game < lo) lo = game;
    if (game > hi) hi = game;
    const float shown = static_cast<float>(game) + pan;
    if (shown < static_cast<float>(lo)) {
        *vel = 0.0f;
        return static_cast<float>(lo - game);
    }
    if (shown > static_cast<float>(hi)) {
        *vel = 0.0f;
        return static_cast<float>(hi - game);
    }
    return pan;
}

}  // namespace

// At 0x02024794 (str r5, [r4, #0x44]) in 0x02024528, the field map's step:
// r4 = map, r5 / r6 = the centre in map pixels, sp = the step's frame (sp+0x18
// the map id, sp+0x58 / sp+0x60 the player's world x / z).
NDS_TITLE_HOOK void Dq9TopMapPan(uint32_t* map_reg, uint32_t* cx, uint32_t* cy,
                                 uint32_t* sp) {
    const uint32_t map = *map_reg;
    const int game_x = static_cast<int>(*cx), game_y = static_cast<int>(*cy);
    const int id = static_cast<int>(bus_read_u32(*sp + 0x18u));
    const bool grotto = id >= kGrottoFirst && id <= kGrottoLast;
    MapPan& p = g_pan;

    // A new map, zoom or layout, or the map not shown for a while: start
    // again from the game's centre.
    const uint32_t key[6] = {bus_read_u32(map + kMapZoom),
                             bus_read_u32(map + kMapOriginX),
                             bus_read_u32(map + kMapOriginY),
                             bus_read_u32(map + kMapPlaced),
                             bus_read_u32(map + kMapPlacedCount),
                             static_cast<uint32_t>(id)};
    const auto now = std::chrono::steady_clock::now();
    if (std::memcmp(key, p.key, sizeof key) != 0 ||
        now - p.seen > std::chrono::milliseconds(500)) {
        std::memcpy(p.key, key, sizeof key);
        p.x = p.y = p.vx = p.vy = 0.0f;
    }
    p.seen = now;

    int dx = 0, dy = 0;
    if (nds_title_top_drag(&dx, &dy)) {
        if (!p.dragging && !p.ignored) {
            // Only a press on the map itself: not on the name tabs (the row,
            // or a raised tab, which a click opens).
            const bool on_tab = dy >= kRowTop ||
                                (g_raised_x >= 0 && dx >= g_raised_x &&
                                 dx < g_raised_x + kTabWidth &&
                                 dy >= g_raised_top);
            p.dragging = !on_tab && dx >= 0 && dx < 256 && dy >= 0;
            p.ignored = !p.dragging;
            p.last_x = dx;
            p.last_y = dy;
            p.vx = p.vy = 0.0f;
        }
        if (p.dragging) {
            // The map moves with the pointer: the centre the other way.
            const float mx = static_cast<float>(p.last_x - dx);
            const float my = static_cast<float>(p.last_y - dy);
            p.x += mx;
            p.y += my;
            p.vx = 0.5f * p.vx + 0.5f * mx;
            p.vy = 0.5f * p.vy + 0.5f * my;
            p.last_x = dx;
            p.last_y = dy;
        }
    } else {
        p.dragging = p.ignored = false;
    }

    // The player walking takes the map back to the game's centre.
    const uint32_t px = bus_read_u32(*sp + 0x58u), pz = bus_read_u32(*sp + 0x60u);
    const bool walking = px != p.player_x || pz != p.player_z;
    p.player_x = px;
    p.player_z = pz;
    if (!p.dragging) {
        if (walking) {
            p.vx = p.vy = 0.0f;
            p.x *= 0.85f;
            p.y *= 0.85f;
            if (std::fabs(p.x) < 0.5f) p.x = 0.0f;
            if (std::fabs(p.y) < 0.5f) p.y = 0.0f;
        } else {
            p.x += p.vx;
            p.y += p.vy;
            p.vx *= 0.9f;
            p.vy *= 0.9f;
            if (std::fabs(p.vx) < 0.1f) p.vx = 0.0f;
            if (std::fabs(p.vy) < 0.1f) p.vy = 0.0f;
        }
    }

    int l = 0, t = 0, r = 0, b = 0;
    if (!MapBounds(map, grotto, &l, &t, &r, &b)) {
        p.x = p.y = p.vx = p.vy = 0.0f;
        return;
    }
    p.x = ClampAxis(p.x, game_x, l, r, kMapCentreX, 256 - kMapCentreX, &p.vx);
    p.y = ClampAxis(p.y, game_y, t, b, kMapCentreY, kMapBottom - kMapCentreY,
                    &p.vy);
    *cx = static_cast<uint32_t>(game_x + static_cast<int>(std::lround(p.x)));
    *cy = static_cast<uint32_t>(game_y + static_cast<int>(std::lround(p.y)));
}

// ---- Top-screen click advances the message box --------------------------------
// The message manager's input check (0x020440DC) runs every frame while a box
// is up; it sets r5 = 1 when a key or a stylus tap asks to go on. A click on
// the top screen counts as that tap. The pad update hook (every frame) stamps
// the time of each new click (NoteTopClick), so one made while no box was up
// is stale after a quarter of a second and does not close the next box.
namespace {
using MsgClock = std::chrono::steady_clock;
unsigned g_msg_click_seen = 0;   // last click count noted
unsigned g_msg_click_used = 0;   // last click count that advanced a box
MsgClock::time_point g_msg_click_at;

void NoteTopClick() {
    int x = 0, y = 0;
    const unsigned clicks = nds_title_top_click(&x, &y);
    if (clicks != g_msg_click_seen) {
        g_msg_click_seen = clicks;
        g_msg_click_at = MsgClock::now();
    }
}
}  // namespace

// The church's power-OFF screen: a press there resets to the title
// (quit_to_title.cpp).
void Dq9PowerOffScreenInput(uint32_t keys);

// Before `strh r0, [r5]` at 0x0201232C (the pad update): r0 = keys held this
// frame. A fresh click on the top screen while a message box is up adds A for
// this one frame (the next frame's update drops it again).
NDS_TITLE_HOOK void Dq9MessageTopClick(uint32_t* held) {
    Dq9PowerOffScreenInput(*held);
    NoteTopClick();
    const uint32_t owner = bus_read_u32(kMessageOwner);
    const uint32_t messages = owner != 0 ? bus_read_u32(owner + 28u) : 0u;
    const bool box_up = messages != 0 && bus_read_u32(messages + 2456u) != 0;
    // A new right click advances a box that has finished printing, as in the
    // dialogues (Dq9TextAdvanceRightClick, text_instant.cpp, sets the
    // manager's advance flag, and Dq9TextInstant prints the text at once
    // while the button is held). The results box waits in manager state 3,
    // which that flag does not release: press A for it. Only in state 3, so
    // the other boxes do not advance twice.
    static bool right_was_down = false;
    const bool right = nds_title_right_mouse_held();
    // A new B press does the same as a new right click (holding B already
    // prints the text at once, Dq9TextInstant).
    static bool b_was_down = false;
    const bool b_down = (*held & 2u) != 0u;
    const bool new_right = right && !right_was_down;
    const bool right_click = new_right || (b_down && !b_was_down);
    right_was_down = right;
    b_was_down = b_down;
    // On a choice a right click is B, which cancels (the last / "No" entry),
    // as on the DS; it never advances the box underneath (the check below).
    if (new_right && Dq9ChoiceListActive()) *held |= 2u;  // B
    // State 3 holds while the text prints too, so a press during printing
    // would print and advance at once. The box has finished printing when
    // its print position (+120) has not moved for a few frames (slow text
    // moves less than a character per frame).
    static uint32_t last_pos = 0, last_box = 0;
    static int still = 0;
    const uint32_t pos = box_up ? bus_read_u32(messages + 120u) : 0u;
    if (messages != last_box || pos != last_pos) still = 0;
    else if (still < 1000) ++still;
    last_box = messages;
    last_pos = pos;
    // Not on the battle records pages: Stella's box there waits in state 3
    // too (once B, with Skip text, has printed her line at once), and the
    // page reads A / X (choose) before B (back), so the A would turn B into
    // a choice.
    if (box_up && right_click && !Dq9ChoiceListActive() &&
        bus_read_u32(messages + 2464u) == 3u && still >= 8 &&
        !dq9::RecordsPage())
        *held |= 1u;  // A
    if (g_msg_click_seen == g_msg_click_used) return;
    g_msg_click_used = g_msg_click_seen;
    if (MsgClock::now() - g_msg_click_at >= std::chrono::milliseconds(250))
        return;
    if (box_up) *held |= 1u;  // A
}
