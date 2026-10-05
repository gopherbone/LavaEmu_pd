// Shared between lava.c and lavax.c (not part of the VM's public API).
#ifndef LAVA_INTERNAL_H
#define LAVA_INTERNAL_H

#include "lava.h"

void lava_glyph(LavaVM* vm, int code, int big, int* w, const uint8_t** data, int* n);
void lava_tdims(LavaVM* vm, int* cols, int* rows, int* rh);
uint32_t lava_cstr_len(LavaVM* vm, uint32_t a);
void lava_path_of(LavaVM* vm, uint32_t a, char* out);
int lava_has_dir(LavaVM* vm, const char* d);

LavaPx* lavax_px_new(int w, int h, int mode);
void lavax_px_free(LavaPx* p);
int lavax_set_mode(LavaPx* p, int m);
void lavax_glyph(LavaVM* vm, int code, int big, int* w, int* h, const uint8_t** g, int* n);
void lavax_trender(LavaVM* vm, int which);
int lavax_sys_px(LavaVM* vm, int op);
int lavax_sys(LavaVM* vm, int op);
uint32_t lavax_ext_op(LavaVM* vm, int op, uint32_t pc, uint32_t fb, int32_t* last);
void lavax_default_palette(uint8_t pal[256][3]);

#endif
