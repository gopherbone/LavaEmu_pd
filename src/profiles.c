#include "profiles.h"

#include <string.h>

#include "lava.h"

#define K(c, l) {(c), (l)}
#define NONE {0, 0}

static const Profile profiles[] = {
    {"frog", "Frog Monopoly",
     {K('y', "Yes"), K(LK_PGDN, "Next tab"), K('n', "No"), K(LK_PGUP, "Prev tab")},
     {K('y', "Yes"), K('n', "No"), K(LK_PGDN, "Next tab"), K(LK_PGUP, "Prev tab"), K(LK_F2, "Discard card"),
      K('p', "Discard (P)"), K('s', "Quick save"), K('r', "Quick load"), K('q', "Prev tab / Quit"), K('w', "Next tab"), K('a', "Back"),
      K(LK_HELP, "Help")},
     12, "Yes/No prompts that don't read arrows put Yes and No on the D-pad. On the map S and R quick-save and "
         "-load; in the menu PgUp/PgDn page the tabs."},
    {"ace", "Phoenix Wright: Ace Attorney",
     {K('r', "Court Record"), NONE, NONE, NONE},
     {K('r', "Court Record")},
     1, "R opens the Court Record (and presents evidence). Everything else is Enter, Esc and arrows."},
    {"newhero", "New Heroes' Altar",
     {K('y', "Yes"), NONE, K('n', "No"), NONE},
     {K('y', "Yes"), K('n', "No"), K('c', "Challenge"), K('k', "Kill"), K('h', "Head off"), K('b', "Body off"),
      K('d', "Hand off"), K('f', "Feet off"), K(LK_PGUP, "PgUp"), K(LK_CAPS, "Caps"), K('r', "Reset keys")},
     11, "Esc on the map opens the status and menu pages. Fight mode: C challenge, K kill. Gear page: H B D F take "
        "off head, body, hand, feet."},
    {"shushan", "Heroes of Mount Shu",
     {K(LK_F1, "Gear"), K(LK_F2, "Items"), K(LK_F3, "Status"), K(LK_F4, "Arts")},
     {K(LK_F1, "Gear"), K(LK_F2, "Items / Del"), K(LK_F3, "Status"), K(LK_F4, "Arts"), K('p', "Points"),
      K('y', "Yes"), K('n', "No"), K(LK_CAPS, "Caps"), K(LK_SHIFT, "Shift")},
     9, "Esc opens Options/Quit/Save/Load. Names and passwords: Keyboard; F2 deletes a letter."},
    {"seal", "Sky & Land II",
     {K(LK_F1, "Menu"), K('y', "Yes"), K('p', "Pets"), K('n', "No")},
     {K(LK_F1, "Menu"), K('o', "Cast"), K('p', "Pets"), K('d', "Drop"), K('y', "Yes"), K('n', "No / 2"), K('b', "1"),
      K('m', "3"), K(LK_SPACE, "Space"), K(LK_CAPS, "Caps"), K(LK_PGUP, "PgUp")},
     11, "Enter attacks, O casts. Number choices are typed with B N M (1 2 3). Account names: Keyboard."},
    {"skyland", "Sky & Land",
     {K(LK_F1, "Stats"), K(LK_F2, "Gear"), K(LK_F3, "Skills"), K(LK_F4, "Items")},
     {K('o', "Cast"), K('p', "End turn"), K(LK_HELP, "System"), K(LK_F1, "Stats"), K(LK_F2, "Gear"),
      K(LK_F3, "Skills"), K(LK_F4, "Items"), K('d', "Drop"), K('y', "Yes"), K('n', "No / 2"), K('b', "1"),
      K('m', "3"), K(LK_SPACE, "Space"), K(LK_CAPS, "Caps"), K(LK_SHIFT, "Shift")},
     15, "Enter attacks. Number choices are typed with B N M (1 2 3). Account names: Keyboard."},
    {"mario", "Mario Pipes",
     {K(LK_F1, "Save"), NONE, NONE, NONE},
     {K(LK_F1, "Save"), K(LK_ENTER, "Enter"), K('u', "Jump")},
     3, "The D-pad sends W D S A (walk with A/D), A jumps (U). F1 on the stage card saves. Record names: Keyboard.",
     {'w', 'd', 's', 'a'}, K('u', "Jump")},
    {"fujia", "The Millionaire of 3 Kingdoms",
     {K(LK_F1, "Buy / Recruit"), K(LK_F2, "Sell / Dismiss"), K(LK_F4, "Map cursor"), K(LK_HELP, "General")},
     {K(LK_F1, "Buy / Recruit"), K(LK_F2, "Sell / Dismiss"), K(LK_F4, "Map cursor"), K(LK_HELP, "General"),
      K(LK_PGUP, "PgUp"), K(LK_PGDN, "PgDn"), K('b', "1"), K('n', "2"), K('m', "3"), K('g', "4"), K('h', "5"),
      K('j', "6"), K('t', "7"), K('y', "8"), K('u', "9"), K('0', "0")},
     16, "Numbers are typed on the letter keypad: B N M G H J T Y U = 1-9."},
    {"sanguo", "Three Kingdoms",
     {NONE, NONE, NONE, NONE},
     {K(LK_SPACE, "Space")},
     1, "Arrows, Enter and Esc play the whole game. Typing LAVA at the academy computer: Keyboard."},
    {"snowman", "The Story of the Snowman",
     {K('p', "Pause"), NONE, NONE, NONE},
     {K('p', "Pause"), K('s', "Jump"), K('a', "Throw")},
     3, "D-pad up jumps (S), down throws a snowball (A); left and right walk. The game reads one key at a time.",
     {'s', 0, 'a', 0}},
    {"warcraft", "WarCraft",
     {K(LK_PGUP, "Base / Shop"), K('j', "End turn"), K(LK_PGDN, "Hero"), K(LK_HELP, "System")},
     {K('j', "End turn"), K(LK_PGUP, "Base / Shop"), K(LK_PGDN, "Hero"), K(LK_CAPS, "Stats"),
      K(LK_SHIFT, "Status bar"), K(LK_HELP, "System")},
     6, "Caps shows a unit's stats or a description; PgUp in the bag opens the shop."},
    {"pokemon", "Pocket Monsters Grey",
     {K('q', "Quit"), NONE, NONE, NONE},
     {K('q', "Quit")},
     1, "Enter acts and opens the menu, Esc goes back. Its grey is flicker, blended (Options > Flicker grey).",
     {0, 0, 0, 0}, NONE, 20},
    {"school", "High School Legend",
     {K(LK_HELP, "Menu"), K('y', "Yes"), NONE, NONE},
     {K(LK_HELP, "Menu"), K('y', "Yes"), K(LK_SHIFT, "Shift"), K(LK_F1, "F1"), K(LK_F2, "F2 / Delete"),
      K(LK_F3, "F3"), K(LK_F4, "F4"), K(LK_SPACE, "Space")},
     8, "Help opens the main menu. Your name: Keyboard (Enter alone gives Qiang)."},
    {"jianghu", "Jianghu",
     {K(LK_HELP, "Menu"), K('y', "Yes"), K('a', "Run"), NONE},
     {K(LK_HELP, "Menu"), K('y', "Yes"), K('a', "Run"), K(LK_F2, "Delete"), K(LK_SPACE, "Space")},
     5, "Help opens the menu. The diary takes typed letters: Keyboard."},
    {"worms", "Worms",
     {K('t', "Jump"), K('g', "Weapon mode"), K('m', "Map"), K('h', "Everyone's HP")},
     {K(LK_SPACE, "Start"), K('t', "Jump"), K('g', "Weapon mode"), K('q', "Game menu"), K('h', "Everyone's HP"),
      K('m', "Map")},
     6, "Hold Enter (A) to charge a shot. Space starts a game."},
    {"mota", "Magic Tower",
     {K(LK_PGUP, "Fly up"), K(LK_F2, "Water of Life"), K(LK_PGDN, "Fly down"), K(LK_F1, "Iron Spade")},
     {K(LK_PGUP, "Fly up"), K(LK_PGDN, "Fly down"), K(LK_F1, "Iron Spade"), K(LK_F2, "Water of Life"),
      K('y', "Yes"), K('n', "No")},
     6, "Esc opens the menu (Status, Monsters, Save, Load, Quit) and breaks off a fight. Fly up/down needs the "
        "Leaping Scepter and a staircase next to you."},
    {"yongzhe", "Legend of the Brave",
     {K(LK_F1, "Restart level"), NONE, NONE, NONE},
     {K(LK_F1, "Restart level")},
     1, "Enter on an arrow block moves it one cell its way. Esc goes back to the title."},
    {"rushout", "Rush Out the Tunnel",
     {NONE, K('x', "Bomb"), K('y', "Yes"), K('z', "Pause")},
     {K('x', "Bomb"), K('z', "Pause"), K('y', "Yes"), K(LK_SPACE, "Space"), K(LK_F2, "F2")},
     5, "Hold up to climb, let go to sink. Enter resumes from pause; Esc asks to quit."},
    {"tetris", "Tetris",
     {K(LK_F1, "Clock (hold)"), K('y', "Yes"), NONE, NONE},
     {K(LK_F1, "Clock (hold)"), K('y', "Yes"), K(LK_F2, "Delete")},
     3, "Up rotates, Enter pauses (Setup redefines the five play keys). Names: Keyboard, F2 deletes."},
    {"zuanshi", "Diamond Blocks",
     {K(LK_F1, "Clock (hold)"), K('y', "Yes"), NONE, NONE},
     {K(LK_F1, "Clock (hold)"), K('y', "Yes"), K(LK_F2, "Delete")},
     3, "Up turns the blocks in the piece, Enter pauses (Setup redefines the play keys). Names: Keyboard."},
    {"huanying", "Phantom Fighter",
     {K('p', "Pause"), K(LK_ENTER, "Enter (choose)"), K('z', "Start stage +1"), NONE},
     {K(LK_ENTER, "Enter (choose)"), K('p', "Pause"), K('z', "Start stage +1")},
     3, "A fires (the game's A key); menu items are chosen with Enter: B + right, or the palette.",
     {0, 0, 0, 0}, K('a', "Fire")},
    {"mofa", "Magic Blocks",
     {K('y', "Yes (save keys)"), NONE, NONE, NONE},
     {K('y', "Yes (save keys)")},
     1, "Up cycles the blocks, Enter pauses. Hall of Fame names: Keyboard."},
    {"zhuangqiu", "Billiards Master",
     {K(LK_ENTER, "Enter"), K('p', "Speed settings"), K('y', "Yes"), K('n', "No")},
     {K(LK_ENTER, "Enter"), K(LK_PGUP, "Turbo aim (hold)"), K('p', "Speed settings"), K('y', "Yes"),
      K('n', "No"), K(LK_F2, "Delete")},
     6, "Hold A to charge a shot (Space), let go to strike; menus take Enter on B + up or the palette. "
        "Turbo (PgUp at first) is held from the palette while the D-pad aims.",
     {0, 0, 0, 0}, K(LK_SPACE, "Charge / shoot")},
    {"huaxue", "Power Ski",
     {K('q', "Stunt Q"), K('w', "Stunt W"), K('e', "Stunt E"), K('a', "Stunt A")},
     {K('q', "Stunt Q"), K('w', "Stunt W"), K('e', "Stunt E"), K('a', "Stunt A"), K('s', "Stunt S"),
      K('d', "Stunt D"), K(LK_HELP, "Status")},
     7, "Up jumps (and gets you up after a fall); in the air, stunts. Help shows your status."},
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
