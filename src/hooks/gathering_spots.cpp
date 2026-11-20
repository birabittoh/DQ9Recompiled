// Gathering spots keep their unpicked items across a church save: the field
// start no longer empties them on a Continue. See config/hooks/dq9_arm9.toml.
#include "title_hooks.h"

// At `bl 0x0208EC14` (0x0208EA6C): true jumps past the call.
NDS_TITLE_HOOK bool Dq9KeepGatheringSpots() {
    return true;
}
