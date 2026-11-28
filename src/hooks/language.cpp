// Language: switches DQ9's text language while it runs (Game > Language,
// [title] language in settings.toml). "System" (the default) leaves the
// game untouched: it reads the firmware language, set by [system] language
// in game.toml. See config/hooks/dq9_arm9.toml for the guest code.
//
// DQ9 picks the language each time it loads a text file, so the override
// reaches menus, dialogue, item names and battles as soon as they are next
// loaded (a menu that is already open keeps its text until it is reopened).
// The one text set it loads once per session, the articles and standard
// strings, is reloaded through the game's own loader on the next frame, and
// the camp menu, made once per field, loads its text again when it next
// opens.
//
// The title screen loads its menu text and its logo once, when it starts:
// string table 402 from str_op_<LG>.bin (the main menu's rows and the save
// summary) and the top screen's background 105 from bg_up_<LG>.pac. When
// the language changes on the title, both go back to their first load step
// and the game loads them again, in the new language. The logo shows it at
// once, the main menu when it is next built (Back from the Options pages
// waits for it: TitleTextPending).
#include <cstdio>
#include <deque>
#include <functional>
#include <initializer_list>

#include "dq9_scene.h"
#include "lang.h"
#include "title_hooks.h"
#include "title_settings.h"

namespace {

// Values are the DS firmware language codes (0 here means "System").
constexpr const char* kChoices[] = {"System", "English", "French",
                                    "German", "Italian", "Spanish"};

constexpr NdsTitleSetting kLanguage = {
    "language",
    "Language",
    "The game's text language. System uses the console language.",
    0, 5, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    0,
};

const int kLanguageId = nds_title_setting_register(&kLanguage);

// Firmware user settings, as the boot code leaves them in main RAM:
// +0x64 bits 0-2 are the language (OS_GetOwnerInfo reads them).
constexpr uint32_t kFirmwareLanguage = 0x027FFCE4;

// The game's context (0x0200F398 returns it): +5 is the language the game
// last read, refreshed by 0x0200FB10 before every text file load.
constexpr uint32_t kGameLanguage = 0x020F33D8 + 5;

// Session text tables loaded by 0x020E5124 (articles, standard strings):
// their arena and the first of their two 24-byte slots (bit 31 of the
// slot's first word is set once it holds a table).
constexpr uint32_t kSessionTextArena = 0x02153680;
constexpr uint32_t kSessionTextSlot = 0x02153694;

constexpr uint32_t kMainLoopHead = 0x02000E2C;
constexpr uint32_t kFieldLoopSwapReturn = 0x0218C7ACu;  // ov17's swap call
constexpr uint32_t kSwapResume = 0x020D86E4u;

// The language code the game would settle on: as 0x0200FB10 does, any
// code it has no text for (Japanese, Chinese, Korean) becomes English.
uint32_t GameLanguage(uint32_t code) {
    return code >= 2 && code <= 5 ? code : 1;
}

uint32_t WantedLanguage() {
    const int value = nds_title_setting_value(kLanguageId);
    if (value != 0) return static_cast<uint32_t>(value);
    return GameLanguage(bus_read_u16(kFirmwareLanguage) & 7u);
}

// ---- The title's text and logo -----------------------------------------

// Widgets (ov23), in the scene's list at scene+0x118 (next at +0x18).
constexpr uint32_t kWidgetType = 0x04u;  // u16
constexpr uint32_t kWidgetId = 0x06u;    // u16
constexpr uint32_t kWidgetGroup = 0x08u;  // u16
constexpr uint32_t kWidgetNext = 0x18u;
// Every widget with a file has a load step at +0x1C: 0 asks the file
// manager for it (with "<LG>" replaced by the language then), 1 waits for
// it and parses it, 2 loaded.
constexpr uint32_t kLoadStep = 0x1Cu;
constexpr uint32_t kStepRequest = 0u, kStepLoaded = 2u;
// Both are in group 3, made by opening.stb with 24 KB.
constexpr uint32_t kTitleGroup = 3u;
// String table 402 (type 4, ov23 0x021FA298, steps 0x021FEA0C): the menu
// text, str_op_<LG>.bin in bin/menu/str_op.gp2.
constexpr uint32_t kStringTableType = 4u;
constexpr uint32_t kTextId = 402u;
// Background 105 (type 2, ov23 0x021F745C, steps 0x021FD8FC; made by ov4
// 0x0216E0C0): the logo on the top screen, bg_up_<LG>.pac in
// menu/bg_up.gp2. Its parse (0x021F7AAC) loads the CHAR and PAL chunks
// into its VRAM and appends each SCRN chunk to the screen list of its
// animation (+0x28), which counts the screens added at +0x1D and draws
// them every frame.
constexpr uint32_t kBackgroundType = 2u;
constexpr uint32_t kLogoId = 105u;
constexpr uint32_t kScreensAdded = 0x28u + 0x1Du;  // u8

// Both parses allocate in their group's heap, which frees nothing until the
// title ends: a reload there would leak with every language change. A
// reload parses into memory of its own instead: free memory at the end of
// overlay slot 0, past overlay 4's image and bss (they end at 0x02171060)
// and below options_row.cpp's strings (0x02183BE0), unused while the title
// runs. Its state is kept there too, in guest memory.
//
// The parses allocate everything through 0x02032554(heap, size): during a
// reload the group lookup gives them a node whose heap that hook answers
// from the region. The heap's own word is 0 ("no heap"), which every other
// function of the heap API returns early on. Nothing frees the region: the
// title's data lives as long as the title, which leaves it to the next
// overlay.
constexpr uint32_t kReload = 0x02171060u;
constexpr uint32_t kReloadMagic = kReload + 0x00u;
constexpr uint32_t kReloadScene = kReload + 0x04u;
constexpr uint32_t kReloadText = kReload + 0x08u;      // the string table
constexpr uint32_t kReloadLanguage = kReload + 0x0Cu;  // the language loaded
constexpr uint32_t kReloadActive = kReload + 0x10u;    // widgets reloading
constexpr uint32_t kReloadNext = kReload + 0x14u;      // next free byte
constexpr uint32_t kReloadNode = kReload + 0x18u;      // the group node
constexpr uint32_t kReloadHeap = kReloadNode + 4u;     // its heap: word 0
constexpr uint32_t kReloadLogo = kReload + 0x20u;      // the logo
constexpr uint32_t kReloadData = kReload + 0x40u;
constexpr uint32_t kReloadEnd = 0x02183BE0u;
constexpr uint32_t kMagic = 0x474C5144u;  // "DQLG"
constexpr uint32_t kActiveText = 1u, kActiveLogo = 2u;

bool InMainRam(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

// Widget `id` of `type` in the title's group, or 0.
uint32_t FindWidget(uint32_t scene, uint32_t id, uint32_t type) {
    uint32_t w = bus_read_u32(scene + 0x118u);
    for (int guard = 0; w && guard < 4096; ++guard) {
        if (!InMainRam(w)) return 0;
        if (bus_read_u16(w + kWidgetId) == id &&
            bus_read_u16(w + kWidgetType) == type &&
            bus_read_u16(w + kWidgetGroup) == kTitleGroup)
            return w;
        w = bus_read_u32(w + kWidgetNext);
    }
    return 0;
}

// The reload state is this scene's, with this string table.
bool ReloadIsFor(uint32_t scene, uint32_t text) {
    return bus_read_u32(kReloadMagic) == kMagic &&
           bus_read_u32(kReloadScene) == scene &&
           bus_read_u32(kReloadText) == text;
}

// A widget that reloads now (`bit` of the active ones).
bool Reloading(uint32_t widget, uint32_t address, uint32_t bit) {
    return widget && bus_read_u32(kReloadMagic) == kMagic &&
           bus_read_u32(address) == widget &&
           (bus_read_u32(kReloadActive) & bit) != 0u;
}

// Once per frame on the title, with its scene: text loaded in the language
// wanted when the table was first seen; once that differs, reload the text
// and the logo.
void TitleReload(uint32_t scene) {
    const uint32_t text = FindWidget(scene, kTextId, kStringTableType);
    if (!text) return;
    const uint32_t logo = FindWidget(scene, kLogoId, kBackgroundType);
    const uint32_t wanted = WantedLanguage();
    if (!ReloadIsFor(scene, text)) {
        bus_write_u32(kReloadMagic, kMagic);
        bus_write_u32(kReloadScene, scene);
        bus_write_u32(kReloadText, text);
        bus_write_u32(kReloadLanguage, wanted);
        bus_write_u32(kReloadActive, 0u);
        bus_write_u32(kReloadNext, kReloadData);
        bus_write_u32(kReloadHeap, 0u);
        bus_write_u32(kReloadLogo, 0u);
        return;
    }
    uint32_t active = bus_read_u32(kReloadActive);
    if (active) {
        if ((active & kActiveText) && bus_read_u32(text + kLoadStep) == kStepLoaded)
            active &= ~kActiveText;
        const uint32_t reloading_logo = bus_read_u32(kReloadLogo);
        if ((active & kActiveLogo) &&
            (reloading_logo != logo ||
             bus_read_u32(logo + kLoadStep) == kStepLoaded))
            active &= ~kActiveLogo;
        bus_write_u32(kReloadActive, active);
        if (active) return;
        std::fprintf(stderr, "[language] title text and logo reloaded (%u bytes)\n",
                     bus_read_u32(kReloadNext) - kReloadData);
    }
    if (bus_read_u32(kReloadLanguage) == wanted ||
        bus_read_u32(text + kLoadStep) != kStepLoaded)
        return;
    // Each keeps its old data until its parse, with the new file in hand,
    // replaces it; that data stays readable until then.
    active = kActiveText;
    bus_write_u32(text + kLoadStep, kStepRequest);
    if (logo && bus_read_u32(logo + kLoadStep) == kStepLoaded) {
        active |= kActiveLogo;
        bus_write_u32(logo + kLoadStep, kStepRequest);
    }
    bus_write_u32(kReloadLogo, logo);
    bus_write_u32(kReloadLanguage, wanted);
    bus_write_u32(kReloadActive, active);
    bus_write_u32(kReloadNext, kReloadData);
    bus_write_u32(kReloadHeap, 0u);
    std::fprintf(stderr, "[language] reloading the title's text%s in language %u\n",
                 logo ? " and logo" : "", wanted);
}

}  // namespace

bool dq9::TitleTextPending() {
    if (!dq9::TitleScreen() || bus_read_u32(kReloadMagic) != kMagic) return false;
    const uint32_t scene = bus_read_u32(kReloadScene);
    if (!InMainRam(scene)) return false;
    const uint32_t text = FindWidget(scene, kTextId, kStringTableType);
    if (!text || !ReloadIsFor(scene, text)) return false;
    return bus_read_u32(kReloadActive) != 0u ||
           bus_read_u32(kReloadLanguage) != WantedLanguage();
}

// ov23 0x021F68DC(widgets = scene+0x118, scene), at its entry: the scene
// step's widget update, every frame.
NDS_TITLE_HOOK void Dq9TitleWidgets(uint32_t* r0, uint32_t* r1) {
    if (!dq9::TitleScreen() || !InMainRam(*r1) || *r0 != *r1 + 0x118u) return;
    TitleReload(*r1);
}

// ov23 0x021FA670, in the string table parser 0x021FA644 (r8 = the table),
// after it looked up the table's group node (r0) for the heap at +4: a
// reload parses into its own memory.
NDS_TITLE_HOOK void Dq9TitleTextHeap(uint32_t* r0, uint32_t* r8) {
    if (*r0 && Reloading(*r8, kReloadText, kActiveText)) *r0 = kReloadNode;
}

// ov23 0x021F7AD0, in the background parser 0x021F7AAC (r10 = the
// background), after it looked up its group node (r0): a reload of the
// logo parses into its own memory, and its screens start over.
NDS_TITLE_HOOK void Dq9TitleLogoHeap(uint32_t* r0, uint32_t* r10) {
    if (!*r0 || !Reloading(*r10, kReloadLogo, kActiveLogo)) return;
    *r0 = kReloadNode;
    bus_write_u8(*r10 + kScreensAdded, 0u);
}

// The heap allocator 0x02032554(heap, size), at its entry: for the reload's
// heap, return the next bytes of its memory (true: return r0 to the caller),
// 4-aligned as the game's heaps give them, or 0 when it is full.
NDS_TITLE_HOOK bool Dq9TitleTextAlloc(uint32_t* r0, uint32_t* r1) {
    if (*r0 != kReloadHeap || bus_read_u32(kReloadMagic) != kMagic) return false;
    const uint32_t next = bus_read_u32(kReloadNext);
    const uint32_t size = (*r1 + 3u) & ~3u;
    if (next < kReloadData || next > kReloadEnd || size > kReloadEnd - next) {
        std::fprintf(stderr, "[language] title reload out of memory\n");
        *r0 = 0u;
        return true;
    }
    for (uint32_t a = next; a < next + size; a += 4) bus_write_u32(a, 0u);
    bus_write_u32(kReloadNext, next + size);
    *r0 = next;
    return true;
}

// OS_GetOwnerInfo (0x020C99D8), before `strb r2, [r4]` at 0x020C99F8:
// r2 is the firmware language it is about to return.
NDS_TITLE_HOOK void Dq9Language(uint32_t* r2) {
    const int value = nds_title_setting_value(kLanguageId);
    if (value != 0) *r2 = static_cast<uint32_t>(value);
}

namespace {

// ---- The session text ---------------------------------------------------

// The language 0x020E5124 last loaded the session text tables in, 0 before
// it ran. Kept apart from the context's language, which every text file
// load refreshes: a load between the change and the next reload point
// would hide that the tables are still in the old language.
uint32_t g_session_language = 0;

// Once a frame, from a main loop: the context gets the wanted language at
// once (the hooks' own text follows it). True when the session text tables
// are loaded in another one: the caller then calls their loader 0x020E5124.
bool SessionReloadDue() {
    // The window title shows the game's name in this language (the ROM banner
    // has one per language); host state only.
    nds_title_set_language(static_cast<int>(WantedLanguage()));
    const uint32_t current = bus_read_u8(kGameLanguage);
    if (current < 1 || current > 5) return false;  // not initialised yet
    const uint32_t wanted = WantedLanguage();
    if (current != wanted) bus_write_u8(kGameLanguage, static_cast<uint8_t>(wanted));
    if (g_session_language == 0 || g_session_language == wanted) return false;
    if (bus_read_u32(kSessionTextArena) == 0 ||
        (bus_read_u32(kSessionTextSlot) & 0x80000000u) == 0) {
        return false;
    }
    std::fprintf(stderr, "[language] reloading the session text in language %u\n",
                 wanted);
    g_session_language = wanted;
    return true;
}

// ---- The camp menu's text -----------------------------------------------

// The camp menu (ov2) is made once per field and kept, idle in state 43,
// while it is closed: its text (str_tm_<LG>.nat, at camp+0x20) and icons
// (obj_tm_<LG>.pac) are loaded only by its first state, 0. Opened again
// after a language change, it goes through state 0 once more, which loads
// both again (into heaps it resets first) and opens the top window as the
// first opening does.
constexpr uint32_t kCampState = 0x1BB8u;     // u32: the state (window id)
constexpr uint32_t kCampSubState = 0x1BC0u;  // u32
constexpr uint32_t kCampClosed = 43u;
// The window manager (camp + 0xEC8): +152 non-zero with windows up.
constexpr uint32_t kCampWindowsUp = 0xEC8u + 152u;

uint32_t g_camp = 0;           // the camp menu whose text language is known
uint32_t g_camp_language = 0;  // the language it loaded its text in
uint32_t g_camp_state = 0;     // its state on its previous step
uint32_t g_camp_pass = 0;      // the field loop's pass of that step
bool g_camp_rebuild = false;   // its text was reloaded while it was open

// Passes of the field's loop (Dq9LanguageReloadField). The closed camp menu
// is not stepped, so a step after a gap is an opening.
uint32_t g_field_pass = 0;

}  // namespace

// ov2 0x02155C34, in the camp menu's step (r5 = the camp menu), before it
// reads the state to run.
NDS_TITLE_HOOK void Dq9CampTextLanguage(uint32_t* r5) {
    const uint32_t camp = *r5;
    if (!InMainRam(camp)) return;
    const uint32_t state = bus_read_u32(camp + kCampState);
    const bool known = camp == g_camp;
    const bool opening = !known || g_camp_state == kCampClosed ||
                         g_field_pass - g_camp_pass > 2u;
    if (!known) g_camp_language = 0;
    g_camp = camp;
    g_camp_state = state;
    g_camp_pass = g_field_pass;
    const uint32_t wanted = WantedLanguage();
    if (state == 0u) g_camp_rebuild = false;
    if (state == 1u && g_camp_rebuild) {
        // Back on the top window after a reload of its text in place: that
        // window keeps the size it was opened at, for the old language's
        // words (the window manager sizes a window from its text when it
        // opens, and leaves an open one as it is). With the manager emptied
        // (+152 windows up, +179 how many, +180 the next slot), its first
        // state opens its windows again, as the first opening did.
        g_camp_rebuild = false;
        std::fprintf(stderr, "[language] camp menu reopened for its new text\n");
        bus_write_u32(camp + kCampWindowsUp, 0u);
        bus_write_u8(camp + 0xEC8u + 179u, 0u);
        bus_write_u8(camp + 0xEC8u + 180u, 0u);
        bus_write_u32(camp + kCampState, 0u);
        bus_write_u32(camp + kCampSubState, 0u);
        g_camp_state = 0u;
        g_camp_language = wanted;
        return;
    }
    if (state == 0u) {
        // Its text is requested now, in the language the game reads.
        if (bus_read_u32(camp + kCampSubState) == 0u) {
            if (g_camp_language && g_camp_language != wanted)
                std::fprintf(stderr, "[language] camp menu text reloading in language %u\n",
                             wanted);
            g_camp_language = wanted;
        }
        return;
    }
    if (!opening || state == kCampClosed) return;
    if (!g_camp_language || g_camp_language == wanted) return;
    if (bus_read_u32(camp + kCampWindowsUp) != 0u) {
        std::fprintf(stderr, "[language] camp menu opened in state %u with its "
                     "windows up: its text stays in language %u\n",
                     state, g_camp_language);
        return;
    }
    bus_write_u32(camp + kCampState, 0u);
    bus_write_u32(camp + kCampSubState, 0u);
    g_camp_state = 0u;
}

// 0x020E5124, at its entry: the session text tables are (re)loaded, in the
// language the game reads.
NDS_TITLE_HOOK void Dq9SessionTextLoad() {
    g_session_language = WantedLanguage();
}

// The boot loop's head (0x02000E2C, `mov r0, r7`, r0-r3, r12, lr and the
// flags dead): the title and the screens before it. True: jump to the
// loader with lr = here; it returns here and the hook finds nothing to do.
NDS_TITLE_HOOK bool Dq9LanguageReload(uint32_t* lr) {
    if (!SessionReloadDue()) return false;
    *lr = kMainLoopHead;
    return true;
}

// ---- In the field: calling the game's own code --------------------------

// The field's loop (ov17 0x0218B710: field, towns and battles) ends each
// pass with G3_SwapBuffers 0x020D86E0(1, 0). Its second instruction
// (0x020D86E4, r0 and r1 the arguments, r2 = 0x04000540, nothing else
// used yet), called from the loop (lr = 0x0218C7AC), runs the calls below
// one at a time: true jumps to a `bx r12` (0x0200171C) with r12 = the
// function and lr = 0x020D86E4, so the hook runs again when it returns and
// takes its r0; with nothing left, the registers are put back.
namespace {

struct GuestCall {
    uint32_t target = 0;
    uint32_t args[4] = {0, 0, 0, 0};
    std::function<void(uint32_t r0)> then;
};

struct FieldDriver {
    bool active = false;
    uint32_t r0 = 0, r1 = 0, r2 = 0, r3 = 0, r12 = 0, lr = 0;
    std::deque<GuestCall> queue;
    std::function<void(uint32_t)> then;
};

FieldDriver g_driver;

void Call(uint32_t target, std::initializer_list<uint32_t> args,
          std::function<void(uint32_t)> then = nullptr) {
    GuestCall call;
    call.target = target;
    size_t i = 0;
    for (const uint32_t a : args)
        if (i < 4) call.args[i++] = a;
    call.then = std::move(then);
    g_driver.queue.push_back(std::move(call));
}

// Game functions (main binary).
constexpr uint32_t kSessionTextLoad = 0x020E5124u;
constexpr uint32_t kFileRequest = 0x0202FD3Cu;   // (fm, archive, name, 0): handle
constexpr uint32_t kFileRelease = 0x020301D8u;   // (fm, handle)
constexpr uint32_t kHeapAlloc = 0x02032554u;     // (heap, size): memory or 0
constexpr uint32_t kHeapFree = 0x02032628u;      // (heap, memory)
constexpr uint32_t kHeapReset = 0x02032698u;     // (heap)
constexpr uint32_t kTextParse = 0x020DFED0u;     // (table, heap, data, size)
constexpr uint32_t kTextLookup = 0x020E0444u;    // (table, id): the text

// The file manager (0x0202F7A8 returns it): +292 its requests, 68 bytes
// each from +296: s16 +48 the handle, +50 bits 0-3 the state (2 loaded,
// 3 and 4 failed), +56 the data, +60 its size.
constexpr uint32_t kFileManagerPointer = 0x02104308u;

struct FileState {
    bool found = false;
    uint32_t state = 0, data = 0, size = 0;
};

FileState File(uint32_t handle) {
    FileState out;
    const uint32_t fm = bus_read_u32(kFileManagerPointer);
    if (!InMainRam(fm)) return out;
    const uint32_t count = bus_read_u32(fm + 292u);
    for (uint32_t i = 0; i < count && i < 64u; ++i) {
        const uint32_t e = fm + 296u + 68u * i;
        if ((bus_read_u16(e + 48u) & 0xFFFFu) != (handle & 0xFFFFu)) continue;
        out.found = true;
        out.state = bus_read_u8(e + 50u) & 0xFu;
        out.data = bus_read_u32(e + 56u);
        out.size = bus_read_u32(e + 60u);
        break;
    }
    return out;
}

// A file asked for in the language `language`, until it is loaded.
struct FileJob {
    bool waiting = false;
    uint32_t handle = 0, language = 0;
};

// Asks for the file; `job` waits for it from then on.
void Request(FileJob& job, uint32_t archive, uint32_t name, uint32_t language) {
    job.waiting = true;
    job.handle = 0xFFFFFFFFu;
    job.language = language;
    Call(kFileRequest, {bus_read_u32(kFileManagerPointer), archive, name, 0u},
         [&job](uint32_t handle) {
             job.handle = handle;
             if (static_cast<int32_t>(handle) < 0) {
                 std::fprintf(stderr, "[language] file request failed\n");
                 job.waiting = false;
             }
         });
}

void Release(FileJob& job) {
    Call(kFileRelease, {bus_read_u32(kFileManagerPointer), job.handle});
    job.waiting = false;
}

// ---- The spell and ability names ----------------------------------------

// The actions' table (0x020797EC returns it), loaded once by the field
// loop's start (0x02079910, from actdt_a_<LG>.nat, into the heap at
// field + 0x150): +0 bit 31 loaded, bits 0-11 the count, bits 12-30 the
// strings' size; +4 the records, 60 bytes each; +8 the strings. A record
// keeps its name (+0) and its other text (+52) as pointers into the
// strings (offsets in the file, -1 for none). Battle panels keep pointers
// to the records, so a reload keeps them where they are, copies the new
// ones over them and points them at new strings.
constexpr uint32_t kActionTable = 0x02108E10u;
constexpr uint32_t kActionArchive = 0x020F0DCCu;  // "data/prm/actdt_a.gp2"
constexpr uint32_t kActionFile = 0x020F0DE1u;     // "actdt_a_<LG>.nat"
constexpr uint32_t kActionRecord = 60u;
constexpr uint32_t kFieldHeap = 0x150u;           // of the field loop's r10

// NNS expanded heap ("EXPH"): +0x18 its start, +0x1C its end.
constexpr uint32_t kExpHeapMagic = 0x45585048u;

uint32_t g_action_language = 0;  // what the table was loaded in, 0 unknown
FileJob g_action_job;

bool HeapHolds(uint32_t heap, uint32_t address) {
    if (!InMainRam(heap)) return false;
    const uint32_t head = bus_read_u32(heap);
    if (!InMainRam(head) || bus_read_u32(head) != kExpHeapMagic) return false;
    return address >= bus_read_u32(head + 0x18u) && address < bus_read_u32(head + 0x1Cu);
}

void ActionNamesDone(uint32_t language, bool ok) {
    g_action_language = language;
    std::fprintf(stderr, "[language] spell and ability names %s language %u\n",
                 ok ? "reloaded in" : "could not be reloaded in", language);
}

void ActionNames(uint32_t field) {
    const uint32_t wanted = WantedLanguage();
    FileJob& job = g_action_job;
    const uint32_t table = kActionTable;
    const uint32_t heap = field + kFieldHeap;
    if (!job.waiting) {
        if (!g_action_language || g_action_language == wanted) return;
        const uint32_t header = bus_read_u32(table);
        if ((header & 0x80000000u) == 0u || !InMainRam(bus_read_u32(table + 4u)) ||
            !HeapHolds(heap, bus_read_u32(table + 8u)))
            return;
        Request(job, kActionArchive, kActionFile, wanted);
        return;
    }
    if (static_cast<int32_t>(job.handle) < 0) return;  // being asked for
    const FileState file = File(job.handle);
    if (file.found && file.state != 2u && file.state != 3u && file.state != 4u)
        return;  // loading
    const uint32_t language = job.language;
    if (!file.found || file.state != 2u || !InMainRam(file.data)) {
        if (file.found) Release(job); else job.waiting = false;
        ActionNamesDone(language, false);
        return;
    }
    const uint32_t old_header = bus_read_u32(table);
    const uint32_t header = bus_read_u32(file.data);
    const uint32_t count = header & 0xFFFu;
    const uint32_t strings_size = (header << 1) >> 13;
    const uint32_t records = bus_read_u32(table + 4u);
    const uint32_t old_strings = bus_read_u32(table + 8u);
    if (count != (old_header & 0xFFFu) || strings_size == 0u ||
        file.size < 4u + kActionRecord * count + strings_size ||
        !InMainRam(records) || !HeapHolds(heap, old_strings)) {
        Release(job);
        ActionNamesDone(language, false);
        return;
    }
    const uint32_t data = file.data;
    Call(kHeapAlloc, {heap, strings_size},
         [=](uint32_t strings) {
             if (!InMainRam(strings)) {
                 Release(g_action_job);
                 ActionNamesDone(language, false);
                 return;
             }
             const uint32_t source = data + 4u + kActionRecord * count;
             for (uint32_t i = 0; i < strings_size; ++i)
                 bus_write_u8(strings + i, bus_read_u8(source + i));
             for (uint32_t i = 0; i < count; ++i) {
                 const uint32_t from = data + 4u + kActionRecord * i;
                 const uint32_t to = records + kActionRecord * i;
                 for (uint32_t b = 0; b < kActionRecord; b += 4)
                     bus_write_u32(to + b, bus_read_u32(from + b));
                 for (const uint32_t at : {0u, 52u}) {
                     const uint32_t offset = bus_read_u32(from + at);
                     bus_write_u32(to + at, offset == 0xFFFFFFFFu ? 0u : strings + offset);
                 }
             }
             bus_write_u32(table, header | 0x80000000u);
             bus_write_u32(table + 8u, strings);
             Call(kHeapFree, {heap, old_strings});
             Release(g_action_job);
             ActionNamesDone(language, true);
         });
}

// ---- The camp menu's text, while it is open -----------------------------

constexpr uint32_t kCampTable = 0x20u;       // its text table
constexpr uint32_t kCampTextHeap = 0x83Cu;   // the heap only that table uses
constexpr uint32_t kCampText0 = 0x1BDCu;     // its text 0, kept by state 0
constexpr uint32_t kCampTextHandle = 0x1BA8u;  // state 0's request, -1 idle
constexpr uint32_t kCampArchive = 0x0216CEF8u;  // ov2: "data/bin/menu/str_tm.gp2"
constexpr uint32_t kCampFile = 0x0216CF11u;     // ov2: "str_tm_<LG>.nat"

FileJob g_camp_job;
uint32_t g_camp_job_camp = 0;

bool CampOpen() { return g_camp && g_field_pass - g_camp_pass <= 2u; }

void CampText() {
    const uint32_t wanted = WantedLanguage();
    FileJob& job = g_camp_job;
    if (!job.waiting) {
        if (!CampOpen() || !g_camp_language || g_camp_language == wanted) return;
        const uint32_t camp = g_camp;
        if (bus_read_u32(camp + kCampState) == 0u ||
            bus_read_u32(camp + kCampTextHandle) != 0xFFFFFFFFu)
            return;  // in its own loading
        g_camp_job_camp = camp;
        Request(job, kCampArchive, kCampFile, wanted);
        return;
    }
    if (static_cast<int32_t>(job.handle) < 0) return;
    const FileState file = File(job.handle);
    if (file.found && file.state != 2u && file.state != 3u && file.state != 4u)
        return;
    const uint32_t camp = g_camp_job_camp;
    const uint32_t language = job.language;
    if (!file.found || file.state != 2u || !InMainRam(file.data) ||
        !CampOpen() || g_camp != camp) {
        // The menu opening again goes through its state 0 instead.
        if (file.found) Release(job); else job.waiting = false;
        std::fprintf(stderr, "[language] camp menu text not reloaded in place\n");
        return;
    }
    Call(kHeapReset, {camp + kCampTextHeap});
    Call(kTextParse, {camp + kCampTable, camp + kCampTextHeap, file.data, file.size});
    Call(kTextLookup, {camp + kCampTable, 0u}, [camp, language](uint32_t text) {
        bus_write_u32(camp + kCampText0, text);
        g_camp_language = language;
        g_camp_rebuild = true;
        std::fprintf(stderr, "[language] camp menu text reloaded in language %u\n",
                     language);
    });
    Release(job);
}

// What to call this pass (r10 = the field loop's object).
void PlanField(uint32_t field) {
    if (SessionReloadDue()) Call(kSessionTextLoad, {});
    if (InMainRam(field)) ActionNames(field);
    CampText();
}

}  // namespace

// 0x02079910(table, heap), at its entry: the field loop's start loads the
// actions' table, in the language the game reads.
NDS_TITLE_HOOK void Dq9ActionTableLoad() {
    g_action_language = WantedLanguage();
    g_action_job = FileJob{};
}

NDS_TITLE_HOOK bool Dq9LanguageReloadField(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                           uint32_t* r3, uint32_t* r10, uint32_t* r12,
                                           uint32_t* lr) {
    FieldDriver& d = g_driver;
    if (d.active) {
        if (auto then = std::move(d.then)) then(*r0);
    } else {
        if (*lr != kFieldLoopSwapReturn) return false;
        ++g_field_pass;
        d.r0 = *r0; d.r1 = *r1; d.r2 = *r2; d.r3 = *r3; d.r12 = *r12; d.lr = *lr;
        PlanField(*r10);
        if (d.queue.empty()) return false;
        d.active = true;
    }
    while (!d.queue.empty()) {
        GuestCall call = std::move(d.queue.front());
        d.queue.pop_front();
        *r0 = call.args[0];
        *r1 = call.args[1];
        *r2 = call.args[2];
        *r3 = call.args[3];
        *r12 = call.target;
        *lr = kSwapResume;
        d.then = std::move(call.then);
        return true;
    }
    *r0 = d.r0; *r1 = d.r1; *r2 = d.r2; *r3 = d.r3; *r12 = d.r12; *lr = d.lr;
    d = FieldDriver{};
    return false;
}
