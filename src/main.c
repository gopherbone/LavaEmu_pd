// LavaEmu for Playdate: game list, emulation loop, input (buttons, B-chords,
// the crank key palette, an on-screen keyboard), options, save states.
//
// Files live in the game's Data folder:
//   Games/<Folder>/{game.txt, *.lav, LavaData/*}   games you add (Bundled/ in the pdx has the same layout)
//   Saves/<Folder>/LavaData/*                      files the games wrote (their own save files)
//   States/<Folder>/<program>-slot<N>.state        save states
//   Config/<Folder>.txt, settings.txt

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pd_api.h"
#include "lava.h"
#include "profiles.h"

static PlaydateAPI* pd;
static LCDFont* font;        // Asheville Sans 14 Bold
static LCDFont* small_font;  // Roobert 10 Bold

#define REFRESH_RATE 30
#define STATE_SLOTS 3
#define MAX_ENTRIES 128
#define LCD_X 40             // the 160x80 screen at 2x: 320x160
#define LCD_Y 40
#define LCD_Y_KEYBOARD 4     // moved up while the keyboard panel is open
#define TAP_MAX_FRAMES 30    // a released key stays down until the program sees it, at most this long
#define REPEAT_DELAY 18      // VM frames (60 Hz) before a held arrow repeats
#define REPEAT_INTERVAL 6
#define CRANK_STEP 24.0f     // degrees per palette item

typedef enum { SCREEN_PICKER, SCREEN_GAME, SCREEN_OPTIONS, SCREEN_KEYS, SCREEN_CREDITS, SCREEN_MESSAGE } Screen;

static Screen screen = SCREEN_PICKER;
static int needs_redraw = 1;

// MARK: Files

static void* pd_realloc(void* p, size_t n) { return pd->system->realloc(p, n); }

static void free_buf(void* p) {
    if (p) pd->system->realloc(p, 0);
}

static uint8_t* read_file(const char* path, size_t* len) {
    FileStat st;
    if (pd->file->stat(path, &st) != 0 || st.isdir) return NULL;
    SDFile* f = pd->file->open(path, kFileReadData | kFileRead);
    if (!f) return NULL;
    uint8_t* buf = pd->system->realloc(NULL, st.size ? st.size : 1);
    if (!buf) {
        pd->file->close(f);
        return NULL;
    }
    size_t got = 0;
    while (got < st.size) {
        int n = pd->file->read(f, buf + got, st.size - got);
        if (n <= 0) break;
        got += n;
    }
    pd->file->close(f);
    if (got != st.size) {
        free_buf(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

// Writes via a temporary file so a crash mid-write never leaves a torn save.
static int write_file(const char* path, const uint8_t* data, size_t len) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    SDFile* f = pd->file->open(tmp, kFileWrite);
    if (!f) return 0;
    size_t put = 0;
    while (put < len) {
        int n = pd->file->write(f, data + put, len - put);
        if (n <= 0) break;
        put += n;
    }
    pd->file->close(f);
    if (put != len) {
        pd->file->unlink(tmp, 0);
        return 0;
    }
    pd->file->unlink(path, 0);
    return pd->file->rename(tmp, path) == 0;
}

// Parses "key=value" lines.
static void read_kv(const char* path, void (*apply)(const char* key, const char* value, void* ud), void* ud) {
    size_t len;
    char* text = (char*)read_file(path, &len);
    if (!text) return;
    char* end = text + len;
    char* line = text;
    while (line < end) {
        char* nl = memchr(line, '\n', end - line);
        if (!nl) nl = end;
        char* eq = memchr(line, '=', nl - line);
        if (eq) {
            char key[32], value[300];
            size_t klen = eq - line, vlen = nl - eq - 1;
            if (vlen && eq[vlen] == '\r') vlen--;
            if (klen < sizeof key && vlen < sizeof value) {
                memcpy(key, line, klen);
                key[klen] = 0;
                memcpy(value, eq + 1, vlen);
                value[vlen] = 0;
                apply(key, value, ud);
            }
        }
        line = nl + 1;
    }
    free_buf(text);
}

// mkdir -p for the directories of `path`.
static void mkdirs_for(const char* path) {
    char d[300];
    snprintf(d, sizeof d, "%s", path);
    for (char* p = d + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            pd->file->mkdir(d);
            *p = '/';
        }
    }
}

// VM path bytes (GB2312 possible) -> a FAT-safe relative path: other bytes as %XX.
static void escape_path(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && n + 4 < cap; p++) {
        unsigned c = *p;
        if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == '/') out[n++] = (char)c;
        else n += snprintf(out + n, cap - n, "%%%02X", c);
    }
    out[n] = 0;
}

static void unescape_path(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p && n + 1 < cap; p++) {
        unsigned v;
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && sscanf(p + 1, "%2x", &v) == 1) {
            out[n++] = (char)v;
            p += 2;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = 0;
}

// MARK: Settings

enum { BORDER_WHITE, BORDER_BLACK, BORDER_DEVICE, BORDER_COUNT };

typedef struct {
    int border;
    int show_perf;
    int speed;          // 1, 2 or 4
} Settings;

static Settings settings = {BORDER_WHITE, 0, 1};

static void apply_setting(const char* key, const char* value, void* ud) {
    (void)ud;
    int v = atoi(value);
    if (!strcmp(key, "border")) settings.border = v >= 0 && v < BORDER_COUNT ? v : BORDER_WHITE;
    else if (!strcmp(key, "show_perf")) settings.show_perf = v != 0;
    else if (!strcmp(key, "speed")) settings.speed = v == 2 || v == 4 ? v : 1;
}

static void save_settings(void) {
    char buf[96];
    int n = snprintf(buf, sizeof buf, "border=%d\nshow_perf=%d\nspeed=%d\n", settings.border, settings.show_perf,
                     settings.speed);
    write_file("settings.txt", (uint8_t*)buf, n);
}

// MARK: Fonts (GVmakerSE's, also used to draw Chinese titles)

static LavaFonts vm_fonts;

static void load_vm_fonts(void) {
    if (vm_fonts.gb16) return;
    size_t n;
    vm_fonts.gb12 = read_file("fonts/gbfont.bin", &n), vm_fonts.gb12_len = vm_fonts.gb12 ? (uint32_t)n : 0;
    vm_fonts.gb16 = read_file("fonts/gbfont16.bin", &n), vm_fonts.gb16_len = vm_fonts.gb16 ? (uint32_t)n : 0;
    vm_fonts.asc12 = read_file("fonts/ascii.bin", &n), vm_fonts.asc12_len = vm_fonts.asc12 ? (uint32_t)n : 0;
    vm_fonts.asc16 = read_file("fonts/ascii8.bin", &n), vm_fonts.asc16_len = vm_fonts.asc16 ? (uint32_t)n : 0;
}

// Draws GB2312 bytes with the 16-px fonts; returns the width.
static int draw_gb(const uint8_t* s, int len, int x, int y, int white) {
    uint8_t* frame = pd->graphics->getFrame();
    int x0 = x;
    for (int i = 0; i < len;) {
        int c = s[i++];
        const uint8_t* g = NULL;
        int w = 8, bpr = 1;
        if (c >= 0x80 && i < len) {
            int c2 = s[i++];
            int hi = c - 0xA1;
            if (hi > 8) hi -= 6;
            long off = ((long)hi * 94 + c2 - 0xA1) * 32;
            if (vm_fonts.gb16 && off >= 0 && off + 32 <= (long)vm_fonts.gb16_len) g = vm_fonts.gb16 + off;
            w = 16, bpr = 2;
        } else if (vm_fonts.asc16 && (c + 1) * 16 <= (int)vm_fonts.asc16_len) {
            g = vm_fonts.asc16 + c * 16;
        }
        if (g) {
            for (int r = 0; r < 16; r++) {
                int py = y + r;
                if (py < 0 || py >= LCD_ROWS) continue;
                unsigned bits = bpr == 2 ? (g[2 * r] << 8 | g[2 * r + 1]) : (unsigned)g[r] << 8;
                for (int cx = 0; cx < w; cx++) {
                    int px = x + cx;
                    if (!(bits >> (15 - cx) & 1) || px < 0 || px >= LCD_COLUMNS) continue;
                    uint8_t* b = &frame[py * LCD_ROWSIZE + px / 8];
                    uint8_t m = 0x80 >> (px & 7);
                    *b = white ? (*b | m) : (*b & ~m);
                }
            }
        }
        x += w;
    }
    pd->graphics->markUpdatedRows(y < 0 ? 0 : y, y + 15 >= LCD_ROWS ? LCD_ROWS - 1 : y + 15);
    return x - x0;
}

// MARK: Text helpers

// drawText and getTextWidth take a length in characters, not bytes.
static int utf8_chars(const char* s, size_t bytes) {
    int n = 0;
    for (size_t i = 0; i < bytes && s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

static void text(LCDFont* f, const char* s, int x, int y) {
    pd->graphics->setFont(f);
    pd->graphics->drawText(s, utf8_chars(s, strlen(s)), kUTF8Encoding, x, y);
}

static int text_w(LCDFont* f, const char* s) {
    return pd->graphics->getTextWidth(f, s, utf8_chars(s, strlen(s)), kUTF8Encoding, 0);
}

// Word-wrapped text; returns the y after the last line.
static int draw_wrapped(LCDFont* f, const char* s, int x, int y, int width, int draw) {
    int line_h = pd->graphics->getFontHeight(f) + 3;
    pd->graphics->setFont(f);
    const char* p = s;
    while (*p) {
        const char* line_end = p;
        const char* best = NULL;
        while (*line_end && *line_end != '\n') {
            const char* next = line_end;
            while (*next && *next != ' ' && *next != '\n') next++;
            if (pd->graphics->getTextWidth(f, p, utf8_chars(p, next - p), kUTF8Encoding, 0) > width && best) break;
            best = next;
            line_end = next;
            if (*line_end == ' ') line_end++;
        }
        if (!best) best = line_end;
        if (draw && y > -line_h && y < LCD_ROWS) pd->graphics->drawText(p, utf8_chars(p, best - p), kUTF8Encoding, x, y);
        y += line_h;
        p = best;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    return y;
}

// MARK: Message screen

static char message_title[64];
static char message_body[512];
static Screen message_return = SCREEN_PICKER;

static void show_message(const char* title, const char* body, Screen back) {
    snprintf(message_title, sizeof message_title, "%s", title);
    snprintf(message_body, sizeof message_body, "%s", body);
    message_return = back;
    screen = SCREEN_MESSAGE;
    needs_redraw = 1;
}

static void message_update(void) {
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    if (pushed & (kButtonA | kButtonB)) {
        screen = message_return;
        needs_redraw = 1;
        return;
    }
    if (!needs_redraw) return;
    needs_redraw = 0;
    pd->graphics->clear(kColorWhite);
    text(font, message_title, 16, 14);
    pd->graphics->drawLine(16, 36, 384, 36, 1, kColorBlack);
    draw_wrapped(small_font, message_body, 16, 46, 368, 1);
    text(small_font, "Ⓐ OK", 16, 218);
}

// MARK: Game list

typedef struct {
    char folder[64];        // directory name
    char base[16];          // "Bundled" (in the pdx) or "Games" (Data folder)
    char title[80];
    uint8_t title_gb[48];
    int title_gb_len;
    char program[64];       // .lav file in the folder
    char label[64];         // second program's purpose ("Register an account (first)")
    char profile[16];
    char credit[300];
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count;
static int picker_selected;
static float picker_crank;

typedef struct {
    Entry e;
    int programs;
    char progs[4][64];
    char labels[4][64];
} GameTxt;

static void apply_game_txt(const char* key, const char* value, void* ud) {
    GameTxt* g = ud;
    if (!strcmp(key, "title")) snprintf(g->e.title, sizeof g->e.title, "%s", value);
    else if (!strcmp(key, "profile")) snprintf(g->e.profile, sizeof g->e.profile, "%s", value);
    else if (!strcmp(key, "credit")) snprintf(g->e.credit, sizeof g->e.credit, "%s", value);
    else if (!strcmp(key, "title_gb")) {
        int n = 0;
        for (const char* p = value; p[0] && p[1] && n < (int)sizeof g->e.title_gb; p += 2) {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1) break;
            g->e.title_gb[n++] = (uint8_t)v;
        }
        g->e.title_gb_len = n;
    } else if (!strcmp(key, "program") && g->programs < 4) {
        char* bar = strchr(value, '|');
        if (bar) *bar = 0;
        snprintf(g->progs[g->programs], 64, "%s", value);
        snprintf(g->labels[g->programs], 64, "%s", bar ? bar + 1 : "");
        g->programs++;
    }
}

static int ends_with_ci(const char* s, const char* ext) {
    size_t n = strlen(s), m = strlen(ext);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)ext[i])) return 0;
    return 1;
}

static GameTxt* scan_tmp;
static void collect_lav(const char* name, void* ud) {
    (void)ud;
    if (ends_with_ci(name, ".lav") && scan_tmp->programs < 4) {
        snprintf(scan_tmp->progs[scan_tmp->programs], 64, "%s", name);
        scan_tmp->labels[scan_tmp->programs][0] = 0;
        scan_tmp->programs++;
    }
}

static const char* scan_base;
static void collect_folder(const char* name, void* ud) {
    (void)ud;
    size_t n = strlen(name);
    if (name[0] == '.' || n < 2 || name[n - 1] != '/' || entry_count >= MAX_ENTRIES) return;
    char folder[64];
    snprintf(folder, sizeof folder, "%.*s", (int)(n - 1), name);
    // a folder in Games/ hides a bundled one with the same name
    for (int i = 0; i < entry_count; i++)
        if (!strcmp(entries[i].folder, folder)) return;
    static GameTxt g;
    memset(&g, 0, sizeof g);
    snprintf(g.e.title, sizeof g.e.title, "%s", folder);
    char path[200];
    snprintf(path, sizeof path, "%s/%s/game.txt", scan_base, folder);
    read_kv(path, apply_game_txt, &g);
    if (!g.programs) {
        scan_tmp = &g;
        snprintf(path, sizeof path, "%s/%s", scan_base, folder);
        pd->file->listfiles(path, collect_lav, NULL, 0);
    }
    for (int i = 0; i < g.programs && entry_count < MAX_ENTRIES; i++) {
        Entry* e = &entries[entry_count++];
        *e = g.e;
        snprintf(e->folder, sizeof e->folder, "%s", folder);
        snprintf(e->base, sizeof e->base, "%s", scan_base);
        snprintf(e->program, sizeof e->program, "%s", g.progs[i]);
        snprintf(e->label, sizeof e->label, "%s", g.labels[i]);
        if (g.programs > 1 && !g.labels[i][0] && i > 0) snprintf(e->label, sizeof e->label, "%s", g.progs[i]);
    }
}

static int compare_entries(const void* a, const void* b) {
    const Entry* x = a;
    const Entry* y = b;
    // bundled first, then by title; programs of one game keep their order
    int bx = strcmp(x->base, "Bundled") != 0, by = strcmp(y->base, "Bundled") != 0;
    if (bx != by) return bx - by;
    int zx = strstr(x->folder, "-zh") != NULL, zy = strstr(y->folder, "-zh") != NULL;
    if (zx != zy) return zx - zy;
    int c = strcmp(x->title, y->title);
    if (c) return c;
    return (x->label[0] != 0) - (y->label[0] != 0);
}

static void scan_games(void) {
    entry_count = 0;
    scan_base = "Games";
    pd->file->listfiles("Games", collect_folder, NULL, 0);
    scan_base = "Bundled";
    pd->file->listfiles("Bundled", collect_folder, NULL, 0);
    qsort(entries, entry_count, sizeof entries[0], compare_entries);
    if (picker_selected > entry_count) picker_selected = entry_count;   // entry_count = the Credits row
}

// MARK: Game session

static LavaVM* vm;
static uint8_t* program;
static Entry game;
static const Profile* profile;
static Profile auto_profile;
static int state_slot = 1;

static float frame_cost_ms, render_ms;
static float ops_per_frame;
static int frame_acc;               // thousandths of a VM frame
static unsigned int last_ms;
static int game_frames;
static float bench_ms, bench_result;
static int toast_frames;
static char toast_text[64];

static void toast(const char* t) {
    snprintf(toast_text, sizeof toast_text, "%s", t);
    toast_frames = REFRESH_RATE * 2;
}

static void save_dir(char* out, size_t cap) { snprintf(out, cap, "Saves/%s", game.folder); }

// VM file callbacks: games' own saves go to Saves/<Folder>/.
static void on_file_written(void* ud, const char* name, const uint8_t* data, uint32_t len) {
    (void)ud;
    char rel[200], path[300], dir[100];
    escape_path(name[0] == '/' ? name + 1 : name, rel, sizeof rel);
    save_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    mkdirs_for(path);
    if (!write_file(path, data, len)) toast("Couldn't write a save file");
}

static void on_file_deleted(void* ud, const char* name) {
    (void)ud;
    char rel[200], path[300], dir[100];
    escape_path(name[0] == '/' ? name + 1 : name, rel, sizeof rel);
    save_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    pd->file->unlink(path, 0);
    // remember deletions of bundled files
    snprintf(path, sizeof path, "%s/deleted.txt", dir);
    size_t len = 0;
    uint8_t* old = read_file(path, &len);
    size_t nl = strlen(name);
    uint8_t* buf = pd->system->realloc(NULL, len + nl + 2);
    if (old) memcpy(buf, old, len);
    memcpy(buf + len, name, nl);
    buf[len + nl] = '\n';
    mkdirs_for(path);
    write_file(path, buf, len + nl + 1);
    free_buf(old);
    free_buf(buf);
}

static void on_get_time(void* ud, int out[7]) {
    (void)ud;
    unsigned int ms;
    uint32_t secs = pd->system->getSecondsSinceEpoch(&ms);
    secs += pd->system->getTimezoneOffset();
    struct PDDateTime dt;
    pd->system->convertEpochToDateTime(secs, &dt);
    out[0] = dt.year, out[1] = dt.month, out[2] = dt.day, out[3] = dt.hour, out[4] = dt.minute, out[5] = dt.second;
    out[6] = dt.weekday % 7;
}

// Mounting: files under the game folder. LavaData/x -> /LavaData/x, other
// subfolders likewise; loose files (not .lav/.txt) -> /LavaData/.
static char mount_root[160];
static void mount_dir(const char* rel);
static char mount_rel[160];

static void mount_cb(const char* name, void* ud) {
    (void)ud;
    if (name[0] == '.') return;
    char rel[200];
    snprintf(rel, sizeof rel, "%s%s", mount_rel, name);
    size_t n = strlen(name);
    if (name[n - 1] == '/') {
        char saved[160];
        snprintf(saved, sizeof saved, "%s", mount_rel);
        mount_dir(rel);
        snprintf(mount_rel, sizeof mount_rel, "%s", saved);
        return;
    }
    if (!mount_rel[0] && (ends_with_ci(name, ".lav") || ends_with_ci(name, ".txt") || ends_with_ci(name, ".pac"))) return;
    char path[300], vmname[LAVA_NAME_MAX];
    snprintf(path, sizeof path, "%s/%s", mount_root, rel);
    size_t len;
    uint8_t* data = read_file(path, &len);
    if (!data) return;
    if (mount_rel[0]) snprintf(vmname, sizeof vmname, "/%s", rel);
    else snprintf(vmname, sizeof vmname, "/LavaData/%s", rel);
    lava_add_file(vm, vmname, data, (uint32_t)len, 0);
    free_buf(data);
}

static void mount_dir(const char* rel) {
    snprintf(mount_rel, sizeof mount_rel, "%s", rel);
    char path[300];
    snprintf(path, sizeof path, "%s/%s", mount_root, rel);
    pd->file->listfiles(path, mount_cb, NULL, 0);
}

// Saves: Saves/<Folder>/<escaped vm path>
static char saves_rel[160];
static void saves_dir(const char* rel);

static void saves_cb(const char* name, void* ud) {
    (void)ud;
    if (name[0] == '.' || ends_with_ci(name, ".tmp")) return;
    char rel[200];
    snprintf(rel, sizeof rel, "%s%s", saves_rel, name);
    size_t n = strlen(name);
    if (name[n - 1] == '/') {
        char saved[160];
        snprintf(saved, sizeof saved, "%s", saves_rel);
        saves_dir(rel);
        snprintf(saves_rel, sizeof saves_rel, "%s", saved);
        return;
    }
    if (!saves_rel[0] && !strcmp(name, "deleted.txt")) return;
    char path[300], dir[100], vmname[LAVA_NAME_MAX + 1];
    save_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    size_t len;
    uint8_t* data = read_file(path, &len);
    if (!data) return;
    vmname[0] = '/';
    unescape_path(rel, vmname + 1, sizeof vmname - 1);
    lava_add_file(vm, vmname, data, (uint32_t)len, 1);
    free_buf(data);
}

static void saves_dir(const char* rel) {
    snprintf(saves_rel, sizeof saves_rel, "%s", rel);
    char path[300], dir[100];
    save_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    pd->file->listfiles(path, saves_cb, NULL, 0);
}

static void apply_deleted(void) {
    char path[300], dir[100];
    save_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/deleted.txt", dir);
    size_t len;
    char* t = (char*)read_file(path, &len);
    if (!t) return;
    char* end = t + len;
    for (char* line = t; line < end;) {
        char* nl = memchr(line, '\n', end - line);
        if (!nl) nl = end;
        *nl = 0;
        LavaFile* f = lava_find_file(vm, line);
        // still deleted unless the game wrote it again (a save file overrides)
        if (f && !f->dirty) {
            int i = (int)(f - vm->files);
            lava_free_file(vm, i);
        }
        line = nl + 1;
    }
    free_buf(t);
}

static void apply_config(const char* key, const char* value, void* ud) {
    (void)ud;
    if (!strcmp(key, "slot")) {
        int v = atoi(value);
        state_slot = v < 1 ? 1 : v > STATE_SLOTS ? STATE_SLOTS : v;
    }
}

static void save_config(void) {
    char path[200], buf[32];
    snprintf(path, sizeof path, "Config/%s.txt", game.folder);
    int n = snprintf(buf, sizeof buf, "slot=%d\n", state_slot);
    write_file(path, (uint8_t*)buf, n);
}

// MARK: Input state

typedef struct {
    uint8_t code;
    uint8_t phys;           // the button (or palette/keyboard press) is still down
    uint8_t active;
    uint16_t frames;        // VM frames since pressed
    uint16_t repeat;
} KeySlot;

static KeySlot slots[8];

static void key_press(int code) {
    if (!vm || !code) return;
    for (int i = 0; i < 8; i++) {
        if (slots[i].active && slots[i].code == code) {
            slots[i].phys = 1;
            slots[i].frames = 0;
            vm->seen[code] = 0;
            lava_key_down(vm, code);
            return;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!slots[i].active) {
            slots[i] = (KeySlot){(uint8_t)code, 1, 1, 0, 0};
            vm->seen[code] = 0;
            lava_key_down(vm, code);
            return;
        }
    }
}

static void key_release(int code) {
    for (int i = 0; i < 8; i++)
        if (slots[i].active && slots[i].code == code) slots[i].phys = 0;
}

static void release_all_keys(void) {
    memset(slots, 0, sizeof slots);
    if (vm) lava_release_all(vm);
}

static int is_arrow(int c) { return c >= LK_UP && c <= LK_LEFT; }

// Before each VM frame: released keys come up once the program has seen them
// (or after TAP_MAX_FRAMES); held arrows repeat.
static void tick_keys(void) {
    for (int i = 0; i < 8; i++) {
        KeySlot* s = &slots[i];
        if (!s->active) continue;
        s->frames++;
        if (!s->phys) {
            if (vm->seen[s->code] || s->frames >= TAP_MAX_FRAMES) {
                lava_key_up(vm, s->code, 0);
                s->active = 0;
            }
        } else if (is_arrow(s->code) && s->frames >= REPEAT_DELAY) {
            if (++s->repeat >= REPEAT_INTERVAL) {
                s->repeat = 0;
                lava_key_down(vm, s->code);
            }
        }
    }
}

// Buttons, the palette and the keyboard.
static int palette_open;          // crank undocked
static int palette_sel;           // 0 = Enter (home)
static float palette_crank;
static int palette_pressed = -1;  // item held down with A
static int keyboard_open;
static int kb_row, kb_col;
static int kb_pressed;            // key held with A on the keyboard
static int b_down, b_chorded;
static int chord_code[4];         // key sent by each held direction while B is down
static int dpad_code[4];          // key sent by each held direction
static int a_code;
static int chrome_dirty;          // border bands need redrawing

#define PALETTE_EXTRA 2           // Enter (home) and Keyboard
static int palette_count(void) { return profile->npalette + PALETTE_EXTRA; }
static int palette_code(int i) {
    if (i == 0) return LK_ENTER;
    if (i <= profile->npalette) return profile->palette[i - 1].code;
    return -1;     // Keyboard
}
static const char* palette_label(int i) {
    if (i == 0) return "Enter";
    if (i <= profile->npalette) {
        const char* l = profile->palette[i - 1].label;
        return l ? l : key_name(profile->palette[i - 1].code);
    }
    return "Keyboard";
}

// The on-screen keyboard: every LAVA key.
typedef struct {
    uint8_t code;
    const char* label;
    uint8_t span;
} KbKey;

#define KB_ROWS 5
#define KB_COLS 11
static const KbKey kb[KB_ROWS][KB_COLS] = {
    {{LK_F1, "F1", 1}, {LK_F2, "F2", 1}, {LK_F3, "F3", 1}, {LK_F4, "F4", 1}, {LK_HELP, "Help", 1}, {LK_PGUP, "PgUp", 1},
     {LK_PGDN, "PgDn", 1}, {LK_SHIFT, "Shift", 1}, {LK_CAPS, "Caps", 1}, {LK_ESC, "Esc", 1}, {LK_ENTER, "Enter", 1}},
    {{'1', "1", 1}, {'2', "2", 1}, {'3', "3", 1}, {'4', "4", 1}, {'5', "5", 1}, {'6', "6", 1}, {'7', "7", 1},
     {'8', "8", 1}, {'9', "9", 1}, {'0', "0", 1}, {'.', ".", 1}},
    {{'q', "Q", 1}, {'w', "W", 1}, {'e', "E", 1}, {'r', "R", 1}, {'t', "T", 1}, {'y', "Y", 1}, {'u', "U", 1},
     {'i', "I", 1}, {'o', "O", 1}, {'p', "P", 1}, {LK_UP, "Up", 1}},
    {{'a', "A", 1}, {'s', "S", 1}, {'d', "D", 1}, {'f', "F", 1}, {'g', "G", 1}, {'h', "H", 1}, {'j', "J", 1},
     {'k', "K", 1}, {'l', "L", 1}, {LK_LEFT, "Left", 1}, {LK_RIGHT, "Right", 1}},
    {{'z', "Z", 1}, {'x', "X", 1}, {'c', "C", 1}, {'v', "V", 1}, {'b', "B", 1}, {'n', "N", 1}, {'m', "M", 1},
     {LK_SPACE, "Space", 1}, {0, "", 0}, {LK_DOWN, "Down", 1}, {0, "Close", 1}},
};

static void game_add_menu_items(void);

static void open_keyboard(int open) {
    keyboard_open = open;
    kb_pressed = 0;
    needs_redraw = 1;
}

// MARK: Rendering

static uint8_t shown[LAVA_SCREEN_BYTES];
static int shown_valid;
static uint16_t dbl[256];   // a byte's 8 pixels doubled to 16, inverted (LCD 1 = black, frame 1 = white)

static void init_dbl(void) {
    for (int b = 0; b < 256; b++) {
        uint16_t v = 0;
        for (int i = 0; i < 8; i++)
            if (b & (0x80 >> i)) v |= 0xC000 >> (2 * i);
        dbl[b] = (uint16_t)~v;
    }
}

static int lcd_y(void) { return keyboard_open ? LCD_Y_KEYBOARD : LCD_Y; }

// Draws rows of the VM's LCD that changed since the last call.
static void render_lcd(void) {
    const uint8_t* lcd = lava_lcd(vm);
    uint8_t* frame = pd->graphics->getFrame();
    int y0 = lcd_y();
    int first = -1, last = -1;
    for (int y = 0; y < LAVA_H; y++) {
        const uint8_t* src = lcd + y * LAVA_BPL;
        if (shown_valid && !memcmp(src, shown + y * LAVA_BPL, LAVA_BPL)) continue;
        memcpy(shown + y * LAVA_BPL, src, LAVA_BPL);
        uint8_t* d = frame + (y0 + 2 * y) * LCD_ROWSIZE + LCD_X / 8;
        for (int i = 0; i < LAVA_BPL; i++) {
            uint16_t v = dbl[src[i]];
            d[2 * i] = (uint8_t)(v >> 8);
            d[2 * i + 1] = (uint8_t)v;
        }
        memcpy(d + LCD_ROWSIZE, d, LAVA_BPL * 2);
        if (first < 0) first = y;
        last = y;
    }
    shown_valid = 1;
    if (first >= 0) pd->graphics->markUpdatedRows(y0 + 2 * first, y0 + 2 * last + 1);
}

static LCDPattern bezel_pattern = {
    0xEE, 0xFF, 0xBB, 0xFF, 0xEE, 0xFF, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static int dark_border(void) { return settings.border != BORDER_WHITE; }

// Border around the LCD: white, black, or a Wenquxing-style bezel.
static void draw_chrome(void) {
    int y0 = lcd_y();
    if (settings.border == BORDER_DEVICE && !keyboard_open) {
        pd->graphics->clear(kColorBlack);
        pd->graphics->fillRoundRect(14, 3, 372, 234, 12, (LCDColor)bezel_pattern);
        pd->graphics->drawRoundRect(14, 3, 372, 234, 12, 1, kColorBlack);
        pd->graphics->fillRoundRect(LCD_X - 8, y0 - 8, 320 + 16, 160 + 16, 4, kColorBlack);
        // 文曲星 in the VM's own font, then "LAVA"
        static const uint8_t wqx[] = {0xCE, 0xC4, 0xC7, 0xFA, 0xD0, 0xC7};
        int w = 48 + 8 + text_w(small_font, "LAVA");
        draw_gb(wqx, 6, 200 - w / 2, 212, 0);
        text(small_font, "LAVA", 200 - w / 2 + 56, 214);
    } else {
        pd->graphics->clear(dark_border() ? kColorBlack : kColorWhite);
        pd->graphics->drawRect(LCD_X - 3, y0 - 3, 320 + 6, 160 + 6, dark_border() ? kColorWhite : kColorBlack);
    }
    shown_valid = 0;
}

// Top band: toast, performance, or the chord hints while B is held.
static void draw_top_band(void) {
    char t[128] = "";
    if (b_down) {
        static const char* arrows[4] = {"▲", "▶", "▼", "◀"};
        int n = 0;
        for (int d = 0; d < 4; d++) {
            const ProfileKey* k = &profile->chord[d];
            if (!k->code) continue;
            n += snprintf(t + n, sizeof t - n, "%s%s %s", n ? "   " : "B+", arrows[d], k->label ? k->label : key_name(k->code));
        }
        if (!n) snprintf(t, sizeof t, "Release B for Esc");
    } else if (toast_frames > 0) {
        snprintf(t, sizeof t, "%s", toast_text);
    } else if (settings.show_perf) {
        int load = (int)(frame_cost_ms * 60.0f / 10.0f + 0.5f);
        int n = snprintf(t, sizeof t, "VM %d.%02d ms/f (%d%%)  %d op/f  draw %d.%d ms  %dfps", (int)frame_cost_ms,
                         (int)(frame_cost_ms * 100) % 100, load, (int)ops_per_frame, (int)render_ms,
                         (int)(render_ms * 10) % 10, (int)(pd->display->getFPS() + 0.5f));
        if (bench_result > 0)
            snprintf(t + n, sizeof t - n, "  bench %d.%02d", (int)bench_result, (int)(bench_result * 100) % 100);
    }
    int dark = dark_border();
    if (settings.border == BORDER_DEVICE && !keyboard_open) dark = 0;
    int y = keyboard_open ? -100 : 8;   // no top band while the keyboard is open
    if (y < 0) return;
    pd->graphics->fillRect(LCD_X - 4, 4, 328, 22, settings.border == BORDER_DEVICE ? (LCDColor)bezel_pattern
                                                                                    : (dark ? kColorBlack : kColorWhite));
    if (!t[0]) return;
    int w = text_w(small_font, t);
    if (settings.border == BORDER_DEVICE) {
        pd->graphics->fillRect(200 - w / 2 - 4, y - 2, w + 8, 18, kColorWhite);
    }
    if (dark) pd->graphics->setDrawMode(kDrawModeFillWhite);
    text(small_font, t, 200 - w / 2, y);
    pd->graphics->setDrawMode(kDrawModeCopy);
}

// Bottom band: the crank palette, a reel of the game's keys; the selected
// one in the middle. Selection 0 is Enter, where it rests.
static void draw_palette(void) {
    int y = 207, h = 31;
    LCDColor bg = settings.border == BORDER_DEVICE ? (LCDColor)bezel_pattern : (dark_border() ? kColorBlack : kColorWhite);
    pd->graphics->fillRect(0, y - 2, 400, 240 - (y - 2), bg);
    if (!palette_open) {
        if (settings.border == BORDER_DEVICE) draw_chrome();
        return;
    }
    int n = palette_count();
    int cw = 98, gap = 6;
    int cx = 200 - cw / 2;
    int inv_bg = dark_border() && settings.border != BORDER_DEVICE;
    // A reel without wrap-around: Enter (home) in the middle when the crank
    // rests, the game's keys to its right, Keyboard last.
    for (int k = -2; k <= 2; k++) {
        int i = palette_sel + k;
        if (i < 0 || i >= n) continue;
        int x = cx + k * (cw + gap);
        int sel = k == 0;
        const char* label = palette_label(i);
        int code = palette_code(i);
        LCDColor fg = inv_bg ? kColorWhite : kColorBlack, bgc = inv_bg ? kColorBlack : kColorWhite;
        pd->graphics->fillRoundRect(x, y, cw, h, 5, sel ? fg : bgc);
        if (!sel) pd->graphics->drawRoundRect(x, y, cw, h, 5, 1, fg);
        if (sel != inv_bg) pd->graphics->setDrawMode(kDrawModeFillWhite);
        pd->graphics->setClipRect(x + 2, y, cw - 4, h);
        const char* kn = code == LK_ENTER ? "Ⓐ" : code > 0 ? key_name(code) : "every key";
        int two = strcmp(kn, label) != 0;
        int w = text_w(small_font, label);
        text(small_font, label, x + (cw - w) / 2, two ? y + 2 : y + 9);
        if (two) {
            int kw = text_w(small_font, kn);
            text(small_font, kn, x + (cw - kw) / 2, y + 16);
        }
        pd->graphics->clearClipRect();
        pd->graphics->setDrawMode(kDrawModeCopy);
        if (palette_pressed == i && sel) pd->graphics->drawRoundRect(x - 2, y - 2, cw + 4, h + 4, 6, 2, kColorXOR);
    }
    // where the reel can go: little marks at the ends
    if (palette_sel > 2) text(small_font, "◀", 4, y + 9);
    if (palette_sel + 2 < n - 1) text(small_font, "▶", 384, y + 9);
}

// Keyboard panel under the moved-up LCD.
static void draw_keyboard(void) {
    int y0 = 170, rh = 14, cw = 36, x0 = 2;
    pd->graphics->fillRect(0, y0 - 2, 400, 240 - y0 + 2, kColorWhite);
    pd->graphics->drawLine(0, y0 - 3, 400, y0 - 3, 1, kColorBlack);
    for (int r = 0; r < KB_ROWS; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            const KbKey* k = &kb[r][c];
            if (!k->span) continue;
            int x = x0 + c * cw, y = y0 + r * rh;
            int sel = r == kb_row && c == kb_col;
            if (sel) {
                pd->graphics->fillRect(x + 1, y, cw - 2, rh - 1, kColorBlack);
                pd->graphics->setDrawMode(kDrawModeFillWhite);
            }
            int w = text_w(small_font, k->label);
            text(small_font, k->label, x + (cw - w) / 2, y);
            pd->graphics->setDrawMode(kDrawModeCopy);
        }
    }
}

// MARK: Game start/stop

static void end_game(void) {
    if (!vm) return;
    lava_free(vm);
    free_buf(vm);
    vm = NULL;
    free_buf(program);
    program = NULL;
    pd->system->removeAllMenuItems();
    release_all_keys();
}

static void state_path(char* out, size_t cap, int slot) {
    snprintf(out, cap, "States/%s/%s-slot%d.state", game.folder, game.program, slot);
}

static void save_state(void) {
    char path[300];
    state_path(path, sizeof path, state_slot);
    mkdirs_for(path);
    uint32_t n = lava_state_size(vm);
    uint8_t* buf = pd->system->realloc(NULL, n);
    int ok = buf && lava_state_save(vm, buf, n) == n && write_file(path, buf, n);
    free_buf(buf);
    char msg[64];
    snprintf(msg, sizeof msg, ok ? "Saved state %d" : "Couldn't save state %d", state_slot);
    toast(msg);
}

static void load_state(void) {
    char path[300], msg[64];
    state_path(path, sizeof path, state_slot);
    size_t len;
    uint8_t* data = read_file(path, &len);
    if (!data) {
        snprintf(msg, sizeof msg, "State %d is empty", state_slot);
    } else if (lava_state_load(vm, data, (uint32_t)len)) {
        snprintf(msg, sizeof msg, "Loaded state %d", state_slot);
        release_all_keys();
        shown_valid = 0;
    } else {
        snprintf(msg, sizeof msg, "State %d doesn't fit this game", state_slot);
    }
    free_buf(data);
    toast(msg);
}

static void menu_keyboard(void* ud);
static void menu_options(void* ud);
static void menu_quit(void* ud);

static void game_add_menu_items(void) {
    pd->system->removeAllMenuItems();
    pd->system->addMenuItem("keyboard", menu_keyboard, NULL);
    pd->system->addMenuItem("options", menu_options, NULL);
    pd->system->addMenuItem("game list", menu_quit, NULL);
}

static void start_game(const Entry* e) {
    game = *e;
    char path[300];
    snprintf(path, sizeof path, "%s/%s/%s", e->base, e->folder, e->program);
    size_t len;
    program = read_file(path, &len);
    if (!program) {
        show_message("Couldn't open game", path, SCREEN_PICKER);
        return;
    }
    load_vm_fonts();
    vm = pd->system->realloc(NULL, sizeof(LavaVM));
    if (!vm || !lava_init(vm, program, (uint32_t)len, &vm_fonts)) {
        free_buf(vm);
        vm = NULL;
        free_buf(program);
        program = NULL;
        show_message("Not a LAVA program", "The file isn't a GVmaker 1.0 (LAV\\x12) program.", SCREEN_PICKER);
        return;
    }
    vm->host = (LavaHost){NULL, on_file_written, on_file_deleted, on_get_time};
    snprintf(mount_root, sizeof mount_root, "%s/%s", e->base, e->folder);
    mount_dir("");
    saves_dir("");
    apply_deleted();
    profile = profile_find(e->profile);
    if (!profile) {
        profile_auto(program, (uint32_t)len, &auto_profile);
        profile = &auto_profile;
    }
    state_slot = 1;
    snprintf(path, sizeof path, "Config/%s.txt", e->folder);
    read_kv(path, apply_config, NULL);

    release_all_keys();
    palette_open = !pd->system->isCrankDocked();
    palette_sel = 0;
    palette_pressed = -1;
    keyboard_open = 0;
    b_down = b_chorded = 0;
    memset(chord_code, 0, sizeof chord_code);
    memset(dpad_code, 0, sizeof dpad_code);
    a_code = 0;
    frame_acc = 0;
    frame_cost_ms = render_ms = ops_per_frame = 0;
    game_frames = 0;
    bench_ms = bench_result = 0;
    toast_frames = 0;
    if (!pd->system->isCrankDocked()) toast("Crank palette open: turn to pick a key, Ⓐ presses it");
    last_ms = pd->system->getCurrentTimeMilliseconds();
    game_add_menu_items();
    screen = SCREEN_GAME;
    needs_redraw = 1;
}

static void restart_game(void) {
    Entry e = game;
    end_game();
    start_game(&e);
}

// MARK: Game loop

static const PDButtons dirs[4] = {kButtonUp, kButtonRight, kButtonDown, kButtonLeft};
static const int arrow_codes[4] = {LK_UP, LK_RIGHT, LK_DOWN, LK_LEFT};

static void handle_buttons(PDButtons cur, PDButtons pushed, PDButtons released) {
    // Keyboard panel: D-pad/crank move, A presses, B closes.
    if (keyboard_open) {
        int moved = 0;
        if (pushed & kButtonUp) kb_row = (kb_row + KB_ROWS - 1) % KB_ROWS, moved = 1;
        if (pushed & kButtonDown) kb_row = (kb_row + 1) % KB_ROWS, moved = 1;
        if (pushed & kButtonLeft) kb_col = (kb_col + KB_COLS - 1) % KB_COLS, moved = 1;
        if (pushed & kButtonRight) kb_col = (kb_col + 1) % KB_COLS, moved = 1;
        if (!kb[kb_row][kb_col].span) kb_col = (kb_col + 1) % KB_COLS;
        if (moved) needs_redraw = 1;
        if (pushed & kButtonA) {
            const KbKey* k = &kb[kb_row][kb_col];
            if (!k->code) {
                open_keyboard(0);
                return;
            }
            kb_pressed = k->code;
            key_press(k->code);
        }
        if ((released & kButtonA) && kb_pressed) {
            key_release(kb_pressed);
            kb_pressed = 0;
        }
        if (pushed & kButtonB) open_keyboard(0);
        return;
    }

    // B: tap = Esc (sent on release); held with a direction = that direction's chord key.
    if (pushed & kButtonB) {
        b_down = 1;
        b_chorded = 0;
        chrome_dirty = 1;
    }
    for (int d = 0; d < 4; d++) {
        if (pushed & dirs[d]) {
            int code = arrow_codes[d];
            if (b_down && profile->chord[d].code) {
                code = profile->chord[d].code;
                b_chorded = 1;
                chord_code[d] = code;
            }
            dpad_code[d] = code;
            key_press(code);
        }
        if ((released & dirs[d]) && dpad_code[d]) {
            key_release(dpad_code[d]);
            dpad_code[d] = 0;
            chord_code[d] = 0;
        }
    }
    if (released & kButtonB) {
        if (b_down && !b_chorded) {
            key_press(LK_ESC);
            key_release(LK_ESC);
        }
        b_down = 0;
        chrome_dirty = 1;
    }

    // A: Enter, or the palette's selected key.
    if (pushed & kButtonA) {
        int code = LK_ENTER;
        if (palette_open && palette_sel != 0) {
            code = palette_code(palette_sel);
            if (code < 0) {
                open_keyboard(1);
                palette_sel = 0;
                return;
            }
            palette_pressed = palette_sel;
            chrome_dirty = 1;
        }
        a_code = code;
        key_press(code);
    }
    if ((released & kButtonA) && a_code) {
        key_release(a_code);
        a_code = 0;
        if (palette_pressed >= 0) {
            palette_pressed = -1;
            palette_sel = 0;        // back to Enter
            chrome_dirty = 1;
        }
    }
    (void)cur;
}

#ifdef LAVA_AUTOTEST
static int autotest_crank_override = -1;   // 1 = palette forced open
#endif

static void handle_crank(void) {
#ifdef LAVA_AUTOTEST
    if (autotest_crank_override >= 0) return;
#endif
    int docked = pd->system->isCrankDocked();
    float change = pd->system->getCrankChange();
    if (docked) {
        if (palette_open) {
            palette_open = 0;
            palette_sel = 0;
            chrome_dirty = 1;
        }
        return;
    }
    if (!palette_open) {
        palette_open = 1;
        palette_sel = 0;
        palette_crank = 0;
        chrome_dirty = 1;
    }
    if (keyboard_open) {
        // the crank walks the keyboard
        palette_crank += change;
        while (palette_crank >= CRANK_STEP || palette_crank <= -CRANK_STEP) {
            int s = palette_crank > 0 ? 1 : -1;
            palette_crank -= s * CRANK_STEP;
            do {
                int i = (kb_row * KB_COLS + kb_col + s + KB_ROWS * KB_COLS) % (KB_ROWS * KB_COLS);
                kb_row = i / KB_COLS, kb_col = i % KB_COLS;
            } while (!kb[kb_row][kb_col].span);
            needs_redraw = 1;
        }
        return;
    }
    if (palette_pressed >= 0) return;   // hold still while a key is down
    palette_crank += change;
    int n = palette_count();
    while (palette_crank >= CRANK_STEP || palette_crank <= -CRANK_STEP) {
        int s = palette_crank > 0 ? 1 : -1;
        palette_crank -= s * CRANK_STEP;
        palette_sel = (palette_sel + s + n) % n;
        chrome_dirty = 1;
    }
}

static void game_update(void) {
    if (needs_redraw) {
        needs_redraw = 0;
        draw_chrome();
        chrome_dirty = 1;
    }
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    handle_crank();
#ifdef LAVA_AUTOTEST
    if (autotest_crank_override >= 0 && palette_open != autotest_crank_override) {
        palette_open = autotest_crank_override;
        chrome_dirty = 1;
    }
#endif
    handle_buttons(cur, pushed, released);
    if (needs_redraw) {     // the keyboard opened or closed
        needs_redraw = 0;
        draw_chrome();
        chrome_dirty = 1;
    }

    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    frame_acc += (int)(now - last_ms) * 60 * settings.speed;
    last_ms = now;
    int frames = frame_acc / 1000;
    frame_acc -= frames * 1000;
    int cap = 4 * settings.speed;
    if (frames > cap) frames = cap, frame_acc = 0;

    float t0 = pd->system->getElapsedTime();
    uint64_t ops0 = vm->ops;
    for (int i = 0; i < frames && !vm->ended; i++) {
        tick_keys();
        lava_run_frame(vm);
    }
    float spent = (pd->system->getElapsedTime() - t0) * 1000.0f;
    if (frames > 0) {
        if (game_frames >= 300 && game_frames + frames <= 600) bench_ms += spent;
        game_frames += frames;
        if (game_frames >= 600 && bench_result == 0) bench_result = bench_ms / 300;
        float cost = spent / frames;
        frame_cost_ms = frame_cost_ms == 0 ? cost : frame_cost_ms * 0.9f + cost * 0.1f;
        float opf = (float)(vm->ops - ops0) / frames;
        ops_per_frame = ops_per_frame * 0.9f + opf * 0.1f;
        pd->system->resetElapsedTime();
    }
    if (vm->ended) {
        char msg[200];
        if (vm->error) snprintf(msg, sizeof msg, "The VM stopped: %s", vm->errmsg);
        else snprintf(msg, sizeof msg, "%s ended.", game.title);
        end_game();
        show_message(vm ? "Game ended" : "Game ended", msg, SCREEN_PICKER);
        return;
    }

    float r0 = pd->system->getElapsedTime();
    render_lcd();
    render_ms = render_ms * 0.9f + (pd->system->getElapsedTime() - r0) * 1000.0f * 0.1f;

    static char last_status[8];
    if (toast_frames > 0) {
        toast_frames--;
        if (toast_frames == 0) chrome_dirty = 1;
    }
    if (chrome_dirty || settings.show_perf || toast_frames > 0 || b_down) {
        if (keyboard_open) {
            draw_keyboard();
        } else {
            draw_top_band();
            if (chrome_dirty) draw_palette();
        }
        chrome_dirty = 0;
    }
    (void)last_status;
}

// MARK: Options

enum { OPT_SAVE, OPT_LOAD, OPT_SLOT, OPT_BORDER, OPT_SPEED, OPT_PERF, OPT_KEYS, OPT_RESET, OPT_QUIT, OPT_COUNT };
static int opt_selected;

static void option_label(int i, char* out, size_t cap) {
    static const char* border_names[BORDER_COUNT] = {"White", "Black", "Device"};
    switch (i) {
    case OPT_SAVE: snprintf(out, cap, "Save state"); break;
    case OPT_LOAD: snprintf(out, cap, "Load state"); break;
    case OPT_SLOT: snprintf(out, cap, "State slot\t%d", state_slot); break;
    case OPT_BORDER: snprintf(out, cap, "Border\t%s", border_names[settings.border]); break;
    case OPT_SPEED: snprintf(out, cap, "Speed\t%dx", settings.speed); break;
    case OPT_PERF: snprintf(out, cap, "Show performance\t%s", settings.show_perf ? "On" : "Off"); break;
    case OPT_KEYS: snprintf(out, cap, "Keys for this game"); break;
    case OPT_RESET: snprintf(out, cap, "Reset game"); break;
    case OPT_QUIT: snprintf(out, cap, "Back to game list"); break;
    }
}

static void options_draw(void) {
    pd->graphics->clear(kColorWhite);
    text(font, game.title, 12, 8);
    pd->graphics->drawLine(12, 30, 388, 30, 1, kColorBlack);
    int row_h = 20, y0 = 38;
    for (int i = 0; i < OPT_COUNT; i++) {
        char label[96];
        option_label(i, label, sizeof label);
        char* value = strchr(label, '\t');
        if (value) *value++ = 0;
        int y = y0 + i * row_h;
        if (i == opt_selected) {
            pd->graphics->fillRect(8, y - 1, 384, row_h, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        text(font, label, 16, y + 2);
        if (value) {
            char shown_v[48];
            snprintf(shown_v, sizeof shown_v, "< %s >", value);
            text(font, shown_v, 384 - text_w(font, shown_v), y + 2);
        }
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
}

static void options_close(void) {
    last_ms = pd->system->getCurrentTimeMilliseconds();
    screen = SCREEN_GAME;
    needs_redraw = 1;
}

static float opt_crank;

static void options_update(void) {
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    int changed = needs_redraw;
    needs_redraw = 0;
    int step = 0;
    if (pushed & kButtonUp) step = -1;
    if (pushed & kButtonDown) step = 1;
    opt_crank += pd->system->getCrankChange();
    while (opt_crank >= 20 || opt_crank <= -20) {
        int s = opt_crank > 0 ? 1 : -1;
        opt_crank -= s * 20;
        step += s;
    }
    if (step) opt_selected = ((opt_selected + step) % OPT_COUNT + OPT_COUNT) % OPT_COUNT, changed = 1;
    int dir = (pushed & kButtonRight) ? 1 : (pushed & kButtonLeft) ? -1 : (pushed & kButtonA) ? 1 : 0;
    if (pushed & kButtonB) {
        options_close();
        return;
    }
    if (dir) {
        changed = 1;
        switch (opt_selected) {
        case OPT_SLOT:
            state_slot = (state_slot - 1 + dir + STATE_SLOTS) % STATE_SLOTS + 1;
            save_config();
            break;
        case OPT_BORDER:
            settings.border = (settings.border + dir + BORDER_COUNT) % BORDER_COUNT;
            save_settings();
            break;
        case OPT_SPEED: {
            static const int speeds[3] = {1, 2, 4};
            int i = settings.speed == 4 ? 2 : settings.speed == 2 ? 1 : 0;
            settings.speed = speeds[(i + dir + 3) % 3];
            save_settings();
            break;
        }
        case OPT_PERF:
            settings.show_perf = !settings.show_perf;
            save_settings();
            break;
        }
        if (pushed & kButtonA) {
            switch (opt_selected) {
            case OPT_SAVE: save_state(); options_close(); return;
            case OPT_LOAD: load_state(); options_close(); return;
            case OPT_KEYS: screen = SCREEN_KEYS; needs_redraw = 1; return;
            case OPT_RESET: restart_game(); return;
            case OPT_QUIT:
                end_game();
                screen = SCREEN_PICKER;
                needs_redraw = 1;
                return;
            }
        }
    }
    if (changed) options_draw();
}

// MARK: Key view

static int keys_scroll;
static float keys_crank;

static void keys_draw(void) {
    pd->graphics->clear(kColorWhite);
    int y = 8 - keys_scroll;
    char line[160];
    text(font, profile->name, 12, y);
    y += 22;
    pd->graphics->drawLine(12, y, 388, y, 1, kColorBlack);
    y += 6;
    text(small_font, "Buttons", 12, y);
    text(small_font, "D-pad: arrows    Ⓐ: Enter    Ⓑ (tap): Esc", 120, y);
    y += 18;
    static const char* names[4] = {"Ⓑ + ▲", "Ⓑ + ▶", "Ⓑ + ▼", "Ⓑ + ◀"};
    text(small_font, "Chords", 12, y);
    int any = 0;
    for (int d = 0; d < 4; d++) {
        const ProfileKey* k = &profile->chord[d];
        if (!k->code) continue;
        snprintf(line, sizeof line, "%s: %s (%s)", names[d], k->label ? k->label : key_name(k->code), key_name(k->code));
        text(small_font, line, 120, y);
        y += 16;
        any = 1;
    }
    if (!any) y += 16;
    y += 4;
    text(small_font, "Crank palette", 12, y);
    text(small_font, "Undock the crank, turn it to a key, Ⓐ presses it:", 120, y);
    y += 16;
    for (int i = 0; i < profile->npalette; i++) {
        const ProfileKey* k = &profile->palette[i];
        snprintf(line, sizeof line, "%s (%s)", k->label ? k->label : key_name(k->code), key_name(k->code));
        text(small_font, line, 120 + (i % 2) * 140, y);
        if (i % 2) y += 16;
    }
    if (profile->npalette % 2) y += 16;
    text(small_font, "Keyboard (every key): last palette item, or Menu > keyboard", 120, y);
    y += 22;
    if (profile->notes) y = draw_wrapped(small_font, profile->notes, 12, y, 376, 1);
    text(small_font, "Ⓑ back", 340, 222);
}

static void keys_update(void) {
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    int changed = needs_redraw;
    needs_redraw = 0;
    keys_crank += pd->system->getCrankChange();
    int dy = (int)keys_crank;
    keys_crank -= dy;
    if (pushed & kButtonDown) dy += 40;
    if (pushed & kButtonUp) dy -= 40;
    if (dy) {
        keys_scroll += dy;
        if (keys_scroll < 0) keys_scroll = 0;
        if (keys_scroll > 200) keys_scroll = 200;
        changed = 1;
    }
    if (pushed & (kButtonA | kButtonB)) {
        keys_scroll = 0;
        screen = SCREEN_OPTIONS;
        needs_redraw = 1;
        return;
    }
    if (changed) keys_draw();
}

// MARK: Credits

static int credits_scroll;
static float credits_crank;

static const char* credits_head =
    "LavaEmu plays Wenquxing LAVA games: programs for the GVmaker 1.0 virtual machine "
    "that BBK built into its NC2000/NC3000-era electronic dictionaries.\n"
    "Made with AI: written by Anthropic's Claude (Claude Code), directed by gopherbone.\n";

static const char* credits_tail =
    "The VM: a C port of lavaemu (wqx_tl), which follows GVmaker by Eastsun as published in "
    "arucil/GVmakerSE and arucil/MyGVM (MIT, (c) 2018 plodsoft). The 12 and 16 px fonts are "
    "GVmakerSE's (MIT).\n"
    "The English games draw their text in bbk_tl Sans, the proportional pixel font from the bbk_tl "
    "translation project, with a small text routine written in LAVA bytecode.\n"
    "Frontend ideas, file layout and the device border follow bbk_playdate. Playdate SDK by Panic.\n"
    "The games belong to their authors; the English versions are unofficial fan translations.";

static void credits_draw(void) {
    pd->graphics->clear(kColorWhite);
    int y = 8 - credits_scroll;
    text(font, "Credits", 12, y);
    y += 24;
    y = draw_wrapped(small_font, credits_head, 12, y, 376, 1) + 6;
    text(font, "Games", 12, y);
    y += 22;
    char last_folder[64] = "";
    for (int i = 0; i < entry_count; i++) {
        Entry* e = &entries[i];
        if (!strcmp(e->folder, last_folder) || !e->credit[0]) continue;
        snprintf(last_folder, sizeof last_folder, "%s", e->folder);
        text(font, e->title, 12, y);
        if (e->title_gb_len) draw_gb(e->title_gb, e->title_gb_len, 20 + text_w(font, e->title), y + 1, 0);
        y += 20;
        y = draw_wrapped(small_font, e->credit, 24, y, 364, 1) + 6;
    }
    y += 6;
    text(font, "Software", 12, y);
    y += 22;
    draw_wrapped(small_font, credits_tail, 12, y, 376, 1);
}

static void credits_update(void) {
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    int changed = needs_redraw;
    needs_redraw = 0;
    credits_crank += pd->system->getCrankChange();
    int dy = (int)credits_crank;
    credits_crank -= dy;
    if (cur & kButtonDown) dy += 6;
    if (cur & kButtonUp) dy -= 6;
    if (dy) {
        credits_scroll += dy;
        if (credits_scroll < 0) credits_scroll = 0;
        if (credits_scroll > 600) credits_scroll = 600;
        changed = 1;
    }
    if (pushed & (kButtonA | kButtonB)) {
        credits_scroll = 0;
        screen = SCREEN_PICKER;
        needs_redraw = 1;
        return;
    }
    if (changed) credits_draw();
}

// MARK: Picker

static void picker_draw(void) {
    pd->graphics->clear(kColorWhite);
    text(font, "LavaEmu", 12, 8);
    static const uint8_t wqx[] = {0xCE, 0xC4, 0xC7, 0xFA, 0xD0, 0xC7};
    draw_gb(wqx, 6, 98, 9, 0);
    const char* sub = "LAVA games for the Wenquxing";
    text(small_font, sub, 388 - text_w(small_font, sub), 12);
    pd->graphics->drawLine(12, 30, 388, 30, 1, kColorBlack);

    int rows = entry_count + 1;     // + Credits
    int row_h = 36, visible = 5, y0 = 34;
    int top = picker_selected - visible / 2;
    if (top > rows - visible) top = rows - visible;
    if (top < 0) top = 0;
    for (int i = top; i < rows && i < top + visible; i++) {
        int y = y0 + (i - top) * row_h;
        int sel = i == picker_selected;
        if (sel) {
            pd->graphics->fillRoundRect(8, y, 384, row_h - 2, 4, kColorBlack);
            pd->graphics->setDrawMode(kDrawModeFillWhite);
        }
        if (i == entry_count) {
            text(font, "Credits and licences", 16, y + 9);
        } else {
            Entry* e = &entries[i];
            text(font, e->title, 16, y + 2);
            char second[100];
            if (e->label[0]) snprintf(second, sizeof second, "%s", e->label);
            else if (!strcmp(e->base, "Games")) snprintf(second, sizeof second, "Games/%s", e->folder);
            else snprintf(second, sizeof second, "%s", strstr(e->folder, "-zh") ? "Chinese original" : "English");
            text(small_font, second, 16, y + 20);
            if (e->title_gb_len) {
                int w = e->title_gb_len * 8;
                draw_gb(e->title_gb, e->title_gb_len, 384 - w, y + 9, sel);
            }
        }
        pd->graphics->setDrawMode(kDrawModeCopy);
    }
    if (entry_count == 0)
        draw_wrapped(small_font, "No games found. Put a folder with a .lav program (and its LavaData) in Games/ in "
                                 "LavaEmu's Data folder.", 16, 80, 368, 1);
    text(small_font, "Ⓐ play     crank or ▲▼ choose", 16, 222);
}

static void picker_update(void) {
    if (needs_redraw) {
        load_vm_fonts();
        scan_games();
    }
    PDButtons cur, pushed, released;
    pd->system->getButtonState(&cur, &pushed, &released);
    int changed = needs_redraw;
    needs_redraw = 0;
    int rows = entry_count + 1;
    int step = 0;
    if (pushed & kButtonUp) step = -1;
    if (pushed & kButtonDown) step = 1;
    if (pushed & kButtonLeft) step = -5;
    if (pushed & kButtonRight) step = 5;
    picker_crank += pd->system->getCrankChange();
    while (picker_crank >= 20 || picker_crank <= -20) {
        int s = picker_crank > 0 ? 1 : -1;
        picker_crank -= s * 20;
        step += s;
    }
    if (step) {
        int next = picker_selected + step;
        if (next < 0) next = step == -1 ? rows - 1 : 0;
        if (next >= rows) next = step == 1 ? 0 : rows - 1;
        if (next != picker_selected) picker_selected = next, changed = 1;
    }
    if (pushed & kButtonA) {
        if (picker_selected == entry_count) {
            screen = SCREEN_CREDITS;
            needs_redraw = 1;
        } else {
            start_game(&entries[picker_selected]);
        }
        return;
    }
    if (changed) picker_draw();
}

// MARK: System menu

static void menu_keyboard(void* ud) {
    (void)ud;
    if (screen == SCREEN_OPTIONS || screen == SCREEN_KEYS) options_close();
    if (screen != SCREEN_GAME) return;
    release_all_keys();
    open_keyboard(!keyboard_open);
}

static void menu_options(void* ud) {
    (void)ud;
    if (screen != SCREEN_GAME) return;
    release_all_keys();
    opt_selected = 0;
    screen = SCREEN_OPTIONS;
    needs_redraw = 1;
}

static void menu_quit(void* ud) {
    (void)ud;
    end_game();
    screen = SCREEN_PICKER;
    needs_redraw = 1;
}

// MARK: Autotest

#ifdef LAVA_AUTOTEST
// `make autotest`: plays a script per game through the real frontend and
// writes screenshots (PBM) and timings to the Data folder's autotest/.
static int autotest_started, autotest_done;

static void autotest_printf(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    SDFile* f = pd->file->open("autotest/log.txt", kFileAppend);
    if (!f) return;
    pd->file->write(f, line, n);
    pd->file->close(f);
}

static void autotest_shot(const char* name) {
    char path[96];
    snprintf(path, sizeof path, "autotest/%s.pbm", name);
    SDFile* f = pd->file->open(path, kFileWrite);
    if (!f) return;
    const char* header = "P4\n400 240\n";
    pd->file->write(f, header, strlen(header));
    uint8_t* frame = pd->graphics->getFrame();
    uint8_t row[50];
    for (int y = 0; y < 240; y++) {
        for (int i = 0; i < 50; i++) row[i] = ~frame[y * LCD_ROWSIZE + i];
        pd->file->write(f, row, 50);
    }
    pd->file->close(f);
}

// Each script: tokens separated by spaces. KEY taps a LAVA key by name (ENTER,
// ESC, UP, F1, y...), ~N waits N VM frames, shot:NAME saves the screen,
// pal:N opens the crank palette with item N selected, pal:off closes it,
// kb:on/kb:off the keyboard, opts / keys open those screens, stats logs timings.
typedef struct {
    const char* program;
    const char* script;
} AutotestGame;

static const AutotestGame autotest_games[] = {
    {"FrogMonopoly.lav", "~240 stats shot:frog-title ENTER ~60 ENTER ~60 ENTER ~60 ENTER ~90 ENTER ~60 DOWN ~20 "
                         "ENTER ~60 ENTER ~60 y ~60 DOWN ~20 ENTER ~200 shot:frog-map ESC ~60 pal:1 ~20 shot:frog-palette "
                         "palpress ~60 shot:frog-nexttab pal:off ~10 stats"},
    {"AceAttorney.lav", "~300 shot:ace-title ENTER ~120 ENTER ~120 ENTER ~120 ENTER ~120 shot:ace-intro pal:1 ~10 "
                        "shot:ace-palette pal:off stats"},
    {"Hero.lav", "~400 shot:newhero-title ENTER ~200 shot:newhero-2 ENTER ~200 ENTER ~200 shot:newhero-3 stats"},
    {"ShuRegister.lav", "~300 shot:shushan-register kb:on ~10 shot:shushan-keyboard kb:off stats"},
    {"ShuHeroes.lav", "~400 shot:shushan-title ENTER ~200 shot:shushan-2 stats"},
    {"SkyLand2.lav", "~300 shot:seal-title ENTER ~120 shot:seal-menu ENTER ~300 ENTER ~200 shot:seal-intro y ~60 "
                     "pal:1 ~10 shot:seal-palette pal:off stats keys ~10 shot:seal-keys"},
};

static int autotest_index = -1;
static const char* autotest_pos;
static int autotest_wait_until;
static int autotest_last_entry = -1;

static int key_by_name(const char* t) {
    static const struct { const char* n; int c; } names[] = {
        {"ENTER", LK_ENTER}, {"ESC", LK_ESC}, {"UP", LK_UP}, {"DOWN", LK_DOWN}, {"LEFT", LK_LEFT}, {"RIGHT", LK_RIGHT},
        {"F1", LK_F1}, {"F2", LK_F2}, {"F3", LK_F3}, {"F4", LK_F4}, {"PGUP", LK_PGUP}, {"PGDN", LK_PGDN},
        {"HELP", LK_HELP}, {"SPACE", LK_SPACE}, {"SHIFT", LK_SHIFT}, {"CAPS", LK_CAPS}};
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(t, names[i].n)) return names[i].c;
    if (strlen(t) == 1) return tolower((unsigned char)t[0]);
    return 0;
}

static void autotest_stats(const char* what) {
    autotest_printf("   %s: frames %d, VM %d.%02d ms/frame, %d ops/frame, render %d.%02d ms, bench %d.%02d", what,
                    game_frames, (int)frame_cost_ms, (int)(frame_cost_ms * 100) % 100, (int)ops_per_frame,
                    (int)render_ms, (int)(render_ms * 100) % 100, (int)bench_result, (int)(bench_result * 100) % 100);
}

static void autotest_update(void) {
    if (autotest_done) return;
    if (!autotest_started) {
        autotest_started = 1;
        pd->file->mkdir("autotest");
        pd->file->unlink("autotest/log.txt", 0);
        settings.show_perf = 0;
        settings.border = BORDER_WHITE;
        return;
    }
    if (screen == SCREEN_PICKER && !vm) {
        if (autotest_last_entry < 0) {
            scan_games();
            picker_draw();
            autotest_shot("list");
        }
        // next entry with a script
        for (;;) {
            autotest_last_entry++;
            if (autotest_last_entry >= entry_count) {
                screen = SCREEN_CREDITS;
                credits_draw();
                autotest_shot("credits");
                screen = SCREEN_PICKER;
                needs_redraw = 1;
                autotest_printf("AUTOTEST DONE");
                autotest_done = 1;
                return;
            }
            if (strstr(entries[autotest_last_entry].folder, "-zh")) continue;
            autotest_index = -1;
            for (unsigned i = 0; i < sizeof autotest_games / sizeof autotest_games[0]; i++)
                if (!strcmp(autotest_games[i].program, entries[autotest_last_entry].program)) autotest_index = (int)i;
            if (autotest_index >= 0) break;
        }
        picker_selected = autotest_last_entry;
        start_game(&entries[autotest_last_entry]);
        autotest_printf("== %s (%s)", entries[autotest_last_entry].title, entries[autotest_last_entry].program);
        autotest_pos = autotest_games[autotest_index].script;
        autotest_wait_until = 0;
        autotest_crank_override = 0;
        return;
    }
    if (screen == SCREEN_KEYS || screen == SCREEN_OPTIONS) {
        if (game_frames >= autotest_wait_until) {
            screen = SCREEN_GAME;
            needs_redraw = 1;
        } else {
            game_frames += 2;
            return;
        }
    }
    if (!vm || screen != SCREEN_GAME || game_frames < autotest_wait_until) return;
    for (int i = 0; i < 8; i++)
        if (slots[i].active) return;
    while (*autotest_pos == ' ') autotest_pos++;
    if (!*autotest_pos) {
        autotest_crank_override = -1;
        end_game();
        screen = SCREEN_PICKER;
        needs_redraw = 1;
        return;
    }
    char tok[32];
    size_t n = strcspn(autotest_pos, " ");
    snprintf(tok, sizeof tok, "%.*s", (int)n, autotest_pos);
    autotest_pos += n;
    if (tok[0] == '~') {
        autotest_wait_until = game_frames + atoi(tok + 1);
    } else if (!strncmp(tok, "shot:", 5)) {
        autotest_shot(tok + 5);
    } else if (!strcmp(tok, "stats")) {
        autotest_stats("stats");
    } else if (!strncmp(tok, "pal:", 4)) {
        if (!strcmp(tok + 4, "off")) {
            autotest_crank_override = 0;
            palette_sel = 0;
        } else {
            autotest_crank_override = 1;
            palette_open = 1;
            palette_sel = atoi(tok + 4);
        }
        chrome_dirty = 1;
    } else if (!strcmp(tok, "palpress")) {
        int code = palette_code(palette_sel);
        if (code > 0) {
            key_press(code);
            key_release(code);
        }
        palette_sel = 0;
        chrome_dirty = 1;
    } else if (!strncmp(tok, "kb:", 3)) {
        open_keyboard(!strcmp(tok + 3, "on"));
    } else if (!strcmp(tok, "keys")) {
        screen = SCREEN_KEYS;
        keys_draw();
        autotest_wait_until = game_frames + 2;
    } else {
        int code = key_by_name(tok);
        if (code) {
            key_press(code);
            key_release(code);
        }
        autotest_wait_until = game_frames + 2;
    }
}
#endif

// MARK: Entry points

static int update(void* ud) {
    (void)ud;
#ifdef LAVA_AUTOTEST
    autotest_update();
#endif
    switch (screen) {
    case SCREEN_PICKER: picker_update(); break;
    case SCREEN_GAME: game_update(); break;
    case SCREEN_OPTIONS: options_update(); break;
    case SCREEN_KEYS:
#ifdef LAVA_AUTOTEST
        if (autotest_started && !autotest_done) break;
#endif
        keys_update();
        break;
    case SCREEN_CREDITS: credits_update(); break;
    case SCREEN_MESSAGE: message_update(); break;
    }
    return 1;
}

#if TARGET_PLAYDATE
// newlib's snprintf drags in its stdio layer, which wants these POSIX hooks.
struct stat;
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat* st) { (void)fd; (void)st; return -1; }
int _getpid(void) { return 1; }
int _isatty(int fd) { (void)fd; return 0; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return -1; }
int _read(int fd, void* buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
int _write(int fd, const void* buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
void _exit(int code) {
    (void)code;
    for (;;) {}
}
#endif

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    switch (event) {
    case kEventInit: {
        pd = playdate;
        lava_realloc = pd_realloc;
        const char* err;
        font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &err);
        small_font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &err);
        if (!small_font) small_font = font;
        const char* mk[] = {"Games", "Saves", "States", "Config"};
        for (size_t i = 0; i < sizeof mk / sizeof mk[0]; i++) pd->file->mkdir(mk[i]);
        read_kv("settings.txt", apply_setting, NULL);
        init_dbl();
        pd->display->setRefreshRate(REFRESH_RATE);
        pd->system->setUpdateCallback(update, NULL);
        break;
    }
    case kEventPause:
    case kEventLock:
    case kEventLowPower:
    case kEventTerminate:
        if (vm) release_all_keys();
        break;
    case kEventResume:
    case kEventUnlock:
        last_ms = pd->system->getCurrentTimeMilliseconds();
        if (screen == SCREEN_GAME) needs_redraw = 1;
        break;
    default:
        break;
    }
    return 0;
}
