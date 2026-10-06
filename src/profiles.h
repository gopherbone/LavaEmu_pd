// Per-game key profiles: what the B+D-pad chords and the crank palette send,
// labelled with what the keys do in that game. Found by instrumenting the VM
// on each game's QA routes (tools/keyreport.py, tools/keystatic.py) and
// checking each key's effect (tools/keyshots.py).
#ifndef PROFILES_H
#define PROFILES_H

#include <stdint.h>

#define PROFILE_MAX_KEYS 24

typedef struct {
    uint8_t code;           // LAVA key code
    const char* label;      // what it does in this game ("Next tab"); NULL = key name
} ProfileKey;

typedef struct {
    const char* id;         // game.txt "profile=" value
    const char* name;       // shown in the key view
    ProfileKey chord[4];    // B + up, right, down, left (code 0 = none)
    ProfileKey palette[PROFILE_MAX_KEYS];
    int npalette;
    const char* notes;      // a line for the key view
    uint8_t dpad[4];        // keys the D-pad sends (up, right, down, left); 0 = the arrows
    ProfileKey a_key;       // what A sends; code 0 = Enter
    uint8_t min_hold;       // VM frames a tap stays down at least (games that poll slowly)
    uint8_t a_live;         // A sends a_key only while the game reads it (live keys), Enter otherwise
} Profile;

const Profile* profile_find(const char* id);

// A profile for a game without one: the chords hold F1-F4 and the palette
// lists every key the program compares a key it read against (a static scan
// of the bytecode, as tools/keystatic.py). `out` must stay alive while used.
void profile_auto(const uint8_t* code, uint32_t len, Profile* out);

// "PgDn", "F1", "Y", ...
const char* key_name(int code);

#endif
