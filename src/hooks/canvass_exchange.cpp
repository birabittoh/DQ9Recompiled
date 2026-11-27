// Canvass for guests (tag mode): let the last frame of the guest card go
// out before the link is ended. See config/hooks/dq9_arm9_ov003.toml.
//
// Both sides of an exchange send their 224-byte card as 12-byte messages,
// five per MP frame. Each frame the exchange state (host 0x02171764, child
// 0x02171BA0) asks 0x0205E498 whether everything was sent: the send queue
// is empty and the outgoing frame holds no message. Once it has said so and
// the peer's card is in, the next frame ends the link (0x0202B104). The
// answer turns true as soon as the last messages are handed to WM, before
// the MP frame that carries them is on the air, and the end request then
// beats that frame: the peer never gets the end of the card and drops the
// round (a PC hosting a phone lost the last 4 of 19 messages every time).
// "Sent" only counts once it has held for a few frames, as the game's own
// 3 s exchange timeout leaves room for.
//
// Frames alone are not enough over the internet: a client only sends a
// reply, which carries its messages, when a host cmd comes in, and through
// a relay the cmds arrive in bursts with gaps longer than the grace. So
// "sent" also waits until this side has put two more MP frames of its own
// (host: cmds, client: replies) on the link, the one with the last
// messages and the next. Frames on the link arrive in order, the end of the
// link after them.
#include <cstdint>

#include "title_hooks.h"
#include "wifi_net.h"

namespace {

constexpr int kGraceFrames = 6;
constexpr uint64_t kGraceMpFrames = 2;

uint32_t g_obj = 0;
int g_sent_frames = 0;
uint64_t g_mp_base = 0;

// MP cmds and replies this runner has sent so far.
uint64_t MpFramesSent() {
    NdsLocalMpStats stats;
    if (!nds_wifi_local_mp_stats(&stats)) return 0;
    return stats.sent_by_type[1] + stats.sent_by_type[2];
}

}  // namespace

// At the cmp after `bl 0x0205E498` in both exchange states: r0 = "all
// sent", r6 = the canvass object.
NDS_TITLE_HOOK void Dq9CanvassSendGrace(uint32_t* sent, uint32_t* obj) {
    if (*obj != g_obj) {
        g_obj = *obj;
        g_sent_frames = 0;
    }
    if (*sent == 0) {
        g_sent_frames = 0;
        return;
    }
    const uint64_t mp = MpFramesSent();
    if (g_sent_frames++ == 0) g_mp_base = mp;
    if (g_sent_frames < kGraceFrames || mp - g_mp_base < kGraceMpFrames)
        *sent = 0;
}
