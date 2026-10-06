#include "live.h"

#include <string.h>

static int oplen(int op) {
    switch (op) {
    case 0x01: case 0x43: case 0x6E: case 0x70: case 0x71: return 1;
    case 0x03: return 4;
    case 0x39: case 0x3A: case 0x3B: case 0x3D: case 0x3E: case 0x73: case 0x74: return 3;
    case 0x3C: case 0x6F: return 2;
    }
    if ((op >= 0x02 && op <= 0x0C) || (op >= 0x0E && op <= 0x19) || (op >= 0x45 && op <= 0x51)) return 2;
    return 0;
}

static int keyish(int v) {
    return v == LK_ENTER || v == LK_PGDN || v == LK_CAPS || v == LK_PGUP || (v >= LK_UP && v <= LK_LEFT) ||
           v == LK_HELP || v == LK_SHIFT || v == LK_ESC || (v >= LK_F1 && v <= LK_SPACE) || (v >= 'a' && v <= 'z') ||
           (v >= '0' && v <= '9') || v == '.';
}

static int is_load(int op) {
    return (op >= 0x04 && op <= 0x06) || (op >= 0x0E && op <= 0x10);   // ld8/16/32, lld8/16/32
}

static int is_read(int op) { return op == 0x81 || op == 0x93 || op == 0xC4 || op == 0xBC; }

// Scans forward from `pc` (just after a read site, or a return address) for
// comparisons on one variable (or on the value the read pushed). Returns 1 if
// it found any.
static int rmin, rmax;      // range-test bounds over letters/digits seen in this computation

static int scan(LavaVM* vm, uint32_t pc, LiveSet* out) {
    const uint8_t* code = vm->code;
    uint32_t len = vm->code_len;
    int var_op = -1, var_arg = -1;      // the key variable, once known
    int prev_op = -1, prev_arg = 0;     // the instruction before
    int pprev_op = -1, pprev_arg = 0;
    int found = 0, direct = 1;          // direct: still right after the read (value on the stack)
    for (int n = 0; n < 400 && pc < len; n++) {
        int op = code[pc];
        if (!(op <= 0x74 || (op >= 0x80 && op <= 0xD6))) break;
        if (op == 0x3E || op == 0x40) break;                // next function / end
        if (n > 0 && is_read(op)) break;                    // the next key read
        int arg = 0;
        uint32_t size;
        if (op == 0x0D) {
            uint32_t e = pc + 1;
            while (e < len && code[e] != vm->xorkey) e++;
            size = e + 1 - pc;
        } else if (op == 0x41) {
            if (pc + 5 > len) break;
            size = 5 + (code[pc + 3] | code[pc + 4] << 8);
        } else {
            int k = oplen(op);
            size = 1 + (uint32_t)k;
            if (pc + size > len) break;
            if (k == 1) arg = code[pc + 1];
            else if (k == 2) arg = (int16_t)(code[pc + 1] | code[pc + 2] << 8);
            if (is_load(op)) arg = code[pc + 1] | code[pc + 2] << 8;
        }
        // a compare against a constant: eqi/nei/range-i, or push K; eq/ne/range
        int cmp_k = -1, range = 0, src_op = -1, src_arg = -1;
        if (op >= 0x4C && op <= 0x51) {
            cmp_k = arg, range = op >= 0x4E;
            src_op = prev_op, src_arg = prev_arg;
        } else if (op >= 0x2F && op <= 0x34 && (prev_op == 0x01 || prev_op == 0x02)) {
            cmp_k = prev_arg, range = op >= 0x31;
            src_op = pprev_op, src_arg = pprev_arg;
        }
        if (cmp_k >= 0) {
            int ok = 0;
            if (direct && n <= 1) ok = 1;                   // read; eqi K
            else if (is_load(src_op)) {
                if (var_op < 0 && keyish(cmp_k)) var_op = src_op, var_arg = src_arg;
                ok = src_op == var_op && src_arg == var_arg;
            }
            if (ok) {
                if (range) {
                    // range tests over letters or digits (not over arrow codes): text entry if
                    // the bounds span a run like '0'..'9' or 'a'..'z', not a few hotkeys
                    if (cmp_k >= '0' && cmp_k <= 'z') {
                        if (cmp_k < rmin) rmin = cmp_k;
                        if (cmp_k > rmax) rmax = cmp_k;
                    }
                } else if (cmp_k > 0 && cmp_k < 128 && keyish(cmp_k)) {
                    if (!out->keys[cmp_k]) out->keys[cmp_k] = 1, out->count++;
                    found = 1;
                }
            }
        }
        if (n >= 1 && op != 0x81 && op != 0x93 && op != 0xC4 && op != 0xBC) direct = 0;
        pprev_op = prev_op, pprev_arg = prev_arg;
        prev_op = op, prev_arg = arg;
        pc += size;
    }
    return found;
}

void live_compute(LavaVM* vm, int window, LiveSet* out) {
    memset(out, 0, sizeof *out);
    rmin = 1000, rmax = -1;
    int32_t since = vm->frame - window;
    for (int i = 0; i < LAVA_READ_SITES; i++) {
        const LavaReadSite* r = &vm->reads[i];
        if (!r->pc || r->frame < since) continue;
        // the read itself, then the caller that gets the key back, then its caller
        // all three levels: a helper often tests a few keys and returns the rest (the arrows,
        // say) to its caller
        int f = scan(vm, r->pc, out);
        if (r->ret0) f |= scan(vm, r->ret0, out);
        if (r->ret1) f |= scan(vm, r->ret1, out);
        out->known |= f;
    }
    for (int k = 1; k < 128; k++) {
        if (vm->checked[k] && vm->checked[k] > since && keyish(k) && !out->keys[k]) {
            out->keys[k] = 1, out->count++;
            out->known = 1;
        }
    }
    if (rmax - rmin >= 9) out->open = 1;
    int alnum = 0;
    for (int k = '0'; k <= 'z'; k++)
        if (out->keys[k] && ((k >= '0' && k <= '9') || (k >= 'a' && k <= 'z'))) alnum++;
    const uint8_t* k = out->keys;
    // GetWord-style fields: Caps and Shift change case and F2 deletes (the Wenquxing has no
    // Backspace); a field edited with Left as backspace reads Enter and Left but no other arrow
    int fieldkeys = k[LK_CAPS] && k[LK_SHIFT] && k[LK_F2];
    int backspace = k[LK_ENTER] && k[LK_LEFT] && !k[LK_RIGHT] && !k[LK_UP] && !k[LK_DOWN];
    // range or isalpha-style tests mean text only where the screen also offers a delete key
    // (F2, or Left as backspace): menus that range-check a few hotkeys don't
    int del = k[LK_F2] || (k[LK_LEFT] && !k[LK_RIGHT]);
    int classified = vm->classified && vm->classified > since;
    out->text = ((out->open || classified) && del) || alnum >= 12 || fieldkeys || backspace;
    out->arrows = out->keys[LK_UP] || out->keys[LK_DOWN] || out->keys[LK_LEFT] || out->keys[LK_RIGHT];
}
