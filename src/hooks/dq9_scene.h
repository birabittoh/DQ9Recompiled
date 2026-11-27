// dq9_scene.h — which DQ9 scene is running, from guest state, for the
// presentation hooks (UI size, camera distance).
#pragma once

#include <atomic>
#include <cstdint>

#include "title_hooks.h"

namespace dq9 {

// The overlay loader (0x020A1950) keeps the id loaded in each slot as a
// signed byte at 0x01FFD364 + 6 * *(u32*)0x01FFD348 + slot (the slot of
// overlay n is the word at 0x020E8F30 + 8n): slot 0 = overlays 0..6,
// 1 = 7..14, 2 = 15..21, 3 = 22..30, 4 = 31..32, 5 = 33..34. -1 = none.
inline int LoadedOverlay(uint32_t slot) {
    const uint32_t bank = bus_read_u32(0x01FFD348);
    return static_cast<int8_t>(bus_read_u8(0x01FFD364 + 6 * bank + slot));
}

// The boot and title screens: the main loop (ctx 0x020F33D8, mode byte at
// +4) runs them in mode 6. Continue sets mode 0 at once, but the title's
// fade-out still runs from overlay 4 (slot 0) after that, so the title is
// over only when overlay 4 is gone from its slot (see text_box_size.cpp,
// InGame, which tests the same two things).
//
// The camp menu's Attributes page loads overlay 4 in slot 0 too, from the
// field (mode 0), so overlay 4 only counts as the title's when it was
// already there in the title's mode: remembered here until it leaves its
// slot.
inline bool TitleScreen() {
    constexpr uint32_t kMainMode = 0x020F33D8 + 4;
    constexpr uint8_t kTitleMode = 6;
    constexpr int kTitleOverlay = 4;
    static std::atomic<bool> title_overlay{false};
    const bool overlay = LoadedOverlay(0) == kTitleOverlay;
    if (bus_read_u8(kMainMode) == kTitleMode) {
        title_overlay.store(overlay, std::memory_order_relaxed);
        return true;
    }
    if (!overlay) {
        title_overlay.store(false, std::memory_order_relaxed);
        return false;
    }
    return title_overlay.load(std::memory_order_relaxed);
}

// A story movie (data/movie/*.mods) is playing: the field asks for one and
// the main loop (0x02000C9C) switches to mode 3, which loads the player,
// overlay 16 ("movieview"), in slot 2 in place of the field's overlay 17,
// runs it to the end and goes back to mode 0. The mode byte is in main RAM,
// safe to read from the frontend thread; the overlay slots are not (they
// read as 0 there on some frames).
inline bool MoviePlaying() {
    constexpr uint32_t kMainMode = 0x020F33D8 + 4;
    constexpr uint8_t kMovieMode = 3;
    return bus_read_u8(kMainMode) == kMovieMode;
}

// Samples the scene for the forced screen layouts (screen_layouts.cpp), at a
// consistent point on the CPU thread: G3_SwapBuffers, from Dq9WindowLayers.
void SampleScreenLayout();
// The same for the camera's on-screen buttons (camera_rotation.cpp).
void SampleCameraButtons();

// In battle: the battle routes engine A (the 3D) to the top LCD, POWCNT1
// bit 15 set, for as long as it runs, from the transition in to the fade
// back; the field, towns and the camp menu keep it on the bottom LCD.
// Slot 0 does not tell: it holds the area's overlay (0 in the overworld,
// 3 in towns), and the battle loads its own in slot 3.
inline bool InBattle() {
    constexpr uint32_t kPowcnt1 = 0x04000304;
    constexpr uint16_t kEngineAOnTop = 0x8000;
    return (bus_read_u16(kPowcnt1) & kEngineAOnTop) != 0;
}

// The game proper is running: the main loop (0x02000C9C) switches on the
// mode byte at ctx+4 (ctx = 0x020F33D8, read by 0x0200FBAC). Modes 0, 4, 5
// and 9 run the game (0x020D6CC4 with overlay 17). Mode 2 is a new game:
// character creation (0x0218B5FC), then, once the byte at ctx+25556 is set,
// the game too (the new game's prologue, the Observatory). The boot and
// title screens run in mode 6.
inline bool GameModeRunning() {
    constexpr uint32_t kCtx = 0x020F33D8;
    switch (bus_read_u8(kCtx + 4)) {
        case 0: case 4: case 5: case 9: return true;
        case 2: return bus_read_u8(kCtx + 25556) != 0;
        default: return false;
    }
}

// The field (map walking, towns, dungeons, the field's events) runs from
// overlay 17 in slot 2, at 0x0218B5A0; its main object is the word at
// 0x021D82E0 + 4 (0x0218B5B0). The battle keeps it loaded.
inline bool FieldLoaded() { return LoadedOverlay(2) == 17; }

inline uint32_t FieldObject() {
    const uint32_t field = bus_read_u32(0x021D82E0 + 4);
    return field >= 0x02000000u && field + 0x4400u <= 0x02400000u ? field
                                                                  : 0;
}

// A field cutscene: the field runs its actions as tasks queued at
// field+14076 (0x021A3498 runs the head; byte 0 is the task type,
// dispatched by 0x021A3544). Story events and a new game's 2D prologue run
// as type 4, the title's opening as 0x2D (in the title's mode 6); talking
// to people runs no task. An event also takes the
// camera off the hero: the field camera (ctx 0x020F33D8 + 0x3B0, in DTCM)
// follows the hero (0x020A213C) only while its word +568 is set.
inline bool FieldCutscene() {
    if (!FieldLoaded()) return false;
    const uint32_t camera = bus_read_u32(0x020F33D8 + 0x3B0);
    if (camera >= 0x027E0000u && camera + 572u <= 0x027E4000u &&
        bus_read_u32(camera + 568) == 0)
        return true;
    const uint32_t field = FieldObject();
    if (!field) return false;
    const uint32_t queue = bus_read_u32(field + 14076);
    if (queue < 0x02000000u || queue + 4u > 0x02400000u) return false;
    const uint32_t head = bus_read_u32(queue);
    if (head < 0x02000000u || head + 1u > 0x02400000u) return false;
    const uint8_t type = bus_read_u8(head);
    return type == 4 || type == 0x2D;
}

// A new game's opening: the main loop's mode 2 (see GameModeRunning), from
// character creation through the 2D prologue (its lines shown one at a
// time through the 2D message window) to the Observatory.
inline bool NewGameOpening() { return bus_read_u8(0x020F33D8 + 4) == 2; }

// The battle records pages (see FullScreenPage), with the Alchenomicon
// and the accolade popup (below). The battle records (camp
// menu > Battle records, overlay 8 in slot 1; the field and the camp menu have overlay 11 there) fill engine A
// with a wallpaper on BG1 (screen base 30), draw the buttons, the cursor
// and "Back" as OBJs, and Stella and her speech box in 3D on BG0, blended
// over BG1 (BLDCNT 0x0241, EVA 15 / EVB 16). Its Defeated Monster List
// loads overlay 14 in the same slot and is laid out the same way (a 3D
// monster over BG1, engine A on the top LCD).
//
// The overlay stays loaded after the camp menu closes (the field does not
// reload slot 1), and so does BLDCNT, so the page also needs its BG1 on
// screen: engine A's BG1CNT screen base is 30 on the records page
// (0x1E02 / 0x1E03) and 31 on the monster list (0x1F00 / 0x1F01), and goes
// back to 0 (0x0004) as soon as the page closes; the camp menu uses 28 and
// 29. The blend is checked too: BLDCNT = 0x0241 (alpha, BG0 over BG1).
// Reading these registers has no side effects.
//
// The "awarded the accolade" popup, shown over the records page as it
// opens, keeps BG1 on base 30 and draws its frame on BG3 (0x1D08), dimming
// the rest with a brightness fade: BLDCNT = 0x00D7 while it is up.
//
// The Alchenomicon (records page > Alchenomicon) loads overlay 6 in slot 0,
// with the records overlay still in slot 1, and sets BG1CNT 0x1D00 (BG2
// 0x1E01, BG3 0x1F0B; BLDCNT 0x0144). The field keeps 0x0004 there after
// it closes.
//
// The Item List and Accolades Earnt (records page > either) load overlay 4
// in slot 0, the field's overlay 11 back in slot 1, BLDCNT 0x0844. Item
// List: BG1CNT 0x1C08, BG2 0x1D09 (BG3 0x1E03, then 0x1E02); Accolades:
// BG1CNT 0x0E00, BG2 0x0F01 (BG3 0x1F0A). The other overlay 4 users have
// 0x1D00 (Attributes), 0x1D01 (treasure map), the field's 0x0004 (grottoes)
// and 0x0104 / 0x0105 (title) there.
inline bool RecordsListPage() {
    constexpr uint32_t kFieldSlot = 0;
    constexpr int kListOverlay = 4;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint32_t kBg2cntA = 0x0400000C;
    if (LoadedOverlay(kFieldSlot) != kListOverlay) return false;
    const uint16_t bg1 = bus_read_u16(kBg1cntA);
    const uint16_t bg2 = bus_read_u16(kBg2cntA);
    return (bg1 == 0x1C08 && bg2 == 0x1D09) || (bg1 == 0x0E00 && bg2 == 0x0F01);
}

inline bool RecordsPage() {
    if (RecordsListPage()) return true;
    constexpr uint32_t kFieldSlot = 0;
    constexpr uint32_t kMenuSlot = 1;
    constexpr int kRecordsOverlay = 8;
    constexpr int kMonsterListOverlay = 14;
    constexpr int kAlchenomiconOverlay = 6;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint32_t kBldcntA = 0x04000050;
    constexpr uint16_t kPageBlend = 0x0241;
    constexpr uint16_t kAccoladeBlend = 0x00D7;
    constexpr uint16_t kAlchenomiconBg1 = 0x1D00;
    const int menu = LoadedOverlay(kMenuSlot);
    if (menu != kRecordsOverlay && menu != kMonsterListOverlay) return false;
    const uint16_t bg1 = bus_read_u16(kBg1cntA);
    if (menu == kRecordsOverlay && bg1 == kAlchenomiconBg1 &&
        LoadedOverlay(kFieldSlot) == kAlchenomiconOverlay)
        return true;
    const int screen_base = (bg1 >> 8) & 0x1F;
    if (screen_base != 30 && screen_base != 31) return false;
    const uint16_t blend = bus_read_u16(kBldcntA);
    return blend == kPageBlend || blend == kAccoladeBlend;
}

// Character pages: the Attributes page (below), the equipment screen (camp menu > Equipment, overlay 5
// in slot 0) and character creation (overlay 9 in slot 1). They fill
// engine A with 2D layers and frame the character's 3D model beside them.
// Engine A's BG1CNT is 0x1D03 on both (screen base 29, priority 3; BG2/BG3
// 0x1E02/0x1F04 on the equipment screen, 0x1E09/0x1F0C on creation). The
// field has 0x0004, and the camp menu (overlay 2 in slot 0) 0x1D02, which
// must not match: its windows are scaled. The overlays stay loaded after
// the page closes, so BG1 is what tells the page apart.
//
// The Attributes page (camp menu > Attributes) loads overlay 4 in slot 0,
// the title's overlay, with BG1CNT 0x1D00 (BG2 0x1E09/0x1E0A, BG3 0x1F0B);
// the title screen has 0x0004 / 0x0104 / 0x0105 there.
inline bool CharacterPage() {
    constexpr uint32_t kFieldSlot = 0;
    constexpr uint32_t kMenuSlot = 1;
    constexpr int kAttributesOverlay = 4;
    constexpr int kEquipmentOverlay = 5;
    constexpr int kCreationOverlay = 9;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint16_t kPageBg1 = 0x1D03;
    constexpr uint16_t kAttributesBg1 = 0x1D00;
    const uint16_t bg1 = bus_read_u16(kBg1cntA);
    if (bg1 == kAttributesBg1)
        return LoadedOverlay(kFieldSlot) == kAttributesOverlay;
    if (bg1 != kPageBg1) return false;
    return LoadedOverlay(kFieldSlot) == kEquipmentOverlay ||
           LoadedOverlay(kMenuSlot) == kCreationOverlay;
}

// The Quester's Rest pages, opened by talking to the inn, not from the camp
// menu:
//   * The Krak Pot loads overlay 6 in slot 0, as the Alchenomicon does
//     (RecordsPage), and sets the same BG1CNT 0x1D00 (BG2 0x1E01, BG3
//     0x1F0B), but with the field's overlay 11 in slot 1.
//   * "Canvass for guests" turns the wireless on (overlay 31 in slot 4 for
//     part of it) and sets BG1CNT 0x0F01 (BG0 0x0000, BG2 0x0E0A, BG3
//     0x0003) for as long as it is up. The field has 0x0004 there, its 2D
//     message box 0x0D09 and the camp menu 0x1D02.
//   * The guestbook keeps BG1CNT and BG2CNT at the 2D message box's 0x0D09
//     / 0x0E0A, but also turns on BG3 with 0x0F03 (BG0 0x0000, BLDCNT
//     0x0844), and fades in and out with 0x0D08 / 0x0E09 / 0x0F02 (BG0
//     0x0003). The message box has BG3CNT 0x0000 and the field 0x0003.
inline bool InnPage() {
    constexpr uint32_t kFieldSlot = 0;
    constexpr int kKrakPotOverlay = 6;
    constexpr uint32_t kBg0cntA = 0x04000008;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint32_t kBg3cntA = 0x0400000E;
    constexpr uint16_t kKrakPotBg1 = 0x1D00;
    constexpr uint16_t kCanvassBg1 = 0x0F01;
    const uint16_t bg1 = bus_read_u16(kBg1cntA);
    if (bg1 == kKrakPotBg1)
        return LoadedOverlay(kFieldSlot) == kKrakPotOverlay;
    if (bg1 == kCanvassBg1) return bus_read_u16(kBg0cntA) == 0;
    const uint16_t bg3 = bus_read_u16(kBg3cntA);
    return (bg1 == 0x0D09 && bg3 == 0x0F03) || (bg1 == 0x0D08 && bg3 == 0x0F02);
}

// The treasure map (camp menu > Items > a treasure map), where the treasure
// search starts: overlay 4 in slot 0, as the Attributes page, with BG0CNT
// 0x0000, BG1CNT 0x1D01 (BG2 0x1E02, BG3 0x1F0B). Grottoes load overlay 4
// there too, with the field's BG0CNT 0x0001 / 0x0002.
inline bool TreasureMapPage() {
    constexpr uint32_t kFieldSlot = 0;
    constexpr int kMapOverlay = 4;
    constexpr uint32_t kBg0cntA = 0x04000008;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint32_t kBg2cntA = 0x0400000C;
    return bus_read_u16(kBg1cntA) == 0x1D01 &&
           bus_read_u16(kBg2cntA) == 0x1E02 &&
           bus_read_u16(kBg0cntA) == 0 &&
           LoadedOverlay(kFieldSlot) == kMapOverlay;
}

// The overworld's treasure search: the field keeps walking under the stick,
// with its HUD on BG2. Engine A shows BG0..BG2 and the OBJs (DISPCNT
// layers 0x17, BG3 off), with BG1CNT 0x1D01, BG2CNT 0x1E00 and BLDALPHA
// 0x0709. The field walk has BG2 off (layers 0x13), the camp menu
// 0x1D02 / 0x1E01 and BG3 on, the 2D message box BG3 on.
inline bool TreasureSearch() {
    constexpr uint32_t kDispcntA = 0x04000000;
    constexpr uint32_t kBg1cntA = 0x0400000A;
    constexpr uint32_t kBg2cntA = 0x0400000C;
    constexpr uint32_t kBldalphaA = 0x04000052;
    constexpr uint32_t kLayers = 0x1F00;
    constexpr uint32_t kSearchLayers = 0x1700;
    return (bus_read_u32(kDispcntA) & kLayers) == kSearchLayers &&
           bus_read_u16(kBg1cntA) == 0x1D01 &&
           bus_read_u16(kBg2cntA) == 0x1E00 &&
           bus_read_u16(kBldalphaA) == 0x0709;
}

// Full-screen pages, left as on the DS by the UI size and the camera
// distance: the battle records pages, the character pages, the inn's and
// the treasure map.
inline bool FullScreenPage() {
    return RecordsPage() || CharacterPage() || InnPage() || TreasureMapPage();
}

// The UI size's scale about the screen centre, 1 = as on the DS
// (text_box_size.cpp).
double UiScale();

}  // namespace dq9
