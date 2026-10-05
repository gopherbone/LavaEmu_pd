// LavaX support: the pixel screen (lavaemu screenx.py, after LeeSoft's
// lava.c), ops 0x52-0x74 and system calls 0xCB-0xD6 (lavaemu vm.py
// _ext_op, _sys_px, _sys_lavax). One byte a pixel, LCD and buffer planes
// outside LAVA RAM, modes 1 (2 colours), 4 (16 greys: 0 white .. 15 black)
// and 8 (256 colours).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lava_internal.h"

static inline uint32_t W16(int32_t v) { return (uint32_t)v & 0xFFFF; }
static inline int32_t S16(int32_t v) { return (int16_t)(uint16_t)v; }

// ---------------------------------------------------------------------------
// Allocation

LavaPx* lavax_px_new(int w, int h, int mode) {
    LavaPx* p = lava_realloc(NULL, sizeof *p);
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->lcd = lava_realloc(NULL, (size_t)w * h);
    p->buf = lava_realloc(NULL, (size_t)w * h);
    if (!p->lcd || !p->buf) {
        lavax_px_free(p);
        return NULL;
    }
    memset(p->lcd, 0, (size_t)w * h);
    memset(p->buf, 0, (size_t)w * h);
    p->w = (uint16_t)w, p->h = (uint16_t)h, p->mode = (uint8_t)mode;
    p->bg = 0;
    p->fg = mode != 8 ? 15 : 255;
    return p;
}

void lavax_px_free(LavaPx* p) {
    if (!p) return;
    if (p->lcd) lava_realloc(p->lcd, 0);
    if (p->buf) lava_realloc(p->buf, 0);
    lava_realloc(p, 0);
}

static inline uint8_t pmask(const LavaPx* p) { return p->mode == 1 ? 1 : p->mode == 4 ? 15 : 255; }
static inline uint8_t* plane(LavaPx* p, int screen) { return screen ? p->lcd : p->buf; }

int lavax_set_mode(LavaPx* p, int m) {
    int old = p->mode;
    m &= 0xFF;
    if (m == 1 || m == 4 || m == 8) {
        if (m != p->mode) {
            if (m == 4) p->bg = 0, p->fg = 15;
            else if (m == 8) p->bg = 0, p->fg = 255;
            memset(p->lcd, m == 1 ? 0 : p->bg, (size_t)p->w * p->h);
            p->mode = (uint8_t)m;
        }
        return old;
    }
    return m == 0 ? old : 0;
}

// ---------------------------------------------------------------------------
// Blits. A source is 1, 4 or 8 bits a pixel in memory, or a 1-bpp glyph
// coloured with fg/bg (blit_glyph's expansion, without the round trip).

enum { SRC_1BPP, SRC_4BPP, SRC_8BPP, SRC_GLYPH };

typedef struct {
    const uint8_t* data;
    uint32_t len;     // bytes readable from data (beyond: 0)
    uint32_t addr;
    int kind, bpl;
    uint8_t fg, bg;
} Src;

static inline uint8_t sbyte(const Src* s, uint32_t i) { return i < s->len ? s->data[i] : 0; }

static inline uint8_t spix(const Src* s, int r, int k) {
    uint32_t row = s->addr + (uint32_t)r * s->bpl;
    switch (s->kind) {
    case SRC_1BPP: return (sbyte(s, row + (k >> 3)) >> (7 - (k & 7))) & 1;
    case SRC_4BPP: {
        uint8_t b = sbyte(s, row + (k >> 1));
        return k & 1 ? b & 15 : b >> 4;
    }
    case SRC_8BPP: return sbyte(s, row + k);
    default: return (sbyte(s, row + (k >> 3)) >> (7 - (k & 7))) & 1 ? s->fg : s->bg;
    }
}

// write_comm (screenx.py blit). x, y, w, h as LavaX reads them (words).
static void blit(LavaPx* p, int screen, int32_t xi, int32_t yi, int32_t wi, int32_t hi, Src* s, int lcmd, int mirror) {
    uint32_t x = W16(xi), y = W16(yi), w = W16(wi), h = W16(hi);
    if (!w || !h) return;
    int m = p->mode;
    if (s->kind != SRC_GLYPH) {
        s->kind = m == 1 ? SRC_1BPP : m == 4 ? SRC_4BPP : SRC_8BPP;
        s->bpl = m == 1 ? (int)((w + 7) >> 3) : m == 4 ? (int)((w + 1) >> 1) : (int)w;
    } else {
        s->bpl = (int)((w + 7) >> 3);
    }
    uint32_t sw = p->w, sh = p->h;
    // mirror needs whole bytes in the mode's format
    if (mirror && m != 8 && (w & (m == 1 ? 7u : 1u))) mirror = 0;
    uint32_t r0 = 0;
    if (y >= sh) {
        uint32_t t = 0x10000 - y;
        if (h <= t) return;
        h -= t;
        r0 = t;
        y = 0;
    }
    if (y + h > sh) {
        if (sh <= y) return;
        h = sh - y;
    }
    uint32_t c0 = 0;
    if (x >= sw) {
        uint32_t t = 0x10000 - x;
        if (w <= t) return;
        c0 = t;
        x = 0;
    }
    uint32_t c1 = c0 + sw - x;
    if (c1 > w) c1 = w;
    if (c1 <= c0) return;
    int inv = (lcmd & 8) || (lcmd & 7) == 2;
    int op = lcmd & 7;
    uint8_t msk = pmask(p);
    uint8_t* pl = plane(p, screen);
    for (uint32_t r = 0; r < h; r++) {
        uint8_t* d = pl + (y + r) * sw + x;
        int sr = (int)(r0 + r);
        for (uint32_t j = c0; j < c1; j++) {
            int k = mirror ? (int)(w - 1 - j) : (int)j;
            uint8_t v = spix(s, sr, k);
            if (inv) v ^= msk;
            uint8_t* q = d + (j - c0);
            if (op == 3) *q |= v;
            else if (op == 4) *q &= v;
            else if (op == 5) *q ^= v;
            else if (op == 6 && m == 8 && !inv) {
                if (v) *q = v;
            } else *q = v;
        }
    }
}

void lavax_glyph(LavaVM* vm, int code, int big, int* w, int* h, const uint8_t** g, int* n) {
    static const uint8_t zeros[32];
    *h = big ? 16 : 12;
    if (code < 0x100 && code >= 128) {
        *w = big ? 8 : 6;
        *g = zeros;
        *n = ((*w + 7) / 8) * *h;
        return;
    }
    lava_glyph(vm, code, big, w, g, n);
}

static void blit_glyph(LavaVM* vm, int screen, uint32_t x, uint32_t y, int w, int h, const uint8_t* g, int n, int lcmd,
                       int mirror) {
    LavaPx* p = vm->px;
    Src s = {g, (uint32_t)n, 0, p->mode == 1 ? SRC_1BPP : SRC_GLYPH, 0, p->fg, p->bg};
    blit(p, screen, (int32_t)x, (int32_t)y, w, h, &s, lcmd, mirror);
}

static uint32_t text(LavaVM* vm, int screen, int32_t xi, int32_t yi, const uint8_t* data, int n, int big, int lcmd,
                     int mirror) {
    LavaPx* p = vm->px;
    uint32_t x = W16(xi), y = W16(yi);
    int i = 0;
    while (i < n) {
        if (x >= p->w) break;
        int c = data[i++];
        int code = c;
        if (c >= 0x80) {
            int c2 = 0;
            if (i < n) c2 = data[i++];
            code = c | c2 << 8;
        }
        int w, h, gn;
        const uint8_t* g;
        lavax_glyph(vm, code, big, &w, &h, &g, &gn);
        blit_glyph(vm, screen, x, y, w, h, g, gn, lcmd, mirror);
        x = (x + (uint32_t)w) & 0xFFFF;
    }
    return x;
}

// ---------------------------------------------------------------------------
// Points and shapes (lcmd 0 background, 1 foreground, 2 invert)

static void dot(LavaPx* p, int screen, int32_t xi, int32_t yi, int lcmd) {
    uint32_t x = W16(xi), y = W16(yi);
    if (x >= p->w || y >= p->h) return;
    uint8_t* q = plane(p, screen) + y * p->w + x;
    if (lcmd == 0) *q = p->mode == 1 ? 0 : p->bg;
    else if (lcmd == 1) *q = p->mode == 1 ? 1 : p->fg;
    else if (lcmd == 2) *q ^= pmask(p);
}

static void hline(LavaPx* p, int screen, int x0, int x1, int y, int lcmd) {
    uint8_t* q = plane(p, screen) + y * p->w;
    if (lcmd == 0) memset(q + x0, p->mode == 1 ? 0 : p->bg, (size_t)(x1 - x0 + 1));
    else if (lcmd == 1) memset(q + x0, p->mode == 1 ? 1 : p->fg, (size_t)(x1 - x0 + 1));
    else if (lcmd == 2) {
        uint8_t m = pmask(p);
        for (int i = x0; i <= x1; i++) q[i] ^= m;
    }
}

static void vline(LavaPx* p, int screen, int32_t x, int32_t y0, int32_t y1, int lcmd) {
    if (y0 > y1) {
        int32_t t = y0;
        y0 = y1, y1 = t;
    }
    for (int32_t yy = y0; yy <= y1; yy++) dot(p, screen, x, yy, lcmd);
}

static void block_check(LavaPx* p, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    int32_t a = (int32_t)W16(*x0), b = (int32_t)W16(*y0), c = (int32_t)W16(*x1), d = (int32_t)W16(*y1), t;
    if (b > d) t = b, b = d, d = t;
    if (a > c) t = a, a = c, c = t;
    int32_t w = p->w, h = p->h;
    *x0 = a < w - 1 ? a : w - 1;
    *y0 = b < h - 1 ? b : h - 1;
    *x1 = c < w - 1 ? c : w - 1;
    *y1 = d < h - 1 ? d : h - 1;
}

static void pblock(LavaPx* p, int screen, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int lcmd) {
    block_check(p, &x0, &y0, &x1, &y1);
    for (int32_t yy = y0; yy <= y1; yy++) hline(p, screen, x0, x1, yy, lcmd);
}

static void prect(LavaPx* p, int screen, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int lcmd) {
    block_check(p, &x0, &y0, &x1, &y1);
    hline(p, screen, x0, x1, y0, lcmd);
    vline(p, screen, x0, y0, y1, lcmd);
    vline(p, screen, x1, y0, y1, lcmd);
    hline(p, screen, x0, x1, y1, lcmd);
}

static void pline(LavaPx* p, int screen, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int lcmd) {
    x0 = (int32_t)W16(x0), y0 = (int32_t)W16(y0), x1 = (int32_t)W16(x1), y1 = (int32_t)W16(y1);
    if (x0 == x1) {
        vline(p, screen, x0, y0, y1, lcmd);
        return;
    }
    if (y0 == y1) {
        if (y0 >= p->h) return;
        if (x0 > x1) {
            int32_t t = x0;
            x0 = x1, x1 = t;
        }
        if (x0 >= p->w) return;
        hline(p, screen, x0, x1 < p->w - 1 ? x1 : p->w - 1, y0, lcmd);
        return;
    }
    if (x1 < x0) {
        int32_t t = x0;
        x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
    }
    int32_t dx = x1 - x0, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t inc = y1 > y0 ? 1 : -1;
    int32_t dist = dx > dy ? dx : dy;
    int32_t xe = 0, ye = 0, xx = x0, yy = y0, t = 0;
    for (;;) {
        dot(p, screen, xx, yy, lcmd);
        xe += dx;
        ye += dy;
        if (xe >= dist) xe -= dist, xx++;
        if (ye >= dist) ye -= dist, yy += inc;
        t++;
        if (dist < t) break;
    }
}

typedef struct {
    LavaPx* p;
    int screen, lcmd, fill;
    int32_t x0, y0;
    int32_t* cbuf;
} Ell;

static void eput(Ell* e, int32_t x, int32_t y) {
    if (x >= 0 && x < e->p->w && y >= 0 && y < e->p->h) dot(e->p, e->screen, x, y, e->lcmd);
}

static void eput4(Ell* e, int32_t x, int32_t y) {
    if (e->fill) {
        int32_t ys[2] = {e->y0 - y, e->y0 + y};
        for (int i = 0; i < 2; i++)
            if (ys[i] >= 0 && ys[i] < e->p->h && x >= e->cbuf[ys[i]]) e->cbuf[ys[i]] = x;
    } else if (x == 0) {
        eput(e, e->x0, e->y0 + y), eput(e, e->x0, e->y0 - y);
    } else if (y == 0) {
        eput(e, e->x0 + x, e->y0), eput(e, e->x0 - x, e->y0);
    } else {
        eput(e, e->x0 + x, e->y0 + y), eput(e, e->x0 - x, e->y0 + y);
        eput(e, e->x0 + x, e->y0 - y), eput(e, e->x0 - x, e->y0 - y);
    }
}

// LavaX's Ellipsex
static void pellipse(LavaPx* p, int screen, int32_t x0, int32_t y0, int32_t ra, int32_t rb, int fill, int lcmd) {
    static int32_t cbuf[256];
    Ell e = {p, screen, lcmd, fill, S16(x0), S16(y0), cbuf};
    ra = (int32_t)W16(ra), rb = (int32_t)W16(rb);
    if (fill)
        for (int i = 0; i < p->h; i++) cbuf[i] = -1;
    if (ra == 0) {
        eput(&e, e.x0, e.y0);
        return;
    }
    int32_t r = ra > rb ? ra : rb;
    int32_t incx = -1, incy = 1, fy = 1, fx = 1 - 2 * r, fxy = 0, dx = 0, dy = 0, tx = ra, ty = 0;
    int started = 0;
    eput4(&e, tx, ty);
    for (;;) {
        if (fxy >= 0) {
            dx = (dx + ra) & 0xFFFF;
            if (dx >= r) {
                tx += incx;
                dx -= r;
                if (tx + 1 != ra) eput4(&e, tx, ty);
            }
            fxy -= fx < 0 ? -fx : fx;
            fx += 2;
            if (!(fx < 0 || fx >= 3)) {
                incy = -incy;
                fy = -fy + 2;
                fxy = -fxy;
            }
        } else {
            dy = (dy + rb) & 0xFFFF;
            if (dy >= r) {
                dy -= r;
                ty += incy;
                if ((ty == 1 || ty == 2) && !started) {
                    eput4(&e, ra, ty);
                } else {
                    started = 1;
                    eput4(&e, tx, ty);
                }
            }
            fxy += fy < 0 ? -fy : fy;
            fy += 2;
            if (!(fy < 0 || fy > 2)) {
                incx = -incx;
                fx = -fx + 2;
                fxy = -fxy;
            }
        }
        if (!tx) break;
    }
    if (fill) {
        for (int yy = 0; yy < p->h; yy++) {
            int32_t c = cbuf[yy];
            if (c >= 0)
                for (int32_t j = 0; j < c * 2 + 1; j++) eput(&e, e.x0 - c + j, yy);
        }
    }
}

static void xdraw(LavaPx* p, int32_t mode) {
    uint8_t* b = p->buf;
    int w = p->w, h = p->h;
    uint8_t fill = p->mode == 1 ? 0 : p->bg;
    int t = mode & 0xFF;
    if (t == 0) {
        for (int r = 0; r < h; r++) {
            memmove(b + r * w, b + r * w + 1, (size_t)w - 1);
            b[r * w + w - 1] = fill;
        }
    } else if (t == 1) {
        for (int r = 0; r < h; r++) {
            memmove(b + r * w + 1, b + r * w, (size_t)w - 1);
            b[r * w] = fill;
        }
    } else if (t == 2) {
        memmove(b, b + w, (size_t)w * (h - 1));
        memset(b + w * (h - 1), fill, (size_t)w);
    } else if (t == 3) {
        memmove(b + w, b, (size_t)w * (h - 1));
        memset(b, fill, (size_t)w);
    } else if (t == 4) {
        for (int r = 0; r < h; r++)
            for (int i = 0; i < w / 2; i++) {
                uint8_t v = b[r * w + i];
                b[r * w + i] = b[r * w + w - 1 - i];
                b[r * w + w - 1 - i] = v;
            }
    } else if (t == 5) {
        for (int r = 0; r < h / 2; r++)
            for (int i = 0; i < w; i++) {
                uint8_t v = b[r * w + i];
                b[r * w + i] = b[(h - 1 - r) * w + i];
                b[(h - 1 - r) * w + i] = v;
            }
    } else if (t == 6) {
        memcpy(b, p->lcd, (size_t)w * h);
    }
}

static void get_block(LavaVM* vm, int screen, int32_t xi, int32_t yi, int32_t wi, int32_t hi, uint32_t addr) {
    LavaPx* p = vm->px;
    int m = p->mode;
    uint32_t h = W16(hi), bw;
    if (m == 1) bw = W16(wi) >> 3;
    else if (m == 4) bw = (W16(wi) & 0xFFF8) >> 1;
    else bw = W16(wi);
    uint32_t y = W16(yi);
    uint32_t x = m == 8 ? W16(xi) : W16(xi) & 0xFFF8;
    if (!bw || !h) return;
    const uint8_t* pl = plane(p, screen);
    uint32_t sw = p->w, total = (uint32_t)p->w * p->h;
    uint32_t npx = bw * (m == 1 ? 8 : m == 4 ? 2 : 1);
    for (uint32_t r = 0; r < h; r++) {
        uint32_t o = (y + r) * sw + x;
        int in = (y + r) < p->h;
#define PX(i) (in && o + (i) < total ? pl[o + (i)] : 0)
        for (uint32_t i = 0; i < bw; i++) {
            uint8_t v;
            if (m == 1) {
                v = 0;
                for (int b = 0; b < 8; b++)
                    if (PX(i * 8 + b)) v |= 0x80 >> b;
            } else if (m == 4) {
                v = (uint8_t)((PX(i * 2) & 15) << 4 | (PX(i * 2 + 1) & 15));
            } else {
                v = PX(i);
            }
            vm->mem[(addr + i) & 0xFFFF] = v;
        }
#undef PX
        addr = (addr + bw) & 0xFFFF;
    }
    (void)npx;
}

// ---------------------------------------------------------------------------
// Text grid on the pixel screen (_trender_px)

void lavax_trender(LavaVM* vm, int which) {
    LavaPx* p = vm->px;
    int cols, rows, rh;
    lava_tdims(vm, &cols, &rows, &rh);
    uint32_t mask = rows <= 8 ? 0x80 : rows <= 16 ? 0x8000 : 0x800000;
    int x0 = 0, y0 = 0;
    if (!vm->tbig) {
        x0 = (p->w - cols * 6) / 2;
        y0 = (p->h - (rows * 13 - 1)) / 2;
        int down = p->h - (rows * 13 - 1) - y0;
        uint8_t c = p->mode == 1 ? 0 : p->bg;
        for (int yy = 0; yy < y0; yy++) memset(p->lcd + yy * p->w, c, p->w);
        for (int yy = p->h - down; yy < p->h; yy++) memset(p->lcd + yy * p->w, c, p->w);
        for (int r = 1; r < rows; r++) {
            int yy = r * 13 + y0 - 1;
            if (yy >= 0 && yy < p->h) memset(p->lcd + yy * p->w, c, p->w);
        }
        for (int i = 0; i < x0; i++) {
            vline(p, 1, i, 0, p->h - 1, 0);
            vline(p, 1, p->w - 1 - i, 0, p->h - 1, 0);
        }
    }
    for (int r = 0; r < rows; r++) {
        if ((uint32_t)which & mask) {
            mask >>= 1;
            continue;
        }
        mask >>= 1;
        const uint8_t* row = vm->mem + LAVA_TEXT + r * cols;
        int j = 0;
        while (j < cols) {
            int c = row[j], code, adv;
            if (c < 128) {
                code = c, adv = 1;
            } else {
                int c2 = j + 1 < cols ? row[j + 1] : vm->mem[LAVA_TEXT + r * cols + cols];
                code = c | c2 << 8, adv = 2;
            }
            int w, h, gn;
            const uint8_t* g;
            lavax_glyph(vm, code, vm->tbig, &w, &h, &g, &gn);
            blit_glyph(vm, 1, (uint32_t)(x0 + j * (vm->tbig ? 8 : 6)), (uint32_t)(y0 + r * rh), w, h, g, gn, 1, 0);
            j += adv;
        }
    }
}

// ---------------------------------------------------------------------------
// Drawing system calls on the pixel screen. Returns 1 if handled.

#define POPN(n)                            \
    int32_t* A = vm->stack + vm->sp - (n); \
    vm->sp -= (n)
#define RET(v)                     \
    do {                           \
        int32_t _v = (int32_t)(v); \
        vm->last = _v;             \
        vm->stack[vm->sp++] = _v;  \
    } while (0)

int lavax_sys_px(LavaVM* vm, int op) {
    LavaPx* p = vm->px;
    uint8_t* mem = vm->mem;
    switch (op) {
    case 0x8A: {
        POPN(4);
        uint32_t s = (uint32_t)A[2] & 0xFFFF;
        int32_t t = A[3];
        text(vm, (t & 0x40) != 0, A[0], A[1], mem + s, (int)lava_cstr_len(vm, s), (t & 0x80) != 0, t & 0xF,
             (t & 0x20) != 0);
        return 1;
    }
    case 0x88: {
        POPN(6);
        int32_t t = A[4];
        Src s = {mem, LAVA_MEM_LOGICAL, (uint32_t)A[5] & 0xFFFF, SRC_1BPP, 0, 0, 0};
        blit(p, (t & 0x40) != 0, A[0], A[1], A[2], A[3], &s, t & 0xF, (t & 0x20) != 0);
        return 1;
    }
    case 0x89: memcpy(p->lcd, p->buf, (size_t)p->w * p->h); return 1;
    case 0x8B:
    case 0x8C: {
        POPN(5);
        int32_t t = A[4];
        (op == 0x8B ? pblock : prect)(p, (t & 0x40) != 0, A[0], A[1], A[2], A[3], t & 3);
        return 1;
    }
    case 0x8E: memset(p->buf, p->mode == 1 ? 0 : p->bg, (size_t)p->w * p->h); return 1;
    case 0x94: {
        POPN(3);
        dot(p, !(A[2] & 0x40), A[0], A[1], A[2] & 3);
        return 1;
    }
    case 0x95: {
        POPN(2);
        uint32_t x = W16(A[0]), y = W16(A[1]);
        RET(x >= p->w || y >= p->h ? 0 : p->lcd[y * p->w + x]);
        return 1;
    }
    case 0x96: {
        POPN(5);
        pline(p, !(A[4] & 0x40), A[0], A[1], A[2], A[3], A[4] & 3);
        return 1;
    }
    case 0x97: {
        POPN(6);
        ((A[4] & 0xFF) ? pblock : prect)(p, 1, A[0], A[1], A[2], A[3], A[5] & 3);
        return 1;
    }
    case 0x98: {
        POPN(5);
        pellipse(p, !(A[4] & 0x40), A[0], A[1], A[2], A[2], (A[3] & 0xFFFF) != 0, A[4] & 3);
        return 1;
    }
    case 0x99: {
        POPN(6);
        pellipse(p, !(A[5] & 0x40), A[0], A[1], A[2], A[3], (A[4] & 0xFFFF) != 0, A[5] & 3);
        return 1;
    }
    case 0xC5: {
        POPN(1);
        xdraw(p, A[0]);
        return 1;
    }
    case 0xC7: {
        POPN(6);
        get_block(vm, (A[4] & 0x40) != 0, A[0], A[1], A[2], A[3], (uint32_t)A[5] & 0xFFFF);
        return 1;
    }
    case 0xCA: vm->sp -= 3; return 1;
    case 0x85: {    // SetScreen: the text grid only
        POPN(1);
        vm->tbig = (A[0] & 0xFF) == 0;
        vm->trow = vm->tcol = 0;
        int cols, rows, rh;
        lava_tdims(vm, &cols, &rows, &rh);
        memset(mem + LAVA_TEXT, 0, (size_t)cols * rows);
        return 1;
    }
    case 0x92: {
        POPN(2);
        int cols, rows, rh;
        lava_tdims(vm, &cols, &rows, &rh);
        int r = A[0] & 0xFF, c = A[1] & 0xFF;
        vm->trow = (uint8_t)(r < rows - 1 ? r : rows - 1);
        vm->tcol = (uint8_t)(c < cols - 1 ? c : cols - 1);
        return 1;
    }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Floats (stack slots as IEEE singles)

static inline float F(int32_t v) {
    float f;
    uint32_t u = (uint32_t)v;
    memcpy(&f, &u, 4);
    return f;
}

static inline int32_t FB(double x) {
    float f = (float)x;
    uint32_t u;
    memcpy(&u, &f, 4);
    return (int32_t)u;
}

static int32_t f2i(double x) {
    if (x != x || isinf(x) || !(x >= -2147483648.0 && x < 2147483648.0)) return INT32_MIN;
    return (int32_t)x;
}

static void vm_error(LavaVM* vm, const char* msg, uint32_t pc) {
    vm->error = vm->ended = 1;
    snprintf(vm->errmsg, sizeof vm->errmsg, "%s at %#x", msg, (unsigned)pc);
}

// Ops 0x52-0x74. Returns the new pc; *pushed says whether a value was pushed
// (and *last holds it).
uint32_t lavax_ext_op(LavaVM* vm, int op, uint32_t pc, uint32_t fb, int32_t* last) {
    int32_t* st = vm->stack;
    const uint8_t* code = vm->code;
    uint8_t* mem = vm->mem;
#define POP() (st[--vm->sp])
    int32_t v = 0;
    if (op == 0x52) {
        uint32_t a = (uint32_t)POP() & 0xFFFF;
        v = (int16_t)(mem[a] | mem[a + 1] << 8);
    } else if (op == 0x53) {
        uint32_t a = (uint32_t)POP() & 0xFFFF;
        v = (int32_t)((uint32_t)mem[a] | (uint32_t)mem[a + 1] << 8 | (uint32_t)mem[a + 2] << 16 |
                      (uint32_t)mem[a + 3] << 24);
    } else if (op == 0x54) {
        v = FB((double)POP());
    } else if (op == 0x55) {
        v = f2i((double)F(POP()));
    } else if (op >= 0x56 && op <= 0x61) {
        int32_t b = POP(), a = POP();
        int k = (op - 0x56) % 3, f = (op - 0x56) / 3;
        double x = k != 2 ? (double)F(a) : (double)a;
        double y = k != 1 ? (double)F(b) : (double)b;
        if (f == 3 && b == 0) {
            vm_error(vm, "float division by zero", pc - 1);
            return pc;
        }
        double r;
        if (f == 0) r = x + y;
        else if (f == 1) r = x - y;
        else if (f == 2) r = x * y;
        else if (y == 0.0) r = x != 0.0 ? copysign((double)INFINITY, x) : (double)NAN;   // Python's ZeroDivisionError path
        else r = x / y;
        v = FB(r);
    } else if (op == 0x62) {
        v = FB(-(double)F(POP()));
    } else if (op >= 0x63 && op <= 0x68) {
        double b = F(POP()), a = F(POP());
        int r = op == 0x63 ? a < b : op == 0x64 ? a > b : op == 0x65 ? a == b : op == 0x66 ? a != b
              : op == 0x67 ? a <= b : a >= b;
        v = r ? -1 : 0;
    } else if (op == 0x69) {
        v = POP() & 0x7FFFFFFF;
    } else if (op == 0x6A) {
        v = (POP() & 0xFFFF) | 0x20000;
    } else if (op == 0x6B) {
        v = (POP() & 0xFFFF) | 0x40000;
    } else if (op == 0x6C) {
        v = POP() & 0xFF;
    } else if (op == 0x6D) {
        v = (int16_t)(uint16_t)POP();
    } else if (op == 0x6E) {    // store with a size byte
        int t = code[pc++];
        v = POP();
        uint32_t a = (uint32_t)POP();
        if (t & 0x80) a += fb;
        int n = t & 0x7F;
        n = n == 1 || n == 2 ? n : 4;
        for (int i = 0; i < n; i++) mem[(a + i) & 0xFFFF] = (uint8_t)((uint32_t)v >> (8 * i));
    } else if (op == 0x6F) {
        v = (int32_t)((uint32_t)(code[pc] | code[pc + 1] << 8) + ((uint32_t)POP() & 0xFFFF));
        pc += 2;
    } else if (op == 0x70) {    // ++/-- with a size/mode byte
        int t = code[pc++];
        uint32_t a = (uint32_t)POP();
        if (t & 0x80) a += fb;
        a &= 0xFFFF;
        int n = t & 0x1F;
        int32_t old = n == 1 ? mem[a]
                    : n == 2 ? (int16_t)(mem[a] | mem[a + 1] << 8)
                             : (int32_t)((uint32_t)mem[a] | (uint32_t)mem[a + 1] << 8 | (uint32_t)mem[a + 2] << 16 |
                                         (uint32_t)mem[a + 3] << 24);
        int k = (t >> 5) & 3;
        int32_t nv = (int32_t)((uint32_t)old + (k == 0 || k == 2 ? 1u : 0xFFFFFFFFu));
        v = k < 2 ? nv : old;
        int w = n == 1 || n == 2 ? n : 4;
        for (int i = 0; i < w; i++) mem[(a + i) & 0xFFFF] = (uint8_t)((uint32_t)nv >> (8 * i));
    } else if (op == 0x71) {
        return pc + 1;
    } else if (op == 0x72) {
        return pc;
    } else if (op == 0x73) {
        vm->line = (int32_t)(code[pc] | code[pc + 1] << 8 | code[pc + 2] << 16);
        return pc + 3;
    } else {    // 0x74
        return pc + 3;
    }
#undef POP
    st[vm->sp++] = v;
    *last = v;
    return pc;
}

// ---------------------------------------------------------------------------
// System calls 0xCB-0xD6

// Names in a directory as FindFile numbers them: "..", sub-directories, files (sorted).
#define MAX_ENTS (LAVA_MAX_FILES + LAVA_MAX_DIRS + 1)
static int lower_eq_prefix(const char* s, const char* pre) {
    for (; *pre; s++, pre++) {
        unsigned char a = (unsigned char)*s, b = (unsigned char)*pre;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int cmp_str(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

static int vfs_entries(LavaVM* vm, const char* path, char ents[][LAVA_NAME_MAX]) {
    char d[LAVA_NAME_MAX + 2];
    int n = (int)strlen(path);
    while (n > 0 && path[n - 1] == '/') n--;
    memcpy(d, path, (size_t)n);
    d[n] = '/';
    d[n + 1] = 0;
    int dl = n + 1;
    static char subs[MAX_ENTS][LAVA_NAME_MAX], names[MAX_ENTS][LAVA_NAME_MAX];
    int ns = 0, nn = 0;
    for (int i = 0; i < vm->nfiles + vm->ndirs; i++) {
        const char* k = i < vm->nfiles ? vm->files[i].name : vm->dirs[i - vm->nfiles];
        if (!lower_eq_prefix(k, d) || (int)strlen(k) <= dl) continue;
        const char* rest = k + dl;
        const char* sl = strchr(rest, '/');
        char item[LAVA_NAME_MAX];
        int isdir = sl != NULL;
        size_t il = isdir ? (size_t)(sl - rest) : strlen(rest);
        memcpy(item, rest, il);
        item[il] = 0;
        char(*set)[LAVA_NAME_MAX] = isdir ? subs : names;
        int* cnt = isdir ? &ns : &nn;
        int dup = 0;
        for (int j = 0; j < *cnt; j++)
            if (!strcmp(set[j], item)) dup = 1;
        if (!dup && *cnt < MAX_ENTS) strcpy(set[(*cnt)++], item);
    }
    const char* ps[MAX_ENTS];
    int out = 0;
    strcpy(ents[out++], "..");
    for (int i = 0; i < ns; i++) ps[i] = subs[i];
    qsort(ps, (size_t)ns, sizeof ps[0], cmp_str);
    for (int i = 0; i < ns; i++) strcpy(ents[out++], ps[i]);
    int m = 0;
    for (int i = 0; i < nn; i++) {
        int isd = 0;
        for (int j = 0; j < ns; j++)
            if (!strcmp(subs[j], names[i])) isd = 1;
        if (!isd) ps[m++] = names[i];
    }
    qsort(ps, (size_t)m, sizeof ps[0], cmp_str);
    for (int i = 0; i < m && out < MAX_ENTS + 1; i++) strcpy(ents[out++], ps[i]);
    return out;
}

static void flm_decode(LavaVM* vm, uint32_t dst, uint32_t src) {
    uint8_t* mem = vm->mem;
    src &= 0xFFFF, dst &= 0xFFFF;
    uint32_t ln = mem[src] | mem[(src + 1) & 0xFFFF] << 8;
    int kind = (int)((ln >> 13) & 7);
    uint32_t end = src + (ln & 0x1FFF), s = src + 2, o = dst;
    if (kind == 0) {
        for (int32_t i = 0; i < (int32_t)(ln & 0x1FFF) - 2; i++) mem[(o + i) & 0xFFFF] = mem[(s + i) & 0xFFFF];
        return;
    }
    if (kind != 1 && kind != 2) return;
    while (s < end) {
        int c = mem[s & 0xFFFF];
        s++;
        int n = (c & 0x3F) ? (c & 0x3F) : 64;
        for (int i = 0; i < n; i++) {
            uint32_t a = o & 0xFFFF;
            uint8_t v;
            if (c < 64) v = kind == 1 ? 0 : mem[a];
            else if (c < 128) v = kind == 1 ? 0xFF : (uint8_t)(mem[a] + 0xFF);
            else if (c < 192) {
                v = kind == 1 ? mem[s & 0xFFFF] : (uint8_t)(mem[a] + mem[s & 0xFFFF]);
                s++;
            } else v = kind == 1 ? mem[s & 0xFFFF] : (uint8_t)(mem[s & 0xFFFF] + mem[a]);
            mem[a] = v;
            o++;
        }
        if (c >= 192) s++;
    }
}

static int32_t lsystem(LavaVM* vm, uint32_t n) {
    switch (n) {
    case 0: return 0x4C000001;
    case 1: vm->brightness = vm->stack[--vm->sp]; break;
    case 2: return 0;
    case 6:
    case 8: vm->sp -= 1; break;
    case 9:
    case 10:
    case 11: vm->sp -= 2; break;
    case 12: vm->sp -= 3; break;
    case 15: {
        int32_t s = vm->stack[--vm->sp], d = vm->stack[--vm->sp];
        flm_decode(vm, (uint32_t)d, (uint32_t)s);
        break;
    }
    case 20: vm->sp -= 3; break;
    case 29: vm->sp -= 5; break;
    case 30: vm->sp -= 2; return -1;
    case 31: return (int32_t)((vm->us * 256 / 1000000) & 0x7FFFFFFF);
    case 32:
    case 33: vm->sp -= 2; break;
    }
    return 0;
}

static int32_t lmath(LavaVM* vm, int32_t n) {
    if (n == 0) return 0x100;
    if (!((n >= 7 && n <= 15) || n == 19)) return 0;
    double x = F(vm->stack[--vm->sp]), r;
    switch (n) {
    case 7: r = isinf(x) ? (double)NAN : sin(x); break;
    case 8: r = isinf(x) ? (double)NAN : cos(x); break;
    case 9: r = isinf(x) ? (double)NAN : tan(x); break;
    case 10: r = x < -1 || x > 1 ? (double)NAN : asin(x); break;
    case 11: r = x < -1 || x > 1 ? (double)NAN : acos(x); break;
    case 12: r = atan(x); break;
    case 13: r = x < 0 ? (double)NAN : sqrt(x); break;
    case 14: r = exp(x); break;
    case 15: r = x <= 0 ? (double)NAN : log(x); break;
    default: r = fabs(x); break;
    }
    return FB(r);
}

// Switches a classic program to the pixel screen, keeping what's drawn.
static int to_px(LavaVM* vm) {
    LavaPx* p = lavax_px_new(LAVA_W, LAVA_H, 1);
    if (!p) return 0;
    for (int y = 0; y < LAVA_H; y++)
        for (int x = 0; x < LAVA_W; x++) {
            int bit = 0x80 >> (x & 7);
            p->lcd[y * LAVA_W + x] = (vm->mem[LAVA_GRAPH + y * LAVA_BPL + (x >> 3)] & bit) != 0;
            p->buf[y * LAVA_W + x] = (vm->mem[LAVA_GBUF + y * LAVA_BPL + (x >> 3)] & bit) != 0;
        }
    vm->px = p;
    return 1;
}

int lavax_sys(LavaVM* vm, int op) {
    uint8_t* mem = vm->mem;
    switch (op) {
    case 0xCB: {
        POPN(1);
        int m = A[0] & 0xFF;
        if (!vm->px) {
            if (m == 4 || m == 8) {
                if (!to_px(vm)) {
                    vm_error(vm, "out of memory for the pixel screen", vm->pc - 1);
                    return 0;
                }
                RET(lavax_set_mode(vm->px, m));
            } else {
                RET(m == 0 || m == 1 ? 1 : 0);
            }
        } else {
            RET(lavax_set_mode(vm->px, m));
        }
        break;
    }
    case 0xCC:
    case 0xCD: {
        POPN(1);
        if (vm->px) {
            uint8_t c = (uint8_t)(A[0] & (vm->px->mode == 8 ? 0xFF : 0xF));
            if (op == 0xCC) vm->px->bg = c;
            else vm->px->fg = c;
        }
        break;
    }
    case 0xCE: vm->sp -= 2; break;
    case 0xCF: {
        POPN(1);
        LavaPx* p = vm->px;
        if (p && p->mode != 1) {
            int fa = (A[0] & 0xF) ^ 0xF;
            size_t n = (size_t)p->w * p->h;
            if (fa)
                for (size_t i = 0; i < n; i++) p->lcd[i] = p->buf[i] > fa ? p->buf[i] : (uint8_t)fa;
            else memcpy(p->lcd, p->buf, n);
        }
        break;
    }
    case 0xD0: vm->sp -= 3; RET(-1); break;
    case 0xD1: {    // FindFile(from, num, buf)
        POPN(3);
        uint32_t frm = (uint32_t)A[0] & 0xFFFF, num = (uint32_t)A[1] & 0xFFFF, a = (uint32_t)A[2] & 0xFFFF;
        static char ents[MAX_ENTS + 1][LAVA_NAME_MAX];
        int ne = vfs_entries(vm, vm->cwd, ents);
        uint32_t i;
        for (i = 0; i < num; i++) {
            uint32_t k = frm + i;
            if ((int)k >= ne) {
                mem[(a + i * 16) & 0xFFFF] = 0;
                break;
            }
            size_t l = strlen(ents[k]);
            if (l > 15) l = 15;
            for (size_t j = 0; j < l; j++) mem[(a + i * 16 + j) & 0xFFFF] = (uint8_t)ents[k][j];
            mem[(a + i * 16 + l) & 0xFFFF] = 0;
        }
        RET(i);
        break;
    }
    case 0xD2: {    // GetFileNum(path)
        POPN(1);
        char p[LAVA_NAME_MAX], d[LAVA_NAME_MAX + 2];
        lava_path_of(vm, (uint32_t)A[0], p);
        int n = (int)strlen(p);
        while (n > 0 && p[n - 1] == '/') n--;
        memcpy(d, p, (size_t)n);
        d[n] = '/', d[n + 1] = 0;
        int ok = lava_has_dir(vm, d);
        for (int i = 0; i < vm->nfiles && !ok; i++)
            if (lower_eq_prefix(vm->files[i].name, d)) ok = 1;
        if (ok) {
            static char ents[MAX_ENTS + 1][LAVA_NAME_MAX];
            RET(vfs_entries(vm, d, ents) - 1);
        } else {
            RET(-1);
        }
        break;
    }
    case 0xD3: {    // System(..., n)
        POPN(1);
        int32_t r = lsystem(vm, (uint32_t)A[0]);
        RET(r);
        if (vm->sp > 256) {     // unpopped arguments: keep the last 64
            memmove(vm->stack, vm->stack + vm->sp - 64, 64 * sizeof(int32_t));
            vm->sp = 64;
        }
        break;
    }
    case 0xD4: {
        POPN(1);
        RET(lmath(vm, A[0]));
        break;
    }
    case 0xD5: {    // SetPalette(start, num, pal)
        POPN(3);
        int start = A[0] & 0xFF, num = A[1] & 0x7FFF;
        if (num > 256 - start) num = 256 - start;
        if (vm->px) {
            LavaPx* p = vm->px;
            if (!p->has_pal) lavax_default_palette(p->pal), p->has_pal = 1;
            for (int i = 0; i < num; i++) {
                uint32_t o = ((uint32_t)A[2] + i * 4) & 0xFFFF;
                p->pal[start + i][0] = mem[o], p->pal[start + i][1] = mem[(o + 1) & 0xFFFF],
                p->pal[start + i][2] = mem[(o + 2) & 0xFFFF];
            }
        }
        RET(num);
        break;
    }
    case 0xD6: {
        POPN(1);
        mem[(uint32_t)A[0] & 0xFFFF] = 0;
        break;
    }
    default: return 0;
    }
    return 1;
}

void lavax_default_palette(uint8_t pal[256][3]) {
    static const uint8_t lv9[9] = {0, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0, 0xFF};
    static const uint8_t lv5[5] = {0, 0x40, 0x80, 0xC0, 0xFF};
    memset(pal, 0, 256 * 3);
    for (int i = 0; i < 225; i++) {
        pal[16 + i][0] = lv5[i % 5];
        pal[16 + i][1] = lv9[i / 25];
        pal[16 + i][2] = lv5[(i / 5) % 5];
    }
    pal[255][0] = pal[255][1] = pal[255][2] = 255;
}
