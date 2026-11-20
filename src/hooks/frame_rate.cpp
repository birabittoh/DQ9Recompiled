// 60 FPS mode (Game > 60 FPS mode, [title] fps_mode in settings.toml, Off
// by default). It replaces the separate "3D frame rate" setting and the
// runner's Smooth 3D row, which the smoothing query below hides.
//
//   Off       30 images a second, as the DS.
//   Fast      30 images, and the runner's 3D smoothing (gpu3d_interp.h)
//             shows the halfway image on each free frame: 60 on screen for
//             the host cost of 30, 3D one frame late, may glitch.
//   Accurate  the game draws 60 (below); needs about twice the host time.
//
// Accurate lets the game's main loop draw a 3D image on every VBlank instead
// of every second one.
//
// The loop (ov17 0x0218B710, the field, towns and battles) ends each pass
// with
//
//     for (i = 1; i < 2 - ctx->vblanks; ++i) OS_WaitVBlankIntr();
//     G3_SwapBuffers(); OS_WaitVBlankIntr();
//
// where ctx (0x020F33D8) + 0x3C8 counts the VBlanks since the last pass (the
// VBlank handler at 0x02012BE8 adds one). A pass that took less than a
// VBlank so waits for a second one: 30 images a second, 20 when a pass took
// more than two. The game then times its own logic from what the pass took
// (0x02010160: milliseconds at ctx+0x3B4/+0x3B8, VBlanks at ctx+0x3C4, about
// 250 readers), so it already runs at the right speed at 30 and at 20.
//
// The hook replaces the 2 with a 1: no extra wait, so a pass that fits in
// one VBlank is shown at the next one and the game sees a 16 ms pass. A pass
// that takes longer is paced as before. See config/hooks/dq9_arm9_ov017.toml.
//
// On the DS's ARM9 a pass takes about one VBlank, so the rate would flip
// between 60 and 30. 60 FPS therefore also runs the emulated ARM9 at twice
// its hardware clock (nds_title_set_arm9_overclock), which holds 60 in towns
// too; timers, sound and the ARM7 keep the hardware's timing. Off and Fast
// put the hardware clock back.
//
// While a dialogue box is open the loop also reads the cartridge on every
// pass (the polling read at 0x020D0BF8: about three quarters of the ARM9's
// time went there, and the rate fell to 30). The card is not on the ARM9's
// clock, so 60 FPS makes its transfers 16 times as fast as well
// (nds_title_set_card_speedup).
//
// Twice the passes is about twice the host work: a town took 16 ms of the
// 16.7 ms frame on an i7-13650HX and a dialogue box 17.7, so the host fell
// to 52 fps. 60 FPS therefore also lets the scheduler run the ARM9 in longer
// stretches while the ARM7 sleeps (nds_title_set_event_windows), which cuts
// the cost of each frame.
//
// While the text of a 2D message window is appearing, the game redoes the
// whole window on every pass, which at 60 FPS still cost about 2 ms a frame
// on that host. Dq9TextWindowHalfRate skips that redraw on every other pass
// of the main loop, so the window is redrawn 30 times a second as in the
// original while 3D stays at 60 (except while a choice list is shown, and
// in a new game's opening).
//
// The hooks that only do the guest's work natively (heap lookup, kerning,
// 4bpp blit, GXFIFO send, geometry command buffer, box-test wait) run in
// every mode: Off in a town needed 20 ms a frame on a Snapdragon 7s Gen 3
// without them. The pacing, clocks, event windows and the half-rate text
// window stay Accurate-only.
#include "gpu3d.h"
#include "io.h"
#include "title_hooks.h"
#include "title_settings.h"

#include "dq9_scene.h"

namespace {

enum FpsMode { kOff, kFast, kAccurate };

constexpr const char* kChoices[] = {"Off", "Fast", "Accurate"};

constexpr NdsTitleSetting kFpsMode = {
    "fps_mode",
    "60 FPS mode",
    "Fast blends images in between (3D a frame late, may glitch). "
    "Accurate draws them all; needs a fast PC.",
    kOff, kAccurate, 1,
    kChoices, sizeof(kChoices) / sizeof(kChoices[0]), nullptr,
    kOff,
};

const int kFpsModeId = nds_title_setting_register(&kFpsMode);

// The game draws on every VBlank (Accurate).
bool Draw60() { return nds_title_setting_value(kFpsModeId) == kAccurate; }

// The runner smooths 3D while this is true (title_settings.h).
bool Smooth() { return nds_title_setting_value(kFpsModeId) == kFast; }

const bool kSmoothRegistered = (nds_title_set_smooth_3d_query(Smooth), true);

// The return address of the main loop's call of 0x020439C0 (ov17
// 0x0218C910): the other callers run their own loops.
constexpr uint32_t kMainLoopTextDraw = 0x0218C914u;

// The window manager's choice list: 0x02044540 (called from the redraw,
// 0x02066D00) draws it and its hand cursor when the state at +0x9A0 is 6 and
// the byte at +0x19BA is set.
constexpr uint32_t kWindowState = 0x9A0u;
constexpr uint32_t kChoiceState = 6u;
constexpr uint32_t kChoiceShown = 0x19BAu;

// The last main-loop pass skipped the 2D window's redraw (or, with a choice
// list up, its tile upload).
bool g_text_window_skipped = false;
// This call of 0x020439C0 runs the block but skips the tile upload.
bool g_skip_tile_upload = false;

}  // namespace

// Before `b 0x0218C798` at 0x0218C78C: r0 = VBlanks since the last pass,
// r7 = 2 - r0 = the loop's bound (it waits while 1 <= i < r7).
NDS_TITLE_HOOK void Dq9FrameRate(uint32_t* r0, uint32_t* r7) {
    const bool draw60 = Draw60();
    nds_title_set_arm9_overclock(draw60 ? 2 : 1);
    nds_title_set_card_speedup(draw60 ? 16 : 1);
    nds_title_set_event_windows(draw60);
    if (draw60) *r7 = 1u - *r0;
}

namespace {

// ctx + 0x3B8: the milliseconds the last pass took (0x02010160).
constexpr uint32_t kPassMs = 0x3B8u;

}  // namespace

// The battle's slow motion (a reflected spell, Bounce, Mirror Armour...):
// ov025 starts it through 0x02163440(battle, scale, length ms, delay ms),
// e.g. scale 0xCC (5% speed) for 500 ms. Every pass, ov000 0x02160620 works
// out the real time that passed as
//
//     (int)((float)ctx[0x3B4] / (scale / 4096.0f))
//
// where ctx+0x3B4 is the pass's milliseconds times the scale, already cut to
// an integer (0x02010160). At 30 images a second that is (int)(33 * 0.05) =
// 1, so 20 ms; at 60 it is (int)(16 * 0.05) = 0, the slow motion never runs
// out and the battle stays frozen. At 60 FPS the hook gives the real
// milliseconds of the pass (ctx+0x3B8) instead.
//
// Before `add r2, r7, #0x6E00` at 0x02160A10, right after the float to int
// call: r0 = the milliseconds counted, r4 = ctx.
NDS_TITLE_HOOK void Dq9BattleSlowMotionElapsed(uint32_t* r0, uint32_t* r4) {
    if (Draw60()) *r0 = bus_read_u32(*r4 + kPassMs);
}

// Before `beq 0x02043E94` at 0x020439DC: r2 != 0 while the 2D message window
// is up, r10 = the window manager, lr = the caller. True takes the branch past
// the window's redraw. Never two passes in a row, so the window is at most one
// pass (16 ms) late. While a choice list is shown the block runs on every
// pass, since it places the hand cursor (a sprite) and skipping it made the
// cursor vanish on every other frame; on those passes only the tile upload is
// skipped (Dq9TextWindowSkipUpload). A new game's opening also runs it on
// every pass: its 2D prologue shows its lines through this window, and with
// the block skipped every other pass most of them never appeared.
NDS_TITLE_HOOK bool Dq9TextWindowHalfRate(uint32_t* r2, uint32_t* r10,
                                          uint32_t* lr) {
    g_skip_tile_upload = false;
    if (*lr != kMainLoopTextDraw) return false;
    if (!Draw60() || *r2 == 0u || g_text_window_skipped ||
        dq9::NewGameOpening()) {
        g_text_window_skipped = false;
        return false;
    }
    g_text_window_skipped = true;
    const bool choice = bus_read_u32(*r10 + kWindowState) == kChoiceState &&
                        bus_read_u8(*r10 + kChoiceShown) != 0u;
    if (choice) {
        g_skip_tile_upload = true;
        return false;
    }
    return true;
}

// Before `beq 0x02043C98` at 0x02043C2C, later in the same call: r3 = the
// tile buffer's descriptor (0 = none). True takes the branch past the upload;
// VRAM keeps the last pass's tiles.
NDS_TITLE_HOOK bool Dq9TextWindowSkipUpload(uint32_t* r3) {
    const bool skip = g_skip_tile_upload && *r3 != 0u;
    g_skip_tile_upload = false;
    return skip;
}

namespace {

// Literals of the ITCM lookup at 0x01FF88C4: the heap's base and its block
// table (128 entries of 6 bytes: u16 offset, u16, u16 flags, bit 0 = live).
constexpr uint32_t kHeapBaseLiteral = 0x01FF8914u;
constexpr uint32_t kHeapTableLiteral = 0x01FF8918u;
constexpr uint32_t kHeapSpan = 0x5000u;
constexpr uint32_t kHeapEntries = 0x80u;
constexpr uint32_t kHeapEntrySize = 6u;

constexpr uint32_t kCpsrN = 1u << 31;
constexpr uint32_t kCpsrZ = 1u << 30;
constexpr uint32_t kCpsrC = 1u << 29;
constexpr uint32_t kCpsrV = 1u << 28;

// NZCV of `cmp a, b`.
uint32_t CmpFlags(uint32_t a, uint32_t b) {
    const uint32_t d = a - b;
    uint32_t f = 0u;
    if (d & 0x80000000u) f |= kCpsrN;
    if (d == 0u) f |= kCpsrZ;
    if (a >= b) f |= kCpsrC;
    if (((a ^ b) & (a ^ d)) & 0x80000000u) f |= kCpsrV;
    return f;
}

}  // namespace

// At the entry of the ITCM lookup 0x01FF88C4 (r1 = a pointer into the heap):
// returns r0 = the live block entry whose offset is r1 - base, or 0. At
// every mode it runs the search natively and returns true (the bank then does
// `bx lr`), leaving r0-r3 and NZCV as the guest code would.
NDS_TITLE_HOOK bool Dq9HeapBlockLookup(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                       uint32_t* r3, uint32_t* cpsr) {
    const uint32_t nzcv_mask = kCpsrN | kCpsrZ | kCpsrC | kCpsrV;
    const uint32_t off = *r1 - bus_read_u32(kHeapBaseLiteral);
    *r2 = off;
    if (off >= kHeapSpan) {
        *r0 = 0u;
        *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(off, kHeapSpan);
        return true;
    }
    uint32_t entry = bus_read_u32(kHeapTableLiteral);
    for (uint32_t i = 0u; i < kHeapEntries; ++i, entry += kHeapEntrySize) {
        *r1 = bus_read_u16(entry + 4u);
        if ((*r1 & 1u) == 0u) continue;
        *r1 = bus_read_u16(entry);
        if (*r1 == off) {
            *r0 = entry;
            *r3 = i;
            *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(*r1, off);
            return true;
        }
    }
    *r0 = 0u;
    *r3 = kHeapEntries;
    *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(kHeapEntries, kHeapEntries);
    return true;
}

// At the entry of the kerning search 0x020425F4(prev = r0, cur = r1,
// font = r2): r0 = the signed adjustment of the pair (prev & 0xFF) |
// (cur << 8) in the font's table, or 0. It scans natively and
// returns true (the bank then does `bx lr`), leaving r0-r3, r12 and NZCV as
// the guest code would.
NDS_TITLE_HOOK bool Dq9KerningLookup(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                     uint32_t* r3, uint32_t* r12,
                                     uint32_t* cpsr) {
    constexpr uint32_t kFontTable = 0x0210782Cu;
    const uint32_t nzcv_mask = kCpsrN | kCpsrZ | kCpsrC | kCpsrV;
    const uint32_t key = ((*r0 & 0xFFu) | ((*r1 << 24) >> 16)) & 0xFFFFu;
    const uint32_t font = bus_read_u32(kFontTable + (*r2 << 2));
    const uint32_t count = bus_read_u32(font + 8u);
    uint32_t entry = bus_read_u32(font + 0xCu);
    for (uint32_t i = 0u; i < count; ++i, entry += 4u) {
        *r1 = bus_read_u16(entry);
        if (*r1 == key) {
            *r0 = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int8_t>(bus_read_u8(entry + 2u))));
            *r2 = entry;
            *r3 = count;
            *r12 = i;
            *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(*r1, key);
            return true;
        }
    }
    *r0 = 0u;
    *r2 = entry;
    *r3 = count;
    *r12 = count;
    *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(count, count);
    return true;
}

namespace {

int32_t S16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

}  // namespace

// At the entry of the 4bpp blitter 0x0204E1D8: r0 = the buffer descriptor,
// r1 = the source bitmap (one byte per pair of pixels), r2 = x, r3 = y; on the
// stack w, h, the source mask for odd columns and the one for even columns.
// It runs the guest loop natively, step for step, and returns true
// (the bank then does `bx lr`, as the original's pop {pc}). r0, r4-r11 and
// sp are as the guest leaves them; r1, r3 and r12 get their final values.
NDS_TITLE_HOOK bool Dq9Blit4bpp(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                uint32_t* r3, uint32_t* r12, uint32_t* sp) {
    if (*r1 == 0u) return true;
    const uint32_t desc = *r0;
    const int32_t w = static_cast<int32_t>(bus_read_u32(*sp + 0u));
    const int32_t h = static_cast<int32_t>(bus_read_u32(*sp + 4u));
    const uint32_t mask_odd = bus_read_u32(*sp + 8u);
    const uint32_t mask_even = bus_read_u32(*sp + 0xCu);
    const int32_t tiles_x = S16(bus_read_u16(desc + 0xA8u));
    const int32_t tiles_y = S16(bus_read_u16(desc + 0xAAu));
    const uint32_t buf = bus_read_u32(desc + 8u);
    const bool x_odd = (*r2 & 1u) != 0u;
    uint32_t src = *r1;
    int32_t y = static_cast<int32_t>(*r3);
    for (int32_t row = 0; row < h; row = S16(static_cast<uint32_t>(row) + 1u)) {
        const int32_t ty = S16(static_cast<uint32_t>(y >> 3));
        const bool row_ok = ty >= 0 && ty < tiles_y;
        const int32_t y_byte = static_cast<int32_t>(
            static_cast<uint32_t>(y & 7) << 18) >> 16;
        // smulbb: the low halves, signed.
        const int32_t base = S16(static_cast<uint32_t>(ty)) * tiles_x;
        int32_t x = static_cast<int32_t>(*r2);
        for (int32_t col = 0; col < w; col = S16(static_cast<uint32_t>(col) + 1u)) {
            const int32_t tx = S16(static_cast<uint32_t>(x >> 3));
            const bool ok = (tx >= 0 && tx < tiles_x) || row_ok;
            const int32_t tile = S16(static_cast<uint32_t>(base + tx));
            const int32_t tile_bytes =
                static_cast<int32_t>(static_cast<uint32_t>(tile) << 21) >> 16;
            int32_t idx = S16(static_cast<uint32_t>(
                (S16(static_cast<uint32_t>(x & 7) << 16 >> 16) >> 1) + tile_bytes));
            idx = S16(static_cast<uint32_t>(idx + y_byte));
            const uint32_t addr = buf + static_cast<uint32_t>(idx);
            const bool odd_col = (col & 1) != 0;
            if (ok) {
                uint32_t b = bus_read_u8(addr);
                const uint32_t v =
                    bus_read_u8(src) & (odd_col ? mask_odd : mask_even) & 0xFFu;
                if (v != 0u) {
                    if (x_odd) {
                        b = odd_col ? ((b & 0xF0u) | (v >> 4))
                                    : ((b & 0x0Fu) | ((v << 4) & 0xFFu));
                    } else {
                        b = odd_col ? ((b & 0x0Fu) | v) : ((b & 0xF0u) | v);
                    }
                }
                bus_write_u8(addr, static_cast<uint8_t>(b));
            }
            if (odd_col) ++src;
            x = S16(static_cast<uint32_t>(x) + 1u);
        }
        y = S16(static_cast<uint32_t>(y) + 1u);
    }
    *r1 = src;
    *r3 = static_cast<uint32_t>(y);
    *r12 = buf;
    return true;
}

namespace {

constexpr uint32_t kGxFifo = 0x04000400u;

}  // namespace

// At the entry of MI_CpuSend32(src = r0, dest = r1, size = r2), 0x020CA440:
//
//     add r12, r0, r2
//     loop: cmp r0, r12 ; ldmlt r0!, {r2} ; strlt r2, [r1] ; blt loop
//     bx lr
//
// The 3D model renderer pushes its display lists into GXFIFO through it,
// one guest load, store and MMIO write per word: in a town at 60 FPS this
// loop and the bus path under it were the busiest MMIO site of the ARM9.
// The hook feeds the words to the geometry engine natively and
// returns, leaving r0, r2, r12 and NZCV as the loop would. It stops as soon
// as the FIFO stalls (the engine's stall queue drops what is written past
// it): r0 and r2 then hold the rest and the guest loop sends it, so the
// stall ends the slice exactly as before. Other destinations, and sources
// outside main RAM, run the guest loop.
NDS_TITLE_HOOK bool Dq9GxFifoSend(uint32_t* r0, uint32_t* r1, uint32_t* r2,
                                  uint32_t* r12, uint32_t* cpsr) {
    if (*r1 != kGxFifo) return false;
    const uint32_t src = *r0;
    const uint32_t end = src + *r2;
    if (static_cast<int32_t>(*r2) <= 0 || (src & 3u) != 0u ||
        src - 0x02000000u >= 0x01000000u || end - 0x02000000u > 0x01000000u)
        return false;
    uint32_t p = src;
    uint32_t last = *r2;
    while (static_cast<int32_t>(p - end) < 0) {
        if (nds_gxfifo_stalled()) break;
        last = bus_read_u32(p);
        p += 4u;
        nds_gpu3d_write(kGxFifo, last, 4u);
    }
    if (static_cast<int32_t>(p - end) < 0) {
        // Stalled: hand the remaining words to the guest loop, which starts
        // over at `add r12, r0, r2`.
        *r0 = p;
        *r2 = end - p;
        return false;
    }
    const uint32_t nzcv_mask = kCpsrN | kCpsrZ | kCpsrC | kCpsrV;
    *r0 = p;
    *r2 = last;
    *r12 = end;
    *cpsr = (*cpsr & ~nzcv_mask) | CmpFlags(p, end);
    return true;
}

namespace {

// Literal of 0x020B6ADC: the geometry command buffer's state,
// {u32* buffer, u32 busy}. buffer[0] counts the words queued after it.
constexpr uint32_t kGeBufferStateLiteral = 0x020B6BB8u;
constexpr uint32_t kGeBufferWords = 0xC0u;
// One command's parameters (MTX_LOAD_4x4 has 16, MTX_MULT_4x4 16, the
// largest is 32): with the FIFO not stalled at entry, every entry written
// past a stall fits in the engine's 64-entry stall queue.
constexpr uint32_t kGeDirectMaxParams = 32u;

}  // namespace

// At the entry of NNS G3D's G3dGeBufferOP_N(cmd = r0, params = r1, n = r2),
// 0x020B6ADC, before its push. With a buffer and busy set, it appends cmd and
// the n parameter words when they fit in 0xC0 words; with no queued words
// and busy clear it writes cmd to GXFIFO and sends the parameters through
// MI_CpuSend32. Called for every geometry command of every model. The hook does those two paths natively and returns; a full buffer (flush
// through 0x020B69A8), a busy wait (0x020B6A04), a stalled FIFO and longer
// parameter lists run the guest code. r0-r3 and r12 are scratch for the
// callers (compiled C); the result leaves r4-r6 untouched, as the pop would.
NDS_TITLE_HOOK bool Dq9GeBufferCommand(uint32_t* r0, uint32_t* r1,
                                       uint32_t* r2) {
    const uint32_t cmd = *r0;
    const uint32_t params = *r1;
    const uint32_t n = *r2;
    const uint32_t state = bus_read_u32(kGeBufferStateLiteral);
    const uint32_t buffer = bus_read_u32(state);
    const uint32_t busy = bus_read_u32(state + 4u);
    if (buffer != 0u && busy != 0u) {
        const uint32_t count = bus_read_u32(buffer);
        if (count + 1u + n > kGeBufferWords) return false;
        bus_write_u32(buffer, count + 1u);
        bus_write_u32(buffer + (count + 1u) * 4u, cmd);
        for (uint32_t i = 0u; i < n; ++i)
            bus_write_u32(buffer + (count + 2u + i) * 4u,
                          bus_read_u32(params + i * 4u));
        if (n != 0u) bus_write_u32(buffer, count + 1u + n);
        return true;
    }
    if (busy != 0u || n > kGeDirectMaxParams || nds_gxfifo_stalled())
        return false;
    if (buffer != 0u && bus_read_u32(buffer) != 0u) return false;  // flush
    nds_gpu3d_write(kGxFifo, cmd, 4u);
    for (uint32_t i = 0u; i < n; ++i)
        nds_gpu3d_write(kGxFifo, bus_read_u32(params + i * 4u), 4u);
    return true;
}

// At the head of the three box-test culling loops (0x020173F8, 0x0203F6F8,
// 0x0207EA14), each `do r = G3X_GetBoxTestResult(&res); while (r != 0);`
// right after the BOX_TEST write. The ARM9 spins there until the geometry
// engine reaches the test, ~2,500 polls a frame in a town, each a call, a
// dispatch and a GXSTAT read. The hook has the scheduler hold the
// ARM9 until the test is done instead (the same guest time passes); the
// guest's next poll then reads the result.
NDS_TITLE_HOOK void Dq9BoxTestWait() {
    nds_title_wait_gx_test();
}
