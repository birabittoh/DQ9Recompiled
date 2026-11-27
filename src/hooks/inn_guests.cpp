// Inn guests drawn as live character models. See config/hooks/dq9_arm9.toml.
//
// The NPC spawner (0x0203D038) walks the map's NPC list. An inn guest
// (type 4) is built as character slot 5 in the scene's one spare character
// heap (0x0218F064 mode 0x4000), then 0x0203E908 renders it into a sprite
// (n001a_test.spr) and the slot is freed. The NPC code still handles an NPC
// that holds the character itself in npc[7]: position (0x02040708,
// 0x02040784), the idle animation and ground height after the spawn switch,
// and the draw (0x02040920 -> 0x0218EAFC, the party's character draw).
//
// The per-frame NPC update (0x02040C3C) only steps sprites (npc[5]) and map
// models (npc[6]). For an NPC with only npc[7], its entry tail-calls the
// step the party update (0x0218E674) gives the members who are not walking,
// 0x02052AF8(character): animation and model transform. The NPC's facing set
// by talking (0x02040B3C) goes through the same character.
//
// Slot 5 has room for one character, and every guest reuses it, so only the
// last guest in the list can keep it: for that one the hook skips the sprite,
// the capture and the teardown, and the spawner goes on as for an NPC with
// npc[7] set.
#include <cstdint>
#include <cstdio>

#include "title_hooks.h"

namespace {

constexpr uint8_t kGuestType = 4;

uint32_t g_live_entry = 0;

}  // namespace

// Spawner entry, r0 = NPC manager. The map's NPC list starts at *(mgr+144):
// u16 seat/id at +0, type byte at +2, next entry at +16.
NDS_TITLE_HOOK void Dq9InnGuestSpawn(uint32_t* mgr) {
    g_live_entry = 0;
    int guests = 0;
    for (uint32_t e = bus_read_u32(*mgr + 144); e && guests < 64;
         e = bus_read_u32(e + 16)) {
        if (bus_read_u8(e + 2) == kGuestType) {
            g_live_entry = e;
            ++guests;
        }
    }
    if (guests)
        std::fprintf(stderr, "[dq9-guest] %d guest(s), live entry 0x%08X\n",
                     guests, g_live_entry);
}

// 0x0203D390, guest case with slot 5 built and npc[7] = its character, just
// before the sprite is allocated (r5 = list entry, r7 = npc). True jumps to
// 0x0203D46C, which drops the "building" flag and leaves the switch.
NDS_TITLE_HOOK bool Dq9InnGuestKeepModel(uint32_t* entry, uint32_t* npc) {
    if (*entry != g_live_entry) return false;
    std::fprintf(stderr, "[dq9-guest] live model npc=0x%08X char=0x%08X\n",
                 *npc, bus_read_u32(*npc + 28));
    return true;
}

// Talking sets the NPC's target yaw (0x02040B00), for a character at +724,
// and waits until the current yaw (+84, 0x02040A30) reaches it
// (0x020656F4). A map model turns toward its target in its own update
// (0x02041388); a character instead turns its yaw toward its heading (s16
// at +174) in its step (0x02033720, at its own turn rate, snapping to the
// heading at the end). So the target goes to the heading as the party code
// sets one (0x02033844): heading = target, +196 bit 15 cleared. States 3 and
// 4 (+190) take another path there and are left alone.
static void HeadToTarget(uint32_t chr) {
    const uint32_t target = bus_read_u32(chr + 724);
    if (bus_read_u16(chr + 174) == (target & 0xFFFFu)) return;
    const uint8_t state = bus_read_u8(chr + 190);
    if (state == 3 || state == 4) return;
    bus_write_u16(chr + 174, static_cast<uint16_t>(target));
    bus_write_u16(chr + 196, bus_read_u16(chr + 196) & 0x7FFFu);
}

// 0x02040C3C entry, r0 = npc, lr = back into the NPC loop (0x0203DD30).
// True jumps to 0x02052AF8 with r0 = the character: a tail call that
// returns to the loop. The update's own tail clears npc flag 0x20000.
NDS_TITLE_HOOK bool Dq9InnGuestStep(uint32_t* r0) {
    const uint32_t npc = *r0;
    if (bus_read_u32(npc + 20) || bus_read_u32(npc + 24)) return false;
    const uint32_t chr = bus_read_u32(npc + 28);
    if (!chr) return false;
    HeadToTarget(chr);
    bus_write_u32(npc, bus_read_u32(npc) & ~0x20000u);
    *r0 = chr;
    return true;
}
