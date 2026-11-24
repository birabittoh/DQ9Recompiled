// Unlock camera rotation: lets L and R turn DQ9's field camera all the way
// round instead of stopping 45 degrees either side of the default (Game >
// Unlock camera rotation, [title] unlock_camera_rotation in settings.toml).
// Off leaves the game untouched; L+R still resets the camera. See
// config/hooks/dq9_arm9.toml for the guest code.
//
// Extra buttons (Controls > Extra buttons opacity, [title] camera_buttons,
// off by default; else their opacity, 25% to 100%): on-screen buttons (ndsrecomp runner/src/touch_buttons.h) for
// play without a pad. Two hold L and R to turn the camera, halfway down the
// left and right edges of the top screen, and one holds B to emote, at the
// bottom center (a latch: B stays down, and a tap on an emote on the
// touchscreen presses its D-pad direction), and in its place, while flying
// the Starflight Express, one holds A to land, all shown while the field is
// being walked (a turn button hides while the camera is at its limit that
// way, with the rotation locked, unless the other one is held). Touching
// both turn buttons at once holds L+R, which recenters the camera as on a
// pad. The runner's fast-forward toggle shows at the top center whenever
// the option is on. With it on, a touch on either screen (not on these
// buttons) also stops an emote that loops until a face button.
//
// The pad's right stick is L and R too, everywhere in the game: left and
// right hold L and R, pressing it holds both (which recenters the field
// camera); L and R still work. Up and down on it still zoom (Camera zoom).
#include <atomic>
#include <chrono>
#include <cstdint>

#include "title_hooks.h"
#include "title_settings.h"
#include "touch_buttons.h"

#include "dq9_scene.h"

namespace {

constexpr const char* kChoices[] = {"Off", "On"};

constexpr NdsTitleSetting kUnlockRotation = {
    "unlock_camera_rotation",
    "Unlock camera rotation",
    "Lets L and R turn the field camera all the way round. Off, it stops "
    "45 degrees either side, as in the original. L+R still resets it.",
    0, 1, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    0,
};

const int kUnlockRotationId = nds_title_setting_register(&kUnlockRotation);

constexpr const char* kOpacityChoices[] = {"Off", "25%", "50%", "75%",
                                           "100%"};
constexpr int kOpacityValues[] = {0, 25, 50, 75, 100};
static_assert(sizeof(kOpacityChoices) / sizeof(kOpacityChoices[0]) ==
                  sizeof(kOpacityValues) / sizeof(kOpacityValues[0]),
              "choices");

constexpr NdsTitleSetting kCameraButtons = {
    "camera_buttons",
    "Extra buttons opacity",
    "On-screen buttons for play without a pad. Off hides them.",
    0, 100, 25,
    kOpacityChoices, sizeof(kOpacityChoices) / sizeof(kOpacityChoices[0]),
    kOpacityValues,
    0,
};

const int kCameraButtonsId = nds_title_setting_register(&kCameraButtons);

// ---- camera buttons ---------------------------------------------------------

// The field walk: in game (dq9::GameModeRunning), not on the title, not in
// battle or a cutscene (dq9::FieldCutscene: the game's opening, story
// events, where L and R do nothing), no full-screen page, and no 2D window
// up on engine A (every one turns BG2 on: the message box, the camp
// menu; the walk shows BG1 alone). Read at G3_SwapBuffers, where the
// overlay slots are consistent.
constexpr uint32_t kDispcntA = 0x04000000;
constexpr uint32_t kBg2Bg3 = 0x0C00;

bool FieldWalk() {
    return dq9::GameModeRunning() && !dq9::TitleScreen() && !dq9::InBattle() &&
           !dq9::FieldCutscene() && !dq9::FullScreenPage() &&
           (bus_read_u32(kDispcntA) & kBg2Bg3) == 0;
}

// Whether B would start the emote mode now: the tests of 0x02199F08 (run
// by the field's step 0x0218CBD4, overlay 17 in slot 2), on the field
// (0x0218B5B0: the word at 0x021D82E0 + 4) and the hero (ctx 0x020F33D8
// entity, index byte ctx + 14716). Off with a menu or a message up, while
// talking or in an event, and once the emote mode is on.
constexpr uint32_t kCtx = 0x020F33D8;

bool InMainRam(uint32_t address, uint32_t size) {
    return address >= 0x02000000u && address + size <= 0x02400000u;
}

bool InDtcm(uint32_t address, uint32_t size) {
    return address >= 0x027E0000u && address + size <= 0x027E4000u;
}

bool EmoteReady() {
    if (!dq9::FieldLoaded()) return false;
    const uint32_t field = bus_read_u32(0x021D82E0 + 4);
    if (!InMainRam(field, 16516)) return false;
    // The field's input state (0x02046B18) is idle, no emote mode yet.
    const uint32_t state = bus_read_u32(field + 14076);
    if (!InMainRam(state, 4) || bus_read_u32(state) != 0) return false;
    if (bus_read_u32(field + 16512) != 0) return false;
    const uint8_t index = bus_read_u8(kCtx + 14716);
    if (index >= 233) return false;
    const uint32_t hero = bus_read_u32(kCtx + 8 + 4u * index);
    if (!InMainRam(hero, 612)) return false;
    return (bus_read_u32(hero + 196) & 0x7FFF) == 0 &&  // 0x0203403C
           bus_read_u32(hero + 608) == 0xFFFFFFFFu &&    // no emote
           bus_read_u8(hero + 594) != 0 &&               // 0x02039830
           (bus_read_u32(hero + 396) & 1) == 0 &&
           static_cast<int8_t>(bus_read_u8(hero + 380)) <= 0 &&  // 0x020535D8
           (bus_read_u32(hero + 172) & 0xFFFF) == 0;
}

// The camera's yaw limits, with the rotation locked: the field camera
// (ctx 0x020F33D8 + 0x3B0, 0x020100CC; it lives in DTCM, at 0x027E0000)
// keeps its yaw at +0x70 (0x0202E800),
// in [0, 25736), and 0x020A213C clamps one landing in the band
// (0xC91, 0x57F7) to its edge: L, which adds, stops at 0xC91 and R at
// 0x57F7. Halfway round tells which edge a yaw in the band is at.
constexpr int32_t kYawLeftEdge = 0xC91;
constexpr int32_t kYawRightEdge = 0x57F7;
constexpr int32_t kYawHalf = 25736 / 2;

enum : uint8_t { kAtLeftLimit = 1, kAtRightLimit = 2 };

uint8_t CameraAtLimit() {
    if (nds_title_setting_value(kUnlockRotationId)) return 0;
    const uint32_t camera = bus_read_u32(kCtx + 0x3B0);
    if (!InDtcm(camera, 0x74)) return 0;
    const int32_t yaw = static_cast<int32_t>(bus_read_u32(camera + 0x70));
    if (yaw >= kYawLeftEdge && yaw < kYawHalf) return kAtLeftLimit;
    if (yaw <= kYawRightEdge && yaw >= kYawHalf) return kAtRightLimit;
    return 0;
}

// Flying the Starflight Express: the sky is map 10100 (the current map id
// at 0x020FB3F0, as the Discord activity reads it; the sea is 10000).
// There A lands and B (or any tap on the touchscreen) asks to go inside
// the train, so without a pad there is no way down. Shown while the
// field's input state is idle, as for EmoteReady: not with the "go
// inside?" prompt or an event up.
constexpr uint32_t kMapId = 0x020FB3F0;
constexpr uint16_t kSkyMap = 10100;

bool FlightReady() {
    if (bus_read_u16(kMapId) != kSkyMap) return false;
    const uint32_t field = bus_read_u32(0x021D82E0 + 4);
    if (!InMainRam(field, 16516)) return false;
    const uint32_t state = bus_read_u32(field + 14076);
    return InMainRam(state, 4) && bus_read_u32(state) == 0;
}

std::atomic<bool> g_field_at_swap{false};
std::atomic<bool> g_flight_ready_at_swap{false};
std::atomic<uint8_t> g_camera_limit_at_swap{0};
std::atomic<bool> g_emote_ready_at_swap{false};
// KEYINPUT as the guest sees it (bits low while held).
std::atomic<uint16_t> g_keys_at_swap{0x03FF};
std::atomic<uint32_t> g_swaps{0};

// On the frontend thread, once per frame. The sample only holds while 3D
// frames keep coming: a scene without them (or a paused game) hides the
// buttons. A scene change stops and restarts them in bursts, so the
// buttons hide at once but show again only once the field has held for a
// while, or they would flicker through it.
bool CameraButtonsShown() {
    using Clock = std::chrono::steady_clock;
    constexpr auto kStale = std::chrono::milliseconds(250);
    constexpr auto kSettle = std::chrono::milliseconds(500);
    static uint32_t seen = 0;
    static Clock::time_point last_swap{};
    static Clock::time_point held_since{};
    static bool holding = false;
    if (!nds_title_setting_value(kCameraButtonsId)) {
        holding = false;
        return false;
    }
    const Clock::time_point now = Clock::now();
    const uint32_t swaps = g_swaps.load(std::memory_order_relaxed);
    if (swaps != seen) {
        seen = swaps;
        last_swap = now;
    }
    const bool field = now - last_swap < kStale &&
                       g_field_at_swap.load(std::memory_order_relaxed);
    if (!field) {
        holding = false;
        return false;
    }
    if (!holding) {
        holding = true;
        held_since = now;
    }
    return now - held_since >= kSettle;
}

constexpr uint16_t kKeyB = 1u << 1;
constexpr uint16_t kKeyR = 1u << 8;
constexpr uint16_t kKeyL = 1u << 9;

bool KeyHeld(uint16_t key) {
    return (g_keys_at_swap.load(std::memory_order_relaxed) & key) == 0;
}

// A turn button at its limit still shows while the other is held, so a
// second finger can land on it for L+R.
bool TurnLeftShown() {
    return CameraButtonsShown() &&
           (!(g_camera_limit_at_swap.load(std::memory_order_relaxed) &
              kAtLeftLimit) ||
            KeyHeld(kKeyR));
}

bool TurnRightShown() {
    return CameraButtonsShown() &&
           (!(g_camera_limit_at_swap.load(std::memory_order_relaxed) &
              kAtRightLimit) ||
            KeyHeld(kKeyL));
}

const NdsTouchButtonSpec kTurnLeft = {
    {
        "...........",
        "....#......",
        "...##......",
        "..#######..",
        ".########..",
        "..#######..",
        "...##......",
        "....#......",
        "...........",
    },
    NdsTouchButtonPlace::LeftMiddle,
    kKeyL,
    TurnLeftShown,
};

const NdsTouchButtonSpec kTurnRight = {
    {
        "...........",
        "......#....",
        "......##...",
        "..#######..",
        "..########.",
        "..#######..",
        "......##...",
        "......#....",
        "...........",
    },
    NdsTouchButtonPlace::RightMiddle,
    kKeyR,
    TurnRightShown,
};

// Holding B in the field (0x02199F08 starts the emote mode) brings up the
// emote panel on the touchscreen, and with B held a D-pad press performs
// one at once (the hero's step 0x02037D98: up, left, right, then down).
// The panel's four rows (0x0219A388; tables at 0x021D6448..) are 2 tiles
// high, a cross in the lower half of the screen, in tiles:
//   up    x 9,  y 17, 14 wide      left  x 2,  y 19, 14 wide
//   right x 16, y 19, 14 wide      down  x 8,  y 21, 16 wide
// A tap presses the direction of the row it hits, else of the nearest
// row's middle (rows are much wider than high, so height counts more).
uint16_t EmotePick(int x, int y) {
    struct Row {
        int x, y, w;  // DS pixels; 16 high
        uint16_t key;
    };
    static constexpr Row kRows[] = {
        {72, 136, 112, 1u << 6},   // up
        {16, 152, 112, 1u << 5},   // left
        {128, 152, 112, 1u << 4},  // right
        {64, 168, 128, 1u << 7},   // down
    };
    constexpr int kRowHeight = 16;
    for (const Row& row : kRows)
        if (x >= row.x && x < row.x + row.w && y >= row.y &&
            y < row.y + kRowHeight)
            return row.key;
    uint16_t best = 0;
    int best_distance = 0;
    for (const Row& row : kRows) {
        const int dx = x - (row.x + row.w / 2);
        const int dy = (y - (row.y + kRowHeight / 2)) * 4;
        const int distance = dx * dx + dy * dy;
        if (!best || distance < best_distance) {
            best = row.key;
            best_distance = distance;
        }
    }
    return best;
}

bool EmoteShown() {
    return CameraButtonsShown() &&
           g_emote_ready_at_swap.load(std::memory_order_relaxed);
}

const NdsTouchButtonSpec kEmote = {
    {
        // A smiling face.
        "...........",
        "...#####...",
        "..#.....#..",
        ".#..#.#..#.",
        ".#.......#.",
        ".#.#...#.#.",
        ".#..###..#.",
        "..#.....#..",
        "...#####...",
    },
    NdsTouchButtonPlace::BottomCenter,
    kKeyB,
    EmoteShown,
    // B stays held after the tap, while an emote is picked on the
    // touchscreen, which presses its direction.
    true,
    EmotePick,
};

bool LandShown() {
    return CameraButtonsShown() &&
           g_flight_ready_at_swap.load(std::memory_order_relaxed);
}

constexpr uint16_t kKeyA = 1u << 0;

// In the emote button's place, which never shows in the sky.
const NdsTouchButtonSpec kLand = {
    {
        // An arrow down onto the ground.
        "...........",
        ".....#.....",
        ".....#.....",
        ".....#.....",
        "...#####...",
        "....###....",
        ".....#.....",
        "...........",
        ".#########.",
    },
    NdsTouchButtonPlace::BottomCenter,
    kKeyA,
    LandShown,
};

// Some emotes loop until A, B, X or Y is pressed (0x02053644, see the hook
// in dq9_arm9.toml), which a touchscreen alone cannot do: with the extra
// buttons on, a new touch on either screen stops them too. A press on an
// extra button is neither (the frontend keeps it from the guest and from
// the top screen's clicks), so the camera still turns meanwhile.
//
// The hero's emote: parts at +380 (1, one emote), phase at +382 (2, the
// loop).
bool HeroEmoteLooping() {
    const uint8_t index = bus_read_u8(kCtx + 14716);
    if (index >= 233) return false;
    const uint32_t hero = bus_read_u32(kCtx + 8 + 4u * index);
    return InMainRam(hero, 612) && bus_read_u8(hero + 380) == 1 &&
           bus_read_u8(hero + 382) == 2;
}

unsigned TopClicks() {
    int x = 0, y = 0;
    return nds_title_top_click(&x, &y);
}

// The top screen's click count once stopping was last possible: only
// clicks after it stop the emote. Emulation thread.
unsigned g_top_clicks_seen = 0;

// The touch manager: +85 a press began this frame.
constexpr uint32_t kTouch = 0x02114E54u;

// The right stick is L and R (nds_title_camera_stick), always.
bool CameraStickActive() { return true; }

bool FastForwardShown() {
    return nds_title_setting_value(kCameraButtonsId) != 0;
}

int ButtonsOpacity() { return nds_title_setting_value(kCameraButtonsId); }

bool RegisterButtons() {
    const int ids[] = {
        nds_touch_button_register(&kTurnLeft),
        nds_touch_button_register(&kTurnRight),
        nds_touch_button_register(&kEmote),
        nds_touch_button_register(&kLand),
        nds_touch_button_fast_forward(),
    };
    nds_touch_button_set_query(nds_touch_button_fast_forward(),
                               FastForwardShown);
    for (const int id : ids)
        nds_touch_button_set_opacity_query(id, ButtonsOpacity);
    return true;
}

const bool kButtonsRegistered = RegisterButtons();

const bool kCameraStickBound =
    (nds_title_set_camera_stick(CameraStickActive, kKeyL, kKeyR), true);

}  // namespace

namespace dq9 {

void SampleCameraButtons() {
    const bool field = FieldWalk();
    g_field_at_swap.store(field, std::memory_order_relaxed);
    g_camera_limit_at_swap.store(field ? CameraAtLimit() : 0,
                                 std::memory_order_relaxed);
    g_emote_ready_at_swap.store(field && EmoteReady(),
                                std::memory_order_relaxed);
    g_flight_ready_at_swap.store(field && FlightReady(),
                                 std::memory_order_relaxed);
    g_keys_at_swap.store(bus_read_u16(0x04000130), std::memory_order_relaxed);
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    // Top screen clicks from before the emote's loop do not stop it.
    if (!HeroEmoteLooping()) g_top_clicks_seen = TopClicks();
}

}  // namespace dq9

// Before `cmp r6, r1` at 0x020A2364 (L) and 0x020A23C8 (R): true jumps
// past the yaw clamp to the call that sets the yaw.
NDS_TITLE_HOOK bool Dq9UnlockCameraRotation() {
    return nds_title_setting_value(kUnlockRotationId) != 0;
}

// At 0x02053AA4, the looping emote's key test on the hero: true takes the
// press's path, which ends it.
NDS_TITLE_HOOK bool Dq9TouchStopsEmote() {
    const unsigned clicks = TopClicks();
    const bool clicked = clicks != g_top_clicks_seen;
    g_top_clicks_seen = clicks;
    if (!nds_title_setting_value(kCameraButtonsId)) return false;
    return clicked || bus_read_u8(kTouch + 85u) != 0;
}
