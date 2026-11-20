// Heap leak in the background file loader (overlay 34): after about one
// load in four the game allocates 8 bytes from the main heap and never
// frees them. See config/hooks/dq9_arm9_ov034.toml.
#include "title_hooks.h"

// Before `tst r0, #0xFF` at 0x022A2868: a nonzero r0 skips the allocation.
NDS_TITLE_HOOK void Dq9LoaderSkipHeapLeak(uint32_t* hash_bits) {
    *hash_bits = 1u;
}
