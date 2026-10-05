#include "profiles.h"

#include <string.h>

#include "lava.h"

#define K(c, l) {(c), (l)}
#define NONE {0, 0}

static const Profile profiles[] = {
    {"frog", "Frog Monopoly",
     {K(LK_F2, "Discard card"), K(LK_PGDN, "Next tab"), K('s', "Quick save"), K(LK_PGUP, "Prev tab")},
     {K(LK_PGDN, "Next tab"), K(LK_PGUP, "Prev tab"), K(LK_F2, "Discard card"), K('y', "Yes"), K('n', "No"),
      K('a', "Back"), K('s', "Quick save"), K('r', "Quick load"), K('q', "Quit menu"), K(LK_HELP, "Help")},
     10, "On the map any other key opens the menu. Menus: Next/Prev tab page through cards, info, status."},
    {"ace", "Phoenix Wright: Ace Attorney",
     {K('r', "Court Record"), NONE, NONE, NONE},
     {K('r', "Court Record")},
     1, "R opens the Court Record (and presents evidence). Everything else is Enter, Esc and arrows."},
    {"newhero", "New Heroes' Altar",
     {K('y', "Yes"), NONE, K('n', "No"), NONE},
     {K('y', "Yes"), K('n', "No"), K('c', "C"), K('k', "K"), K('b', "B"), K('d', "D"), K('f', "F"), K('h', "H"),
      K('r', "R")},
     9, "Esc on the map opens the status and menu pages; Enter talks and confirms."},
    {"shushan", "Heroes of Mount Shu",
     {K(LK_F1, "Gear"), K(LK_F2, "Items"), K(LK_F3, "Status"), K(LK_F4, "Arts")},
     {K(LK_F1, "Gear"), K(LK_F2, "Items / Del"), K(LK_F3, "Status"), K(LK_F4, "Arts"), K('p', "Points"),
      K('y', "Yes"), K('n', "No"), K(LK_CAPS, "Caps"), K(LK_SHIFT, "Shift")},
     9, "Esc opens Options/Quit/Save/Load. Names and passwords: Keyboard; F2 deletes a letter."},
    {"seal", "Sky & Land II",
     {K(LK_F1, "Menu"), K('y', "Yes"), K('p', "Pets"), K('n', "No")},
     {K(LK_F1, "Menu"), K('p', "Pets"), K('d', "Drop"), K('y', "Yes"), K('n', "No / 2"), K('b', "1"),
      K('m', "3"), K(LK_SPACE, "Space"), K(LK_CAPS, "Caps"), K(LK_PGUP, "PgUp")},
     10, "Number choices are typed with B N M (1 2 3). Account names: Keyboard."},
};

const Profile* profile_find(const char* id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < sizeof profiles / sizeof profiles[0]; i++)
        if (!strcmp(profiles[i].id, id)) return &profiles[i];
    return NULL;
}

const char* key_name(int c) {
    static char buf[2];
    switch (c) {
    case LK_ENTER: return "Enter";
    case LK_PGDN: return "PgDn";
    case LK_CAPS: return "Caps";
    case LK_PGUP: return "PgUp";
    case LK_UP: return "Up";
    case LK_DOWN: return "Down";
    case LK_RIGHT: return "Right";
    case LK_LEFT: return "Left";
    case LK_HELP: return "Help";
    case LK_SHIFT: return "Shift";
    case LK_ESC: return "Esc";
    case LK_F1: return "F1";
    case LK_F2: return "F2";
    case LK_F3: return "F3";
    case LK_F4: return "F4";
    case LK_SPACE: return "Space";
    }
    if (c >= 'a' && c <= 'z') buf[0] = (char)(c - 32);
    else buf[0] = (char)c;
    buf[1] = 0;
    return buf;
}

// Operand bytes per opcode (lavaemu lav.py OPLEN); 0x0D and 0x41 are variable.
static int oplen(int op) {
    switch (op) {
    case 0x01: case 0x43: return 1;
    case 0x03: return 4;
    case 0x39: case 0x3A: case 0x3B: case 0x3D: case 0x3E: return 3;
    case 0x3C: return 2;
    }
    if ((op >= 0x02 && op <= 0x0C) || (op >= 0x0E && op <= 0x19) || (op >= 0x45 && op <= 0x51)) return 2;
    return 0;
}

static int keyish(int v) {
    return v == LK_ENTER || v == LK_PGDN || v == LK_CAPS || v == LK_PGUP || (v >= LK_UP && v <= LK_LEFT) ||
           v == LK_HELP || v == LK_SHIFT || v == LK_ESC || (v >= LK_F1 && v <= LK_SPACE) || (v >= 'a' && v <= 'z') ||
           (v >= '0' && v <= '9');
}

void profile_auto(const uint8_t* code, uint32_t len, Profile* out) {
    memset(out, 0, sizeof *out);
    out->id = "auto";
    out->name = "Keys this program reads";
    out->chord[0] = (ProfileKey){LK_F1, 0};
    out->chord[1] = (ProfileKey){LK_F2, 0};
    out->chord[2] = (ProfileKey){LK_F3, 0};
    out->chord[3] = (ProfileKey){LK_F4, 0};
    out->notes = "No profile for this game: the palette lists the keys its code compares, the chords send F1-F4.";
    uint8_t seen[128] = {0};
    // Pass 1: per function (0x3E), does it read keys? Pass 2 collects constants.
    uint32_t pc = 16, fstart = 16;
    int reads = 0, prev_op = -1, prev_arg = 0;
    uint8_t cand[128] = {0};
    while (pc < len) {
        int op = code[pc];
        if (!(op <= 0x51 || (op >= 0x80 && op <= 0xCA))) break;
        uint32_t size;
        int arg = 0;
        if (op == 0x0D) {
            uint32_t e = pc + 1;
            while (e < len && code[e]) e++;
            size = e + 1 - pc;
        } else if (op == 0x41) {
            if (pc + 5 > len) break;
            size = 5 + (code[pc + 3] | code[pc + 4] << 8);
        } else {
            int n = oplen(op);
            size = 1 + n;
            if (n == 1) arg = code[pc + 1];
            else if (n == 2 && pc + 2 < len) arg = (int16_t)(code[pc + 1] | code[pc + 2] << 8);
        }
        if (op == 0x3E) {
            if (reads)
                for (int i = 0; i < 128; i++) seen[i] |= cand[i];
            memset(cand, 0, sizeof cand);
            reads = 0;
            fstart = pc;
        }
        if (op == 0x81 || op == 0x93 || op == 0xC4 || op == 0xBC) reads = 1;
        if ((op == 0x4C || op == 0x4D) && arg > 0 && arg < 128 && keyish(arg)) cand[arg] = 1;
        if (op == 0xBC && (prev_op == 0x01 || prev_op == 0x02) && prev_arg > 0 && prev_arg < 128 && keyish(prev_arg))
            cand[prev_arg] = 1;
        prev_op = op;
        prev_arg = arg;
        pc += size;
    }
    (void)fstart;
    if (reads)
        for (int i = 0; i < 128; i++) seen[i] |= cand[i];
    // Order: F-keys, page keys, Help, then letters and digits; arrows, Enter
    // and Esc are on the buttons already.
    static const uint8_t order[] = {LK_F1, LK_F2, LK_F3, LK_F4, LK_PGUP, LK_PGDN, LK_HELP, LK_SPACE, LK_SHIFT, LK_CAPS};
    int n = 0;
    for (unsigned i = 0; i < sizeof order && n < PROFILE_MAX_KEYS; i++)
        if (seen[order[i]]) out->palette[n++] = (ProfileKey){order[i], 0};
    for (int c = 'a'; c <= 'z' && n < PROFILE_MAX_KEYS; c++)
        if (seen[c]) out->palette[n++] = (ProfileKey){(uint8_t)c, 0};
    for (int c = '0'; c <= '9' && n < PROFILE_MAX_KEYS; c++)
        if (seen[c]) out->palette[n++] = (ProfileKey){(uint8_t)c, 0};
    if (n == 0) {
        static const uint8_t dflt[] = {LK_F1, LK_F2, LK_F3, LK_F4, LK_PGUP, LK_PGDN, LK_HELP, 'y', 'n', LK_SPACE};
        for (unsigned i = 0; i < sizeof dflt; i++) out->palette[n++] = (ProfileKey){dflt[i], 0};
    }
    out->npalette = n;
}
