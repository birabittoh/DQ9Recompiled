// Netplay: what DQ9's Rapportal advertises while it is open. The runner
// hands over the WMGameInfo of every beacon this instance transmits as an
// MP host (ndsrecomp runner/src/net/mp_beacon.h); this reads the free party
// slots and the host's name out of DQ9's user game info. Read-only, no
// guest state involved.
//
// User game info (36 bytes), from beacons with 1, 2 and 3 party members
// (owner run, 2026-11-09):
//   0  host name, DQ9 encoding, zero-terminated (the save keeps names in a
//      12-byte field; bytes after the terminator are stale)
//  19  party size, 1-4
//  21  bit 3: canvass for guests (tag mode), set by its host start
//      (0x0202C420) and required by its scanner (0x021719C8)
//  22  u16 counter, +1 per frame while the Rapportal is open
//  24  canvass: the save's 6-byte ID
//  rest constant within a save (20, the rest of 21 unknown)
#include <cstddef>
#include <cstdint>

#include "wifi_net.h"

namespace {

constexpr uint32_t kDq9Ggid = 0x00000785u;
constexpr size_t kNameBytes = 12;
constexpr size_t kPartySize = 19;
constexpr size_t kFlags = 21;
constexpr uint8_t kFlagTag = 0x08;
constexpr int kMaxParty = 4;

// Letters only, checked against the names in the save (Marco, Dazaar,
// Juliana, Gelda). Digits, accents and symbols are not mapped yet.
char DecodeChar(uint8_t c) {
    if (c >= 0x12 && c < 0x12 + 26) return static_cast<char>('A' + c - 0x12);
    if (c >= 0x2C && c < 0x2C + 26) return static_cast<char>('a' + c - 0x2C);
    return '?';
}

bool DecodeRapportal(uint32_t ggid, uint8_t attribute, const uint8_t* user,
                     size_t user_len, NdsNetplayHostInfo* out) {
    if (ggid != kDq9Ggid || user_len <= kPartySize) return false;
    const int party = user[kPartySize];
    if (party < 1 || party > kMaxParty) return false;
    out->tag = (user[kFlags] & kFlagTag) != 0;
    out->free_slots = out->tag ? 0 : kMaxParty - party;
    out->accepting = !out->tag && (attribute & 0x01u) != 0 &&
                     out->free_slots > 0;
    size_t n = 0;
    for (; n < kNameBytes && n + 1 < sizeof(out->host_name) && user[n] != 0;
         ++n)
        out->host_name[n] = DecodeChar(user[n]);
    out->host_name[n] = '\0';
    return true;
}

const bool kRegistered =
    (nds_netplay_set_host_decoder(&DecodeRapportal), true);

}  // namespace
