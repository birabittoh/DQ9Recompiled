// Real-time audio: extra sound driver ticks while the game runs slow.
// Manifest entry: config/hooks/dq9_arm7.toml (Dq9SndCatchUp).
#include "title_hooks.h"
#include "title_settings.h"

namespace {
// The SDK's sound tick: 2728 ticks of the ARM7 timer at 64 system cycles.
constexpr uint32_t kSndTickSystemCycles = 2728u * 64u;
}  // namespace

NDS_TITLE_HOOK bool Dq9SndCatchUp() {
    return nds_title_audio_catchup(kSndTickSystemCycles);
}
