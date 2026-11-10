/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 David Konsumer. Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following
 * conditions: The above copyright notice and this permission notice shall be included in all copies or substantial
 * portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
/* bigscreen.h: Big Screen, a small device's screen shown big on another one (a computer, a phone, fm1buddy), over
 * USB MIDI SysEx. Version 2, and version 1 (pfkellogg's screen_mirror) for the viewers that only know that one.
 *
 * One file, C99, no malloc, nothing needed but stdint.h (no libc). In one .c (or .cpp) file:
 *
 *   #define BIGSCREEN_IMPLEMENTATION
 *   #include "bigscreen.h"
 *
 * and only #include "bigscreen.h" everywhere else. The protocol itself is in README.md, beside this file. A device
 * that doesn't view can #define BS_NO_VIEWER first (and a viewer BS_NO_DEVICE): less code. The settings below
 * (BS_MSG, BS_MAX_TILES, ...) size bs_dev: set them the same before each #include of it.
 *
 * ---- the device (what has the screen): an RGB565 framebuffer, in RAM
 *
 *   static bs_dev big;
 *   bs_dev_init(&big, fb, 240, 240, 240, 0);           // the pixels, w, h, stride (pixels), 1 if stored byte-swapped
 *   // a SysEx came in (all of it, F0 .. F7):
 *   if (bs_dev_sysex(&big, m, n, millis())) { ... }    // 1: it was a viewer's hello (then it's used up)
 *   // you drew something: what it covered (x0, y0 in; x1, y1 out). Cheap when nobody's looking
 *   bs_dev_changed(&big, 0, y0, 240, y1);
 *   // often (the main loop), while there's room to send:
 *   const uint8_t *m;
 *   uint32_t n;
 *   while (room_for(BS_MSG) && (n = bs_dev_next(&big, millis(), &m))) {
 *     send_sysex(m, n);                                // a whole SysEx, F0 .. F7, at most BS_MSG bytes
 *   }
 *
 * Nothing at all goes out unless a viewer said hello in the last BS_TIMEOUT ms. The screen is cut in BS_TILE x
 * BS_TILE tiles; what goes out is each tile that changed since it last went out (a checksum of each one: 4 bytes a
 * tile, no copy of the screen), compressed. The framebuffer is read as each message is made, so call bs_dev_next where
 * nothing is drawing (the main loop, not an interrupt that draws).
 *
 * ---- the viewer
 *
 *   static uint16_t screen[240 * 240];                 // RGB565, as numbers (not byte-swapped)
 *   static bs_view view;
 *   bs_view_init(&view, screen, 240, 240);             // the buffer, its w, h (the most it can show)
 *   // often (every 100 ms or so): a hello, when one is due
 *   uint8_t hello[BS_HELLO_MAX];
 *   uint32_t n = bs_view_hello(&view, millis(), hello);
 *   if (n) send_sysex(hello, n);
 *   // each SysEx that comes in (all of it, F0 .. F7):
 *   bs_rect r;
 *   switch (bs_view_sysex(&view, m, n, &r)) {
 *     case BS_DRAW: redraw(r.x, r.y, r.w, r.h); break;   // those pixels changed in screen[]
 *     case BS_FRAME: present(); break;                   // all of a frame is in (v2): a good time to show it
 *     case BS_INFO: resize(view.w, view.h); break;       // the device's screen size (v2)
 *   }
 *
 * The viewer asks for version 2 first; firmware that only knows version 1 doesn't answer that, and after
 * BS_PROBE_TRIES hellos it asks for version 1 instead (view.ver says which: 0 still asking, 1, 2). Call
 * bs_view_reset when the device is plugged in again (or another one is). */
#ifndef BIGSCREEN_H
#define BIGSCREEN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BS_VERSION 2

#ifndef BS_TILE
#define BS_TILE 16 /* a tile's side, in pixels */
#endif
#ifndef BS_MAX_TILES
#define BS_MAX_TILES 300 /* tiles the device's screen has at most: 320 x 240 is 20 x 15 = 300, 240 x 240 is 225 */
#endif
#ifndef BS_MSG
#define BS_MSG 256 /* the longest message the device sends (bytes, F0 .. F7): at least 72 (version 1 sends 70) */
#endif
#ifndef BS_TIMEOUT
#define BS_TIMEOUT 3000u /* ms: the device stops this long after the last hello */
#endif
#ifndef BS_HELLO_MS
#define BS_HELLO_MS 1000u /* ms between a viewer's hellos */
#endif
#ifndef BS_PROBE_MS
#define BS_PROBE_MS 500u /* ms between hellos while asking which version the device speaks */
#endif
#ifndef BS_PROBE_TRIES
#define BS_PROBE_TRIES 3 /* version 2 hellos without an answer: then it's version 1 */
#endif
#ifndef BS_REPROBE
#define BS_REPROBE 10 /* speaking version 1: every this many hellos, one asks for version 2 (new firmware) */
#endif

#ifndef BS_MAX_W
#define BS_MAX_W 320 /* the device's screen is at most this wide */
#endif

#define BS_HELLO_MAX 8                                    /* the longest hello */
#define BS_RECT_TILES ((BS_MSG - 16) / 3 / BS_TILE)       /* tiles side by side in a version 2 rectangle: a row always fits */
#define BS_ROW_TILES ((BS_MAX_W + BS_TILE - 1) / BS_TILE) /* in a version 1 one (a row goes in as many messages as it takes) */
#define BS_CACHE 64

/* SysEx: F0 7D 'B' 'S' <cmd> ... F7 (version 2), F0 7D 'F' 'S' <cmd> ... F7 (version 1) */
enum { BS_HELLO = 0,
       BS_CMD_INFO = 1,
       BS_CMD_RECT = 2,
       BS_CMD_SYNC = 3 };
enum { BS_HELLO_ALL = 1 }; /* hello flags: send all of the screen again */
enum { BS_RGB565 = 0 };    /* INFO's pixel format */

/* what bs_view_sysex found */
enum { BS_NOT_MINE = 0,
       BS_OK,
       BS_DRAW,
       BS_FRAME,
       BS_INFO };

typedef struct {
  int x, y, w, h;
} bs_rect;

typedef struct {
  const uint16_t* fb;
  int w, h, stride;
  uint8_t swap;            /* fb's pixels are stored byte-swapped (as many SPI panels take them) */
  uint8_t mode;            /* 0 nobody's looking, 1 or 2: the version going out */
  uint8_t info;            /* an INFO is owed (each version 2 hello gets one) */
  uint8_t synced;          /* nothing went out since the last SYNC */
  uint8_t busy;            /* a rectangle is going out */
  uint16_t tw, th, cursor; /* tiles across, down; where the next search for a changed one starts */
  uint32_t heard;          /* ms of the last hello */
  /* the rectangle going out: x, the next row, width, the row after its last, its first tile, tiles; x in the row (v1) */
  uint16_t rx, ry, rw, rend, rt, rn, vx;
  uint32_t acc[BS_ROW_TILES];  /* its tiles' checksums, of the rows that went out */
  uint32_t sent[BS_MAX_TILES]; /* each tile's checksum as it last went out */
  uint8_t pend[(BS_MAX_TILES + 7) / 8];
  uint8_t msg[BS_MSG + 3 * BS_RECT_TILES * BS_TILE + 8];
} bs_dev;

typedef struct {
  uint16_t* fb;
  int bw, bh;    /* the buffer: bw x bh, stride bw */
  int w, h;      /* the device's screen (from INFO; the buffer's size until then) */
  uint8_t ver;   /* 0 asking, 1, 2 */
  uint8_t tries; /* version 2 hellos sent with no INFO back (asking, or it went quiet) */
  uint8_t count; /* hellos sent in version 1 */
  uint8_t all;   /* the next hello asks for all of the screen */
  uint8_t seen;  /* pixels came since the reset */
  uint8_t timed; /* a hello went out (at) */
  uint32_t at;   /* ms of the last hello */
} bs_view;

void bs_dev_init(bs_dev* d, const uint16_t* fb, int w, int h, int stride, int swapped);
int bs_dev_sysex(bs_dev* d, const uint8_t* m, uint32_t n, uint32_t now);
void bs_dev_changed(bs_dev* d, int x0, int y0, int x1, int y1);
uint32_t bs_dev_next(bs_dev* d, uint32_t now, const uint8_t** out);
static inline int bs_dev_active(const bs_dev* d) { return d->mode != 0; }

void bs_view_init(bs_view* v, uint16_t* fb, int w, int h);
void bs_view_reset(bs_view* v);
uint32_t bs_view_hello(bs_view* v, uint32_t now, uint8_t out[BS_HELLO_MAX]);
int bs_view_sysex(bs_view* v, const uint8_t* m, uint32_t n, bs_rect* r);

/* the color cache's slot for an RGB565 color (both ends) */
static inline uint32_t bs_hash(uint32_t c) { return ((c >> 11) * 3u + ((c >> 5) & 63u) * 5u + (c & 31u) * 7u) & 63u; }

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_H */

#ifdef BIGSCREEN_IMPLEMENTATION
#ifndef BIGSCREEN_IMPLEMENTED
#define BIGSCREEN_IMPLEMENTED

#ifdef __cplusplus
extern "C" {
#endif

#define BS__FNV0 2166136261u

static void bs__fill(void* p, int c, uint32_t n) {
  uint8_t* b = (uint8_t*)p;
  while (n--) {
    *b++ = (uint8_t)c;
  }
}
#define BS__FNV(h, c) (((h) ^ (c)) * 16777619u)

#ifndef BS_NO_DEVICE
#ifdef BS_SWAPPED /* (fixed when it's built: less code) */
#define BS__SWAP(d) BS_SWAPPED
#else
#define BS__SWAP(d) (d)->swap
#endif
static inline uint32_t bs__px(const bs_dev* d, int x, int y) {
  uint32_t c = d->fb[y * d->stride + x];
  return BS__SWAP(d) ? ((c >> 8) | (c << 8)) & 0xFFFFu : c;
}

/* F0 7D 'B'|'F' 'S' cmd, then 14-bit numbers (7 bits a byte, low first) */
static uint32_t bs__head(uint8_t* m, uint8_t id, uint8_t cmd) {
  m[0] = 0xF0, m[1] = 0x7D, m[2] = id, m[3] = 'S', m[4] = cmd;
  return 5;
}
static uint32_t bs__u14(uint8_t* m, uint32_t k, uint32_t v) {
  m[k] = (uint8_t)(v & 127u), m[k + 1] = (uint8_t)(v >> 7 & 127u);
  return k + 2;
}

/* a tile's checksum: its pixels, row by row */
static uint32_t bs__tile_hash(const bs_dev* d, int t) {
  int x0 = t % d->tw * BS_TILE, y0 = t / d->tw * BS_TILE, x1 = x0 + BS_TILE, y1 = y0 + BS_TILE, x, y;
  uint32_t h = BS__FNV0;
  x1 = x1 > d->w ? d->w : x1;
  y1 = y1 > d->h ? d->h : y1;
  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      h = BS__FNV(h, bs__px(d, x, y));
    }
  }
  return h;
}

static void bs__all(bs_dev* d) {
  bs__fill(d->pend, 0xFF, sizeof d->pend);
  d->busy = 0;
  d->synced = 0;
}

void bs_dev_init(bs_dev* d, const uint16_t* fb, int w, int h, int stride, int swapped) {
  bs__fill(d, 0, sizeof *d);
  d->fb = fb;
  d->w = w;
  d->h = h;
  d->stride = stride;
  d->swap = (uint8_t)(swapped != 0);
  d->tw = (uint16_t)((w + BS_TILE - 1) / BS_TILE);
  d->th = (uint16_t)((h + BS_TILE - 1) / BS_TILE);
  if (d->tw > BS_ROW_TILES) { /* (bigger than BS_MAX_W, BS_MAX_TILES: only what fits) */
    d->tw = BS_ROW_TILES;
    d->w = BS_ROW_TILES * BS_TILE;
  }
  if (d->tw * d->th > BS_MAX_TILES) {
    d->th = (uint16_t)(BS_MAX_TILES / d->tw);
    d->h = d->th * BS_TILE;
  }
  d->synced = 1;
}

int bs_dev_sysex(bs_dev* d, const uint8_t* m, uint32_t n, uint32_t now) {
  if (n < 6 || m[0] != 0xF0 || m[1] != 0x7D || m[3] != 'S' || m[n - 1] != 0xF7) {
    return 0;
  }
  if (m[2] == 'B' && m[4] == BS_HELLO) { /* F0 7D 'B' 'S' 00 version flags F7 */
    uint8_t flags = n >= 8 ? m[6] : 0;
    if (d->mode != 2 || (flags & BS_HELLO_ALL)) {
      bs__all(d);
    }
    d->mode = 2;
    d->info = 1;
    d->heard = now;
    return 1;
  }
  if (m[2] == 'F' && n == 6 && m[4] < 3) { /* F0 7D 'F' 'S' k F7: 0 hello, 1 and all again, 2 the sound's name */
    if (d->mode == 2 || m[4] == 2) {
      return 1; /* (a version 2 viewer is looking: it has the screen; the name isn't sent here) */
    }
    if (d->mode != 1 || m[4] == 1) {
      bs__all(d);
    }
    d->mode = 1;
    d->heard = now;
    return 1;
  }
  return 0;
}

void bs_dev_changed(bs_dev* d, int x0, int y0, int x1, int y1) {
  int tx, ty;
  if (!d->mode) {
    return;
  }
  x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
  x1 = x1 > d->w ? d->w : x1, y1 = y1 > d->h ? d->h : y1;
  for (ty = y0 / BS_TILE; ty * BS_TILE < y1 && ty < d->th; ty++) {
    for (tx = x0 / BS_TILE; tx * BS_TILE < x1; tx++) {
      int t = ty * d->tw + tx;
      if (!(d->pend[t >> 3] >> (t & 7) & 1) && bs__tile_hash(d, t) != d->sent[t]) {
        d->pend[t >> 3] |= (uint8_t)(1u << (t & 7));
      }
    }
  }
}

/* the next changed tile, and those after it in its row: the rectangle that goes out next */
static int bs__start(bs_dev* d) {
  int total = d->tw * d->th, most = d->mode == 2 ? BS_RECT_TILES : BS_ROW_TILES, i, t = -1;
  for (i = 0; i < total; i++) {
    int k = (d->cursor + i) % total;
    if (d->pend[k >> 3] >> (k & 7) & 1) {
      t = k;
      break;
    }
  }
  if (t < 0) {
    return 0;
  }
  d->rt = (uint16_t)t;
  d->rn = 0;
  while (d->rn < most && t % d->tw + d->rn < d->tw && (d->pend[(t + d->rn) >> 3] >> ((t + d->rn) & 7) & 1)) {
    int k = t + d->rn;
    d->pend[k >> 3] &= (uint8_t)~(1u << (k & 7));
    d->acc[d->rn++] = BS__FNV0;
  }
  d->rx = (uint16_t)(t % d->tw * BS_TILE);
  d->rw = (uint16_t)(d->rn * BS_TILE);
  if (d->rx + d->rw > d->w) {
    d->rw = (uint16_t)(d->w - d->rx);
  }
  d->ry = (uint16_t)(t / d->tw * BS_TILE);
  d->rend = (uint16_t)(d->ry + BS_TILE > d->h ? d->h : d->ry + BS_TILE);
  d->vx = d->rx;
  d->cursor = (uint16_t)((t + d->rn) % total);
  d->busy = 1;
  return 1;
}

/* rows y0..y1 went out: into their tiles' checksums */
static void bs__rows_out(bs_dev* d, int y0, int y1) {
  int y, x, i;
  for (y = y0; y < y1; y++) {
    for (i = 0; i < d->rn; i++) {
      int x0 = d->rx + i * BS_TILE, x1 = x0 + BS_TILE > d->rx + d->rw ? d->rx + d->rw : x0 + BS_TILE;
      uint32_t h = d->acc[i];
      for (x = x0; x < x1; x++) {
        h = BS__FNV(h, bs__px(d, x, y));
      }
      d->acc[i] = h;
    }
  }
}

/* all of the rectangle went out: what it was is what the viewer has. If it changed while it went (part of it in one
 * message, part in a later one), it goes again */
static void bs__done(bs_dev* d) {
  int i;
  for (i = 0; i < d->rn; i++) {
    int t = d->rt + i;
    d->sent[t] = d->acc[i];
    if (bs__tile_hash(d, t) != d->acc[i]) {
      d->pend[t >> 3] |= (uint8_t)(1u << (t & 7));
    }
  }
  d->busy = 0;
}

static uint32_t bs__run(uint8_t* m, uint32_t k, uint32_t run) {
  while (run > 32u) { /* 0110nnnn b: 33..2080 */
    uint32_t r = run > 2080u ? 2080u : run, c = r - 33u;
    m[k++] = (uint8_t)(0x60u | c >> 7);
    m[k++] = (uint8_t)(c & 127u);
    run -= r;
  }
  if (run) { /* 010nnnnn: 1..32 */
    m[k++] = (uint8_t)(0x40u | (run - 1u));
  }
  return k;
}

static uint32_t bs__msg_v2(bs_dev* d) {
  uint8_t* m = d->msg;
  uint16_t cache[BS_CACHE];
  uint32_t k = 13, prev = 0, run = 0, rows = 0, total = (uint32_t)(d->rend - d->ry);
  int x, y;
  bs__fill(cache, 0, sizeof cache);
  for (y = d->ry; rows < total; y++) {
    uint32_t k0 = k, run0 = run;
    for (x = d->rx; x < d->rx + d->rw; x++) {
      uint32_t c = bs__px(d, x, y), i;
      if (c == prev) {
        run++;
        continue;
      }
      k = bs__run(m, k, run);
      run = 0;
      i = bs_hash(c);
      if (cache[i] == c) {
        m[k++] = (uint8_t)i; /* 00iiiiii */
      } else {
        cache[i] = (uint16_t)c;
        m[k++] = (uint8_t)(0x70u | c >> 14); /* 011100hh, then 7 + 7 bits */
        m[k++] = (uint8_t)(c >> 7 & 127u);
        m[k++] = (uint8_t)(c & 127u);
      }
      prev = c;
    }
    if (rows && k + 2u * (run / 2080u + 1u) + 1u > BS_MSG) {
      k = k0, run = run0; /* this row doesn't fit: it starts the next message */
      break;
    }
    rows++;
  }
  k = bs__run(m, k, run);
  m[k++] = 0xF7;
  bs__u14(m, bs__u14(m, bs__u14(m, bs__u14(m, bs__head(m, 'B', BS_CMD_RECT), d->rx), d->ry), d->rw), rows);
  bs__rows_out(d, d->ry, d->ry + (int)rows);
  d->ry = (uint16_t)(d->ry + rows);
  return k;
}

/* version 1: the next part of the row going out, as runs: F0 7D 'F' 'S' 01 row col {count c0-6 c7-13 c14-15}... F7 */
static uint32_t bs__msg_v1(bs_dev* d) {
  uint8_t* m = d->msg;
  uint32_t x = d->vx, end = (uint32_t)(d->rx + d->rw), k = bs__u14(m, bs__u14(m, bs__head(m, 'F', 1), d->ry), x);
  while (x < end && k + 5u <= 70u) { /* (70 bytes at most: what version 1 viewers expect) */
    uint32_t c = bs__px(d, (int)x, d->ry), run = 1;
    while (x + run < end && run < 127u && bs__px(d, (int)(x + run), d->ry) == c) {
      run++;
    }
    m[k] = (uint8_t)run;
    k = bs__u14(m, k + 1, c);
    m[k++] = (uint8_t)(c >> 14);
    x += run;
  }
  m[k++] = 0xF7;
  d->vx = (uint16_t)x;
  if (x >= end) {
    bs__rows_out(d, d->ry, d->ry + 1);
    d->ry++;
    d->vx = d->rx;
  }
  return k;
}

uint32_t bs_dev_next(bs_dev* d, uint32_t now, const uint8_t** out) {
  uint8_t* m = d->msg;
  uint32_t k;
  if (!d->mode) {
    return 0;
  }
  if (now - d->heard >= BS_TIMEOUT) { /* nobody's looking now */
    d->mode = 0;
    d->busy = 0;
    d->synced = 1;
    bs__fill(d->pend, 0, sizeof d->pend);
    return 0;
  }
  *out = m;
  if (d->info) { /* F0 7D 'B' 'S' 01 version w h format F7 */
    d->info = 0;
    bs__head(m, 'B', BS_CMD_INFO);
    m[5] = BS_VERSION;
    k = bs__u14(m, bs__u14(m, 6, (uint32_t)d->w), (uint32_t)d->h);
    m[k++] = BS_RGB565;
    m[k++] = 0xF7;
    return k;
  }
  if (!d->busy && !bs__start(d)) {
    if (d->synced || d->mode != 2) {
      d->synced = 1;
      return 0;
    }
    d->synced = 1; /* F0 7D 'B' 'S' 03 F7: all that changed is out */
    m[bs__head(m, 'B', BS_CMD_SYNC)] = 0xF7;
    return 6;
  }
  k = d->mode == 2 ? bs__msg_v2(d) : bs__msg_v1(d);
  d->synced = 0;
  if (d->ry >= d->rend) {
    bs__done(d);
  }
  return k;
}

#endif /* BS_NO_DEVICE */

/* ---- the viewer */
#ifndef BS_NO_VIEWER

void bs_view_init(bs_view* v, uint16_t* fb, int w, int h) {
  bs__fill(v, 0, sizeof *v);
  v->fb = fb;
  v->bw = v->w = w;
  v->bh = v->h = h;
  bs_view_reset(v);
}

void bs_view_reset(bs_view* v) {
  v->ver = 0;
  v->tries = 0;
  v->all = 1;
  v->seen = 0;
  v->timed = 0;
}

uint32_t bs_view_hello(bs_view* v, uint32_t now, uint8_t out[BS_HELLO_MAX]) {
  uint32_t every = v->ver ? BS_HELLO_MS : BS_PROBE_MS;
  if (v->timed && now - v->at < every) {
    return 0;
  }
  if (v->ver == 2 && v->tries >= BS_PROBE_TRIES) {
    bs_view_reset(v); /* a version 2 device went quiet (another one, or other firmware): ask again */
  }
  if (!v->ver && v->tries >= BS_PROBE_TRIES) {
    v->ver = 1; /* no answer to version 2: version 1 firmware (or none) */
    v->all = 1;
    v->count = 0;
  }
  v->timed = 1;
  v->at = now;
  out[0] = 0xF0, out[1] = 0x7D;
  if (v->ver == 1 && ++v->count % BS_REPROBE) { /* F0 7D 'F' 'S' k F7: 01 all again (until something comes) */
    out[2] = 'F', out[3] = 'S', out[4] = (uint8_t)(v->all || !v->seen), out[5] = 0xF7;
    v->all = 0;
    return 6;
  }
  if (v->ver != 1) {
    v->tries++; /* (its INFO sets it back to 0) */
  }
  out[2] = 'B', out[3] = 'S', out[4] = BS_HELLO, out[5] = BS_VERSION; /* F0 7D 'B' 'S' 00 version flags F7 */
  out[6] = (uint8_t)(v->all || !v->seen || v->ver != 2 ? BS_HELLO_ALL : 0), out[7] = 0xF7;
  if (v->ver == 2) {
    v->all = 0;
  }
  return 8;
}

static void bs__put(bs_view* v, int x, int y, uint32_t c) {
  if (x < v->bw && y < v->bh) {
    v->fb[y * v->bw + x] = (uint16_t)c;
  }
}

int bs_view_sysex(bs_view* v, const uint8_t* m, uint32_t n, bs_rect* r) {
  if (n < 6 || m[0] != 0xF0 || m[1] != 0x7D || m[3] != 'S' || m[n - 1] != 0xF7 || (m[2] != 'B' && m[2] != 'F')) {
    return BS_NOT_MINE;
  }
  n--; /* (the F7) */
  if (m[2] == 'F') {
    uint32_t i, x, y, c;
    if (m[4] != 1 || n < 9) {
      return BS_OK; /* (a hello, the sound's name) */
    }
    y = m[5] | (uint32_t)m[6] << 7, x = m[7] | (uint32_t)m[8] << 7;
    r->x = (int)x, r->y = (int)y, r->h = 1;
    for (i = 9; i + 4 <= n; i += 4) {
      c = m[i + 1] | (uint32_t)m[i + 2] << 7 | (uint32_t)m[i + 3] << 14;
      for (uint32_t j = 0; j < m[i]; j++, x++) {
        bs__put(v, (int)x, (int)y, c);
      }
    }
    r->w = (int)x - r->x;
    v->seen = 1;
    return BS_DRAW;
  }
  switch (m[4]) {
    case BS_CMD_INFO:
      if (n < 11) {
        return BS_OK;
      }
      v->ver = 2;
      v->tries = 0;
      v->w = m[6] | m[7] << 7;
      v->h = m[8] | m[9] << 7;
      return BS_INFO;
    case BS_CMD_SYNC:
      return BS_FRAME;
    case BS_CMD_RECT: {
      uint16_t cache[BS_CACHE];
      uint32_t i = 13, c = 0, p = 0, total;
      int x0, y0, w, h;
      if (n < 13) {
        return BS_OK;
      }
      x0 = m[5] | m[6] << 7, y0 = m[7] | m[8] << 7, w = m[9] | m[10] << 7, h = m[11] | m[12] << 7;
      total = (uint32_t)(w * h);
      bs__fill(cache, 0, sizeof cache);
      while (i < n && p < total) {
        uint32_t b = m[i++], run = 1;
        if (b < 0x40u) { /* 00iiiiii: a color from the cache */
          c = cache[b];
        } else if (b < 0x60u) { /* 010nnnnn: the last color again, 1..32 times */
          run = (b & 31u) + 1u;
        } else if (b < 0x70u) { /* 0110nnnn b: 33..2080 times */
          run = ((b & 15u) << 7 | (i < n ? m[i++] : 0u)) + 33u;
        } else if (b < 0x74u) { /* 011100hh b b: a color */
          c = (b & 3u) << 14 | (uint32_t)(i < n ? m[i] : 0u) << 7 | (i + 1 < n ? m[i + 1] : 0u);
          i += 2;
          cache[bs_hash(c)] = (uint16_t)c;
        } else {
          break; /* (not in version 2: something newer) */
        }
        for (; run && p < total; run--, p++) {
          bs__put(v, x0 + (int)(p % (uint32_t)w), y0 + (int)(p / (uint32_t)w), c);
        }
      }
      r->x = x0, r->y = y0, r->w = w, r->h = h;
      v->seen = 1;
      return BS_DRAW;
    }
  }
  return BS_OK; /* (another viewer's hello, or something newer) */
}
#endif /* BS_NO_VIEWER */

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_IMPLEMENTED */
#endif /* BIGSCREEN_IMPLEMENTATION */
