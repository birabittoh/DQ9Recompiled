// Inn guests drawn as live character models. See config/hooks/dq9_arm9.toml.
//
// The NPC spawner (0x0203D038) walks the map's NPC list. An inn guest
// (type 4) is built as character slot 5 (0x0218F064 mode 0x4000) from the
// extra character record *(gs+9112) into the scene's one spare character
// heap (scene+476), then 0x0203E908 renders it into a sprite (n001a_test.spr)
// and the slot is freed. The NPC code still handles an NPC that holds the
// character itself in npc[7]: position (0x02040708, 0x02040784), the idle
// animation and ground height after the spawn switch, and the draw
// (0x02040920 -> 0x0218EAFC, the party's character draw).
//
// Here every guest keeps its character. Each one is built from its own copy
// of the record into its own frame heap, both in title RAM (main RAM has no
// room for 30 of them): the record's sub-heaps, the character (chr+340
// points back at its record) and its 11 subparts all live in that heap. They
// all still use slot 5, whose entries in the slot table (0x0200FD80: 5 and
// 12*5+19..29) each guest's draw and step put back first. The spawner skips
// the sprite, the capture and the teardown for them and goes on as for an
// NPC with npc[7] set.
//
// The per-frame NPC update (0x02040C3C) only steps sprites (npc[5]) and map
// models (npc[6]). For an NPC with only npc[7], its entry tail-calls the
// step the party update (0x0218E674) gives the members who are not walking,
// 0x02052AF8(character): animation and model transform. The NPC's facing set
// by talking (0x02040B3C) goes through the same character.
//
// All bookkeeping lives in title RAM, so it is guest state: savestates carry
// it and the hooks keep none of their own.
#include <cstdint>
#include <cstdio>

#include "io.h"
#include "title_hooks.h"

namespace {

constexpr uint8_t kGuestType = 4;
constexpr uint32_t kSlot = 5;
constexpr uint32_t kSlotTable = 0x020F33D8u;  // 0x0200F398(); entry id at +8+4*id
constexpr uint32_t kHeapRootList = 0x0210A000u;  // NNS heap root list
constexpr uint32_t kFrmh = 0x46524D48u;          // 'FRMH'

// Title RAM layout.
constexpr uint32_t kRam = 0x08000000u;
constexpr uint32_t kRamSize = 0x400000u;
constexpr uint32_t kMaxGuests = 32;
constexpr uint32_t kBuilding = kRam + 0x0;    // guest index + 1 while built
constexpr uint32_t kNextGuest = kRam + 0x4;   // guests built by this spawn
constexpr uint32_t kSlots = kRam + 0x100;     // per guest: 12 slot entries
constexpr uint32_t kSlotsStride = 0x40;
constexpr uint32_t kHeaders = kRam + 0x1000;  // per guest: heap + allocator
constexpr uint32_t kHeaderStride = 0x20;
constexpr uint32_t kRecords = kRam + 0x2000;  // per guest: the record
constexpr uint32_t kRecordSize = 0x5EC;       // as 0x02097ED8 allocates it
constexpr uint32_t kRecordStride = 0x600;
constexpr uint32_t kHeaps = kRam + 0x10000;   // per guest: its frame heap
constexpr uint32_t kHeapSize = 0x18000;       // one guest uses ~0x1167C
static_assert(kHeaps + kMaxGuests * kHeapSize <= kRam + kRamSize);

// The 12 slot-table ids a character in slot 5 fills: itself and subparts.
uint32_t SlotId(uint32_t k) { return k == 0 ? kSlot : 12 * kSlot + 18 + k; }

// The index of the guest whose record `rec` is, or -1.
int GuestOfRecord(uint32_t rec) {
    if (rec < kRecords || rec >= kRecords + kMaxGuests * kRecordStride ||
        (rec - kRecords) % kRecordStride)
        return -1;
    return static_cast<int>((rec - kRecords) / kRecordStride);
}

int GuestOfCharacter(uint32_t chr) {
    return chr ? GuestOfRecord(bus_read_u32(chr + 340)) : -1;
}

// Puts guest i's character and subparts back in slot 5's table entries.
void SelectGuest(int i) {
    const uint32_t saved = kSlots + i * kSlotsStride;
    for (uint32_t k = 0; k < 12; ++k)
        bus_write_u32(kSlotTable + 8 + 4 * SlotId(k), bus_read_u32(saved + 4 * k));
}

// NNS list append (0x020AEF1C): list {head, tail, u16 count, u16 offset},
// the object's link {prev, next} at +offset.
void ListAppend(uint32_t list, uint32_t obj) {
    const uint32_t off = bus_read_u16(list + 10);
    const uint32_t tail = bus_read_u32(list + 4);
    bus_write_u32(obj + off, tail);
    bus_write_u32(obj + off + 4, 0);
    if (tail) bus_write_u32(tail + off + 4, obj);
    else bus_write_u32(list, obj);
    bus_write_u32(list + 4, obj);
    bus_write_u16(list + 8, static_cast<uint16_t>(bus_read_u16(list + 8) + 1));
}

// Guest i's frame heap, made once as NNS_FndCreateFrmHeap would
// (0x020AF984 -> 0x020AF188) and kept in the root heap list, so the heaps
// the build makes inside it (0x020542C4) become its children as usual.
// Its handle and allocator go to guest i's 20-byte header, which stands in
// for scene+476 (copied from it: allocator functions, alignment).
void PrepareHeap(int i, uint32_t scene_heap) {
    const uint32_t head = kHeaps + i * kHeapSize;
    const uint32_t hdr = kHeaders + i * kHeaderStride;
    if (bus_read_u32(head) != kFrmh) {
        for (uint32_t o = 0; o < 0x30; o += 4) bus_write_u32(head + o, 0);
        bus_write_u32(head, kFrmh);
        bus_write_u16(head + 22, 4);                 // child list link offset
        bus_write_u32(head + 24, head + 0x30);       // start
        bus_write_u32(head + 28, head + kHeapSize);  // end
        bus_write_u32(head + 36, head + 0x30);       // head allocation
        bus_write_u32(head + 40, head + kHeapSize);  // tail allocation
        ListAppend(kHeapRootList, head);
    }
    for (uint32_t o = 0; o < 20; o += 4)
        bus_write_u32(hdr + o, bus_read_u32(scene_heap + o));
    bus_write_u32(hdr, head);      // heap handle
    bus_write_u32(hdr + 8, head);  // allocator's heap
}

// Guest i's record, copied once from the game's, without its heaps (those
// belong to the game's record; the build makes its own).
void PrepareRecord(int i, uint32_t game_rec) {
    const uint32_t rec = kRecords + i * kRecordStride;
    if (bus_read_u32(rec + kRecordStride - 4) == kFrmh) return;
    for (uint32_t o = 0; o < kRecordSize; o += 4)
        bus_write_u32(rec + o, bus_read_u32(game_rec + o));
    for (uint32_t h = 0; h < 10; ++h) bus_write_u32(rec + 4 + 20 * h, 0);
    for (uint32_t off : {1324u, 1344u, 1364u, 1496u}) bus_write_u32(rec + off, 0);
    bus_write_u32(rec + kRecordStride - 4, kFrmh);  // marks it made
}

}  // namespace

// Spawner entry, r0 = NPC manager.
NDS_TITLE_HOOK void Dq9InnGuestSpawn(uint32_t* mgr) {
    int guests = 0;
    for (uint32_t e = bus_read_u32(*mgr + 144); e && guests < 64;
         e = bus_read_u32(e + 16))
        if (bus_read_u8(e + 2) == kGuestType) ++guests;
    if (!guests) return;
    if (!nds_bus_map_title_ram(kRam, kRamSize)) return;
    bus_write_u32(kBuilding, 0);
    bus_write_u32(kNextGuest, 0);
    std::fprintf(stderr, "[dq9-guest] %d guest(s)\n", guests);
}

// 0x0203D2F8, just before the guest case calls 0x0218F064(scene, 5, 0x4000).
// Picks the next guest's storage; with none left the game builds it as usual.
NDS_TITLE_HOOK void Dq9InnGuestBuildBegin() {
    if (!nds_bus_title_ram_contains(kRam)) return;
    const uint32_t i = bus_read_u32(kNextGuest);
    if (i >= kMaxGuests) return;
    bus_write_u32(kNextGuest, i + 1);
    bus_write_u32(kBuilding, i + 1);
    std::fprintf(stderr, "[dq9-guest] build %u begin\n", i);
}

// 0x0218F0C0 in mode 0x4000, r7 = the record *(gs+9112), r11 = scene+476.
// While the spawner builds a guest, both become that guest's.
NDS_TITLE_HOOK void Dq9InnGuestBuildHeap(uint32_t* r7, uint32_t* r11) {
    if (!nds_bus_title_ram_contains(kRam)) return;
    const uint32_t building = bus_read_u32(kBuilding);
    if (!building) return;
    const int i = static_cast<int>(building - 1);
    PrepareHeap(i, *r11);
    PrepareRecord(i, *r7);
    std::fprintf(stderr, "[dq9-guest] build %d: record 0x%08X heap 0x%08X (game's 0x%08X 0x%08X, handle 0x%08X)\n",
                 i, kRecords + i * kRecordStride, kHeaders + i * kHeaderStride,
                 *r7, *r11, bus_read_u32(*r11));
    *r7 = kRecords + i * kRecordStride;
    *r11 = kHeaders + i * kHeaderStride;
}

// 0x0203D30C, right after that call: slot 5 holds the new guest.
NDS_TITLE_HOOK void Dq9InnGuestBuildEnd() {
    if (!nds_bus_title_ram_contains(kRam)) return;
    const uint32_t building = bus_read_u32(kBuilding);
    if (!building) return;
    bus_write_u32(kBuilding, 0);
    const uint32_t saved = kSlots + (building - 1) * kSlotsStride;
    for (uint32_t k = 0; k < 12; ++k)
        bus_write_u32(saved + 4 * k, bus_read_u32(kSlotTable + 8 + 4 * SlotId(k)));
    const uint32_t chr = bus_read_u32(kSlotTable + 8 + 4 * kSlot);
    const uint32_t head = kHeaps + (building - 1) * kHeapSize;
    std::fprintf(stderr, "[dq9-guest] build %u end: char 0x%08X record 0x%08X heap used 0x%X\n",
                 building - 1, chr, chr ? bus_read_u32(chr + 340) : 0,
                 bus_read_u32(head + 36) - (head + 0x30));
}

// 0x0203D390, guest case with npc[7] = its character, just before the
// sprite is allocated (r5 = list entry, r7 = npc). True jumps to 0x0203D46C,
// which drops the "building" flag and leaves the switch.
NDS_TITLE_HOOK bool Dq9InnGuestKeepModel(uint32_t* entry, uint32_t* npc) {
    (void)entry;
    const uint32_t chr = bus_read_u32(*npc + 28);
    const int i = GuestOfCharacter(chr);
    if (i < 0) {
        std::fprintf(stderr, "[dq9-guest] sprite for npc=0x%08X char=0x%08X\n", *npc, chr);
        return false;
    }
    std::fprintf(stderr, "[dq9-guest] live model %d npc=0x%08X char=0x%08X\n",
                 i, *npc, chr);
    return true;
}

// 0x020409A0, NPC draw of a character (r1), before it reads the slot id and
// calls the party draw 0x0218EAFC(scene, slot).
NDS_TITLE_HOOK void Dq9InnGuestDraw(uint32_t* r1) {
    const int i = GuestOfCharacter(*r1);
    if (i >= 0) SelectGuest(i);
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
    const int i = GuestOfCharacter(chr);
    if (i >= 0) SelectGuest(i);
    HeadToTarget(chr);
    bus_write_u32(npc, bus_read_u32(npc) & ~0x20000u);
    *r0 = chr;
    return true;
}

// 0x020CA064 entry, the SDK's DMA address check (r0 = 0 to check, r1 =
// address, r2 = size, r3 = address mode: 0 increment, 0x800000 decrement,
// else fixed). It halts the game (0x020C9BF0) for any range that touches
// I/O or 0x08000000 and up, title RAM included, which the DMA reaches like
// main RAM. True returns, as the check does for a range it accepts.
NDS_TITLE_HOOK bool Dq9InnGuestDmaCheck(uint32_t* r0, uint32_t* r1,
                                        uint32_t* r2, uint32_t* r3) {
    if (*r0 != 0 || !nds_bus_title_ram_contains(*r1)) return false;
    const uint32_t end = *r3 == 0 ? *r1 + *r2 : *r3 == 0x800000u ? *r1 - *r2 : *r1;
    return nds_bus_title_ram_contains(end) || end == kRam + kRamSize;
}

// 0x020C9BF0 entry: the game's halt loop (OS_Terminate), lr = who called it.
NDS_TITLE_HOOK void Dq9TerminateLog(uint32_t* lr) {
    std::fprintf(stderr, "[dq9] OS_Terminate from 0x%08X\n", *lr);
}
