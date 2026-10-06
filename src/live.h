// Live key surfacing: which keys is the program asking for right now?
//
// The VM notes every place it reads a key (getchar, Inkey, GetWord,
// CheckKey(128)) with the two return addresses above it, and every key it
// tests with CheckKey(k). From those read sites this scans the bytecode that
// follows for the comparisons made on the key that was read
// (`ld8 v; eqi 'y'`, `getchar; nei 27`, ...). The union over the last few
// frames is the "live" set. Pure UI: it reads the VM, never changes it.
#ifndef LAVA_LIVE_H
#define LAVA_LIVE_H

#include <stdint.h>

#include "lava.h"

typedef struct {
    uint8_t keys[128];      // 1 = the program compares the key it reads with this code
    int count;              // number of keys set
    int open;               // range tests too (typing letters/digits): the set isn't exhaustive
    int arrows;             // any of the four arrows
    int known;              // there was at least one read site with a recognisable comparison
    int text;               // looks like text entry: ranges, isalpha-style tests or 10+ letters/digits
} LiveSet;

// The live set over the last `window` VM frames.
void live_compute(LavaVM* vm, int window, LiveSet* out);

#endif
