// UI size: shows DQ9's windows (message boxes, menus) at a fixed size in
// window points instead of stretching them with the screen, so a big window
// shows more of the scene around them (Game > UI size, [title] ui_size in
// settings.toml). See config/hooks/dq9_arm9.toml for the guest code.
//
// The size is a multiple of the DS screen: at "2x" the UI is as big as in a
// 384-point-tall window (the default 2x window), whatever the window size.
// The scale k is (192 * n) / the main screen's height in window points, and
// never above 1: a window smaller than that shows the UI as on the DS.
// Everything scales about the screen centre (DS x 128, y 96), so the layout
// between the windows stays as on the DS and the UI stays in the middle of
// the scene. "Scale with window" turns it off. The netplay chat overlay
// (chat_overlay.h) takes the same scale.
//
// DQ9 draws its windows in two ways, handled separately below:
//   * 3D: the message manager in 3D mode draws its window with the 3D
//     engine, in a pixel-space pass (field and battle).
//   * 2D: the message manager in 2D mode and the camp menu draw into the
//     main engine's text BGs.
#include <cstdint>

#include "chat_overlay.h"
#include "gpu2d.h"
#include "gpu3d.h"
#include "title_hooks.h"
#include "title_settings.h"

#include "dq9_scene.h"

namespace {

// Auto is the last value so the saved 0..4 keep their meaning.
constexpr int kAuto = 5;
constexpr const char* kChoices[] = {"Scale with window", "1x", "2x", "3x",
                                    "4x", "Auto"};

constexpr NdsTitleSetting kUiSize = {
    "ui_size",
    "UI size",
    "Shows the text boxes and menus at a fixed size, as big as on a screen "
    "this many times the DS screen, so a big window shows more of the scene "
    "around them. Scale with window stretches them with the screen, as on "
    "the DS. Auto follows the window: half its height, never under 2x or "
    "over 4x. The 3D text boxes shrink only with a 3D resolution above 1x.",
    0, 5, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    kAuto,
};

const int kUiSizeId = nds_title_setting_register(&kUiSize);

// The scale about the screen centre: 1 = unchanged.
double UiScale() {
    const int multiple = nds_title_setting_value(kUiSizeId);
    if (multiple <= 0) return 1.0;
    const int points = nds_title_display_height();
    double reference = 192.0 * multiple;
    if (multiple == kAuto) {
        // Half the window's height, within 2x..4x of the DS screen.
        reference = points / 2.0;
        if (reference < 192.0 * 2) reference = 192.0 * 2;
        if (reference > 192.0 * 4) reference = 192.0 * 4;
    }
    if (points <= reference) return 1.0;
    return reference / static_cast<double>(points);
}

}  // namespace

double dq9::UiScale() { return ::UiScale(); }

namespace {

// The netplay chat drawn over the game follows the same size.
const bool kChatScale = (nds_chat_overlay_set_scale_source(&UiScale), true);

constexpr int kAnchorX = 128;
constexpr int kAnchorY = 96;

// Full-screen pages (dq9::FullScreenPage) are left as on the DS: there is
// no window to keep small. Splitting their BG1 wallpaper off would shrink it
// alone and cover the 3D blended over it, and Stella's 3D speech box would
// leave her behind.
using dq9::FullScreenPage;

// Only in the game proper (field, battle, camp menu, a new game's prologue).
// The boot sequence and the title menu draw their art on the same BGs and
// are left as on the DS. dq9::GameModeRunning tells the game's main loop
// modes from the boot and title screens' (mode 6). Overlay 17 itself is
// already loaded on the title, so it cannot tell them apart.
//
// Continue sets mode 0 at once, but the title's fade-out still runs from
// overlay 4 after that, so the game also needs the title's overlay 4 gone
// (dq9::TitleScreen). Overlay 4 alone does not tell: grottoes load it in
// the same slot too, from the field.
bool InGame() {
    return dq9::GameModeRunning() && !dq9::TitleScreen() && !FullScreenPage();
}

}  // namespace

// ---- 3D message window -----------------------------------------------------
// The field (and battle) draw sets an orthographic projection of the DS
// screen (G3_OrthoW 0..256 x 0..192, near -1024, far 1024) and then
// 0x020444F0 draws the message manager. Every piece of the window (the
// frame's border cells, the dark background, each glyph, the inline choice
// list) pushes the position matrix, translates to its pixel position at
// z = 1024 (the near plane) and pops it again, so all of it is relative to
// the position matrix current when the manager is drawn.
//
// Dq9TextBoxBegin pushes that matrix and multiplies the scale about the
// centre into it, Dq9TextBoxEnd pops it. Only the message window shrinks:
// fades, HUD sprites and emotion balloons in the same pass are drawn outside
// that call. Two limits:
//   * Only while the 3D renders above 1x. The game's own geometry changes, so
//     the native image shrinks too, and at 1x (or with HD off) a 4K window
//     would show a box of a few dozen DS pixels.
//   * Never below 1 / the 3D resolution scale, so a glyph's 16 texels still
//     cover at least 16 raster pixels and the text stays sharp. With a low 3D
//     resolution in a big window the box stops shrinking there.
// The CPU composite (shown while the runtime menu is open, for example)
// draws the same geometry at 1x, so there the shrunk box is blurry.
//
// The push and pop are sent whatever the setting, so they always pair up
// even when the setting or the window changes between the two hooks; the
// pair alone leaves the image unchanged. Nested pairs (a choice list drawn
// by the message window) scale only once.
//
// These hooks write GX matrix commands (0x04000444 and on). That is what the
// game itself does around each piece of the window, through the same FIFO;
// the hooks add at most four commands a frame.
namespace {

constexpr int32_t kOne = 0x1000;  // fx32 1.0

// GX matrix ports.
constexpr uint32_t kMtxPush = 0x04000444;
constexpr uint32_t kMtxPop = 0x04000448;
constexpr uint32_t kMtxScale = 0x0400046C;
constexpr uint32_t kMtxTrans = 0x04000470;

// The 3D window's scale, fx32 (kOne = unchanged).
int32_t TextBoxScale() {
    const int raster = nds_gpu3d_internal_scale();
    // On the title (its prompts, the adventure log delete confirmation
    // among them, use the 3D window too) the UI stays as on the DS, like the
    // 2D layers below.
    if (raster <= 1 || !InGame()) return kOne;
    int32_t k = static_cast<int32_t>(UiScale() * kOne + 0.5);
    const int32_t sharp = (kOne + raster - 1) / raster;
    if (k < sharp) k = sharp;
    return k < kOne ? k : kOne;
}

// Multiplies the scale about the centre into the current position matrix.
void ScaleAboutCentre() {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    // p' = a + k (p - a): translate by a (1 - k), then scale by k.
    bus_write_u32(kMtxTrans, static_cast<uint32_t>(kAnchorX * (kOne - k)));
    bus_write_u32(kMtxTrans, static_cast<uint32_t>(kAnchorY * (kOne - k)));
    bus_write_u32(kMtxTrans, 0);
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(kOne));
}

// How many wrapped draws are open. The message window's draw 0x02066D00
// draws its own choice list through 0x0205C97C (0x02066EA0 ->
// 0x02044540), which is wrapped too: only the outermost one scales, or the
// list would shrink twice.
int g_wrap_depth = 0;

}  // namespace

// Before each `bl 0x02066D00` in 0x020444F0 (r4 = message manager): push the
// position matrix and scale the window about the centre.
NDS_TITLE_HOOK void Dq9TextBoxBegin(uint32_t*) {
    bus_write_u32(kMtxPush, 0);
    if (g_wrap_depth++ == 0) ScaleAboutCentre();
}

// The interaction prompt: the icon shown when the player can talk to an NPC
// or check an object. ov17 0x0219B33C keeps the best target's icon index at
// field+0x36B5, and 0x0219AD84 (same pixel-space pass, right after the
// message window) draws that icon sprite (field + 0x2B90 + 0x88 * index) at
// its fixed screen position: the game pushes the matrix (0x0219B084), steps
// the animation (0x0204725C), draws (0x020473D8, which translates to the
// sprite's position and scales relative to the current matrix) and pops
// (0x0219B0A0). The hook, at the animation step, scales the pushed matrix
// about the centre like the message window; the game's own pop undoes it.
NDS_TITLE_HOOK void Dq9InteractPrompt() {
    ScaleAboutCentre();
}

// The prompt's touch area (ov17 0x0219AA7C) is tested against the icon's
// unscaled rect, but the icon is drawn shrunk about the centre: the touch
// (field walk: read 1:1) is mapped back, p -> a + (p - a) / k, so the area
// matches what is drawn.
NDS_TITLE_HOOK void Dq9InteractPromptTouch(uint32_t* x, uint32_t* y) {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    auto unscale = [k](uint32_t* v, int anchor) {
        const int64_t d = static_cast<int32_t>(*v) - anchor;
        *v = static_cast<uint32_t>(anchor + d * kOne / k);
    };
    unscale(x, kAnchorX);
    unscale(y, kAnchorY);
}

// Icons the field draws over a world point (the harvest spot's sprite, among
// others): ov17 projects the point to the screen (0x0202EC94), stores the
// sprite's position minus (12, 40) px at +0x1C / +0x20 (fx32) and draws it
// with 0x020473D8, which pushes and pops by itself. Begin (before that call,
// r0 = the sprite) pushes the position matrix and scales about the sprite's
// own anchor point (+12, +40 px), so it stays on its point; End (right after)
// pops. Both are always sent, so they pair. The harvest spot's sprite is
// drawn at two sites (ov17 0x021AED10, rising, and 0x021AEDD4, its last
// frames in place), both hooked so it stays scaled until it is freed.
NDS_TITLE_HOOK void Dq9WorldIconBegin(uint32_t* sprite) {
    bus_write_u32(kMtxPush, 0);
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    // The stored position is the projected point minus (12, 40) px (its top
    // left corner): scale about the point itself, so the icon keeps sitting on
    // its spot instead of drifting towards its corner.
    const int64_t ax =
        static_cast<int32_t>(bus_read_u32(*sprite + 0x1C)) + (12 << 12);
    const int64_t ay =
        static_cast<int32_t>(bus_read_u32(*sprite + 0x20)) + (40 << 12);
    bus_write_u32(kMtxTrans, static_cast<uint32_t>(ax * (kOne - k) / kOne));
    bus_write_u32(kMtxTrans, static_cast<uint32_t>(ay * (kOne - k) / kOne));
    bus_write_u32(kMtxTrans, 0);
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(kOne));
}

NDS_TITLE_HOOK void Dq9WorldIconEnd() {
    bus_write_u32(kMtxPop, 1);
}

namespace {
// A choice window was drawn (0x0205C97C) since the last G3_SwapBuffers.
bool g_choice_drawn = false;
}  // namespace

// After the prologue of the choice window draw 0x0205C97C(list, ...): the
// same push and scale as Dq9TextBoxBegin. When the list is shown (list+0x233
// and +0x232, the draw's own test), its cursor, an OBJ, is drawn this frame
// too: Dq9WindowLayers then scales the OBJs with the window.
NDS_TITLE_HOOK void Dq9ChoiceWindowBegin(uint32_t* list) {
    Dq9TextBoxBegin(nullptr);
    if (bus_read_u8(*list + 0x233) != 0 && bus_read_u8(*list + 0x232) != 0)
        g_choice_drawn = true;
}

// Before each `pop {r4, pc}` that follows a Dq9TextBoxBegin, and at the
// exit of the choice window draw 0x0205C97C (also wrapped).
NDS_TITLE_HOOK void Dq9TextBoxEnd() {
    bus_write_u32(kMtxPop, 1);
    if (g_wrap_depth > 0) --g_wrap_depth;
}

// The field's stylus arrow (0x02096C48, see config/hooks/dq9_arm9.toml):
// shrunk like the 3D window, but each piece about its own anchor, so it
// still runs from the press point to the pen. Every scale is written right
// after one of the game's pushes, so the game's pops undo it.
NDS_TITLE_HOOK void Dq9StylusArrowPiece() {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(kOne));
}

// The shaft: thinner only, its length still reaches the head.
NDS_TITLE_HOOK void Dq9StylusArrowShaft() {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    bus_write_u32(kMtxScale, static_cast<uint32_t>(k));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(kOne));
    bus_write_u32(kMtxScale, static_cast<uint32_t>(kOne));
}

// r0 = len - 5 px, the shaft's length under the head: the shrunk head
// covers only 5k px of it.
NDS_TITLE_HOOK void Dq9StylusArrowLength(uint32_t* length) {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    *length = static_cast<uint32_t>(static_cast<int32_t>(*length) +
                                    5 * (kOne - k));
}

// r0 = the pen's distance from the press point, about to be compared with
// 8 px: dividing it by k makes the dead zone 8k px.
NDS_TITLE_HOOK void Dq9StylusDeadZone(uint32_t* distance) {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    const int64_t d = static_cast<int32_t>(*distance);
    const int64_t scaled = d * kOne / k;
    *distance = static_cast<uint32_t>(scaled > INT32_MAX ? INT32_MAX
                                                         : scaled);
}

// r0 = 8 px, the arrow's length inside the dead zone: 8k px with it.
NDS_TITLE_HOOK void Dq9StylusArrowStub(uint32_t* length) {
    const int32_t k = TextBoxScale();
    if (k >= kOne) return;
    *length = static_cast<uint32_t>(
        static_cast<int64_t>(static_cast<int32_t>(*length)) * k / kOne);
}

// ---- 2D windows ------------------------------------------------------------
// The other windows are text BGs on the main engine, and the presenter
// scales those itself (nds_gpu2d_set_fixed_size_layers): the BGs are shown
// at k of the game rect about the centre, their native pixels magnified n
// times instead of filling the screen. That needs no 3D resolution above 1x.
//   * The message manager's 2D mode (byte mgr+0x19CC, chosen per message by
//     the open calls 0x02044F4C / 0x0204501C): 0x020439C0 renders the box
//     into BG2 (translucent background, blended 10/6 over the 3D) and BG3
//     (text) at the fixed rect (2, 116, 252, 74), and turns BG2 and BG3 on.
//     NPC talk and the post-battle results use it.
//   * The camp menu: window backgrounds on BG1 (blended over the 3D), frames
//     and text on BG2.
// In the field and in battle BG1 is on but empty, so the mask is simply
// BG1..BG3; the BGs that are off are left alone.
//
// Set once per 3D frame, at G3_SwapBuffers; the presenter latches it when
// it composes the frame. Presentation only: nothing reaches the guest.
namespace {

// BG1..BG3, and the OBJs: the menu cursors are OBJs drawn over the windows
// (the presenter takes OBJs only on frames that split a BG).
constexpr uint8_t kWindowBgs = 0x1E;
// The OBJs alone.
constexpr uint8_t kWindowObjs = 0x10;

// Whether a 2D window is up on engine A: every one turns BG2 on (the 2D
// message box uses BG2 + BG3, the camp menu BG1 + BG2), while the field
// walk and battle show only BG1, on but empty.
bool Window2dShown() {
    constexpr uint32_t kDispcntA = 0x04000000;
    constexpr uint32_t kBg2Bg3 = 0x0C00;
    return (bus_read_u32(kDispcntA) & kBg2Bg3) != 0;
}

}  // namespace

// G3_SwapBuffers entry, once per 3D frame.
NDS_TITLE_HOOK void Dq9WindowLayers() {
    dq9::SampleScreenLayout();
    dq9::SampleCameraButtons();
    const bool choice = g_choice_drawn;
    g_choice_drawn = false;
    const double scale = UiScale();
    const int32_t choice_scale = TextBoxScale();
    if (scale >= 1.0 || !InGame()) {
        nds_gpu2d_set_fixed_size_layers(0, 1.0f, 0, 0);
        nds_title_set_unscaled_touch_rect(0, 0, 0, 0);
        nds_title_set_touch_unscaled(false);
    } else if (choice && !Window2dShown() && choice_scale < kOne) {
        // A 3D choice window with no 2D window up (Yes/No in the field's
        // message window): its cursor is an OBJ and no BG is split, so the
        // OBJs are split alone, at the 3D window's own scale (it can stop
        // shrinking before the 2D layers do). The touchscreen maps the
        // mouse back through the same scale, onto the list's entries.
        nds_gpu2d_set_fixed_size_layers(
            kWindowObjs, static_cast<float>(choice_scale) / kOne, kAnchorX,
            kAnchorY);
        nds_title_set_unscaled_touch_rect(176, 0, 256, 40);
        nds_title_set_touch_unscaled(false);
    } else {
        nds_gpu2d_set_fixed_size_layers(kWindowBgs, static_cast<float>(scale),
                                        kAnchorX, kAnchorY);
        // The touchscreen's Menu button (top right) is not part of the
        // scaled layers: it stays where it is drawn, so a click there must
        // not be mapped back through the scale. The corner is generous; the
        // frontend only honours it where the scaled UI is not shown.
        nds_title_set_unscaled_touch_rect(176, 0, 256, 40);
        // With no 2D window up (the field walk) the whole touchscreen is
        // the walking stick, drawn and read at the DS's full size: the
        // split layers are empty, so a touch must not be pulled through
        // their scale towards the centre. The treasure search shows its
        // HUD on BG2 but still walks under the stick.
        nds_title_set_touch_unscaled(!Window2dShown() ||
                                     dq9::TreasureSearch());
    }
}
