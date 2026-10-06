// LAVA (GVmaker 1.0) VM. See lava.h. Every behaviour here mirrors
// wqx_tl/lavaemu (vm.py, screen.py); comments point out the places where
// that matters (masked vs unmasked addresses, Python integer semantics).
#include "lava_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* default_realloc(void* p, size_t n);
void* (*lava_realloc)(void* p, size_t n) = default_realloc;

static void* default_realloc(void* p, size_t n) {
    if (n == 0) {
        free(p);
        return NULL;
    }
    return realloc(p, n);
}

#define MEM_END LAVA_MEM_LOGICAL
#define W LAVA_W
#define H LAVA_H
#define BPL LAVA_BPL

// ---------------------------------------------------------------------------
// Blobs

static LavaBlob* blob_new(const uint8_t* data, uint32_t len) {
    LavaBlob* b = lava_realloc(NULL, sizeof *b);
    if (!b) return NULL;
    b->cap = len ? len : 16;
    b->data = lava_realloc(NULL, b->cap);
    if (!b->data) {
        lava_realloc(b, 0);
        return NULL;
    }
    if (len && data) memcpy(b->data, data, len);
    b->len = len;
    b->refs = 1;
    return b;
}

static void blob_unref(LavaBlob* b) {
    if (!b) return;
    if (--b->refs <= 0) {
        lava_realloc(b->data, 0);
        lava_realloc(b, 0);
    }
}

static int blob_reserve(LavaBlob* b, uint32_t n) {
    if (n <= b->cap) return 1;
    uint32_t c = b->cap * 2;
    if (c < n) c = n;
    uint8_t* d = lava_realloc(b->data, c);
    if (!d) return 0;
    b->data = d;
    b->cap = c;
    return 1;
}

// A handle's blob, made private before a write.
static LavaBlob* blob_own(LavaBlob** pb) {
    LavaBlob* b = *pb;
    if (b->refs > 1) {
        LavaBlob* c = blob_new(b->data, b->len);
        if (!c) return NULL;
        b->refs--;
        *pb = c;
        return c;
    }
    return b;
}

// ---------------------------------------------------------------------------
// Init

static void init_rev8(void);
static uint8_t rev8[256];

static const char* const default_dirs[] = {"/", "/LavaData/", "/GVMData/"};

int lava_init(LavaVM* vm, const uint8_t* code, uint32_t len, const LavaFonts* fonts) {
    memset(vm, 0, sizeof *vm);
    if (len <= 16 || code[0] != 'L' || code[1] != 'A' || code[2] != 'V' || code[3] != 0x12) return 0;
    if (!rev8[1]) init_rev8();
    uint8_t err = 0;
    vm->code = code;
    vm->code_len = len;
    if (fonts) vm->fonts = *fonts;
    vm->us_per_op = LAVA_US_PER_OP;
    // LavaX header: byte 8 flags (bits 6-5 graphics mode, bit 7/4 24/32-bit
    // addresses), bytes 9-10 the screen size / 16
    uint8_t f = code[8];
    if (f & 0x90) {
        snprintf(vm->errmsg, sizeof vm->errmsg, "%d-bit LavaX programs are not supported", f & 0x10 ? 32 : 24);
        vm->error = vm->ended = 1;
    }
    int w = code[9] << 4, h = code[10] << 4;
    vm->hdr_w = (uint16_t)(w < 160 ? 160 : w > 320 ? 320 : w);
    vm->hdr_h = (uint16_t)(h < 80 ? 80 : h > 240 ? 240 : h);
    vm->hdr_flags = f;
    vm->hdr_mode = (f & 0x60) == 0x40 ? 4 : (f & 0x60) == 0x60 ? 8 : 1;
    vm->lavax = f || vm->hdr_w != 160 || vm->hdr_h != 80;
    for (int i = 0; i < 3; i++) strcpy(vm->dirs[i], default_dirs[i]);
    vm->ndirs = 3;
    err = vm->error;
    lava_reset(vm);
    if (err) {
        vm->error = vm->ended = 1;
        return 0;
    }
    return vm->px || !vm->lavax;
}

void lava_set_pace(LavaVM* vm, uint32_t us_per_op) { vm->us_per_op = us_per_op ? us_per_op : LAVA_US_PER_OP; }

static void close_handles(LavaVM* vm) {
    for (int i = 0; i < 3; i++) {
        if (vm->fp[i].used) blob_unref(vm->fp[i].blob);
        memset(&vm->fp[i], 0, sizeof vm->fp[i]);
    }
}

void lava_reset(LavaVM* vm) {
    vm->pc = 16;
    vm->sp = 0;
    vm->last = 0;
    vm->fb = vm->fe = 0;
    vm->strp = LAVA_STRSTACK;
    vm->xorkey = 0;
    vm->seed = 0;
    vm->ended = vm->waiting = vm->error = 0;
    vm->us = 0;
    vm->frame = 0;
    vm->ops = 0;
    vm->fe_max = 0;
    memset(vm->mem, 0, sizeof vm->mem);
    memset(vm->held, 0, sizeof vm->held);
    vm->latched = vm->autorelease = 0;
    vm->tbig = 1;
    vm->trow = vm->tcol = 0;
    close_handles(vm);
    strcpy(vm->cwd, "/");
    vm->last_key = 0;
    vm->brightness = 0;
    vm->line = -1;
    if (vm->px) lavax_px_free(vm->px);
    vm->px = vm->lavax ? lavax_px_new(vm->hdr_w, vm->hdr_h, vm->hdr_mode) : NULL;
}

void lava_free(LavaVM* vm) {
    close_handles(vm);
    if (vm->px) lavax_px_free(vm->px);
    vm->px = NULL;
    for (int i = 0; i < vm->nfiles; i++) blob_unref(vm->files[i].blob);
    vm->nfiles = 0;
}

static int ieq(const char* a, const char* b) {
    for (;; a++, b++) {
        unsigned char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int iprefix(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++) {
        unsigned char x = *s, y = *prefix;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

// lavaemu's _find: exact name first, then the first case-insensitive match.
static int find_index(LavaVM* vm, const char* name) {
    for (int i = 0; i < vm->nfiles; i++)
        if (!strcmp(vm->files[i].name, name)) return i;
    for (int i = 0; i < vm->nfiles; i++)
        if (ieq(vm->files[i].name, name)) return i;
    return -1;
}

LavaFile* lava_find_file(LavaVM* vm, const char* name) {
    int i = find_index(vm, name);
    return i < 0 ? NULL : &vm->files[i];
}

// files[name] = blob (takes the reference). Exact-name key, as a Python dict.
static int set_file(LavaVM* vm, const char* name, LavaBlob* blob, int dirty) {
    vm->files_gen++;
    for (int i = 0; i < vm->nfiles; i++) {
        if (!strcmp(vm->files[i].name, name)) {
            blob_unref(vm->files[i].blob);
            vm->files[i].blob = blob;
            vm->files[i].dirty |= dirty;
            return 1;
        }
    }
    if (vm->nfiles >= LAVA_MAX_FILES) {
        blob_unref(blob);
        return 0;
    }
    LavaFile* f = &vm->files[vm->nfiles++];
    snprintf(f->name, sizeof f->name, "%s", name);
    f->blob = blob;
    f->dirty = (uint8_t)dirty;
    return 1;
}

int lava_add_file(LavaVM* vm, const char* name, const uint8_t* data, uint32_t len, int dirty) {
    LavaBlob* b = blob_new(data, len);
    if (!b) return 0;
    return set_file(vm, name, b, dirty);
}

static void delete_file(LavaVM* vm, int i) {
    vm->files_gen++;
    blob_unref(vm->files[i].blob);
    memmove(&vm->files[i], &vm->files[i + 1], (vm->nfiles - i - 1) * sizeof vm->files[0]);
    vm->nfiles--;
}

// ---------------------------------------------------------------------------
// Input

void lava_key_down(LavaVM* vm, int c) {
    c &= 0x7F;
    vm->held[c] = 1;
    vm->latched = (uint8_t)c;
}

void lava_key_up(LavaVM* vm, int c, int keep_latch) {
    c &= 0x7F;
    vm->held[c] = 0;
    if (!keep_latch && vm->latched == c) vm->latched = 0;
}

void lava_release_all(LavaVM* vm) {
    memset(vm->held, 0, sizeof vm->held);
    vm->latched = vm->autorelease = 0;
}

static int min_held(LavaVM* vm) {
    for (int i = 0; i < 128; i++)
        if (vm->held[i]) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Screen (screen.py). Rows are 20 bytes, MSB = leftmost pixel.

void lava_glyph(LavaVM* vm, int code, int big, int* w, const uint8_t** data, int* n) {
    static const uint8_t zeros[32];
    const uint8_t* buf;
    uint32_t len;
    long off;
    if (code <= 0xFF) {
        *n = big ? 16 : 12;
        *w = big ? 8 : 6;
        buf = big ? vm->fonts.asc16 : vm->fonts.asc12;
        len = big ? vm->fonts.asc16_len : vm->fonts.asc12_len;
        off = (long)code * *n;
    } else {
        *n = big ? 32 : 24;
        *w = big ? 16 : 12;
        buf = big ? vm->fonts.gb16 : vm->fonts.gb12;
        len = big ? vm->fonts.gb16_len : vm->fonts.gb12_len;
        int hi = (code & 0xFF) - 0xA1;
        if (hi > 8) hi -= 6;
        off = ((long)hi * 94 + (code >> 8) - 0xA1) * *n;
    }
    if (!buf || off < 0 || off + *n > (long)len) *data = zeros;
    else *data = buf + off;
}

// Source bit k of a row (0 = leftmost); bytes past the logical end read as 0.
static inline int src_bit(const uint8_t* src, uint32_t src_len, uint32_t a, int k) {
    uint32_t i = a + (k >> 3);
    if (i >= src_len) return 0;
    return (src[i] >> (7 - (k & 7))) & 1;
}

// WriteBlock / glyph blit (screen.py draw_data). Each destination byte takes
// 8 source bits through a funnel shift; the destination mask (clipped to the
// screen and to the block) keeps only the block's own pixels, so bits from the
// row padding or the next row never land. Mirrored blocks go pixel by pixel.
static void draw_data(LavaVM* vm, uint32_t base, int x, int y, int w, int h, const uint8_t* src, uint32_t src_len,
                      uint32_t addr, int mode, int mirror, int inverse) {
    if (w <= 0 || h <= 0 || x >= W || y >= H || x + w < 0 || y + h < 0) return;
    int bpl = (w + 7) >> 3;
    int m = mode & 7;
    if (inverse && m == 2) m = 1;
    int x0 = x > 0 ? x : 0;
    int x1 = x + w < W ? x + w : W;
    if (x1 <= x0) return;
    uint8_t* mem = vm->mem;
    int bx0 = x0 >> 3, bx1 = (x1 - 1) >> 3, nb = bx1 - bx0 + 1;
    uint8_t mfirst = 0xFF >> (x0 - bx0 * 8);
    uint8_t mlast = (uint8_t)(0xFF << (bx1 * 8 + 8 - x1));
    if (nb == 1) mfirst &= mlast, mlast = mfirst;
    int sh = (8 - (x & 7)) & 7;                 // source bit offset of a byte's first pixel, mod 8
    int i0 = (8 * bx0 - x + 8 * 1024) / 8 - 1024;  // floor((8*bx0 - x) / 8): may be -1
    uint8_t inv = inverse ? 0xFF : 0;
    int ry0 = y < 0 ? -y : 0, ry1 = y + h > H ? H - y : h;
    for (int r = ry0; r < ry1; r++) {
        uint32_t a = addr + (uint32_t)r * bpl;
        uint8_t* d = mem + base + (y + r) * BPL + bx0;
        uint8_t sb[BPL];
        if (!mirror && (m == 0 || m == 1 || m == 6 || m == 7)) {
            // plain copy, the common case (full-screen pictures): fused, edges masked
            int64_t first = (int64_t)a + i0;
            if (first >= 0 && first + nb + 1 <= (int64_t)src_len) {
                const uint8_t* q = src + first;
                if (sh) {
                    d[0] = (uint8_t)((d[0] & ~mfirst) | (((q[0] << sh | q[1] >> (8 - sh)) ^ inv) & mfirst));
                    for (int j = 1; j < nb - 1; j++) d[j] = (uint8_t)((q[j] << sh | q[j + 1] >> (8 - sh)) ^ inv);
                } else {
                    d[0] = (uint8_t)((d[0] & ~mfirst) | ((q[0] ^ inv) & mfirst));
                    if (inv) for (int j = 1; j < nb - 1; j++) d[j] = (uint8_t)~q[j];
                    else if (nb > 2) memcpy(d + 1, q + 1, nb - 2);
                }
                if (nb > 1) {
                    int j = nb - 1;
                    uint8_t v = sh ? (uint8_t)(q[j] << sh | q[j + 1] >> (8 - sh)) : q[j];
                    d[j] = (uint8_t)((d[j] & ~mlast) | ((v ^ inv) & mlast));
                }
                continue;
            }
        }
        if (!mirror) {
            int64_t first = (int64_t)a + i0;
            if (first >= 0 && first + nb + 1 <= (int64_t)src_len) {
                const uint8_t* q = src + first;
                if (sh) for (int j = 0; j < nb; j++) sb[j] = (uint8_t)(q[j] << sh | q[j + 1] >> (8 - sh));
                else memcpy(sb, q, nb);
            } else {
                for (int j = 0; j < nb; j++) {
                    int64_t i = first + j;
                    uint8_t hi = i >= 0 && i < (int64_t)src_len ? src[i] : 0;
                    uint8_t lo = i + 1 >= 0 && i + 1 < (int64_t)src_len ? src[i + 1] : 0;
                    sb[j] = sh ? (uint8_t)(hi << sh | lo >> (8 - sh)) : hi;
                }
            }
        } else {
            for (int j = 0; j < nb; j++) {
                uint8_t v = 0;
                for (int b = 0; b < 8; b++) {
                    int px = (bx0 + j) * 8 + b - x;
                    if (px < 0 || px >= w) continue;
                    if (src_bit(src, src_len, a, w - 1 - px)) v |= 0x80 >> b;
                }
                sb[j] = v;
            }
        }
        // apply: masks only differ from 0xFF at the two ends
        for (int j = 0; j < nb; j++) {
            uint8_t mask = j == 0 ? mfirst : j == nb - 1 ? mlast : 0xFF;
            uint8_t sv = (uint8_t)((sb[j] ^ inv) & mask), dv = d[j];
            switch (m) {
            case 2: d[j] = (uint8_t)((dv & ~mask) | (~sv & mask)); break;
            case 3: d[j] = dv | sv; break;
            case 4: d[j] = (uint8_t)((dv & ~mask) | (dv & sv)); break;
            case 5: d[j] = dv ^ sv; break;
            default: d[j] = (uint8_t)((dv & ~mask) | sv); break;
            }
        }
    }
}

static void get_block(LavaVM* vm, uint32_t base, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t addr) {
    int bw = (int)(w >> 3), bx = (int)(x >> 3);
    if (bw <= 0 || h == 0) return;
    for (uint32_t r = 0; r < h; r++) {
        uint32_t yy = y + r;
        for (int i = 0; i < bw; i++) {
            uint32_t dst = (addr + r * bw + i) & 0xFFFF;
            if (yy < H && bx + i < BPL) vm->mem[dst] = vm->mem[base + yy * BPL + bx + i];
            else vm->mem[dst] = 0;
        }
    }
}

static void draw_text(LavaVM* vm, uint32_t base, int x, int y, const uint8_t* s, int n, int big, int mode, int mirror,
                      int inverse) {
    int hgt = big ? 16 : 12;
    int i = 0;
    while (i < n) {
        int c = s[i++];
        if (c >= 0x80 && i < n) c |= s[i++] << 8;
        int w, len;
        const uint8_t* g;
        lava_glyph(vm, c, big, &w, &g, &len);
        draw_data(vm, base, x, y, w, hgt, g, (uint32_t)len, 0, mode, mirror, inverse);
        x += w;
    }
}

static void hline(LavaVM* vm, uint32_t base, int x0, int x1, int y, int mode) {
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y < 0 || y >= H || x1 < 0 || x0 >= W) return;
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    int m = mode & 3;
    if (m == 3) return;
    uint8_t* row = vm->mem + base + y * BPL;
    for (int bx = x0 >> 3; bx <= x1 >> 3; bx++) {
        uint8_t mask = 0xFF;
        if (bx * 8 < x0) mask &= 0xFF >> (x0 - bx * 8);
        if (bx * 8 + 7 > x1) mask &= (uint8_t)(0xFF << (bx * 8 + 7 - x1));
        if (m == 0) row[bx] &= ~mask;
        else if (m == 1) row[bx] |= mask;
        else row[bx] ^= mask;
    }
}

static inline void point(LavaVM* vm, uint32_t base, int x, int y, int mode) {
    if (x >= 0 && x < W && y >= 0 && y < H) {
        uint8_t* p = vm->mem + base + y * BPL + (x >> 3);
        uint8_t b = 0x80 >> (x & 7);
        int m = mode & 3;
        if (m == 0) *p &= ~b;
        else if (m == 1) *p |= b;
        else if (m == 2) *p ^= b;
    }
}

static void rect(LavaVM* vm, uint32_t base, int x0, int y0, int x1, int y1, int fill, int mode) {
    int t;
    if (x0 > x1) t = x0, x0 = x1, x1 = t;
    if (y0 > y1) t = y0, y0 = y1, y1 = t;
    if (x0 >= W || x1 < 0 || y0 >= H || y1 < 0) return;
    if (fill) {
        int ya = y0 > 0 ? y0 : 0, yb = y1 < H - 1 ? y1 : H - 1;
        for (int yy = ya; yy <= yb; yy++) hline(vm, base, x0, x1, yy, mode);
    } else {
        hline(vm, base, x0, x1, y0, mode);
        if (y1 > y0) hline(vm, base, x0, x1, y1, mode);
        for (int yy = y0 + 1; yy < y1; yy++) {
            point(vm, base, x0, yy, mode);
            if (x1 > x0) point(vm, base, x1, yy, mode);
        }
    }
}

static void line(LavaVM* vm, uint32_t base, int x0, int y0, int x1, int y1, int mode) {
    int t;
    if (abs(y1 - y0) <= abs(x1 - x0)) {
        if (x0 > x1) t = x0, x0 = x1, x1 = t, t = y0, y0 = y1, y1 = t;
        int dx = x1 - x0, dy = y1 - y0, inc = 1;
        if (dy < 0) dy = -dy, inc = -1;
        int d = 2 * dy - dx, yy = y0;
        for (int xx = x0; xx <= x1; xx++) {
            point(vm, base, xx, yy, mode);
            if (d < 0) d += 2 * dy;
            else yy += inc, d += 2 * (dy - dx);
        }
    } else {
        if (y0 > y1) t = x0, x0 = x1, x1 = t, t = y0, y0 = y1, y1 = t;
        int dy = y1 - y0, dx = x1 - x0, inc = 1;
        if (dx < 0) dx = -dx, inc = -1;
        int d = 2 * dx - dy, xx = x0;
        for (int yy = y0; yy <= y1; yy++) {
            point(vm, base, xx, yy, mode);
            if (d < 0) d += 2 * dx;
            else xx += inc, d += 2 * (dx - dy);
        }
    }
}

static void oval(LavaVM* vm, uint32_t base, int cx, int cy, int a, int b, int fill, int mode) {
    if (cx - a >= W || cx + a < 0 || cy - b >= H || cy + b < 0) return;
#define PT(X, Y) point(vm, base, (X), (Y), mode)
#define HL(X0, X1, Y) hline(vm, base, (X0), (X1), (Y), mode)
    int64_t a2 = (int64_t)a * a, b2 = (int64_t)b * b;
    int x = 0, y = b;
    int64_t px = 0, py = 2 * a2 * y;
    // Python: p = b2 - a2*b + ((a2 + 2) >> 2); >> on Python ints floors
    int64_t p = b2 - a2 * b + ((a2 + 2) >> 2);
    while (px < py) {
        x++;
        px += 2 * b2;
        if (p < 0) {
            p += b2 + px;
        } else {
            if (fill) {
                HL(cx - x + 1, cx + x - 1, cy + y);
                HL(cx - x + 1, cx + x - 1, cy - y);
            }
            y--;
            py -= 2 * a2;
            p += b2 + px - py;
        }
        if (!fill) {
            PT(cx - x, cy - y);
            PT(cx - x, cy + y);
            PT(cx + x, cy - y);
            PT(cx + x, cy + y);
        }
    }
    if (fill) {
        HL(cx - x, cx + x, cy + y);
        HL(cx - x, cx + x, cy - y);
    }
    p = b2 * x * x + b2 * x + a2 * (int64_t)(y - 1) * (y - 1) - a2 * b2 + ((b2 + 2) >> 2);
    y--;
    while (y > 0) {
        py -= 2 * a2;
        if (p > 0) {
            p += a2 - py;
        } else {
            x++;
            px += 2 * b2;
            p += a2 - py + px;
        }
        if (fill) {
            HL(cx - x, cx + x, cy + y);
            HL(cx - x, cx + x, cy - y);
        } else {
            PT(cx - x, cy - y);
            PT(cx - x, cy + y);
            PT(cx + x, cy - y);
            PT(cx + x, cy + y);
        }
        y--;
    }
    if (fill) {
        HL(cx - a, cx + a, cy);
    } else {
        PT(cx, cy + b);
        PT(cx, cy - b);
        PT(cx + a, cy);
        PT(cx - a, cy);
    }
#undef PT
#undef HL
}

static void init_rev8(void) {
    for (int i = 0; i < 256; i++) {
        int r = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b)) r |= 0x80 >> b;
        rev8[i] = (uint8_t)r;
    }
}

static void xdraw(LavaVM* vm, int mode) {
    uint8_t* buf = vm->mem + LAVA_GBUF;
    int m = mode & 7;
    if (m == 0) {
        for (int y = 0; y < H; y++) {
            uint8_t* r = buf + y * BPL;
            for (int i = 0; i < BPL; i++) r[i] = (uint8_t)(r[i] << 1 | (i + 1 < BPL ? r[i + 1] >> 7 : 0));
        }
    } else if (m == 1) {
        for (int y = 0; y < H; y++) {
            uint8_t* r = buf + y * BPL;
            for (int i = BPL - 1; i >= 0; i--) r[i] = (uint8_t)(r[i] >> 1 | (i > 0 ? r[i - 1] << 7 : 0));
        }
    } else if (m == 4) {
        for (int y = 0; y < H; y++) {
            uint8_t* r = buf + y * BPL;
            for (int i = 0; i < BPL / 2; i++) {
                uint8_t t = rev8[r[i]];
                r[i] = rev8[r[BPL - 1 - i]];
                r[BPL - 1 - i] = t;
            }
        }
    } else if (m == 5) {
        uint8_t tmp[BPL];
        for (int y = 0; y < H / 2; y++) {
            memcpy(tmp, buf + y * BPL, BPL);
            memcpy(buf + y * BPL, buf + (H - 1 - y) * BPL, BPL);
            memcpy(buf + (H - 1 - y) * BPL, tmp, BPL);
        }
    }
}

// ---------------------------------------------------------------------------
// Text grid (printf / putchar)

void lava_tdims(LavaVM* vm, int* cols, int* rows, int* rh) {
    LavaPx* p = vm->px;
    if (p && (p->w != W || p->h != H)) {
        if (vm->tbig) *cols = p->w / 8, *rows = p->h / 16, *rh = 16;
        else *cols = ((p->w - 2) / 6) & ~1, *rows = (p->h - 1) / 13, *rh = 13;
        return;
    }
    if (vm->tbig) *cols = 20, *rows = 5, *rh = 16;
    else *cols = 26, *rows = 6, *rh = 13;
}
#define tdims lava_tdims

static void tscroll(LavaVM* vm) {
    int cols, rows, rh;
    tdims(vm, &cols, &rows, &rh);
    memmove(vm->mem + LAVA_TEXT, vm->mem + LAVA_TEXT + cols, (rows - 1) * cols);
    memset(vm->mem + LAVA_TEXT + (rows - 1) * cols, ' ', cols);
    vm->trow = (uint8_t)(rows - 1);
    vm->tcol = 0;
}

static void tadd(LavaVM* vm, int b) {
    int cols, rows, rh;
    tdims(vm, &cols, &rows, &rh);
    if (vm->trow >= rows) tscroll(vm);
    if (b == 0x0D) return;
    if (b == 0x0A) {
        vm->tcol = 0;
        vm->trow++;
        if (vm->trow >= rows) tscroll(vm);
        return;
    }
    vm->mem[LAVA_TEXT + vm->trow * cols + vm->tcol] = (uint8_t)(b == 9 ? 0x20 : b);
    vm->tcol++;
    if (vm->tcol >= cols) {
        vm->tcol = 0;
        vm->trow++;
    }
}

static void tadds(LavaVM* vm, const uint8_t* d, int n) {
    int cols, rows, rh;
    tdims(vm, &cols, &rows, &rh);
    int i = 0;
    while (i < n) {
        int b = d[i++];
        if (b < 0x80) {
            tadd(vm, b);
        } else {
            if (vm->tcol == cols - 1) tadd(vm, 0x20);
            tadd(vm, b);
            if (i < n) tadd(vm, d[i++]);
        }
    }
}

static void trender(LavaVM* vm, int which) {
    if (vm->px) {
        lavax_trender(vm, which);
        return;
    }
    int cols, rows, rh;
    tdims(vm, &cols, &rows, &rh);
    if ((which & 0xFF) == 0xFF) return;
    uint32_t g = LAVA_GRAPH;
    if (!vm->tbig) {
        hline(vm, g, 0, W - 1, 0, 0);
        hline(vm, g, 0, W - 1, H - 1, 0);
        rect(vm, g, 0, 0, 1, H - 1, 1, 0);
        rect(vm, g, W - 2, 0, W - 1, H - 1, 1, 0);
        for (int y = 13; y < H; y += 13) hline(vm, g, 0, W - 1, y, 0);
    }
    int x0 = vm->tbig ? 0 : 2, y0 = vm->tbig ? 0 : 1;
    int mask = 0x100;
    uint8_t row[32];
    for (int r = 0; r < rows; r++) {
        mask >>= 1;
        if (which & mask) continue;
        for (int c = 0; c < cols; c++) {
            uint8_t b = vm->mem[LAVA_TEXT + r * cols + c];
            row[c] = b ? b : ' ';
        }
        rect(vm, g, x0, y0 + r * rh, x0 + cols * (vm->tbig ? 8 : 6) - 1, y0 + r * rh + (vm->tbig ? 16 : 12) - 1, 1, 0);
        draw_text(vm, g, x0, y0 + r * rh, row, cols, vm->tbig, 1, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Helpers with Python semantics

static inline int32_t s16(int32_t v) { return (int16_t)(uint16_t)v; }

static inline int32_t pydiv(int32_t a, int32_t b) {
    int64_t q = (int64_t)a / b;   // C truncates, as lavaemu's _div
    return (int32_t)(uint32_t)q;
}

static inline int32_t pymod(int32_t a, int32_t b) {
    if (b == -1) return 0;
    return a % b;
}

uint32_t lava_cstr_len(LavaVM* vm, uint32_t a);
#define cstr_len lava_cstr_len
uint32_t lava_cstr_len(LavaVM* vm, uint32_t a) {
    uint32_t e = a;
    while (e < MEM_END && vm->mem[e]) e++;
    return e - a;
}

// printf/sprintf formatting (lavaemu _format). args[0] = format address.
static int format(LavaVM* vm, const int32_t* args, int nargs, uint8_t* out, int cap) {
    uint32_t fa = (uint32_t)args[0] & 0xFFFF;
    int ai = 1, n = 0;
    uint8_t* m = vm->mem;
    for (;;) {
        if (fa >= MEM_END) break;
        int c = m[fa];
        if (c == 0) break;
        if (c == 0x25) {
            int width = 0;
            char flag = 0;
            if (vm->px) {   // LavaX: %[-|0]<width>d, %f
                fa++;
                while (m[fa] == 0x30 || m[fa] == 0x2D) {
                    if (!flag) flag = m[fa] == 0x30 ? '0' : '-';
                    if (m[fa] == 0x2D) flag = '-';
                    fa++;
                }
                while (m[fa] >= 0x30 && m[fa] <= 0x39) width = width * 10 + m[fa++] - 0x30;
                fa--;
            }
            int t = m[fa + 1];
            fa += 2;
            if (t == 0) break;
            if (t == 0x64) {
                char num[16];
                int32_t v = ai < nargs ? args[ai] : 0;
                ai++;
                int k = snprintf(num, sizeof num, "%ld", (long)v);
                int pad = width > k ? width - k : 0;
                if (flag != '-')
                    for (int i = 0; i < pad && n < cap; i++) out[n++] = flag == '0' ? '0' : ' ';
                for (int i = 0; i < k && n < cap; i++) out[n++] = (uint8_t)num[i];
                if (flag == '-')
                    for (int i = 0; i < pad && n < cap; i++) out[n++] = ' ';
            } else if (t == 0x66 && vm->px) {
                int32_t v = ai < nargs ? args[ai] : 0;
                ai++;
                char num[48];
                if ((((uint32_t)v >> 23) & 0xFF) == 0xFF) {
                    strcpy(num, "error");
                } else {
                    float f;
                    memcpy(&f, &v, 4);
                    char raw[48];
                    snprintf(raw, sizeof raw, "%g", (double)f);
                    // Python's %g has a 2-digit exponent at least, as C; then "e+0" -> "e+"
                    int j = 0;
                    for (int i = 0; raw[i] && j < 47; i++) {
                        num[j++] = raw[i];
                        if ((raw[i] == '+' || raw[i] == '-') && i > 0 && raw[i - 1] == 'e' && raw[i + 1] == '0') i++;
                    }
                    num[j] = 0;
                }
                for (int i = 0; num[i] && n < cap; i++) out[n++] = (uint8_t)num[i];
            } else if (t == 0x63) {
                if (n < cap) out[n++] = (uint8_t)(ai < nargs ? args[ai] & 0xFF : 0);
                ai++;
            } else if (t == 0x73) {
                uint32_t a = ai < nargs ? (uint32_t)args[ai] & 0xFFFF : 0;
                ai++;
                while (m[a] && n < cap) {
                    out[n++] = m[a];
                    a = (a + 1) & 0xFFFF;
                }
            } else {
                if (n < cap) out[n++] = (uint8_t)t;
            }
        } else {
            if (n < cap) out[n++] = (uint8_t)c;
            fa++;
        }
    }
    return n;
}

#define path_of lava_path_of
void lava_path_of(LavaVM* vm, uint32_t a, char* out) {
    a &= 0xFFFF;
    uint32_t n = cstr_len(vm, a);
    int k = 0;
    if (n == 0 || vm->mem[a] != '/') {
        k = snprintf(out, LAVA_NAME_MAX, "%s", vm->cwd);
    }
    for (uint32_t i = 0; i < n && k < LAVA_NAME_MAX - 1; i++) out[k++] = (char)vm->mem[a + i];
    out[k] = 0;
}

#define has_dir lava_has_dir
int lava_has_dir(LavaVM* vm, const char* d) {
    for (int i = 0; i < vm->ndirs; i++)
        if (!strcmp(vm->dirs[i], d)) return 1;
    return 0;
}

static void add_dir(LavaVM* vm, const char* d) {
    if (has_dir(vm, d) || vm->ndirs >= LAVA_MAX_DIRS) return;
    snprintf(vm->dirs[vm->ndirs++], LAVA_NAME_MAX, "%s", d);
}

// p.rstrip("/") + "/"
static void dir_form(const char* p, char* out) {
    int n = (int)strlen(p);
    while (n > 0 && p[n - 1] == '/') n--;
    if (n > LAVA_NAME_MAX - 2) n = LAVA_NAME_MAX - 2;
    memcpy(out, p, n);
    out[n] = '/';
    out[n + 1] = 0;
}

static int fopen_(LavaVM* vm, const char* name, const char* mode_in) {
    int slot = -1;
    for (int i = 0; i < 3; i++)
        if (!vm->fp[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) return 0;
    char mode[8];
    int k = 0;
    for (const char* p = mode_in; *p && k < 7; p++)
        if (*p != 'b') mode[k++] = *p;
    mode[k] = 0;
    // a mode longer than 7 characters can't be valid anyway
    if (strlen(mode_in) > 12) return 0;
    int idx = find_index(vm, name);
    LavaHandle* h = &vm->fp[slot];
    if (!strcmp(mode, "r") || !strcmp(mode, "r+")) {
        if (idx < 0) return 0;
        LavaFile* f = &vm->files[idx];
        h->blob = f->blob;
        f->blob->refs++;
        memcpy(h->name, f->name, sizeof h->name);
        h->r = 1;
        h->w = mode[1] == '+';
        h->pos = 0;
    } else if (!strcmp(mode, "w") || !strcmp(mode, "w+")) {
        // os.path.dirname(name.rstrip("/")) + "/": the head up to the last
        // slash, its trailing slashes stripped unless it is all slashes.
        // (So a file in the root gives "//", which never matches: as lavaemu.)
        char d[LAVA_NAME_MAX + 2];
        int n = (int)strlen(name);
        while (n > 0 && name[n - 1] == '/') n--;
        int i = n;
        while (i > 0 && name[i - 1] != '/') i--;
        int hl = i, all_slash = 1;
        for (int j = 0; j < hl; j++)
            if (name[j] != '/') all_slash = 0;
        if (hl && !all_slash)
            while (hl > 0 && name[hl - 1] == '/') hl--;
        memcpy(d, name, hl);
        d[hl] = '/';
        d[hl + 1] = 0;
        int ok = has_dir(vm, d);
        if (!ok)
            for (int i = 0; i < vm->nfiles && !ok; i++)
                if (iprefix(vm->files[i].name, d)) ok = 1;
        if (!ok) return 0;
        const char* key = idx >= 0 ? vm->files[idx].name : name;
        char keyc[LAVA_NAME_MAX];
        snprintf(keyc, sizeof keyc, "%s", key);
        h->blob = blob_new(NULL, 0);
        LavaBlob* empty = blob_new(NULL, 0);
        if (!h->blob || !empty) return 0;
        set_file(vm, keyc, empty, 1);
        snprintf(h->name, sizeof h->name, "%s", keyc);
        h->r = mode[1] == '+';
        h->w = 1;
        h->wrote = 1;
        h->pos = 0;
    } else if (!strcmp(mode, "a") || !strcmp(mode, "a+")) {
        if (idx < 0) return 0;
        LavaFile* f = &vm->files[idx];
        h->blob = f->blob;
        f->blob->refs++;
        memcpy(h->name, f->name, sizeof h->name);
        h->r = mode[1] == '+';
        h->w = 1;
        h->pos = f->blob->len;
    } else {
        return 0;
    }
    h->used = 1;
    return slot | 0x80;
}

static LavaHandle* handle(LavaVM* vm, int32_t h) {
    h &= 0xFF;
    if (!(h & 0x80)) return NULL;
    h &= 0x7F;
    if (h >= 3 || !vm->fp[h].used) return NULL;
    return &vm->fp[h];
}

static void fclose_(LavaVM* vm, int32_t hv) {
    LavaHandle* h = handle(vm, hv);
    if (!h) return;
    if (h->w) {
        // files[name] = the handle's bytes (shared; blobs are copy-on-write)
        LavaBlob* b = h->blob;
        b->refs++;
        set_file(vm, h->name, b, h->wrote);
        if (h->wrote && vm->host.file_written) vm->host.file_written(vm->host.ud, h->name, b->data, b->len);
    }
    blob_unref(h->blob);
    memset(h, 0, sizeof *h);
}

// Writes `n` bytes at the handle's position, zero-filling a gap (lavaemu fwrite/putc).
static int hwrite(LavaHandle* h, const uint8_t* src, uint32_t n) {
    LavaBlob* b = blob_own(&h->blob);
    if (!b) return 0;
    h->wrote = 1;
    uint32_t end = h->pos + n;
    uint32_t need = end > b->len ? end : b->len;
    if (h->pos > b->len) need = end;
    if (!blob_reserve(b, need)) return 0;
    if (h->pos > b->len) memset(b->data + b->len, 0, h->pos - b->len);
    memmove(b->data + h->pos, src, n);
    if (end > b->len) b->len = end;
    h->pos = end;
    return 1;
}

static const uint16_t crc_tab[16] = {0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
                                     0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef};

static const int16_t sin_tab[91] = {
    0,   18,  36,  54,  71,  89,  107, 125, 143, 160, 178, 195,  213,  230,  248,  265,  282,  299,  316,
    333, 350, 367, 384, 400, 416, 433, 449, 465, 481, 496, 512,  527,  543,  558,  573,  587,  602,  616,
    630, 644, 658, 672, 685, 698, 711, 724, 737, 749, 761, 773,  784,  796,  807,  818,  828,  839,  849,
    859, 868, 878, 887, 896, 904, 912, 920, 928, 935, 943, 949,  956,  962,  968,  974,  979,  984,  989,
    994, 998, 1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024, 1024};

static int lsin(int32_t deg) {
    int d = (deg & 0x7FFF) % 360;
    int q = d / 90;
    if (q == 0) return sin_tab[d];
    if (q == 1) return sin_tab[180 - d];
    if (q == 2) return -sin_tab[d - 180];
    return -sin_tab[360 - d];
}

static int lcos(int32_t deg) {
    int d = (deg & 0x7FFF) % 360;
    return d >= 270 ? lsin(d - 270) : lsin(d + 90);
}

static inline void wr8(LavaVM* vm, uint32_t a, int32_t v) { vm->mem[a & 0xFFFF] = (uint8_t)v; }

// ---------------------------------------------------------------------------
// System calls. Returns 1 if the call blocks (getchar with no key).

#define POPN(n)                     \
    int32_t* A = vm->stack + vm->sp - (n); \
    vm->sp -= (n)
#define RET(v)                      \
    do {                            \
        int32_t _v = (int32_t)(v);  \
        vm->last = _v;              \
        vm->stack[vm->sp++] = _v;   \
    } while (0)

// Notes a key read site for the frontend's live key hints (UI only).
static void note_read(LavaVM* vm) {
    uint32_t pc = vm->pc - 1, fb = vm->fb & 0xFFFF;
    const uint8_t* m = vm->mem;
    uint32_t r0 = m[fb] | m[fb + 1] << 8 | m[fb + 2] << 16;
    uint32_t fb2 = m[fb + 3] | m[fb + 4] << 8;
    uint32_t r1 = m[fb2] | m[fb2 + 1] << 8 | m[fb2 + 2] << 16;
    if (r0 >= vm->code_len) r0 = 0;
    if (r1 >= vm->code_len) r1 = 0;
    int oldest = 0;
    for (int i = 0; i < LAVA_READ_SITES; i++) {
        LavaReadSite* r = &vm->reads[i];
        if (r->pc == pc && r->ret0 == r0 && r->ret1 == r1) {
            r->frame = vm->frame;
            return;
        }
        if (r->frame < vm->reads[oldest].frame) oldest = i;
    }
    vm->reads[oldest] = (LavaReadSite){pc, r0, r1, vm->frame};
}

#ifdef LAVA_PROFILE
uint64_t lava_sys_count[256];
double lava_sys_time[256];
double lava_prof_now(void);
static int sys_call_(LavaVM* vm, int op);
static int sys_call(LavaVM* vm, int op) {
    double t = lava_prof_now();
    int r = sys_call_(vm, op);
    lava_sys_time[op] += lava_prof_now() - t;
    lava_sys_count[op]++;
    return r;
}
static int sys_call_(LavaVM* vm, int op) {
#else
static int sys_call(LavaVM* vm, int op) {
#endif
    uint8_t* mem = vm->mem;
    vm->sys_since_key++;
    if (op >= 0xCB) {
        if (!lavax_sys(vm, op) && !vm->ended) {
            vm->error = vm->ended = 1;
            snprintf(vm->errmsg, sizeof vm->errmsg, "unknown syscall %#x at %#x", op, (unsigned)vm->pc - 1);
        }
        return 0;
    }
    if (vm->px && lavax_sys_px(vm, op)) return 0;
    switch (op) {
    case 0x8A: {    // TextOut(x, y, str, type)
        POPN(4);
        uint32_t s = (uint32_t)A[2] & 0xFFFF;
        int32_t t = A[3];
        uint32_t base = (t & 0x40) ? LAVA_GRAPH : LAVA_GBUF;
        draw_text(vm, base, s16(A[0]), s16(A[1]), mem + s, (int)cstr_len(vm, s), (t & 0x80) != 0, t, (t & 0x20) != 0,
                  (t & 0x08) != 0);
        break;
    }
    case 0x88: {    // WriteBlock(x, y, w, h, type, data)
        POPN(6);
#ifdef LAVA_PROFILE
        if (getenv("BLITLOG")) fprintf(stderr, "WB x=%d y=%d w=%d h=%d t=%#x\n", s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), A[4]);
#endif
        int32_t t = A[4];
        uint32_t base = (t & 0x40) ? LAVA_GRAPH : LAVA_GBUF;
        draw_data(vm, base, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), mem, MEM_END, (uint32_t)A[5] & 0xFFFF, t,
                  (t & 0x20) != 0, (t & 0x08) != 0);
        break;
    }
    case 0x89:
        memcpy(mem + LAVA_GRAPH, mem + LAVA_GBUF, LAVA_SCREEN_BYTES);
        if (vm->host.on_refresh) vm->host.on_refresh(vm->host.ud, mem + LAVA_GRAPH);
        break;
    case 0x8B:
    case 0x8C: {    // Block / Rectangle
        POPN(5);
        int32_t t = A[4];
        rect(vm, (t & 0x40) ? LAVA_GRAPH : LAVA_GBUF, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), op == 0x8B, t & 3);
        break;
    }
    case 0x8E: memset(mem + LAVA_GBUF, 0, LAVA_SCREEN_BYTES); break;
    case 0x81:
    case 0xC4: {    // getchar / GetWord(mode)
        note_read(vm);
        if (!vm->latched) return 1;
        int k = vm->latched;
        vm->latched = 0;
        if (k == vm->autorelease) {
            vm->held[k] = 0;
            vm->autorelease = 0;
        }
        if (op == 0xC4) vm->sp -= 1;
        vm->seen[k] = 1;
        vm->last_key = k;
        vm->sys_since_key = 0;
        RET(k);
        break;
    }
    case 0x93: {    // Inkey
        note_read(vm);
        int k = vm->latched;
        vm->latched = 0;
        if (k && k == vm->autorelease) {
            vm->held[k] = 0;
            vm->autorelease = 0;
        }
        if (k) vm->last_key = k, vm->sys_since_key = 0, vm->seen[k] = 1;
        RET(k);
        break;
    }
    case 0xBC: {    // CheckKey
        POPN(1);
        int32_t k = A[0];
        if (vm->key_probe) vm->key_probe(vm, 1, k);
        if (k & ~0x7F) note_read(vm);
        else vm->checked[k & 0x7F] = vm->frame + 1;
        if (k & ~0x7F) {
            int m = min_held(vm);
            if (m > 0) vm->last_key = m, vm->sys_since_key = 0, vm->seen[m] = 1;
            RET(m < 0 ? 0 : m);
        } else {
            if (vm->held[k & 0x7F]) vm->seen[k & 0x7F] = 1;
            RET(vm->held[k & 0x7F] ? -1 : 0);
        }
        break;
    }
    case 0xC6: {    // ReleaseKey
        POPN(1);
        int32_t k = A[0];
        if (k & ~0x7F) {
            int m = min_held(vm);
            if (m >= 0) vm->latched = (uint8_t)m;
        } else if (vm->held[k & 0x7F]) {
            vm->latched = (uint8_t)(k & 0x7F);
        }
        break;
    }
    case 0x87: {    // Delay(ms)
        POPN(1);
        vm->us += (int64_t)(A[0] & 0x7FFF) * 1000;
        break;
    }
    case 0xBB: RET((int32_t)((vm->us / 1000) % 1000 * 256 / 1000)); break;   // Getms
    case 0x90:
        vm->seed = vm->seed * 22695477u + 1u;
        RET((vm->seed >> 16) & 0x7FFF);
        break;
    case 0x91: {
        POPN(1);
        vm->seed = (uint32_t)A[0];
        break;
    }
    case 0x82: {    // printf(fmt, ...)
        POPN(1);
        int n = A[0] & 0xFF;
        if (n > vm->sp) n = vm->sp;
        int32_t args[256];
        memcpy(args, vm->stack + vm->sp - n, n * sizeof(int32_t));
        vm->sp -= n;
        uint8_t out[1024];
        int len = n ? format(vm, args, n, out, sizeof out) : 0;
        tadds(vm, out, len);
        trender(vm, 0);
        break;
    }
    case 0x80: {    // putchar
        POPN(1);
        tadd(vm, A[0] & 0xFF);
        trender(vm, 0);
        break;
    }
    case 0xB8: {    // sprintf(dst, fmt, ...)
        POPN(1);
        int n = A[0] & 0xFF;
        if (n > vm->sp) n = vm->sp;
        int32_t args[256];
        memcpy(args, vm->stack + vm->sp - n, n * sizeof(int32_t));
        vm->sp -= n;
        if (n < 2) break;
        uint32_t dst = (uint32_t)args[0] & 0xFFFF;
        uint8_t out[1024];
        int len = format(vm, args + 1, n - 1, out, sizeof out);
        if (dst + len + 1 > MEM_END) len = (int)(MEM_END - dst - 1);
        if (len < 0) len = 0;
        memcpy(mem + dst, out, len);
        mem[dst + len] = 0;
        break;
    }
    case 0x83: {    // strcpy(dst, src)
        POPN(2);
        uint32_t dst = (uint32_t)A[0] & 0xFFFF, src = (uint32_t)A[1] & 0xFFFF;
        uint32_t n = cstr_len(vm, src) + 1;
        if (dst + n > MEM_END) n = MEM_END - dst;
        memmove(mem + dst, mem + src, n);
        break;
    }
    case 0xA6: {    // strcat
        POPN(2);
        uint32_t dst = (uint32_t)A[0] & 0xFFFF, src = (uint32_t)A[1] & 0xFFFF;
        uint32_t d = dst + cstr_len(vm, dst);
        uint32_t n = cstr_len(vm, src) + 1;
        if (d + n > MEM_END) n = MEM_END > d ? MEM_END - d : 0;
        memmove(mem + d, mem + src, n);
        break;
    }
    case 0x84: {
        POPN(1);
        RET(cstr_len(vm, (uint32_t)A[0] & 0xFFFF));
        break;
    }
    case 0x85: {    // SetScreen
        POPN(1);
        vm->tbig = (A[0] & 0xFF) == 0;
        vm->trow = vm->tcol = 0;
        memset(mem + LAVA_TEXT, 0, 160);
        memset(mem + LAVA_GRAPH, 0, LAVA_SCREEN_BYTES);
        break;
    }
    case 0x86: {
        POPN(1);
        trender(vm, A[0] & 0xFF);
        break;
    }
    case 0x92: {    // Locate(row, col)
        POPN(2);
        int cols, rows, rh;
        tdims(vm, &cols, &rows, &rh);
        int32_t r = A[0], c = A[1];
        vm->trow = (uint8_t)(r < 0 ? 0 : r > rows - 1 ? rows - 1 : r);
        vm->tcol = (uint8_t)(c < 0 ? 0 : c > cols - 1 ? cols - 1 : c);
        break;
    }
    case 0x8D:
        vm->sp -= 1;
        vm->ended = 1;
        break;
    case 0x8F: {
        POPN(1);
        int32_t v = A[0];
        RET(v < 0 ? (int32_t)(0u - (uint32_t)v) : v);
        break;
    }
    case 0x94: {    // Point (screen unless bit 6)
        POPN(3);
        point(vm, (A[2] & 0x40) ? LAVA_GBUF : LAVA_GRAPH, s16(A[0]), s16(A[1]), A[2] & 3);
        break;
    }
    case 0x95: {
        POPN(2);
        int x = s16(A[0]), y = s16(A[1]);
        int v = 0;
        if (x >= 0 && x < W && y >= 0 && y < H) v = mem[LAVA_GRAPH + y * BPL + (x >> 3)] & (0x80 >> (x & 7));
        RET(v);
        break;
    }
    case 0x96: {
        POPN(5);
        line(vm, (A[4] & 0x40) ? LAVA_GBUF : LAVA_GRAPH, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), A[4] & 3);
        break;
    }
    case 0x97: {
        POPN(6);
        rect(vm, (A[5] & 0x40) ? LAVA_GBUF : LAVA_GRAPH, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), (A[4] & 0xFF) != 0,
             A[5] & 3);
        break;
    }
    case 0x98: {
        POPN(5);
        oval(vm, (A[4] & 0x40) ? LAVA_GBUF : LAVA_GRAPH, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[2]), (A[3] & 0xFF) != 0,
             A[4] & 3);
        break;
    }
    case 0x99: {
        POPN(6);
        oval(vm, (A[5] & 0x40) ? LAVA_GBUF : LAVA_GRAPH, s16(A[0]), s16(A[1]), s16(A[2]), s16(A[3]), (A[4] & 0xFF) != 0,
             A[5] & 3);
        break;
    }
    case 0x9A: break;   // Beep
    case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4:
    case 0xA5: case 0xAA: case 0xAB: {
        POPN(1);
        // isalpha/isdigit/... on a key just read: text entry (toupper/tolower only normalise hotkeys)
        if (op != 0xAA && op != 0xAB && vm->sys_since_key <= 3 && A[0] == vm->last_key && A[0])
            vm->classified = vm->frame + 1;
        int c = A[0] & 0xFF;
        int dig = c >= '0' && c <= '9', up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z';
        int r = 0;
        switch (op) {
        case 0x9B: r = dig || up || lo; break;
        case 0x9C: r = up || lo; break;
        case 0x9D: r = c < 0x20 || c == 0x7F; break;
        case 0x9E: r = dig; break;
        case 0x9F: r = c >= 0x21 && c <= 0x7E; break;
        case 0xA0: r = lo; break;
        case 0xA1: r = c >= 0x20 && c <= 0x7E; break;
        case 0xA2: r = c >= 0x21 && c <= 0x7E && !(dig || up || lo); break;
        case 0xA3: r = c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32; break;
        case 0xA4: r = up; break;
        case 0xA5: r = dig || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); break;
        case 0xAA: RET(up ? c + 32 : c); return 0;
        case 0xAB: RET(lo ? c - 32 : c); return 0;
        }
        RET(r ? -1 : 0);
        break;
    }
    case 0xA7: {    // strchr
        POPN(2);
        uint32_t a = (uint32_t)A[0] & 0xFFFF;
        int c = A[1] & 0xFF;
        while (a < MEM_END - 1 && mem[a] != c && mem[a]) a++;
        RET(mem[a] == c ? (int32_t)a : 0);
        break;
    }
    case 0xA8: {    // strcmp
        POPN(2);
        uint32_t a = (uint32_t)A[0] & 0xFFFF, b = (uint32_t)A[1] & 0xFFFF;
        int r;
        for (;;) {
            r = (int)mem[a] - (int)mem[b];
            if (r || !mem[a] || a >= MEM_END - 1 || b >= MEM_END - 1) break;
            a++, b++;
        }
        RET(r);
        break;
    }
    case 0xA9: {    // strstr
        POPN(2);
        uint32_t a = (uint32_t)A[0] & 0xFFFF, b = (uint32_t)A[1] & 0xFFFF;
        uint32_t na = cstr_len(vm, a), nb = cstr_len(vm, b);
        int32_t res = 0;
        for (uint32_t i = 0; i + nb <= na; i++) {
            if (!memcmp(mem + a + i, mem + b, nb)) {
                res = (int32_t)(a + i);
                break;
            }
        }
        RET(res);
        break;
    }
    case 0xAC: {    // memset
        POPN(3);
        uint32_t a = (uint32_t)A[0] & 0xFFFF, n = (uint32_t)A[2] & 0xFFFF;
        if (a + n > MEM_END) n = MEM_END - a;
        memset(mem + a, A[1] & 0xFF, n);
        break;
    }
    case 0xAD:
    case 0xBD: {    // memcpy / memmove
        POPN(3);
        uint32_t d = (uint32_t)A[0] & 0xFFFF, s = (uint32_t)A[1] & 0xFFFF, n = (uint32_t)A[2] & 0xFFFF;
        if (op == 0xAD && n > 0x7FFF) n = 0;
        if (d + n > MEM_END) n = MEM_END - d;
        if (s + n > MEM_END) n = MEM_END - s;
        memmove(mem + d, mem + s, n);
        break;
    }
    case 0xAE: {    // fopen(name, mode)
        POPN(2);
        char name[LAVA_NAME_MAX], mode[16];
        path_of(vm, (uint32_t)A[0], name);
        uint32_t ma = (uint32_t)A[1] & 0xFFFF;
        uint32_t ml = cstr_len(vm, ma);
        if (ml > 15) ml = 15;
        memcpy(mode, mem + ma, ml);
        mode[ml] = 0;
        RET(fopen_(vm, name, mode));
        break;
    }
    case 0xAF: {
        POPN(1);
        fclose_(vm, A[0] & 0xFF);
        break;
    }
    case 0xB0:
    case 0xB1: {    // fread/fwrite(ptr, size, n, fp)
        POPN(4);
        uint32_t a = (uint32_t)A[0] & 0xFFFF, n = (uint32_t)A[2] & 0xFFFF;
        LavaHandle* h = handle(vm, A[3] & 0xFF);
        if (!h || (op == 0xB0 && !h->r) || (op == 0xB1 && !h->w)) {
            RET(0);
        } else if (op == 0xB0) {
            uint32_t avail = h->pos < h->blob->len ? h->blob->len - h->pos : 0;
            uint32_t k = n < avail ? n : avail;
            if (a + k > MEM_END) k = MEM_END - a;
            memcpy(mem + a, h->blob->data + h->pos, k);
            h->pos += k;
            RET(k);
        } else {
            uint32_t k = n;
            if (a + k > MEM_END) k = MEM_END - a;
            hwrite(h, mem + a, k);
            RET(n);
        }
        break;
    }
    case 0xB2: {    // fseek(fp, off, base)
        POPN(3);
        LavaHandle* h = handle(vm, A[0] & 0xFF);
        int b = A[2] & 0xFF;
        if (!h || b > 2) {
            RET(-1);
        } else {
            int64_t pos = (b == 0 ? 0 : b == 1 ? (int64_t)h->pos : (int64_t)h->blob->len) + A[1];
            h->pos = pos < 0 ? 0 : (uint32_t)pos;
            RET(h->pos);
        }
        break;
    }
    case 0xB3: {
        POPN(1);
        LavaHandle* h = handle(vm, A[0] & 0xFF);
        RET(h ? (int32_t)h->pos : -1);
        break;
    }
    case 0xB4: {
        POPN(1);
        LavaHandle* h = handle(vm, A[0] & 0xFF);
        RET(!h || h->pos >= h->blob->len ? -1 : 0);
        break;
    }
    case 0xB5: {
        POPN(1);
        LavaHandle* h = handle(vm, A[0] & 0xFF);
        if (h) h->pos = 0;
        break;
    }
    case 0xB6: {    // getc
        POPN(1);
        LavaHandle* h = handle(vm, A[0] & 0xFF);
        if (!h || !h->r || h->pos >= h->blob->len) RET(-1);
        else RET(h->blob->data[h->pos++]);
        break;
    }
    case 0xB7: {    // putc(ch, fp)
        POPN(2);
        LavaHandle* h = handle(vm, A[1] & 0xFF);
        if (!h || !h->w) {
            RET(-1);
        } else {
            uint8_t ch = (uint8_t)A[0];
            hwrite(h, &ch, 1);
            RET(ch);
        }
        break;
    }
    case 0xB9: {    // MakeDir
        POPN(1);
        char p[LAVA_NAME_MAX], d[LAVA_NAME_MAX];
        path_of(vm, (uint32_t)A[0], p);
        dir_form(p, d);
        add_dir(vm, d);
        RET(-1);
        break;
    }
    case 0xBA: {    // DeleteFile
        POPN(1);
        char p[LAVA_NAME_MAX];
        path_of(vm, (uint32_t)A[0], p);
        int i = find_index(vm, p);
        if (i >= 0) {
            if (vm->host.file_deleted) vm->host.file_deleted(vm->host.ud, vm->files[i].name);
            delete_file(vm, i);
        }
        RET(i >= 0 ? -1 : 0);
        break;
    }
    case 0xBE: {    // Crc16
        POPN(2);
        uint32_t a = (uint32_t)A[0] & 0xFFFF;
        uint32_t crc = 0;
        for (uint32_t i = 0; i < ((uint32_t)A[1] & 0xFFFF); i++) {
            uint8_t b = mem[(a + i) & 0xFFFF];
            uint32_t t = (crc >> 8) & 0xFF;
            crc = ((crc << 4) ^ crc_tab[(t >> 4) ^ (b >> 4)]) & 0xFFFF;
            t = (crc >> 8) & 0xFF;
            crc = ((crc << 4) ^ crc_tab[(t >> 4) ^ (b & 15)]) & 0xFFFF;
        }
        RET(crc);
        break;
    }
    case 0xBF: {    // Secret(mem, len, key)
        POPN(3);
        uint32_t a = (uint32_t)A[0] & 0xFFFF, k = (uint32_t)A[2] & 0xFFFF;
        uint32_t kl = cstr_len(vm, k);
        if (kl) {
            uint8_t key[256];
            if (kl > sizeof key) kl = sizeof key;
            memcpy(key, mem + k, kl);
            for (uint32_t i = 0; i < ((uint32_t)A[1] & 0xFFFF); i++) mem[(a + i) & 0xFFFF] ^= key[i % kl];
        }
        break;
    }
    case 0xC0: {    // ChDir
        POPN(1);
        char p[LAVA_NAME_MAX], d[LAVA_NAME_MAX];
        path_of(vm, (uint32_t)A[0], p);
        dir_form(p, d);
        if (has_dir(vm, d)) {
            snprintf(vm->cwd, sizeof vm->cwd, "%s", d);
            RET(-1);
        } else {
            RET(0);
        }
        break;
    }
    case 0xC1:
        vm->sp -= 1;
        RET(0);
        break;
    case 0xC2: {    // GetTime
        POPN(1);
        uint32_t a = (uint32_t)A[0] & 0xFFFF;
        int t[7];
        if (vm->host.get_time) {
            vm->host.get_time(vm->host.ud, t);
        } else {
            int64_t secs = vm->us / 1000000;
            t[0] = 2004, t[1] = 9, t[2] = 25, t[3] = 12, t[4] = (int)((secs / 60) % 60), t[5] = (int)(secs % 60), t[6] = 6;
        }
        wr8(vm, a, t[0]);
        wr8(vm, a + 1, t[0] >> 8);
        for (int i = 0; i < 6; i++) mem[a + 2 + i] = (uint8_t)t[1 + i];
        break;
    }
    case 0xC3: vm->sp -= 1; break;
    case 0xC5: {
        POPN(1);
        xdraw(vm, A[0]);
        break;
    }
    case 0xC7: {    // GetBlock(x, y, w, h, type, data)
        POPN(6);
        get_block(vm, (A[4] & 0x40) ? LAVA_GRAPH : LAVA_GBUF, (uint32_t)A[0] & 0xFFFF, (uint32_t)A[1] & 0xFFFF,
                  (uint32_t)A[2] & 0xFFFF, (uint32_t)A[3] & 0xFFFF, (uint32_t)A[5] & 0xFFFF);
        break;
    }
    case 0xC8: {
        POPN(1);
        RET(lsin(A[0]));
        break;
    }
    case 0xC9: {
        POPN(1);
        RET(lcos(A[0]));
        break;
    }
    case 0xCA: vm->sp -= 3; break;
    default:
        vm->error = 1;
        vm->ended = 1;
        snprintf(vm->errmsg, sizeof vm->errmsg, "unknown syscall %#x at %#x", op, (unsigned)vm->pc - 1);
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The interpreter loop. Registers live in locals for the whole frame and are
// written back only around system calls.

#define RD16(a) ((uint32_t)mem[(a)] | (uint32_t)mem[(a) + 1] << 8)
#define RD32(a) ((uint32_t)mem[(a)] | (uint32_t)mem[(a) + 1] << 8 | (uint32_t)mem[(a) + 2] << 16 | (uint32_t)mem[(a) + 3] << 24)
#define OP16 ((uint32_t)code[pc] | (uint32_t)code[pc + 1] << 8)
#define OP24 ((uint32_t)code[pc] | (uint32_t)code[pc + 1] << 8 | (uint32_t)code[pc + 2] << 16)
#define PUSH(v) (sp[0] = (v), sp++)
#define POP() (*--sp)
#define TOP (sp[-1])

static void run_until(LavaVM* vm, int64_t end_us) {
    const uint8_t* code = vm->code;
    uint8_t* mem = vm->mem;
    uint32_t pc = vm->pc;
    int32_t last = vm->last;
    uint32_t fb = vm->fb, fe = vm->fe;
    int32_t* sp = vm->stack + vm->sp;
    const int64_t upo = vm->us_per_op;
    int64_t budget = (end_us - vm->us) / upo;
    if (end_us - vm->us < 0) budget = 0;
    int64_t n = 0;
    void (*probe)(LavaVM*, int, int) = vm->key_probe;

    while (n < budget) {
        uint32_t op = code[pc++];
        n++;
        switch (op) {
        case 0x00:
        case 0x44: break;
        case 0x01: last = code[pc++]; PUSH(last); break;
        case 0x02: last = (int16_t)OP16; PUSH(last); pc += 2; break;
        case 0x03: last = (int32_t)(OP16 | (uint32_t)code[pc + 2] << 16 | (uint32_t)code[pc + 3] << 24); PUSH(last); pc += 4; break;
        case 0x04: last = mem[OP16]; PUSH(last); pc += 2; break;
        case 0x05: { uint32_t a = OP16; last = (int16_t)RD16(a); PUSH(last); pc += 2; break; }
        case 0x06: { uint32_t a = OP16; last = (int32_t)RD32(a); PUSH(last); pc += 2; break; }
        case 0x07: { uint32_t a = (OP16 + (uint32_t)POP()) & 0xFFFF; last = mem[a]; PUSH(last); pc += 2; break; }
        case 0x08: { uint32_t a = (OP16 + (uint32_t)POP()) & 0xFFFF; last = (int16_t)RD16(a); PUSH(last); pc += 2; break; }
        case 0x09: { uint32_t a = (OP16 + (uint32_t)POP()) & 0xFFFF; last = (int32_t)RD32(a); PUSH(last); pc += 2; break; }
        case 0x0A: last = (int32_t)(((OP16 + (uint32_t)POP()) & 0xFFFF) | 0x10000); PUSH(last); pc += 2; break;
        case 0x0B: last = (int32_t)(((OP16 + (uint32_t)POP()) & 0xFFFF) | 0x20000); PUSH(last); pc += 2; break;
        case 0x0C: last = (int32_t)(((OP16 + (uint32_t)POP()) & 0xFFFF) | 0x40000); PUSH(last); pc += 2; break;
        case 0x0D: {
            uint32_t e = pc;
            // with an op 0x43 key the string and its terminator are stored XORed:
            // it ends at the byte equal to the key
            uint8_t term = vm->xorkey;
            while (e < vm->code_len && code[e] != term) e++;
            uint32_t len = e + 1 - pc;
            uint32_t a = vm->strp;
            if (vm->xorkey) {
                for (uint32_t i = 0; i < len; i++) mem[a + i] = code[pc + i] ^ vm->xorkey;
            } else {
                memcpy(mem + a, code + pc, len);
            }
            vm->strp = a + len;
            if (vm->strp >= LAVA_STRSTACK + LAVA_STRSTACK_SIZE * 3 / 4) vm->strp = LAVA_STRSTACK;
            last = (int32_t)a;
            PUSH(last);
            pc = e + 1;
            break;
        }
        case 0x0E: last = mem[(OP16 + fb) & 0xFFFF]; PUSH(last); pc += 2; break;
        case 0x0F: { uint32_t a = OP16 + fb; last = (int16_t)RD16(a); PUSH(last); pc += 2; break; }
        case 0x10: { uint32_t a = (OP16 + fb) & 0xFFFF; last = (int32_t)RD32(a); PUSH(last); pc += 2; break; }
        case 0x11: { uint32_t a = (OP16 + (uint32_t)POP() + fb) & 0xFFFF; last = mem[a]; PUSH(last); pc += 2; break; }
        case 0x12: { uint32_t a = (OP16 + (uint32_t)POP() + fb) & 0xFFFF; last = (int16_t)RD16(a); PUSH(last); pc += 2; break; }
        case 0x13: { uint32_t a = (OP16 + (uint32_t)POP() + fb) & 0xFFFF; last = (int32_t)RD32(a); PUSH(last); pc += 2; break; }
        case 0x14:
        case 0x15:
        case 0x16:
            last = (int32_t)(((OP16 + (uint32_t)POP() + fb) & 0xFFFF) | (0x10000u << (op - 0x14)));
            PUSH(last);
            pc += 2;
            break;
        case 0x17: last = (int32_t)((OP16 + (uint32_t)POP()) & 0xFFFF); PUSH(last); pc += 2; break;
        case 0x18: last = (int32_t)((OP16 + (uint32_t)POP() + fb) & 0xFFFF); PUSH(last); pc += 2; break;
        case 0x19: last = (int32_t)((OP16 + fb) & 0xFFFF); PUSH(last); pc += 2; break;
        case 0x1A: last = LAVA_TEXT; PUSH(last); break;
        case 0x1B: last = LAVA_GRAPH; PUSH(last); break;
        case 0x42: last = LAVA_GBUF; PUSH(last); break;
        case 0x1C: last = (int32_t)(0u - (uint32_t)POP()); PUSH(last); break;
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20: {
            int32_t p = POP();
            uint32_t a = (uint32_t)p & 0xFFFF;
            if (p & 0x800000) a += fb;
            int ln = (p >> 16) & 0x7F;
            int32_t v, nv;
            int up = op == 0x1D || op == 0x1F;
            if (ln == 1) {
                v = mem[a & 0xFFFF];
                nv = up ? v + 1 : v - 1;
                mem[a & 0xFFFF] = (uint8_t)nv;
            } else if (ln == 2) {
                uint32_t b = a & 0xFFFF;
                v = (int16_t)RD16(b);
                nv = up ? v + 1 : v - 1;
                mem[b] = (uint8_t)nv;
                mem[(a + 1) & 0xFFFF] = (uint8_t)(nv >> 8);
            } else {
                uint32_t b = a & 0xFFFF;
                v = (int32_t)RD32(b);
                nv = (int32_t)((uint32_t)v + (up ? 1u : 0xFFFFFFFFu));
                for (int i = 0; i < 4; i++) mem[(a + i) & 0xFFFF] = (uint8_t)((uint32_t)nv >> (8 * i));
            }
            last = op <= 0x1E ? nv : v;
            PUSH(last);
            break;
        }
        case 0x21: { int32_t b = POP(); last = (int32_t)((uint32_t)POP() + (uint32_t)b); PUSH(last); break; }
        case 0x22: { int32_t b = POP(); last = (int32_t)((uint32_t)POP() - (uint32_t)b); PUSH(last); break; }
        case 0x23: { int32_t b = POP(); last = POP() & b; PUSH(last); break; }
        case 0x24: { int32_t b = POP(); last = POP() | b; PUSH(last); break; }
        case 0x25: last = ~POP(); PUSH(last); break;
        case 0x26: { int32_t b = POP(); last = POP() ^ b; PUSH(last); break; }
        case 0x27: { int32_t b = POP(), a = POP(); last = (a && b) ? -1 : 0; PUSH(last); break; }
        case 0x28: { int32_t b = POP(), a = POP(); last = (a || b) ? -1 : 0; PUSH(last); break; }
        case 0x29: last = POP() == 0 ? -1 : 0; PUSH(last); break;
        case 0x2A: { int32_t b = POP(); last = (int32_t)((uint32_t)POP() * (uint32_t)b); PUSH(last); break; }
        case 0x2B: { int32_t b = POP(), a = POP(); last = b == 0 ? -1 : pydiv(a, b); PUSH(last); break; }
        case 0x2C: { int32_t b = POP(), a = POP(); last = b == 0 ? 0 : pymod(a, b); PUSH(last); break; }
        case 0x2D: { int32_t b = POP(), a = POP(); last = b >= 0 ? (int32_t)((uint32_t)a << (b & 31)) : a; PUSH(last); break; }
        case 0x2E: { int32_t b = POP(), a = POP(); last = b >= 0 ? (int32_t)((uint32_t)a >> (b & 31)) : a; PUSH(last); break; }
        case 0x2F: { int32_t b = POP(), a = POP(); if (probe && (a == vm->last_key || b == vm->last_key)) probe(vm, 2, a == vm->last_key ? b : a); last = a == b ? -1 : 0; PUSH(last); break; }
        case 0x30: { int32_t b = POP(), a = POP(); if (probe && (a == vm->last_key || b == vm->last_key)) probe(vm, 2, a == vm->last_key ? b : a); last = a != b ? -1 : 0; PUSH(last); break; }
        case 0x31: { int32_t b = POP(), a = POP(); last = a <= b ? -1 : 0; PUSH(last); break; }
        case 0x32: { int32_t b = POP(), a = POP(); last = a >= b ? -1 : 0; PUSH(last); break; }
        case 0x33: { int32_t b = POP(), a = POP(); last = a > b ? -1 : 0; PUSH(last); break; }
        case 0x34: { int32_t b = POP(), a = POP(); last = a < b ? -1 : 0; PUSH(last); break; }
        case 0x35: {
            int32_t v = POP(), p = POP();
            uint32_t a = (uint32_t)p & 0xFFFF;
            if (p & 0x800000) a += fb;
            int ln = (p >> 16) & 0x7F;
            if (ln == 1) {
                mem[a & 0xFFFF] = (uint8_t)v;
            } else if (ln == 2) {
                mem[a & 0xFFFF] = (uint8_t)v;
                mem[(a + 1) & 0xFFFF] = (uint8_t)(v >> 8);
            } else {
                a &= 0xFFFF;
                mem[a] = (uint8_t)v;
                mem[a + 1] = (uint8_t)(v >> 8);
                mem[a + 2] = (uint8_t)(v >> 16);
                mem[a + 3] = (uint8_t)(v >> 24);
            }
            last = v;
            PUSH(v);
            break;
        }
        case 0x36: last = mem[(uint32_t)POP() & 0xFFFF]; PUSH(last); break;
        case 0x37: last = (int32_t)(((uint32_t)POP() & 0xFFFF) | 0x10000); PUSH(last); break;
        case 0x38: last = POP(); break;
        case 0x39: if (last == 0) pc = OP24; else pc += 3; break;
        case 0x3A: if (last != 0) pc = OP24; else pc += 3; break;
        case 0x3B: pc = OP24; break;
        case 0x3C: fb = fe = OP16; pc += 2; break;
        case 0x3D: {
            uint32_t t = OP24, r = pc + 3;
            mem[fe] = (uint8_t)r;
            mem[fe + 1] = (uint8_t)(r >> 8);
            mem[fe + 2] = (uint8_t)(r >> 16);
            pc = t;
            break;
        }
        case 0x3E: {
            mem[fe + 3] = (uint8_t)fb;
            mem[fe + 4] = (uint8_t)(fb >> 8);
            fb = fe;
            fe = (fb + OP16) & 0xFFFF;
            int argc = code[pc + 2];
            pc += 3;
            if (argc) {
                int32_t* args = sp - argc;
                sp -= argc;
                uint32_t a = fb + 5;
                for (int i = 0; i < argc; i++, a += 4) {
                    uint32_t v = (uint32_t)args[i];
                    mem[a] = (uint8_t)v;
                    mem[a + 1] = (uint8_t)(v >> 8);
                    mem[a + 2] = (uint8_t)(v >> 16);
                    mem[a + 3] = (uint8_t)(v >> 24);
                }
            }
            if (fe > vm->fe_max) vm->fe_max = fe;
            if (sp - vm->stack > LAVA_STACK_MAX - 512) {
                vm->error = vm->ended = 1;
                snprintf(vm->errmsg, sizeof vm->errmsg, "stack overflow at %#x", (unsigned)pc);
                goto out;
            }
            break;
        }
        case 0x3F:
            pc = mem[fb] | mem[fb + 1] << 8 | mem[fb + 2] << 16;
            fe = fb;
            fb = mem[fb + 3] | mem[fb + 4] << 8;
            break;
        case 0x40: vm->ended = 1; goto out;
        case 0x41: {
            uint32_t a = OP16, ln = (uint32_t)code[pc + 2] | (uint32_t)code[pc + 3] << 8;
            if (a + ln > LAVA_MEM_ALLOC) ln = LAVA_MEM_ALLOC - a;
            memcpy(mem + a, code + pc + 4, ln);
            pc += 4 + ((uint32_t)code[pc + 2] | (uint32_t)code[pc + 3] << 8);
            break;
        }
        case 0x43: vm->xorkey = code[pc++]; break;
        case 0x45: last = (int32_t)((uint32_t)POP() + (uint32_t)(int16_t)OP16); PUSH(last); pc += 2; break;
        case 0x46: last = (int32_t)((uint32_t)POP() - (uint32_t)(int16_t)OP16); PUSH(last); pc += 2; break;
        case 0x47: last = (int32_t)((uint32_t)POP() * (uint32_t)(int16_t)OP16); PUSH(last); pc += 2; break;
        case 0x48: { int32_t v = (int16_t)OP16, a = POP(); last = v == 0 ? -1 : pydiv(a, v); PUSH(last); pc += 2; break; }
        case 0x49: { int32_t v = (int16_t)OP16, a = POP(); last = v == 0 ? 0 : pymod(a, v); PUSH(last); pc += 2; break; }
        case 0x4A: { int32_t v = (int16_t)OP16; last = (int32_t)((uint32_t)POP() << (v & 31)); PUSH(last); pc += 2; break; }
        case 0x4B: { int32_t v = (int16_t)OP16; last = (int32_t)((uint32_t)POP() >> (v & 31)); PUSH(last); pc += 2; break; }
        case 0x4C: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 2, v); last = a == v ? -1 : 0; PUSH(last); pc += 2; break; }
        case 0x4D: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 2, v); last = a != v ? -1 : 0; PUSH(last); pc += 2; break; }
        case 0x4E: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 3, v); last = a > v ? -1 : 0; PUSH(last); pc += 2; break; }
        case 0x4F: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 3, v); last = a < v ? -1 : 0; PUSH(last); pc += 2; break; }
        case 0x50: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 3, v); last = a >= v ? -1 : 0; PUSH(last); pc += 2; break; }
        case 0x51: { int32_t v = (int16_t)OP16, a = POP(); if (probe && a == vm->last_key) probe(vm, 3, v); last = a <= v ? -1 : 0; PUSH(last); pc += 2; break; }
        default:
            if (op >= 0x52 && op <= 0x74) {
                vm->sp = (int32_t)(sp - vm->stack);
                pc = lavax_ext_op(vm, (int)op, pc, fb, &last);
                sp = vm->stack + vm->sp;
                if (vm->ended) goto out;
                break;
            }
            if (op >= 0x80 && op <= 0xD6) {
                vm->pc = pc;
                vm->last = last;
                vm->fb = fb;
                vm->fe = fe;
                vm->sp = (int32_t)(sp - vm->stack);
                vm->ops += n;
                vm->us += n * upo;
                n = 0;
                int blocked = sys_call(vm, op);
                pc = vm->pc;
                last = vm->last;
                sp = vm->stack + vm->sp;
                budget = (end_us - vm->us) / upo;
                if (end_us - vm->us < 0) budget = 0;
                if (blocked) {
                    pc--;
                    vm->waiting = 1;
                    goto out;
                }
                vm->waiting = 0;
                if (vm->ended) goto out;
            } else {
                vm->error = vm->ended = 1;
                snprintf(vm->errmsg, sizeof vm->errmsg, "bad opcode %#x at %#x", (unsigned)op, (unsigned)pc - 1);
                goto out;
            }
            break;
        }
    }
out:
    vm->pc = pc;
    vm->last = last;
    vm->fb = fb;
    vm->fe = fe;
    vm->sp = (int32_t)(sp - vm->stack);
    vm->ops += n;
    vm->us += n * upo;
}

void lava_run_frame(LavaVM* vm) {
    if (vm->ended) {
        vm->frame++;
        return;
    }
    int64_t end = (int64_t)(vm->frame + 1) * LAVA_FRAME_US;
    run_until(vm, end);
    if (vm->us < end) vm->us = end;
    vm->frame++;
}

// ---------------------------------------------------------------------------
// Save states: a flat little-endian record. Files marked dirty (saves the
// game wrote) are included; bundled data files are not.

#define STATE_MAGIC 0x3356414C   // "LAV3"

typedef struct {
    uint8_t* p;
    uint32_t n, cap;
} Out;

static void put(Out* o, const void* d, uint32_t n) {
    if (o->p && o->n + n <= o->cap) memcpy(o->p + o->n, d, n);
    o->n += n;
}
static void put32(Out* o, uint32_t v) { put(o, &v, 4); }

static uint32_t state_write(const LavaVM* vm, uint8_t* buf, uint32_t cap) {
    Out o = {buf, 0, cap};
    put32(&o, STATE_MAGIC);
    put32(&o, vm->code_len);
    put32(&o, vm->pc);
    put32(&o, (uint32_t)vm->last);
    put32(&o, vm->fb);
    put32(&o, vm->fe);
    put32(&o, (uint32_t)vm->sp);
    put(&o, &vm->us, 8);
    put32(&o, (uint32_t)vm->frame);
    put32(&o, vm->strp);
    put32(&o, vm->seed);
    uint8_t flags[8] = {vm->xorkey, vm->ended, vm->waiting, vm->tbig, vm->trow, vm->tcol, 0, 0};
    put(&o, flags, 8);
    put(&o, vm->stack, (uint32_t)vm->sp * 4);
    put(&o, vm->mem, LAVA_MEM_LOGICAL);
    put(&o, vm->cwd, LAVA_NAME_MAX);
    put32(&o, (uint32_t)vm->ndirs);
    for (int i = 0; i < vm->ndirs; i++) put(&o, vm->dirs[i], LAVA_NAME_MAX);
    int nd = 0;
    for (int i = 0; i < vm->nfiles; i++) nd += vm->files[i].dirty;
    put32(&o, (uint32_t)nd);
    for (int i = 0; i < vm->nfiles; i++) {
        if (!vm->files[i].dirty) continue;
        put(&o, vm->files[i].name, LAVA_NAME_MAX);
        put32(&o, vm->files[i].blob->len);
        put(&o, vm->files[i].blob->data, vm->files[i].blob->len);
    }
    // the pixel screen
    uint8_t pxf[8] = {vm->px != NULL, 0, 0, 0, 0, 0, 0, 0};
    if (vm->px) {
        const LavaPx* p = vm->px;
        pxf[1] = p->mode, pxf[2] = p->bg, pxf[3] = p->fg, pxf[4] = p->has_pal;
    }
    put(&o, pxf, 8);
    if (vm->px) {
        const LavaPx* p = vm->px;
        put32(&o, (uint32_t)p->w | (uint32_t)p->h << 16);
        put(&o, p->lcd, (uint32_t)p->w * p->h);
        put(&o, p->buf, (uint32_t)p->w * p->h);
        if (p->has_pal) put(&o, p->pal, sizeof p->pal);
    }
    // open handles
    for (int i = 0; i < 3; i++) {
        const LavaHandle* h = &vm->fp[i];
        uint8_t hf[4] = {h->used, h->r, h->w, 0};
        put(&o, hf, 4);
        if (!h->used) continue;
        put(&o, h->name, LAVA_NAME_MAX);
        put32(&o, h->pos);
        // a handle still sharing its file's bytes is stored as a reference
        const LavaFile* f = NULL;
        for (int k = 0; k < vm->nfiles; k++)
            if (!strcmp(vm->files[k].name, h->name)) f = &vm->files[k];
        if (f && f->blob == h->blob) {
            put32(&o, 0xFFFFFFFFu);
            continue;
        }
        put32(&o, h->blob->len);
        put(&o, h->blob->data, h->blob->len);
    }
    return o.n;
}

uint32_t lava_state_size(const LavaVM* vm) { return state_write(vm, NULL, 0); }

uint32_t lava_state_save(const LavaVM* vm, uint8_t* buf, uint32_t cap) {
    uint32_t n = state_write(vm, buf, cap);
    return n <= cap ? n : 0;
}

typedef struct {
    const uint8_t* p;
    uint32_t n, pos;
    int bad;
} In;

static void get(In* in, void* d, uint32_t n) {
    if (in->pos + n > in->n) {
        in->bad = 1;
        memset(d, 0, n);
        return;
    }
    memcpy(d, in->p + in->pos, n);
    in->pos += n;
}
static uint32_t get32(In* in) {
    uint32_t v;
    get(in, &v, 4);
    return v;
}

int lava_state_load(LavaVM* vm, const uint8_t* buf, uint32_t len) {
    In in = {buf, len, 0, 0};
    if (get32(&in) != STATE_MAGIC || get32(&in) != vm->code_len) return 0;
    uint32_t pc = get32(&in), last = get32(&in), fb = get32(&in), fe = get32(&in), sp = get32(&in);
    if (in.bad || sp > LAVA_STACK_MAX) return 0;
    int64_t us;
    get(&in, &us, 8);
    uint32_t frame = get32(&in), strp = get32(&in), seed = get32(&in);
    uint8_t flags[8];
    get(&in, flags, 8);
    // validate the rest before touching the VM
    uint32_t mark = in.pos;
    in.pos += sp * 4 + LAVA_MEM_LOGICAL + LAVA_NAME_MAX;
    if (in.pos > in.n) return 0;
    in.pos = mark;
    close_handles(vm);
    vm->pc = pc, vm->last = (int32_t)last, vm->fb = fb, vm->fe = fe, vm->sp = (int32_t)sp;
    vm->us = us, vm->frame = (int32_t)frame, vm->strp = strp, vm->seed = seed;
    vm->xorkey = flags[0], vm->ended = flags[1], vm->waiting = flags[2], vm->tbig = flags[3], vm->trow = flags[4],
    vm->tcol = flags[5];
    vm->error = 0;
    get(&in, vm->stack, sp * 4);
    get(&in, vm->mem, LAVA_MEM_LOGICAL);
    get(&in, vm->cwd, LAVA_NAME_MAX);
    vm->cwd[LAVA_NAME_MAX - 1] = 0;
    int nd = (int)get32(&in);
    if (nd > LAVA_MAX_DIRS) nd = LAVA_MAX_DIRS;
    vm->ndirs = nd;
    for (int i = 0; i < nd; i++) {
        get(&in, vm->dirs[i], LAVA_NAME_MAX);
        vm->dirs[i][LAVA_NAME_MAX - 1] = 0;
    }
    int nf = (int)get32(&in);
    for (int i = 0; i < nf && !in.bad; i++) {
        char name[LAVA_NAME_MAX];
        get(&in, name, LAVA_NAME_MAX);
        name[LAVA_NAME_MAX - 1] = 0;
        uint32_t n = get32(&in);
        if (in.pos + n > in.n) {
            in.bad = 1;
            break;
        }
        lava_add_file(vm, name, in.p + in.pos, n, 1);
        if (vm->host.file_written) vm->host.file_written(vm->host.ud, name, in.p + in.pos, n);
        in.pos += n;
    }
    uint8_t pxf[8];
    get(&in, pxf, 8);
    if (vm->px) lavax_px_free(vm->px), vm->px = NULL;
    if (pxf[0] && !in.bad) {
        uint32_t wh = get32(&in);
        int w = (int)(wh & 0xFFFF), h = (int)(wh >> 16);
        if (w < 1 || w > 320 || h < 1 || h > 240 || in.pos + 2u * w * h > in.n) return 0;
        LavaPx* p = lavax_px_new(w, h, pxf[1]);
        if (!p) return 0;
        p->bg = pxf[2], p->fg = pxf[3], p->has_pal = pxf[4];
        get(&in, p->lcd, (uint32_t)w * h);
        get(&in, p->buf, (uint32_t)w * h);
        if (p->has_pal) get(&in, p->pal, sizeof p->pal);
        vm->px = p;
    }
    for (int i = 0; i < 3 && !in.bad; i++) {
        uint8_t hf[4];
        get(&in, hf, 4);
        LavaHandle* h = &vm->fp[i];
        if (!hf[0]) continue;
        get(&in, h->name, LAVA_NAME_MAX);
        h->name[LAVA_NAME_MAX - 1] = 0;
        h->pos = get32(&in);
        uint32_t n = get32(&in);
        if (n == 0xFFFFFFFFu) {
            LavaFile* f = lava_find_file(vm, h->name);
            if (!f) {
                in.bad = 1;
                break;
            }
            h->blob = f->blob;
            f->blob->refs++;
            h->used = 1, h->r = hf[1], h->w = hf[2];
            continue;
        }
        if (in.pos + n > in.n) {
            in.bad = 1;
            break;
        }
        h->blob = blob_new(in.p + in.pos, n);
        in.pos += n;
        h->used = h->blob != NULL;
        h->r = hf[1], h->w = hf[2];
    }
    memset(vm->held, 0, sizeof vm->held);
    vm->latched = vm->autorelease = 0;
    return !in.bad;
}

// Runs until the given virtual time (tools: single-stepping in the lockstep debugger).
void lava_run_until(LavaVM* vm, int64_t end_us) {
    if (!vm->ended) run_until(vm, end_us);
}

void lava_free_file(LavaVM* vm, int i) {
    if (i >= 0 && i < vm->nfiles) delete_file(vm, i);
}
