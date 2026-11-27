// The main menu's Nintendo WFC row becomes an "Options" row, in the
// language the game shows its text in, and the WFC icon stays hidden.
// Choosing the row opens the runtime menu's settings as pages of the title
// menu itself: one page of categories, then one page of options per
// category. See config/hooks/dq9_arm9_ov004.toml for the guest code.
//
// The pages are made of the title scene's own widgets: entry cells drawn in
// its full-screen panel (212), picked through the main menu's grid, and the
// description of the row under the cursor in the main menu's panel (210),
// moved to the bottom of the screen. There is no Back row: B, or the pen on
// the description, goes up a page. The
// hook on the row's handler (0x0216D6A0) runs when a row is chosen, when B
// is pressed and when left or right is pressed (the grid sends all three
// to the handler), and calls the game's functions it needs one after the
// other: it returns true to jump to a `bx r12` with lr set to the handler,
// so each call comes back to the hook.
//
// The rows, their values and their translated labels come from
// options_model.h, which the camp menu's Options row shares.
//
// A page in parts (the keyboard's keys) uses the game's own paging: the
// page's panel draws the arrows and "n/N" at its bottom when it is marked
// paged, and the grid turns the part when the pen taps an arrow (and runs
// the handler); L and R, and left and right on a row without a value, turn
// it too.
//
// The main menu also gets a save row above Options: "Import save file"
// when there is no save, "Export save file" when there is one. It is one
// more entry cell, made by a call the hook on the rows' builder (0x0216B498)
// adds, and laid out with the others when the menu opens its panel. It
// runs the Options row's handler too, which asks the frontend for the file
// dialog (title_save.h); an imported save resets the console.
//
// The self-updater (title_update.h) adds two things the same way. When a
// newer release is out, an "Update to <version>" row goes below Options,
// made by the same builder hook and laid out with the others; choosing it
// asks the frontend to download and stage the update, and the handler then
// keeps the menu waiting (as the running handler, like an open page) until
// the frontend restarts into the new version, hands it to the package
// installer (Android) or reports a failure. And this build's version is
// shown in the bottom-left corner, in a small window of the pages' panel
// (212, closed while the main menu is up) opened by the builder hook too.
//
// Last, a "Quit game" row at the bottom, made and laid out the same way:
// choosing it closes the game through the frontend's normal exit (as the
// window's close button does, pending saves written first).
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dq9_scene.h"
#include "frontend.h"
#include "options_model.h"
#include "save_image.h"
#include "title_save.h"
#include "title_update.h"

namespace {

constexpr uint32_t kCpsrZ = 1u << 30;

// ---- Guest layout -------------------------------------------------------

// Widgets (ov23). The scene keeps them in a list at scene+0x118.
constexpr uint32_t kWidgetType = 0x04u;   // u16: 6 panel, 7 grid, 8 cell
constexpr uint32_t kWidgetId = 0x06u;     // u16
constexpr uint32_t kWidgetGroup = 0x08u;  // u16: the arena it lives in
constexpr uint32_t kWidgetFlags = 0x0Cu;  // u8, bit 3 = hidden
constexpr uint32_t kWidgetNext = 0x18u;
constexpr uint32_t kTypePanel = 6u;
constexpr uint32_t kTypeGrid = 7u;
constexpr uint32_t kTypeCell = 8u;
constexpr uint8_t kHidden = 0x08u;

// Entry cell fields (set up by 0x021F89F4).
constexpr uint32_t kCellText = 0x20u;    // string pointer, 0 = use the text id
constexpr uint32_t kCellLayer = 0x34u;   // u16: the panel it is drawn in
constexpr uint32_t kCellTextId = 0x38u;  // u16
constexpr uint32_t kCellX = 0x3Au;       // s16, pixels inside the panel
constexpr uint32_t kCellY = 0x3Cu;       // s16
constexpr uint32_t kCellAction = 0x48u;  // u16: handler run when chosen

// Grid fields (step: 0x021F8E1C).
constexpr uint32_t kGridCells = 0x20u;   // u16 cell ids, [row * cols + col]
constexpr uint32_t kGridRows = 0x24u;    // u16
constexpr uint32_t kGridCols = 0x26u;    // u16
constexpr uint32_t kGridRow = 0x28u;     // u16 cursor
constexpr uint32_t kGridCol = 0x2Au;     // u16
constexpr uint32_t kGridRedraw = 0x38u;  // u32: move the cursor sprite
constexpr uint32_t kGridHandlers = 0x3Cu;  // u16s up to 0x5C: labels/handlers
constexpr uint32_t kGridHandlersEnd = 0x5Cu;
constexpr uint32_t kGridOnB = 0x46u;      // handler on B
constexpr uint32_t kGridOnPart = 0x48u;   // handler run when the pen turns
constexpr uint32_t kGridOnRight = 0x54u;  // handler on right
constexpr uint32_t kGridOnLeft = 0x56u;   // handler on left
constexpr uint32_t kGridOnL = 0x58u;      // handler on L
constexpr uint32_t kGridOnR = 0x5Au;      // handler on R
constexpr uint32_t kGridPart = 0x5Cu;     // s16: the part shown
constexpr uint32_t kGridParts = 0x5Eu;    // s16: how many (the pen wraps)
constexpr uint32_t kGridSavedBegin = 0x20u;
constexpr uint32_t kGridSavedEnd = 0x64u;

// Scene (ov11).
constexpr uint32_t kSceneWidgets = 0x118u;
constexpr uint32_t kSceneLabel = 0x10Cu;       // script label to jump to
constexpr uint32_t kSceneActiveGrid = 0x1B0u;  // u16
constexpr uint32_t kSceneHandlers = 0x1B4u;    // table of handlers
// u16: the handler the scene step (0x02184A84) calls every frame while no
// handler is running (one that returned non-zero is, until it returns 0).
constexpr uint32_t kSceneFrameHandler = 0x1BEu;
// lr of the handler when the scene step calls it as the running handler.
constexpr uint32_t kRunningHandlerReturn = 0x02184AA8u;
// The title's idle time (0x021848A8 adds to it every frame without input
// and jumps to label 999, back to the logos, past 0xE10).
constexpr uint32_t kSceneIdleTime = 0x1D0u;
constexpr uint32_t kSceneCursorDirty = 0x1C8u;

// Panel: its window object is at +0x20; +197 bit 0 of that is "open".
constexpr uint32_t kPanelWindowOpen = 0x20u + 197u;
// A paged panel (flag bit 2) gets, when 0x021F7EB8 opens it with more than
// one page, the arrows and "page + 1 / pages" at its bottom.
constexpr uint8_t kPanelPaged = 0x04u;
constexpr uint32_t kPanelPage = 0x104u;   // s16
constexpr uint32_t kPanelPages = 0x106u;  // s16

constexpr uint32_t kMainMenuLayer = 210u;
constexpr uint32_t kFileInfoPanel = 211u;
constexpr uint32_t kPagePanel = 212u;  // up to 32 x 24 tiles (opening.stb)
constexpr uint32_t kWfcTextId = 60u;
constexpr uint32_t kCellGroup = 1u;      // the main menu rows' arena (2 KB)
constexpr uint32_t kFileInfoGroup = 2u;
// opening.stb's way back to the main menu from a page on panel 212: it
// resets groups 1 and 2 and runs the main menu's opener (handler 2).
constexpr uint32_t kMainMenuLabel = 301u;
constexpr uint32_t kOptionsHandler = 10u;

// The pad (0x02114E30): +0 held, +2 held on the previous frame.
constexpr uint32_t kPadHeld = 0x02114E30u;
constexpr uint32_t kPadPrevious = 0x02114E32u;
constexpr uint32_t kKeyA = 0x001u, kKeyB = 0x002u;
constexpr uint32_t kKeyRight = 0x010u, kKeyLeft = 0x020u;
constexpr uint32_t kKeyR = 0x100u, kKeyL = 0x200u;

// The main menu (ov4 bss): +3 the mode that picks its rows (1 and 5: a
// save, rows 1; 2: no save, rows 2).
constexpr uint32_t kMenuMode = 0x02171034u + 3u;
constexpr uint32_t kTextRowsId = 2u;  // any text id but the WFC row's
constexpr uint32_t kMainCells[] = {50u, 51u, 52u};

// Guest functions.
constexpr uint32_t kFnHandler = 0x0216D6A0u;      // the hooked handler
constexpr uint32_t kFnRows = 0x0216B498u;         // ov4 (scene, rows, icon)
constexpr uint32_t kFnNewCell = 0x0216AFB0u;      // ov4
constexpr uint32_t kFnPanelClose = 0x021F8120u;   // ov23 (panel)
constexpr uint32_t kFnPanelOpen = 0x021F7EB8u;    // ov23
constexpr uint32_t kFnRemoveGroup = 0x021F6844u;  // ov23 (widgets, group)
constexpr uint32_t kFnFindGroup = 0x021842C8u;    // ov11 (scene, group)
constexpr uint32_t kFnHeapReset = 0x02032698u;    // main (heap)

// Free guest memory: the end of overlay slot 0 (0x021536E0 .. 0x021842A0),
// past overlay 4's image and bss (they end at 0x02171060), so unused while
// overlay 4 runs the menu. Nothing in main or the title's overlays points
// into 0x02171060 .. 0x021842A0.
constexpr uint32_t kRowText = 0x02183E00u;  // kMaxRows strings
constexpr uint32_t kRowTextSize = 0x60u;
constexpr uint32_t kGridArray = 0x02184100u;  // kMaxRows u16
constexpr uint32_t kTextBuffer = 0x02184200u;  // the Options row's string
constexpr uint32_t kTextBufferSize = 0xA0u;

constexpr uint32_t kDescText = 0x02183CE0u;  // kDescLines strings
constexpr uint32_t kSaveRowText = 0x02184120u;  // the save row's string
constexpr uint32_t kSaveRowTextSize = 0x60u;
constexpr uint32_t kMainGridArray = 0x02184180u;  // up to 6 u16

constexpr uint32_t kMaxRows = 8u;
constexpr uint32_t kFirstCell = 110u;  // ids 110..120: unused by opening.stb
// The description: the main menu's panel (210, up to 30 x 11 tiles), reopened
// at the bottom of the screen, with one cell per line.
constexpr uint32_t kDescLines = 3u;
constexpr uint32_t kFirstDescCell = kFirstCell + kMaxRows;
constexpr uint32_t kDescPanel = kMainMenuLayer;
constexpr uint32_t kDescHeight = 6u;  // tiles
constexpr uint32_t kDescTop = 24u - 1u - kDescHeight;
constexpr size_t kDescLineChars = 36;     // the font is proportional
constexpr uint32_t kDescCellTextId = 2u;  // any id but the WFC row's
constexpr int kRowX = 12, kRowY = 6, kRowPitch = 13;
constexpr uint32_t kPageWidth = 30u;  // tiles
constexpr uint32_t kPageVram = 96u;   // as the game opens 212 and 210
// The save row's cell: past the pages' (110..120), in the rows' group.
constexpr uint32_t kSaveCell = kFirstDescCell + kDescLines;

// The update row's cell, after the save row's, and its string (free memory
// above, below the descriptions' strings).
constexpr uint32_t kUpdateCell = kSaveCell + 1u;
constexpr uint32_t kUpdateRowText = 0x02183C80u;
constexpr uint32_t kUpdateRowTextSize = 0x60u;

// The version line: one cell in the pages' panel (212), which the builder
// hook opens as a small window in the bottom-left corner while the main
// menu is up. The save summary (211) starts at tile column 6 from row 12
// down to the bottom (0x0216C6FC opens it at x 6, width 26), so the window
// stays in columns 0..5. Its tiles go at the top of the room the game gives
// panel 212 (up to 32 x 24 tiles from 96), past what the main menu (210) and
// the summary (211) use.
constexpr uint32_t kVersionCell = kUpdateCell + 1u;
constexpr uint32_t kVersionText = 0x02183C40u;
constexpr uint32_t kVersionTextSize = 0x40u;
constexpr uint32_t kVersionX = 0u, kVersionY = 21u;  // tiles
constexpr uint32_t kVersionWidth = 6u, kVersionHeight = 3u;
constexpr uint32_t kVersionVram =
    kPageVram + 32u * 24u * 32u - kVersionWidth * kVersionHeight * 32u;
constexpr int kVersionCellX = 4, kVersionCellY = 6;  // pixels in the window
// The scene whose panel 212 holds the version line's window (u32, free
// memory past the main menu's grid array), for the script hook that closes
// it with the summary.
constexpr uint32_t kVersionScene = 0x021841C0u;
// Where the script command 0x02185504 (ov11) returns from closing a panel.
constexpr uint32_t kVersionCloseReturn = 0x02185544u;

// The Yes/No window of the delete prompt starts at tile row 9, column 26:
// the main menu's panel (from column 1) stays 25 tiles wide when it is tall
// enough to reach it.
constexpr uint32_t kYesNoTop = 9u;
constexpr uint32_t kNarrowWidth = 25u;

// The quit row's cell, after the version line's, and its string (free
// memory below the version line's).
constexpr uint32_t kQuitCell = kVersionCell + 1u;
constexpr uint32_t kQuitRowText = 0x02183BE0u;
constexpr uint32_t kQuitRowTextSize = 0x60u;

// ---- Calling guest code from the hook -----------------------------------

struct GuestCall {
    uint32_t target = 0;  // 0: only run `then`
    std::vector<uint32_t> args;
    std::function<void(uint32_t r0)> then;
};

struct Driver {
    bool active = false;
    bool waiting = false;
    uint32_t saved_lr = 0, saved_sp = 0, call_sp = 0;
    uint32_t result = 0;  // what the handler returns
    std::deque<GuestCall> queue;
    std::function<void(uint32_t)> then;
};

Driver g_driver;

void Call(uint32_t target, std::vector<uint32_t> args,
          std::function<void(uint32_t)> then = nullptr) {
    g_driver.queue.push_back({target, std::move(args), std::move(then)});
}

void Then(std::function<void(uint32_t)> fn) {
    g_driver.queue.push_back({0u, {}, std::move(fn)});
}

// ---- The pages ----------------------------------------------------------

using namespace dq9::options;

struct Menu : Model {
    bool open = false;
    bool closing = false;  // Back chosen, waiting for the title's text
    uint32_t scene = 0;
    uint32_t grid = 0;
    uint32_t described = ~0u;  // the row the description is of
    uint32_t polled = 0;       // frames, to look for changed rows now and then
    uint32_t height = 0;       // tiles: the page's panel as last opened
    uint8_t saved_grid[kGridSavedEnd - kGridSavedBegin] = {};
};

Menu g_menu;

uint32_t FindWidget(uint32_t scene, uint32_t id) {
    for (uint32_t w = bus_read_u32(scene + kSceneWidgets); w;
         w = bus_read_u32(w + kWidgetNext)) {
        if (bus_read_u16(w + kWidgetId) == id) return w;
    }
    return 0;
}

uint32_t FindTyped(uint32_t scene, uint32_t id, uint32_t type) {
    const uint32_t w = FindWidget(scene, id);
    return w && bus_read_u16(w + kWidgetType) == type ? w : 0;
}

uint32_t PageHeight(uint32_t rows) {
    return (kRowY + kRowPitch * (rows ? rows : 1) + 9 + 7) / 8;
}

// Paging off on the page's panel, which the version line uses too.
void ClearPaging(uint32_t scene) {
    if (const uint32_t panel = FindTyped(scene, kPagePanel, kTypePanel)) {
        bus_write_u8(panel + kWidgetFlags,
                     bus_read_u8(panel + kWidgetFlags) & ~kPanelPaged);
        bus_write_u16(panel + kPanelPage, 0u);
        bus_write_u16(panel + kPanelPages, 0u);
    }
}

// Writes the description of the row under the cursor into its cells and
// reopens its panel, which draws them. Its tiles follow the page's.
void ShowDescription() {
    const uint32_t scene = g_menu.scene;
    const uint32_t index = bus_read_u16(g_menu.grid + kGridRow);
    g_menu.described = index;
    std::vector<std::string> lines;
    if (index < g_menu.rows.size())
        lines = Wrap(DescriptionText(g_menu, g_menu.rows[index]), kDescLineChars,
                     kDescLines);
    for (uint32_t i = 0; i < kDescLines; ++i) {
        const uint32_t cell = FindTyped(scene, kFirstDescCell + i, kTypeCell);
        if (!cell) continue;
        const uint32_t text = kDescText + i * kRowTextSize;
        WriteString(text, kRowTextSize, i < lines.size() ? lines[i] : "");
        bus_write_u32(cell + kCellText, text);
        const uint8_t flags = bus_read_u8(cell + kWidgetFlags);
        bus_write_u8(cell + kWidgetFlags,
                     i < lines.size() ? flags & ~kHidden : flags | kHidden);
    }
    const uint32_t panel = FindTyped(scene, kDescPanel, kTypePanel);
    if (!panel) return;
    Call(kFnPanelClose, {panel});
    Call(kFnPanelOpen,
         {panel, scene, kPageVram + kPageWidth * g_menu.height * 32u, 1u,
          kDescTop, kPageWidth, kDescHeight, 0u, 1u, 0u});
}

// Lays the page's rows out in the cells and the grid, then reopens the
// panel at the page's height, which draws them.
void ShowPage() {
    const uint32_t scene = g_menu.scene, grid = g_menu.grid;
    BuildRows(g_menu, kMaxRows, false);
    g_menu.signature = Signature(g_menu);
    uint32_t count = 0;
    for (uint32_t i = 0; i < kMaxRows; ++i) {
        const uint32_t cell = FindTyped(scene, kFirstCell + i, kTypeCell);
        if (!cell) continue;  // the arena was full: fewer rows
        if (count < g_menu.rows.size() && count == i) {
            const uint32_t text = kRowText + i * kRowTextSize;
            WriteString(text, kRowTextSize, RowText(g_menu, g_menu.rows[i]));
            bus_write_u32(cell + kCellText, text);
            bus_write_u16(cell + kCellX, kRowX);
            bus_write_u16(cell + kCellY,
                          static_cast<uint16_t>(kRowY + kRowPitch * i));
            bus_write_u16(cell + kCellAction, kOptionsHandler);
            bus_write_u8(cell + kWidgetFlags,
                         bus_read_u8(cell + kWidgetFlags) & ~kHidden);
            bus_write_u16(kGridArray + 2u * i, kFirstCell + i);
            ++count;
        } else {
            bus_write_u8(cell + kWidgetFlags,
                         bus_read_u8(cell + kWidgetFlags) | kHidden);
        }
    }
    if (count < g_menu.rows.size()) g_menu.rows.resize(count);
    for (uint32_t i = count; i < kMaxRows; ++i)
        bus_write_u16(kGridArray + 2u * i, 0u);

    uint32_t& cursor = g_menu.cursor[g_menu.section];
    if (cursor >= count) cursor = count ? count - 1 : 0;
    bus_write_u32(grid + kGridCells, kGridArray);
    bus_write_u16(grid + kGridRows, static_cast<uint16_t>(count ? count : 1));
    bus_write_u16(grid + kGridCols, 1u);
    bus_write_u16(grid + kGridRow, static_cast<uint16_t>(cursor));
    bus_write_u16(grid + kGridCol, 0u);
    for (uint32_t off = kGridHandlers; off < kGridHandlersEnd; off += 2)
        bus_write_u16(grid + off, 0u);
    bus_write_u16(grid + kGridOnB, kOptionsHandler);
    bus_write_u16(grid + kGridOnLeft, kOptionsHandler);
    bus_write_u16(grid + kGridOnRight, kOptionsHandler);
    // In parts: the pen on the arrows and L and R turn them.
    const bool paged = g_menu.parts > 1;
    const uint32_t part = g_menu.part[g_menu.section];
    bus_write_u16(grid + kGridPart, static_cast<uint16_t>(paged ? part : 0u));
    bus_write_u16(grid + kGridParts,
                  static_cast<uint16_t>(paged ? g_menu.parts : 0u));
    if (paged) {
        bus_write_u16(grid + kGridOnPart, kOptionsHandler);
        bus_write_u16(grid + kGridOnL, kOptionsHandler);
        bus_write_u16(grid + kGridOnR, kOptionsHandler);
    }
    bus_write_u32(grid + kGridRedraw, 1u);
    bus_write_u32(scene + kSceneCursorDirty, 1u);

    const uint32_t panel = FindTyped(scene, kPagePanel, kTypePanel);
    if (!panel) return;
    // Every part as tall as the tallest, with a tile more for the arrows
    // and "n/N" under the last row.
    g_menu.height = paged ? PageHeight(g_menu.tallest) + 1u : PageHeight(count);
    if (paged) {
        bus_write_u8(panel + kWidgetFlags,
                     bus_read_u8(panel + kWidgetFlags) | kPanelPaged);
        bus_write_u16(panel + kPanelPage, static_cast<uint16_t>(part));
        bus_write_u16(panel + kPanelPages, static_cast<uint16_t>(g_menu.parts));
    } else {
        ClearPaging(scene);
    }
    Call(kFnPanelClose, {panel});
    Call(kFnPanelOpen, {panel, scene, kPageVram, 1u, 1u, kPageWidth,
                        g_menu.height, 0u, 1u, 0u});
    Then([scene](uint32_t) {
        ShowDescription();
        bus_write_u32(scene + kSceneCursorDirty, 1u);
    });
}

bool PageShown(uint32_t scene) {
    const uint32_t panel = FindTyped(scene, kPagePanel, kTypePanel);
    return panel && (bus_read_u8(panel + kPanelWindowOpen) & 1u) &&
           FindTyped(scene, kFirstCell, kTypeCell) &&
           g_menu.grid && g_menu.grid == FindTyped(
               scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid) &&
           bus_read_u32(g_menu.grid + kGridCells) == kGridArray;
}

void RestoreGrid() {
    const uint32_t grid = g_menu.grid;
    if (!grid || bus_read_u32(grid + kGridCells) != kGridArray) return;
    for (uint32_t i = 0; i < sizeof(g_menu.saved_grid); ++i)
        bus_write_u8(grid + kGridSavedBegin + i, g_menu.saved_grid[i]);
}

// The page is gone: the grid goes back to the main menu's and the panel
// stops paging.
void Forget() {
    RestoreGrid();
    if (g_menu.scene) ClearPaging(g_menu.scene);
    g_menu.open = false;
    g_menu.closing = false;
}

void Open(uint32_t scene) {
    const uint32_t grid = FindTyped(
        scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid);
    if (!grid || !FindTyped(scene, kPagePanel, kTypePanel)) return;
    g_menu.open = true;
    g_menu.scene = scene;
    g_menu.grid = grid;
    g_menu.section.clear();
    g_menu.pending.clear();
    for (uint32_t i = 0; i < sizeof(g_menu.saved_grid); ++i)
        g_menu.saved_grid[i] = bus_read_u8(grid + kGridSavedBegin + i);

    // The main menu and the save file's summary make room for the page.
    for (const uint32_t id : {kMainMenuLayer, kFileInfoPanel}) {
        if (const uint32_t panel = FindTyped(scene, id, kTypePanel))
            Call(kFnPanelClose, {panel});
    }
    // Hidden: the summary's cells and icons, and the main menu's own rows,
    // which the description's panel would draw (it is theirs).
    for (uint32_t w = bus_read_u32(scene + kSceneWidgets); w;
         w = bus_read_u32(w + kWidgetNext)) {
        const uint32_t id = bus_read_u16(w + kWidgetId);
        const bool main_row =
            bus_read_u16(w + kWidgetType) == kTypeCell &&
            bus_read_u16(w + kCellLayer) == kMainMenuLayer &&
            (id < kFirstCell || id == kSaveCell || id == kUpdateCell ||
             id == kQuitCell);
        // The version line is drawn in the page's own panel.
        const bool version = id == kVersionCell &&
            bus_read_u16(w + kWidgetType) == kTypeCell;
        if (bus_read_u16(w + kWidgetGroup) == kFileInfoGroup || main_row ||
            version)
            bus_write_u8(w + kWidgetFlags,
                         bus_read_u8(w + kWidgetFlags) | kHidden);
    }
    for (uint32_t i = 0; i < kDescLines; ++i) {
        if (FindWidget(scene, kFirstDescCell + i)) continue;
        Call(kFnNewCell,
             {scene, kFirstDescCell + i, kCellGroup, kDescPanel,
              kDescCellTextId, static_cast<uint32_t>(kRowX),
              static_cast<uint32_t>(kRowY + kRowPitch * i), 10u, 1u});
    }
    for (uint32_t i = 0; i < kMaxRows; ++i) {
        if (FindWidget(scene, kFirstCell + i)) continue;
        Call(kFnNewCell,
             {scene, kFirstCell + i, kCellGroup, kPagePanel, kWfcTextId,
              static_cast<uint32_t>(kRowX),
              static_cast<uint32_t>(kRowY + kRowPitch * i), 10u, 1u});
    }
    Then([](uint32_t) { ShowPage(); });
}

void Close();

// Back to the main menu, once the menu's text is in the language chosen on
// the pages (language.cpp reloads it in a few frames): until then the page
// stays up and takes no input, and Frame closes it.
void RequestClose() {
    if (dq9::TitleTextPending()) {
        g_menu.closing = true;
        return;
    }
    Close();
}

// Back to the main menu: what opening.stb's command 44 does for the rows'
// and the summary's groups (remove their widgets, reset their arenas), then
// its own label that rebuilds the main menu.
void Close() {
    const uint32_t scene = g_menu.scene;
    Forget();
    for (const uint32_t id : {kPagePanel, kDescPanel}) {
        if (const uint32_t panel = FindTyped(scene, id, kTypePanel))
            Call(kFnPanelClose, {panel});
    }
    for (const uint32_t group : {kCellGroup, kFileInfoGroup}) {
        Call(kFnRemoveGroup, {scene + kSceneWidgets, group});
        Call(kFnFindGroup, {scene, group}, [](uint32_t node) {
            if (node) g_driver.queue.push_front({kFnHeapReset, {node + 4u}, nullptr});
        });
    }
    Then([scene](uint32_t) {
        bus_write_u32(scene + kSceneLabel, kMainMenuLabel);
    });
}

// ---- The update row -----------------------------------------------------

// While an update the update row asked for is on its way, the handler stays
// the scene's running handler, which keeps the menu from taking input: the
// frontend shows the download's progress, then restarts into the new
// version (the game's process ends) or, on Android, hands it to the
// package installer, and a failure is shown as a notice. Either of the
// last two gives the menu back.
struct UpdateWait {
    bool waiting = false;
    bool busy_seen = false;  // the frontend took the request
    uint32_t frames = 0;
};

UpdateWait g_update;

// Frames to wait for the frontend to take the request (it does on its next
// host frame) before giving up, e.g. in a run with no frontend.
constexpr uint32_t kUpdateStartFrames = 300;

// The row's version, when a newer release is out and the update row has
// something to offer.
std::string UpdateRowVersion() {
    const NdsTitleUpdateStatus status = nds_title_update_status();
    switch (status.state) {
    case NdsTitleUpdateState::Available:
    case NdsTitleUpdateState::Downloading:
    case NdsTitleUpdateState::Failed:
        return status.available_version;
    default:
        return {};
    }
}

// "Update to <version>", in the language the game shows its text in.
std::string UpdateRowText(const std::string& version) {
    std::string text = dq9::Tr("main_menu", "update",
                               dq9::TextLanguage());
    const size_t at = text.find("{}");
    if (at != std::string::npos) text.replace(at, 2, version);
    return text;
}

// The frontend's notice while the update downloads, in the game's language.
// The notice font is plain ASCII, so an accented letter ("<'e>" in the
// game's text) is shown without its accent.
std::string DownloadNoticeText() {
    const std::string text = dq9::Tr("main_menu", "downloading",
                                     bus_read_u8(dq9::kGameTextLanguage));
    std::string plain;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<' && i + 3 < text.size() && text[i + 3] == '>') {
            plain += text[i + 2];
            i += 3;
        } else {
            plain += text[i];
        }
    }
    return plain;
}

// The main menu's update row was chosen: ask for the update and wait.
bool UpdateRowChosen(uint32_t scene) {
    const uint32_t grid = FindTyped(
        scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid);
    if (!grid || bus_read_u32(grid + kGridCells) != kMainGridArray)
        return false;
    const uint32_t row = bus_read_u16(grid + kGridRow);
    if (row >= bus_read_u16(grid + kGridRows) ||
        bus_read_u16(kMainGridArray + 2u * row) != kUpdateCell)
        return false;
    if (!UpdateRowVersion().empty()) {
        nds_title_update_set_download_text(DownloadNoticeText());
        nds_title_update_install();
        g_update = UpdateWait{};
        g_update.waiting = true;
    }
    return true;
}

// Once per frame while waiting for the update.
void UpdateFrame(uint32_t scene) {
    bus_write_u32(scene + kSceneIdleTime, 0u);
    const NdsTitleUpdateStatus status = nds_title_update_status();
    const bool busy = status.state == NdsTitleUpdateState::Downloading ||
                      status.state == NdsTitleUpdateState::Applying;
    if (busy) g_update.busy_seen = true;
    ++g_update.frames;
    // Applying on desktop ends the process: keep waiting for it. On
    // Android the installer is up and the game goes on behind it.
    const bool restarting =
        status.state == NdsTitleUpdateState::Applying && status.restarts;
    const bool done = g_update.busy_seen ? !busy || (!status.restarts &&
                                                     status.state ==
                                                         NdsTitleUpdateState::Applying)
                                         : g_update.frames > kUpdateStartFrames;
    if (done && !restarting) g_update = UpdateWait{};
}

// The main menu's quit row was chosen: close the game.
bool QuitRowChosen(uint32_t scene) {
    const uint32_t grid = FindTyped(
        scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid);
    if (!grid || bus_read_u32(grid + kGridCells) != kMainGridArray)
        return false;
    const uint32_t row = bus_read_u16(grid + kGridRow);
    if (row >= bus_read_u16(grid + kGridRows) ||
        bus_read_u16(kMainGridArray + 2u * row) != kQuitCell)
        return false;
    std::fprintf(stderr, "[title] quit chosen from the main menu\n");
    nds_frontend_request_exit();
    return true;
}

// The main menu's save row was chosen: ask for the dialog.
bool SaveRowChosen(uint32_t scene) {
    const uint32_t grid = FindTyped(
        scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid);
    if (!grid || bus_read_u32(grid + kGridCells) != kMainGridArray)
        return false;
    const uint32_t row = bus_read_u16(grid + kGridRow);
    if (row >= bus_read_u16(grid + kGridRows) ||
        bus_read_u16(kMainGridArray + 2u * row) != kSaveCell)
        return false;
    if (bus_read_u8(kMenuMode) == 2u) {
        nds_title_save_import([](const std::vector<uint8_t>& file,
                                 std::vector<uint8_t>* image,
                                 std::string* error) {
            return dq9::save::Convert(file, image, error);
        });
    } else {
        nds_title_save_export();
    }
    return true;
}

// Up one page, as B: from the categories back to the main menu.
void GoBack() {
    if (g_menu.section.empty()) {
        RequestClose();
    } else {
        g_menu.section = Parent(g_menu.section);
        ShowPage();
    }
}

// The pen was just put on the description's panel (there is no Back row:
// that goes up a page too). The touch manager: +85 a press began this
// frame, its position as 0x02012A94 reads it.
constexpr uint32_t kTouch = 0x02114E54u;

bool DescriptionTouched() {
    if (!bus_read_u8(kTouch + 85u)) return false;
    int x = 0, y = 0;
    if (bus_read_u16(kTouch + 36u)) {
        x = bus_read_u16(kTouch + 32u);
        y = bus_read_u16(kTouch + 34u);
    } else {
        x = static_cast<int>(bus_read_u32(kTouch + 56u));
        y = static_cast<int>(bus_read_u32(kTouch + 60u));
    }
    const int left = 8, top = 8 * static_cast<int>(kDescTop);
    return x >= left && x < left + 8 * static_cast<int>(kPageWidth) &&
           y >= top && y < top + 8 * static_cast<int>(kDescHeight);
}

void Event(uint32_t scene) {
    if (g_menu.open && (scene != g_menu.scene || !PageShown(scene))) {
        // The scene left the page on its own (a reset, another screen). If
        // the grid still had the page's rows and handlers, one of those
        // sent this event: give the grid back and do nothing else.
        const bool stale = g_menu.grid &&
            bus_read_u32(g_menu.grid + kGridCells) == kGridArray;
        Forget();
        if (stale) return;
    }
    // An event while waiting for an update means the scene stopped running
    // the handler (a reset, another screen): the wait is over.
    if (g_update.waiting) g_update = UpdateWait{};
    if (!g_menu.open) {
        if (!SaveRowChosen(scene) && !UpdateRowChosen(scene) &&
            !QuitRowChosen(scene))
            Open(scene);
        return;
    }
    if (g_menu.closing) return;
    const uint32_t held = bus_read_u16(kPadHeld);
    const uint32_t pressed = held & ~bus_read_u16(kPadPrevious);
    const uint32_t index = bus_read_u16(g_menu.grid + kGridRow);
    g_menu.cursor[g_menu.section] = index;
    // The pen turned the part (the grid did, on an arrow).
    if (g_menu.parts > 1) {
        const int16_t turned =
            static_cast<int16_t>(bus_read_u16(g_menu.grid + kGridPart));
        uint32_t& part = g_menu.part[g_menu.section];
        if (turned >= 0 && static_cast<uint32_t>(turned) < g_menu.parts &&
            static_cast<uint32_t>(turned) != part) {
            part = static_cast<uint32_t>(turned);
            ShowPage();
            return;
        }
    }
    int direction = 0;
    if (!(pressed & kKeyA)) {
        if (pressed & kKeyB) {
            GoBack();
            return;
        }
        if (held & (kKeyL | kKeyR)) {
            if (TurnPart(g_menu, (held & kKeyL) ? -1 : 1)) ShowPage();
            return;
        }
        if (held & kKeyLeft) direction = -1;
        else if (held & kKeyRight) direction = 1;
    }
    if (index >= g_menu.rows.size()) return;
    const Row row = g_menu.rows[index];
    // Left and right on a row without a value turn the part.
    if (direction != 0 && !Steps(row)) {
        if (TurnPart(g_menu, direction)) ShowPage();
        return;
    }
    switch (row.kind) {
    case Row::Kind::Category:
        if (direction != 0) break;
        g_menu.section = row.section;
        ShowPage();
        break;
    case Row::Kind::Item:
        if (Change(g_menu, row.item, direction)) ShowPage();
        break;
    default:
        break;
    }
}

// Once per frame while a page is open (the handler is then the scene's
// running handler): follow the cursor with the description, and keep the
// title from going back to the logos while the player reads the options.
void Frame(uint32_t scene) {
    if (g_update.waiting) {
        UpdateFrame(scene);
        return;
    }
    if (!g_menu.open) return;
    if (scene != g_menu.scene || !PageShown(scene)) {
        Forget();
        return;
    }
    bus_write_u32(scene + kSceneIdleTime, 0u);
    if (g_menu.closing) {
        if (!dq9::TitleTextPending()) Close();
        return;
    }
    // Rows the frontend changed on its own (signing in or out, the lobby
    // server's state) redraw the page; a few times a second is enough.
    if (++g_menu.polled % 10 == 0) {
        Model probe = g_menu;
        BuildRows(probe, kMaxRows, false);
        if (Signature(probe) != g_menu.signature) {
            ShowPage();
            return;
        }
    }
    if (DescriptionTouched()) {
        GoBack();
        return;
    }
    if (bus_read_u16(g_menu.grid + kGridRow) != g_menu.described)
        ShowDescription();
    // The scene step skips its every-frame handler while one is running.
    const uint32_t every_frame = bus_read_u16(scene + kSceneFrameHandler);
    const uint32_t table = bus_read_u32(scene + kSceneHandlers);
    if (every_frame && every_frame != kOptionsHandler && table)
        Call(bus_read_u32(table + 4u * every_frame), {scene});
}

}  // namespace

// After 0x021F89F4 set up a main menu entry: point the WFC row at our text.
NDS_TITLE_HOOK void Dq9OptionsRowText(uint32_t* cell) {
    if (bus_read_u16(*cell + kCellLayer) != kMainMenuLayer ||
        bus_read_u16(*cell + kCellTextId) != kWfcTextId) {
        return;
    }
    WriteString(kTextBuffer, kTextBufferSize,
                dq9::Tr("main_menu", "options",
                        dq9::TextLanguage()));
    bus_write_u32(*cell + kCellText, kTextBuffer);
}

// Before `bicne`: leave the icon's hidden bit set.
NDS_TITLE_HOOK void Dq9OptionsRowHideIcon(uint32_t* cpsr) { *cpsr |= kCpsrZ; }

// The entry of the WFC row's handler (0x0216D6A0, r0 = the scene), which the
// Options row and the pages' rows and grid run. True: call *r12 (the hook
// jumps to a `bx r12`) with lr = this instruction, so the hook runs again
// when it returns. False: return to the handler's caller, with 1 while a
// page is open, which makes the scene step call the handler every frame
// (Frame), and 0 otherwise. The handler's own code (it started the Nintendo
// WFC setup) never runs.
NDS_TITLE_HOOK bool Dq9OptionsMenu(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                   uint32_t* r3, uint32_t* r12, uint32_t* sp,
                                   uint32_t* lr) {
    Driver& d = g_driver;
    // A call made below returns here with the stack pointer it was given.
    if (d.active && d.waiting && *sp == d.call_sp) {
        d.waiting = false;
        if (auto then = std::move(d.then)) then(*r0);
    } else {
        d = Driver{};
        d.active = true;
        d.saved_lr = *lr;
        d.saved_sp = *sp;
        const uint32_t scene = *r0;
        if (d.saved_lr == kRunningHandlerReturn)
            Frame(scene);
        else
            Event(scene);
        Then([](uint32_t) {
            g_driver.result = g_menu.open || g_update.waiting ? 1u : 0u;
        });
    }
    while (!d.queue.empty()) {
        GuestCall call = std::move(d.queue.front());
        d.queue.pop_front();
        if (!call.target) {
            if (call.then) call.then(0u);
            continue;
        }
        uint32_t* const regs[] = {r0, r1, r2, r3};
        for (size_t i = 0; i < 4; ++i)
            *regs[i] = i < call.args.size() ? call.args[i] : 0u;
        d.call_sp = (d.saved_sp - 0x40u) & ~7u;
        for (size_t i = 4; i < call.args.size(); ++i)
            bus_write_u32(d.call_sp + 4u * static_cast<uint32_t>(i - 4),
                          call.args[i]);
        *sp = d.call_sp;
        *lr = kFnHandler;
        *r12 = call.target;
        d.then = std::move(call.then);
        d.waiting = true;
        return true;
    }
    *sp = d.saved_sp;
    *lr = d.saved_lr;
    *r0 = d.result;
    d = Driver{};
    return false;
}

// ---- The save row, the update row, the quit row and the version line ---

namespace {

// The calls the rows' builder hook makes before the builder runs (the
// rows' cells, the version line's window), and the builder's own
// registers, given back when the last call returns.
struct BuilderCalls {
    bool waiting = false;
    uint32_t scene = 0;
    uint32_t r[4] = {}, sp = 0, lr = 0, call_sp = 0;
    std::deque<GuestCall> queue;
    std::function<void(uint32_t)> then;
};

BuilderCalls g_builder;

// The main menu's rows top to bottom, the save row's included, the update
// row's when `update`, and the quit row's last, or none in a mode without
// them.
std::vector<uint32_t> MainRows(uint8_t mode, bool update) {
    std::vector<uint32_t> rows;
    if (mode == 1u || mode == 5u)
        rows = {kMainCells[0], kMainCells[1], kSaveCell, kMainCells[2]};
    else if (mode == 2u)
        rows = {kMainCells[0], kSaveCell, kMainCells[1]};
    if (!rows.empty() && update) rows.push_back(kUpdateCell);
    if (!rows.empty()) rows.push_back(kQuitCell);
    return rows;
}

// Points cell `id`'s text at `address`, holding `text`.
void SetCellText(uint32_t scene, uint32_t id, uint32_t address, uint32_t size,
                 const std::string& text) {
    if (const uint32_t cell = FindTyped(scene, id, kTypeCell)) {
        WriteString(address, size, text);
        bus_write_u32(cell + kCellText, address);
    }
}

// What the builder hook asks of the game for rows `rows`: the save row's
// cell, the update row's when an update is out, the quit row's and the
// version line.
void QueueBuilderCalls(uint32_t scene, uint32_t rows) {
    std::deque<GuestCall>& q = g_builder.queue;
    const uint32_t language = dq9::TextLanguage();

    // 0x0216AFB0(scene, cell, group, layer, text, x, y, 10, enabled); the
    // layout hook below places the rows.
    if (!FindWidget(scene, kSaveCell)) {
        q.push_back({kFnNewCell,
                     {scene, kSaveCell, kCellGroup, kMainMenuLayer, kTextRowsId,
                      static_cast<uint32_t>(kRowX), 0u, 10u, 1u},
                     [scene, rows, language](uint32_t) {
                         SetCellText(scene, kSaveCell, kSaveRowText, kSaveRowTextSize,
                                     rows == 2u
                                         ? dq9::Tr("main_menu", "import_save", language)
                                         : dq9::Tr("main_menu", "export_save", language));
                     }});
    }
    const std::string update = UpdateRowVersion();
    if (!update.empty() && !FindWidget(scene, kUpdateCell)) {
        q.push_back({kFnNewCell,
                     {scene, kUpdateCell, kCellGroup, kMainMenuLayer, kTextRowsId,
                      static_cast<uint32_t>(kRowX), 0u, 10u, 1u},
                     [scene, update](uint32_t) {
                         SetCellText(scene, kUpdateCell, kUpdateRowText,
                                     kUpdateRowTextSize, UpdateRowText(update));
                     }});
    }
    if (!FindWidget(scene, kQuitCell)) {
        q.push_back({kFnNewCell,
                     {scene, kQuitCell, kCellGroup, kMainMenuLayer, kTextRowsId,
                      static_cast<uint32_t>(kRowX), 0u, 10u, 1u},
                     [scene, language](uint32_t) {
                         SetCellText(scene, kQuitCell, kQuitRowText, kQuitRowTextSize,
                                     dq9::Tr("main_menu", "quit", language));
                     }});
    }

    const std::string version = nds_title_update_status().current_version;
    const uint32_t panel = FindTyped(scene, kPagePanel, kTypePanel);
    if (version.empty() || !panel) return;
    if (!FindWidget(scene, kVersionCell)) {
        q.push_back({kFnNewCell,
                     {scene, kVersionCell, kCellGroup, kPagePanel, kTextRowsId,
                      static_cast<uint32_t>(kVersionCellX),
                      static_cast<uint32_t>(kVersionCellY), 10u, 1u},
                     nullptr});
    }
    // The text, and the window that draws it, once the cell exists.
    q.push_back({0u, {}, [scene, version](uint32_t) {
        SetCellText(scene, kVersionCell, kVersionText, kVersionTextSize, "v" + version);
        // Shown again if a page hid it and the scene left without closing.
        if (const uint32_t cell = FindTyped(scene, kVersionCell, kTypeCell))
            bus_write_u8(cell + kWidgetFlags, bus_read_u8(cell + kWidgetFlags) & ~kHidden);
        // No arrows: a page in parts may have left the panel paged.
        ClearPaging(scene);
    }});
    q.push_back({kFnPanelClose, {panel}, nullptr});
    q.push_back({kFnPanelOpen,
                 {panel, scene, kVersionVram, kVersionX, kVersionY, kVersionWidth,
                  kVersionHeight, 0u, 1u, 0u},
                 [scene](uint32_t) { bus_write_u32(kVersionScene, scene); }});
}

}  // namespace

// The entry of the main menu's rows builder 0x0216B498 (r0 = the scene,
// r1 = which rows): with rows 1 (a save) or 2 (no save), first make the
// save row's cell, the update row's and the version line's, and open the
// version line's window. True: call *r12 (the hook jumps to a `bx r12`)
// with lr = this instruction, which brings each call back here. False: run
// the builder, with the registers it was called with.
NDS_TITLE_HOOK bool Dq9SaveRowCell(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                   uint32_t* r3, uint32_t* r12, uint32_t* sp,
                                   uint32_t* lr) {
    BuilderCalls& c = g_builder;
    uint32_t* const regs[] = {r0, r1, r2, r3};
    if (c.waiting && *sp == c.call_sp) {
        c.waiting = false;
        if (auto then = std::move(c.then)) then(*r0);
    } else {
        c = BuilderCalls{};
        if (*r1 != 1u && *r1 != 2u) return false;
        c.scene = *r0;
        for (size_t i = 0; i < 4; ++i) c.r[i] = *regs[i];
        c.sp = *sp;
        c.lr = *lr;
        c.call_sp = (*sp - 0x40u) & ~7u;
        QueueBuilderCalls(*r0, *r1);
    }
    while (!c.queue.empty()) {
        GuestCall call = std::move(c.queue.front());
        c.queue.pop_front();
        if (!call.target) {
            if (call.then) call.then(0u);
            continue;
        }
        for (size_t i = 0; i < 4; ++i)
            *regs[i] = i < call.args.size() ? call.args[i] : 0u;
        for (size_t i = 4; i < call.args.size(); ++i)
            bus_write_u32(c.call_sp + 4u * static_cast<uint32_t>(i - 4),
                          call.args[i]);
        *sp = c.call_sp;
        *lr = kFnRows;
        *r12 = call.target;
        c.then = std::move(call.then);
        c.waiting = true;
        return true;
    }
    if (c.scene) {
        for (size_t i = 0; i < 4; ++i) *regs[i] = c.r[i];
        *sp = c.sp;
        *lr = c.lr;
    }
    c = BuilderCalls{};
    return false;
}

// Before the main menu opens its panel (0x021F7EB8, r1 = the scene, the
// height at [sp+8]; r7 = the width, r5 the panel's VRAM base, to which the
// menu adds width * height * 32 for the save summary's panel after the
// call): spread the rows over a taller panel with the save row above
// Options and the update row, if any, at the bottom, and give the grid the
// rows in that order.
NDS_TITLE_HOOK void Dq9SaveRowLayout(uint32_t* r1, uint32_t* r5, uint32_t* r7,
                                     uint32_t* sp) {
    const uint32_t scene = *r1;
    const std::vector<uint32_t> ids =
        MainRows(bus_read_u8(kMenuMode), FindWidget(scene, kUpdateCell) != 0);
    const uint32_t grid = FindTyped(
        scene, bus_read_u16(scene + kSceneActiveGrid), kTypeGrid);
    if (ids.empty() || !grid) return;
    std::vector<uint32_t> cells;
    for (const uint32_t id : ids) {
        const uint32_t cell = FindTyped(scene, id, kTypeCell);
        if (!cell || bus_read_u16(cell + kCellLayer) != kMainMenuLayer) return;
        cells.push_back(cell);
    }
    const uint32_t count = static_cast<uint32_t>(ids.size());
    // As the game's three rows in six tiles; four rows get eight, five ten,
    // six eleven (rows 1 to 11, clear of the summary from row 12).
    const uint32_t height = count >= 6u ? 11u : count == 5u ? 10u : count == 4u ? 8u : 6u;
    const int top = count >= 6u ? 7 : count == 5u ? 9 : count == 4u ? 8 : 6;
    for (uint32_t i = 0; i < count; ++i) {
        bus_write_u16(cells[i] + kCellY,
                      static_cast<uint16_t>(top + kRowPitch * static_cast<int>(i)));
        bus_write_u16(kMainGridArray + 2u * i, static_cast<uint16_t>(ids[i]));
        // Our rows run the Options row's handler, which tells them apart.
        if (ids[i] == kSaveCell || ids[i] == kUpdateCell || ids[i] == kQuitCell)
            bus_write_u16(cells[i] + kCellAction, kOptionsHandler);
    }
    bus_write_u32(grid + kGridCells, kMainGridArray);
    bus_write_u16(grid + kGridRows, static_cast<uint16_t>(count));
    bus_write_u16(grid + kGridCols, 1u);
    if (bus_read_u16(grid + kGridRow) >= count) bus_write_u16(grid + kGridRow, 0u);
    bus_write_u16(grid + kGridCol, 0u);
    bus_write_u32(grid + kGridRedraw, 1u);

    // A panel that reaches the Yes/No window's rows (from tile row 9, at
    // column 26: the delete prompt's) stops short of its column: the two
    // windows' translucent backgrounds would blend into black where they
    // overlap. The game's widths (27 to 30 tiles, by language) were sized for
    // the WFC row's text; the rows left fit in 25.
    const uint32_t panel_top = bus_read_u32(*sp);
    if (panel_top + height > kYesNoTop && *r7 > kNarrowWidth) {
        *r7 = kNarrowWidth;
        bus_write_u32(*sp + 4u, kNarrowWidth);
    }
    const uint32_t old_height = bus_read_u32(*sp + 8u);
    bus_write_u32(*sp + 8u, height);
    *r5 += *r7 * (height - old_height) * 32u;
}

// After the script command 0x02185504 closed a panel (r4) with
// 0x021F8120: when it was the save summary (211), as before the delete
// prompt, whose message window covers the bottom of the screen, also close
// the version line's window (212), which would overlap it. True: call *r12
// (the hook jumps to a `bx r12` in ov11) with lr = this instruction, which
// runs the hook again once the window is closed; then false.
NDS_TITLE_HOOK bool Dq9VersionLineClose(uint32_t* r0, uint32_t* r4,
                                        uint32_t* r12, uint32_t* lr) {
    if (g_menu.open || !dq9::TitleScreen()) return false;
    const uint32_t scene = bus_read_u32(kVersionScene);
    if (scene < 0x02000000u || scene >= 0x02400000u ||
        FindTyped(scene, kFileInfoPanel, kTypePanel) != *r4)
        return false;
    const uint32_t panel = FindTyped(scene, kPagePanel, kTypePanel);
    if (!panel || !(bus_read_u8(panel + kPanelWindowOpen) & 1u)) return false;
    *r0 = panel;
    *r12 = kFnPanelClose;
    *lr = kVersionCloseReturn;
    return true;
}
