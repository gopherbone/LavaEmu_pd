#ifndef LAVA_RENDER_H
#define LAVA_RENDER_H

#include <stdint.h>

// Output rows are 40 bytes (320 pixels) in the Playdate's polarity (1 = white).
void render_init(void);
void render_row_1bpp(const uint8_t* src20, uint8_t* top, uint8_t* bot);
void render_row_blend3(const uint8_t* a, const uint8_t* b, const uint8_t* c, int dither, uint8_t* top, uint8_t* bot);
void render_row_px2x(const uint8_t* src160, int mode, const uint8_t (*pal)[3], uint8_t* top, uint8_t* bot);
void render_row_px1x(const uint8_t* src, int w, int y, int mode, const uint8_t (*pal)[3], uint8_t* out);

#endif
