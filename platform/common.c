/* SPDX-License-Identifier: GPL-3.0-only */
/* What the FM-1 (fm1.c) and the emulator (web.c) share: the screen's framebuffer and drawing, and
 * the sound helpers. Included by both after felucca_font_t, FONT_S / FONT_L and libc.c. */

/* ---- the framebuffer: pixels stored byte-swapped, as the panel takes them, so a flush is one DMA
 * per strip. fb_y0..fb_y1 (exclusive) is what changed since the last flush */
static uint16_t fb[SCREEN_W * SCREEN_H] POOL;
static int fb_y0 = SCREEN_H, fb_y1 = 0;

static inline uint16_t fb_swap(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void fb_dirty(int y0, int y1) {
  if (y0 < fb_y0) {
    fb_y0 = y0;
  }
  if (y1 > fb_y1) {
    fb_y1 = y1;
  }
}

void rect(int x, int y, int w, int h, uint16_t color) {
  int x1 = x + w, y1 = y + h, i, j;
  uint16_t c = fb_swap(color);
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x1 > SCREEN_W) {
    x1 = SCREEN_W;
  }
  if (y1 > SCREEN_H) {
    y1 = SCREEN_H;
  }
  if (x >= x1 || y >= y1) {
    return;
  }
  for (j = y; j < y1; j++) {
    for (i = x; i < x1; i++) {
      fb[j * SCREEN_W + i] = c;
    }
  }
  fb_dirty(y, y1);
}

void clear(uint16_t color) { rect(0, 0, SCREEN_W, SCREEN_H, color); }

void pixel(int x, int y, uint16_t color) {
  if ((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H) {
    fb[y * SCREEN_W + x] = fb_swap(color);
    fb_dirty(y, y + 1);
  }
}

void frame(int x, int y, int w, int h, uint16_t color) {
  rect(x, y, w, 1, color);
  rect(x, y + h - 1, w, 1, color);
  rect(x, y, 1, h, color);
  rect(x + w - 1, y, 1, h, color);
}

void line(int x0, int y0, int x1, int y1, uint16_t color) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, guard = 2000;
  while (guard--) { /* bounded: on screen a line is never longer than 480 px */
    int e2 = 2 * err;
    pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* ---- text: the fonts are 4-bit alpha, blended onto what is already there */
static const felucca_font_t *font_of(int font) { return font == FONT_BIG ? &FONT_L : &FONT_S; }

static uint32_t font_glyph(const felucca_font_t *f, uint32_t ch) {
  if (ch >= 'a' && ch <= 'z' && f->last < 'a') {
    ch -= 32u; /* the big font has capitals only */
  }
  if (ch < f->first || ch > f->last) {
    ch = '?';
  }
  return ch - f->first;
}

static uint16_t blend(uint16_t bg, uint16_t fg, uint32_t a) { /* a: 0..15 */
  uint32_t r = ((bg >> 11) * (15u - a) + (fg >> 11) * a) / 15u;
  uint32_t g = (((bg >> 5) & 63u) * (15u - a) + ((fg >> 5) & 63u) * a) / 15u;
  uint32_t b = ((bg & 31u) * (15u - a) + (fg & 31u) * a) / 15u;
  return (uint16_t)(r << 11 | g << 5 | b);
}

int text(int x, int y, const char *s, uint16_t color, int font) {
  const felucca_font_t *f = font_of(font);
  for (; *s; s++) {
    uint32_t gi = font_glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u) / 2u, gx, gy;
    const uint8_t *gd = f->data + f->off[gi];
    for (gy = 0; gy < f->h; gy++) {
      int py = y + (int)gy;
      if ((unsigned)py >= SCREEN_H) {
        continue;
      }
      for (gx = 0; gx < w; gx++) {
        int px = x - f->pad + (int)gx;
        uint32_t v = gd[gy * bpr + gx / 2u];
        v = (gx & 1u) ? (v & 15u) : (v >> 4);
        if (v && (unsigned)px < SCREEN_W) {
          uint16_t *p = &fb[py * SCREEN_W + px];
          *p = fb_swap(v == 15u ? color : blend(fb_swap(*p), color, v));
        }
      }
    }
    fb_dirty(y < 0 ? 0 : y, y + f->h > SCREEN_H ? SCREEN_H : y + f->h);
    x += f->adv[gi];
  }
  return x;
}

int text_width(const char *s, int font) {
  const felucca_font_t *f = font_of(font);
  int w = 0;
  for (; *s; s++) {
    w += f->adv[font_glyph(f, (uint8_t)*s)];
  }
  return w;
}

int text_height(int font) { return font_of(font)->h; }

/* ---- sound helpers */
int32_t sine(uint32_t phase) { /* a refined parabola, ~0.1 % */
  int32_t x = (int32_t)phase >> 16, ax = x < 0 ? -x : x, y, ay;
  y = (x * (32768 - ax)) >> 13; /* 4x(1-|x|), Q15 */
  ay = y < 0 ? -y : y;
  y += ((((y * ay) >> 15) - y) * 7373) >> 15; /* + 0.225 (y|y| - y) */
  return y > 32767 ? 32767 : y < -32767 ? -32767
                                        : y;
}

/* f * 2^32 / 44100 for MIDI 120..131 (C9..B9); lower octaves shift right */
static const uint32_t INC_C9[12] = {815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u, 1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};

uint32_t note_inc(int note) {
  if (note < 0) {
    note = 0;
  }
  if (note > 127) {
    note = 127;
  }
  return INC_C9[note % 12] >> (10 - note / 12);
}

static uint32_t random_state = 0x1234567u;
uint32_t random32(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 17;
  random_state ^= random_state << 5;
  return random_state;
}

void format_int(char *buf, int32_t v) { fmt_int(buf, v); }
