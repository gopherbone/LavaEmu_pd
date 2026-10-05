// Turns the VM's screen into Playdate frame rows (1 = white): the classic
// 1-bpp LCD at 2x, flicker grey blended over the last three Refreshes, and
// LavaX's pixel screen (grey levels dithered: 2x2 patterns at 2x, a 4x4
// Bayer matrix at 1x). No Playdate dependencies.
#include "render.h"

#include <string.h>

static uint16_t dbl[256];      // 8 pixels -> 16, doubled, inverted
static uint16_t spread[256];   // 8 bits -> the even bits of 16 (bit 7 -> bit 15... as left pixels)

void render_init(void) {
    for (int b = 0; b < 256; b++) {
        uint16_t v = 0, s = 0;
        for (int i = 0; i < 8; i++)
            if (b & (0x80 >> i)) v |= 0xC000 >> (2 * i), s |= 0x8000 >> (2 * i);
        dbl[b] = (uint16_t)~v;
        spread[b] = s;
    }
}

static inline void put16(uint8_t* d, uint16_t v) {
    d[0] = (uint8_t)(v >> 8);
    d[1] = (uint8_t)v;
}

void render_row_1bpp(const uint8_t* src, uint8_t* top, uint8_t* bot) {
    for (int i = 0; i < 20; i++) put16(top + 2 * i, dbl[src[i]]);
    memcpy(bot, top, 40);
}

// n = how many of the three frames have ink at a pixel. Dither: 0, 1, 3 or 4
// black dots of the 2x2 cell (top-left from 1, top-right and bottom-right
// from 2, bottom-left at 3). Majority: ink if 2 or 3.
void render_row_blend3(const uint8_t* a, const uint8_t* b, const uint8_t* c, int dither, uint8_t* top, uint8_t* bot) {
    for (int i = 0; i < 20; i++) {
        uint8_t ge1 = a[i] | b[i] | c[i];
        uint8_t ge2 = (uint8_t)((a[i] & b[i]) | (a[i] & c[i]) | (b[i] & c[i]));
        uint8_t ge3 = a[i] & b[i] & c[i];
        if (dither) {
            put16(top + 2 * i, (uint16_t)~(spread[ge1] | spread[ge2] >> 1));
            put16(bot + 2 * i, (uint16_t)~(spread[ge3] | spread[ge2] >> 1));
        } else {
            put16(top + 2 * i, dbl[ge2]);
            put16(bot + 2 * i, dbl[ge2]);
        }
    }
}

// Darkness 0..255 of a pixel value in a LavaX mode.
static inline int darkness(int v, int mode, const uint8_t (*pal)[3]) {
    if (mode == 1) return v & 1 ? 255 : 0;
    if (mode == 4) return (v & 15) * 17;
    int r = pal[v][0], g = pal[v][1], bl = pal[v][2];
    return 255 - (r * 299 + g * 587 + bl * 114) / 1000;
}

// 2x2 cells: k black dots of 0..4 (TL, BR, TR, BL in that order).
void render_row_px2x(const uint8_t* src, int mode, const uint8_t (*pal)[3], uint8_t* top, uint8_t* bot) {
    memset(top, 0xFF, 40);
    memset(bot, 0xFF, 40);
    for (int x = 0; x < 160; x++) {
        int k = (darkness(src[x], mode, pal) * 4 + 127) / 255;
        if (!k) continue;
        int bx = 2 * x;
        uint8_t ml = (uint8_t)(0x80 >> (bx & 7)), mr = (uint8_t)(0x80 >> ((bx + 1) & 7));
        int o = bx >> 3;
        top[o] &= (uint8_t)~ml;                     // TL
        if (k >= 2) bot[o] &= (uint8_t)~mr;         // BR
        if (k >= 3) top[o] &= (uint8_t)~mr;         // TR
        if (k >= 4) bot[o] &= (uint8_t)~ml;         // BL
    }
}

static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

void render_row_px1x(const uint8_t* src, int w, int y, int mode, const uint8_t (*pal)[3], uint8_t* out) {
    memset(out, 0xFF, 40);
    int x0 = (320 - w) / 2;
    for (int x = 0; x < w; x++) {
        int d = darkness(src[x], mode, pal);
        int thr = bayer4[y & 3][x & 3] * 16 + 8;
        if (d > thr || d == 255) {
            int px = x0 + x;
            out[px >> 3] &= (uint8_t)~(0x80 >> (px & 7));
        }
    }
}
