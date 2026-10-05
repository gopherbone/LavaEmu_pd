// Host build of the VM for tests and tools (tests/lockstep.py, tools/keyscan.py):
// a flat C API over lava.c that Python drives with ctypes.
//   cc -O2 -shared -fPIC -o host/liblava.dylib src/lava.c tools/lavahost.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lava_internal.h"

static uint8_t* slurp(const char* path, uint32_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

typedef struct {
    LavaVM vm;
    uint8_t* code;
    LavaFonts fonts;
    int32_t probes[4096][2];
    int nprobes;
} Host;

static void probe(LavaVM* vm, int kind, int value) {
    Host* h = (Host*)vm;
    if (kind != 1 && vm->sys_since_key > 2) return;
    if (h->nprobes < 4096) {
        h->probes[h->nprobes][0] = kind;
        h->probes[h->nprobes][1] = value;
        h->nprobes++;
    }
}

Host* lh_new(const uint8_t* code, uint32_t len, const char* fontdir) {
    Host* h = calloc(1, sizeof *h);
    h->code = malloc(len);
    memcpy(h->code, code, len);
    char p[1024];
    const char* names[4] = {"gbfont.bin", "gbfont16.bin", "ascii.bin", "ascii8.bin"};
    const uint8_t** ptrs[4] = {&h->fonts.gb12, &h->fonts.gb16, &h->fonts.asc12, &h->fonts.asc16};
    uint32_t* lens[4] = {&h->fonts.gb12_len, &h->fonts.gb16_len, &h->fonts.asc12_len, &h->fonts.asc16_len};
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "%s/%s", fontdir, names[i]);
        *ptrs[i] = slurp(p, lens[i]);
    }
    if (!lava_init(&h->vm, h->code, len, &h->fonts)) {
        free(h->code);
        free(h);
        return NULL;
    }
    return h;
}

void lh_free(Host* h) {
    lava_free(&h->vm);
    free(h->code);
    free(h);
}

void lh_set_probe(Host* h, int on) {
    h->vm.key_probe = on ? probe : NULL;
    h->nprobes = 0;
}
int lh_probes(Host* h, int32_t* out, int cap) {
    int n = h->nprobes < cap ? h->nprobes : cap;
    memcpy(out, h->probes, n * 8);
    h->nprobes = 0;
    return n;
}

void lh_run_frame(Host* h) { lava_run_frame(&h->vm); }
void lh_run_frames(Host* h, int n) {
    for (int i = 0; i < n; i++) lava_run_frame(&h->vm);
}
uint8_t* lh_mem(Host* h) { return h->vm.mem; }
int32_t* lh_stack(Host* h) { return h->vm.stack; }
uint8_t* lh_held(Host* h) { return h->vm.held; }
const char* lh_errmsg(Host* h) { return h->vm.errmsg; }

enum {
    R_PC, R_LAST, R_FB, R_FE, R_SP, R_US, R_FRAME, R_OPS, R_STRP, R_XORKEY, R_ENDED, R_WAITING, R_ERROR, R_SEED,
    R_FEMAX, R_LATCHED, R_AUTOREL, R_TBIG, R_TROW, R_TCOL, R_FILESGEN, R_LASTKEY, R_COUNT
};

int lh_nregs(void) { return R_COUNT; }

void lh_get_regs(Host* h, int64_t* r) {
    LavaVM* v = &h->vm;
    r[R_PC] = v->pc, r[R_LAST] = v->last, r[R_FB] = v->fb, r[R_FE] = v->fe, r[R_SP] = v->sp, r[R_US] = v->us;
    r[R_FRAME] = v->frame, r[R_OPS] = (int64_t)v->ops, r[R_STRP] = v->strp, r[R_XORKEY] = v->xorkey;
    r[R_ENDED] = v->ended, r[R_WAITING] = v->waiting, r[R_ERROR] = v->error, r[R_SEED] = v->seed;
    r[R_FEMAX] = v->fe_max, r[R_LATCHED] = v->latched, r[R_AUTOREL] = v->autorelease, r[R_TBIG] = v->tbig;
    r[R_TROW] = v->trow, r[R_TCOL] = v->tcol, r[R_FILESGEN] = v->files_gen, r[R_LASTKEY] = v->last_key;
}

void lh_set_regs(Host* h, const int64_t* r) {
    LavaVM* v = &h->vm;
    v->pc = (uint32_t)r[R_PC], v->last = (int32_t)r[R_LAST], v->fb = (uint32_t)r[R_FB], v->fe = (uint32_t)r[R_FE];
    v->sp = (int32_t)r[R_SP], v->us = r[R_US], v->frame = (int32_t)r[R_FRAME], v->ops = (uint64_t)r[R_OPS];
    v->strp = (uint32_t)r[R_STRP], v->xorkey = (uint8_t)r[R_XORKEY], v->ended = (uint8_t)r[R_ENDED];
    v->waiting = (uint8_t)r[R_WAITING], v->error = (uint8_t)r[R_ERROR], v->seed = (uint32_t)r[R_SEED];
    v->fe_max = (uint32_t)r[R_FEMAX], v->latched = (uint8_t)r[R_LATCHED], v->autorelease = (uint8_t)r[R_AUTOREL];
    v->tbig = (uint8_t)r[R_TBIG], v->trow = (uint8_t)r[R_TROW], v->tcol = (uint8_t)r[R_TCOL];
}

// files
int lh_nfiles(Host* h) { return h->vm.nfiles; }
const char* lh_file_name(Host* h, int i) { return h->vm.files[i].name; }
uint32_t lh_file_len(Host* h, int i) { return h->vm.files[i].blob->len; }
const uint8_t* lh_file_data(Host* h, int i) { return h->vm.files[i].blob->data; }
void lh_clear_files(Host* h) { lava_free(&h->vm); }
int lh_add_file(Host* h, const char* name, const uint8_t* data, uint32_t len) {
    return lava_add_file(&h->vm, name, data, len, 0);
}

// handles: used, r, w, pos, name, data
int lh_handle(Host* h, int i, int32_t* info, const char** name, const uint8_t** data, uint32_t* len) {
    LavaHandle* f = &h->vm.fp[i];
    info[0] = f->used, info[1] = f->r, info[2] = f->w, info[3] = (int32_t)f->pos;
    if (!f->used) return 0;
    *name = f->name;
    *data = f->blob->data;
    *len = f->blob->len;
    return 1;
}
void lh_set_handle(Host* h, int i, int used, int r, int w, uint32_t pos, const char* name, const uint8_t* data,
                   uint32_t len) {
    // build via a temp file entry so the blob machinery is used
    LavaHandle* f = &h->vm.fp[i];
    memset(f, 0, sizeof *f);
    if (!used) return;
    LavaVM* v = &h->vm;
    lava_add_file(v, "\x01tmp", data, len, 0);
    LavaFile* t = lava_find_file(v, "\x01tmp");
    f->blob = t->blob;
    t->blob->refs++;
    // remove the temp entry
    int k = (int)(t - v->files);
    t->blob->refs--;
    memmove(&v->files[k], &v->files[k + 1], (v->nfiles - k - 1) * sizeof v->files[0]);
    v->nfiles--;
    f->used = 1, f->r = (uint8_t)r, f->w = (uint8_t)w, f->pos = pos, f->wrote = 0;
    snprintf(f->name, sizeof f->name, "%s", name);
}

// dirs and cwd
int lh_ndirs(Host* h) { return h->vm.ndirs; }
const char* lh_dir(Host* h, int i) { return h->vm.dirs[i]; }
void lh_set_dirs(Host* h, const char* joined) {
    // '\n'-separated
    LavaVM* v = &h->vm;
    v->ndirs = 0;
    const char* p = joined;
    while (*p && v->ndirs < LAVA_MAX_DIRS) {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= LAVA_NAME_MAX) n = LAVA_NAME_MAX - 1;
        memcpy(v->dirs[v->ndirs], p, n);
        v->dirs[v->ndirs][n] = 0;
        v->ndirs++;
        p += n;
        if (*p == '\n') p++;
    }
}
const char* lh_cwd(Host* h) { return h->vm.cwd; }
void lh_set_cwd(Host* h, const char* c) { snprintf(h->vm.cwd, sizeof h->vm.cwd, "%s", c); }

void lh_key_down(Host* h, int c) { lava_key_down(&h->vm, c); }
void lh_key_up(Host* h, int c) { lava_key_up(&h->vm, c, 0); }

uint32_t lh_state_save(Host* h, uint8_t* buf, uint32_t cap) {
    uint32_t n = lava_state_size(&h->vm);
    if (!buf) return n;
    return lava_state_save(&h->vm, buf, cap);
}
int lh_state_load(Host* h, const uint8_t* buf, uint32_t len) { return lava_state_load(&h->vm, buf, len); }

// Benchmark helper: run n frames, return ops executed.
uint64_t lh_ops(Host* h) { return h->vm.ops; }
void lh_run_until(Host* h, int64_t end_us) { lava_run_until(&h->vm, end_us); }

// Pace and the LavaX pixel screen
void lh_set_pace(Host* h, int upo) { lava_set_pace(&h->vm, (uint32_t)upo); }
int lh_pace(Host* h) { return (int)h->vm.us_per_op; }
int lh_px_info(Host* h, int32_t* info) {
    LavaPx* p = h->vm.px;
    if (!p) return 0;
    info[0] = p->w, info[1] = p->h, info[2] = p->mode, info[3] = p->bg, info[4] = p->fg, info[5] = p->has_pal;
    return 1;
}
uint8_t* lh_px_plane(Host* h, int lcd) { return h->vm.px ? (lcd ? h->vm.px->lcd : h->vm.px->buf) : NULL; }
uint8_t* lh_px_pal(Host* h) { return h->vm.px ? &h->vm.px->pal[0][0] : NULL; }
void lh_px_set(Host* h, int present, int w, int hh, int mode, int bg, int fg, int has_pal, const uint8_t* lcd,
               const uint8_t* buf, const uint8_t* pal) {
    LavaVM* v = &h->vm;
    if (v->px) lavax_px_free(v->px), v->px = NULL;
    if (!present) return;
    LavaPx* p = lavax_px_new(w, hh, mode);
    p->bg = (uint8_t)bg, p->fg = (uint8_t)fg, p->has_pal = (uint8_t)has_pal;
    memcpy(p->lcd, lcd, (size_t)w * hh);
    memcpy(p->buf, buf, (size_t)w * hh);
    if (has_pal && pal) memcpy(p->pal, pal, sizeof p->pal);
    v->px = p;
}
