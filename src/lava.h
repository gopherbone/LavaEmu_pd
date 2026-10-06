// LAVA (GVmaker 1.0) virtual machine: a C port of wqx_tl's lavaemu, which is
// itself a port of Eastsun's GVmaker as published in arucil/GVmakerSE and
// arucil/MyGVM (both MIT). The semantics, including virtual time, follow
// lavaemu exactly so the two can be run in lockstep (tests/lockstep.py).
//
// No Playdate dependencies: the frontend (main.c) and the host test library
// (tools/host.c) both link this file.
#ifndef LAVA_VM_H
#define LAVA_VM_H

#include <stdint.h>
#include <stddef.h>

#define LAVA_W 160
#define LAVA_H 80
#define LAVA_BPL 20
#define LAVA_SCREEN_BYTES (LAVA_BPL * LAVA_H)

#define LAVA_TEXT 0x0000
#define LAVA_GRAPH 0x0100
#define LAVA_GBUF 0x0900
#define LAVA_STRSTACK 0x1000
#define LAVA_STRSTACK_SIZE 1024

// lavaemu's memory is 0x10000 + 16 bytes; some ops read a few bytes past an
// unmasked address, so the C array has slack beyond that (never compared).
#define LAVA_MEM_LOGICAL (0x10000 + 16)
#define LAVA_MEM_ALLOC (0x20000 + 64)

#define LAVA_STACK_MAX 8192
#define LAVA_US_PER_OP 4
#define LAVA_FRAME_US (1000000 / 60)
#define LAVA_MAX_FILES 64
#define LAVA_MAX_DIRS 32
#define LAVA_NAME_MAX 96

// Key codes (GVmaker's keyboard, NC2000/NC3000 layout).
enum {
    LK_ENTER = 13, LK_PGDN = 14, LK_CAPS = 18, LK_PGUP = 19, LK_UP = 20, LK_DOWN = 21,
    LK_RIGHT = 22, LK_LEFT = 23, LK_HELP = 25, LK_SHIFT = 26, LK_ESC = 27,
    LK_F1 = 28, LK_F2 = 29, LK_F3 = 30, LK_F4 = 31, LK_SPACE = 32,
};

// A file's bytes. Shared copy-on-write between the file table and open
// handles, the way lavaemu copies a file into its handle on fopen.
typedef struct LavaBlob {
    uint8_t* data;
    uint32_t len, cap;
    int refs;
} LavaBlob;

typedef struct {
    char name[LAVA_NAME_MAX];
    LavaBlob* blob;
    uint8_t dirty;      // written (or loaded from a save) since the game started
} LavaFile;

typedef struct {
    uint8_t used, r, w, wrote;
    char name[LAVA_NAME_MAX];
    LavaBlob* blob;
    uint32_t pos;
} LavaHandle;

typedef struct {
    const uint8_t *gb12, *gb16, *asc12, *asc16;
    uint32_t gb12_len, gb16_len, asc12_len, asc16_len;
} LavaFonts;

typedef struct {
    void* ud;
    // Called when fclose writes a file back, and when DeleteFile removes one.
    void (*file_written)(void* ud, const char* name, const uint8_t* data, uint32_t len);
    void (*file_deleted)(void* ud, const char* name);
    // GetTime: year, month, day, hour, minute, second, weekday. NULL = lavaemu's fixed date.
    void (*get_time)(void* ud, int out[7]);
    // After Refresh (classic screen): the frontend's flicker-grey blending samples here.
    void (*on_refresh)(void* ud, const uint8_t* lcd);
} LavaHost;

// Where the program last read keys (for the frontend's live key hints; not VM state)
#define LAVA_READ_SITES 8
typedef struct {
    uint32_t pc, ret0, ret1;    // the read op, and the return addresses of its function and the caller
    int32_t frame;
} LavaReadSite;

// LavaX's pixel screen: one byte a pixel, `w` x `h`, outside LAVA RAM.
// Mode 1: 0/1; mode 4: 0 (white) .. 15 (black); mode 8: palette indices.
typedef struct LavaPx {
    uint16_t w, h;
    uint8_t mode, bg, fg, has_pal;
    uint8_t* lcd;
    uint8_t* buf;
    uint8_t pal[256][3];
} LavaPx;

typedef struct LavaVM {
    // hot state first
    uint32_t pc;
    int32_t last;
    uint32_t fb, fe;
    int32_t sp;                 // number of stack entries
    int64_t us;                 // virtual time, microseconds
    int32_t frame;
    uint64_t ops;
    uint32_t strp;
    uint8_t xorkey;
    uint8_t ended, waiting, error;
    uint32_t seed;
    uint32_t fe_max;
    uint32_t us_per_op;         // virtual microseconds an instruction costs (lavaemu's default 4)

    // LavaX: header (byte 8 flags, screen size) and the pixel screen (NULL: the classic screen in RAM)
    uint8_t hdr_flags, hdr_mode, lavax;
    uint16_t hdr_w, hdr_h;
    LavaPx* px;
    int32_t brightness, line;

    // input
    uint8_t held[128];
    uint8_t latched, autorelease;
    uint8_t seen[128];          // the program has read or polled this key (frontend tap handling)

    // printf/putchar text grid
    uint8_t tbig, trow, tcol;

    // files
    LavaFile files[LAVA_MAX_FILES];
    int nfiles;
    char dirs[LAVA_MAX_DIRS][LAVA_NAME_MAX];
    int ndirs;
    char cwd[LAVA_NAME_MAX];
    LavaHandle fp[3];
    uint32_t files_gen;         // bumped whenever the file table changes

    // screen dirty tracking for the frontend: rows of the LCD written since cleared
    uint8_t lcd_dirty;

    const uint8_t* code;
    uint32_t code_len;
    LavaFonts fonts;
    LavaHost host;
    char errmsg[96];

    // instrumentation (tools only): called for key comparisons and CheckKey
    void (*key_probe)(struct LavaVM* vm, int kind, int value);
    int32_t last_key;           // value returned by the last getchar/Inkey/GetWord/CheckKey(128)
    int32_t sys_since_key;      // system calls since then
    LavaReadSite reads[LAVA_READ_SITES];
    int32_t checked[128];       // frame + 1 when CheckKey(k) last tested k
    int32_t classified;         // frame + 1 when isalpha/isdigit/... last tested a key just read

    int32_t stack_base[16];     // underflow slack
    int32_t stack[LAVA_STACK_MAX];
    uint8_t mem[LAVA_MEM_ALLOC];
} LavaVM;

// Allocation hooks (the frontend points these at the Playdate heap).
extern void* (*lava_realloc)(void* p, size_t n);

// Initialises vm (caller allocates sizeof(LavaVM)); `code` must outlive the VM.
int lava_init(LavaVM* vm, const uint8_t* code, uint32_t len, const LavaFonts* fonts);
void lava_free(LavaVM* vm);
void lava_reset(LavaVM* vm);

// Adds (or replaces) a file in the virtual file system. `dirty` marks it as a save.
int lava_add_file(LavaVM* vm, const char* name, const uint8_t* data, uint32_t len, int dirty);
LavaFile* lava_find_file(LavaVM* vm, const char* name);
void lava_free_file(LavaVM* vm, int index);

// One 1/60 s frame of virtual time.
void lava_run_frame(LavaVM* vm);
void lava_run_until(LavaVM* vm, int64_t end_us);
// A game's pace: virtual microseconds an instruction (4 = lavaemu's default,
// a fast PC emulator; 27 an NC3000, 19 a TC800, 57 an NC2600, 75 an NC1020).
void lava_set_pace(LavaVM* vm, uint32_t us_per_op);
// LeeSoft's default 256-colour palette (R, G, B)
void lavax_default_palette(uint8_t pal[256][3]);

void lava_key_down(LavaVM* vm, int code);
// keep_latch: a released key stays latched until the program reads it (the
// frontend's tap-safe input); lavaemu's key_up clears it (keep_latch = 0).
void lava_key_up(LavaVM* vm, int code, int keep_latch);
void lava_release_all(LavaVM* vm);

// Save states: size, write into buf (returns bytes), read back (returns 0 on error).
uint32_t lava_state_size(const LavaVM* vm);
uint32_t lava_state_save(const LavaVM* vm, uint8_t* buf, uint32_t cap);
int lava_state_load(LavaVM* vm, const uint8_t* buf, uint32_t len);

static inline const uint8_t* lava_lcd(const LavaVM* vm) { return vm->mem + LAVA_GRAPH; }

#endif
