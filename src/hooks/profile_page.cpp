// A party member's profile page (ov8) over a slow link. The page opens
// with a placeholder record (birthday 1 January 2000) and asks the member
// for theirs (message 25; the reply, message 26, fills the record and sets
// bit 1 of the page's receive flags when its last part is in). The last
// load step 0x0218AA24 draws the birthday into the side panel from the
// record as it is then, and nothing draws it again. Over local wireless
// the reply always beats the page's file loads; over the internet it does
// not, and the panel kept January 1st. The step waits for the reply here;
// the page's next state 0x02189C70 waits for the same bit anyway.
#include "title_hooks.h"

namespace {

constexpr uint32_t kReceiveFlags = 0xE98u;  // page byte
constexpr uint8_t kReplyComplete = 0x02u;

}  // namespace

// At 0x02189BE0 `cmp r0, #0`: r0 = go on to the last load step (always 1
// from 0x02094B5C), r10 = the page. 0 keeps the page in that state, which
// asks again next frame.
NDS_TITLE_HOOK void Dq9ProfilePageReady(uint32_t* go, uint32_t* page) {
    if ((bus_read_u8(*page + kReceiveFlags) & kReplyComplete) == 0) *go = 0;
}
