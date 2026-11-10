/* SPDX-License-Identifier: MIT */
/* bigscreen.h's tests: a device and a viewer over a pretend cable. cc -O2 -o /tmp/bstest test.c && /tmp/bstest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BIGSCREEN_IMPLEMENTATION
#include "bigscreen.h"

#define MW 320
#define MH 240

static uint16_t fb[MW * MH], seen[MW * MH];
static bs_dev dev;
static bs_view view;
static int fails, v1_only; /* v1_only: the device is old firmware: it doesn't hear version 2 hellos */
static uint32_t now, sent_bytes, frames;

#define CHECK(c, ...)                \
  do {                               \
    if (!(c)) {                      \
      printf("FAIL %d: ", __LINE__); \
      printf(__VA_ARGS__);           \
      printf("\n");                  \
      fails++;                       \
    }                                \
  } while (0)

static uint16_t swap16(uint16_t c) { return (uint16_t)(c >> 8 | c << 8); }

/* the viewer's hellos to the device; the device's messages to the viewer, until it has nothing more */
static void pump(int w, int h, int swapped) {
  uint8_t hello[BS_HELLO_MAX];
  uint32_t n = bs_view_hello(&view, now, hello);
  if (n && !(v1_only && hello[2] == 'B')) {
    CHECK(bs_dev_sysex(&dev, hello, n, now), "a hello the device didn't take");
  }
  const uint8_t* m;
  while ((n = bs_dev_next(&dev, now, &m))) {
    bs_rect r;
    CHECK(n <= BS_MSG, "a message of %u bytes", n);
    CHECK(m[0] == 0xF0 && m[n - 1] == 0xF7, "not a SysEx");
    for (uint32_t i = 1; i + 1 < n; i++) {
      CHECK(m[i] < 0x80, "byte %u of %u is %02X", i, n, m[i]);
    }
    int k = bs_view_sysex(&view, m, n, &r);
    CHECK(k != BS_NOT_MINE, "the viewer didn't take it");
    if (k == BS_FRAME) {
      frames++;
    }
    sent_bytes += n;
  }
  (void)w, (void)h, (void)swapped;
}

static int same(int w, int h, int swapped) {
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint16_t c = fb[y * w + x];
      if ((swapped ? swap16(c) : c) != seen[y * MW + x]) {
        printf("  differs at %d,%d: %04X vs %04X\n", x, y, swapped ? swap16(c) : c, seen[y * MW + x]);
        return 0;
      }
    }
  }
  return 1;
}

static void scene(int kind, int w, int h, int swapped) {
  int x0 = rand() % w, y0 = rand() % h, x1 = x0 + 1 + rand() % (w - x0), y1 = y0 + 1 + rand() % (h - y0);
  for (int y = y0; y < y1; y++) {
    for (int x = x0; x < x1; x++) {
      uint16_t c;
      switch (kind) {
        case 0:
          c = 0x1234;
          break; /* flat */
        case 1:
          c = (uint16_t)rand();
          break; /* noise: all literals */
        case 2:
          c = (uint16_t)((x * 7 + y * 3) & 0xFFFF);
          break; /* gradient */
        case 3:
          c = (rand() & 3) ? 0x0000 : (uint16_t)(0x8410 + (rand() & 7) * 0x0841);
          break; /* text-ish */
        default:
          c = (x / 5 + y / 7) & 1 ? 0xFFFF : 0x001F;
          break; /* checks */
      }
      fb[y * w + x] = swapped ? swap16(c) : c;
    }
  }
  bs_dev_changed(&dev, x0, y0, x1, y1);
}

static void run(int w, int h, int swapped, int rounds) {
  memset(fb, 0, sizeof fb);
  memset(seen, 0xAA, sizeof seen);
  bs_dev_init(&dev, fb, w, h, w, swapped);
  bs_view_init(&view, seen, MW, MH);
  for (int i = 0; i < 6; i++, now += 600) {
    pump(w, h, swapped); /* hello: version 2, or (v1_only) version 1 after 3 */
  }
  CHECK(view.ver == (v1_only ? 1 : 2), "speaking version %d", view.ver);
  CHECK(v1_only || (view.w == w && view.h == h), "INFO said %dx%d", view.w, view.h);
  CHECK(same(w, h, swapped), "%dx%d%s: first screen", w, h, swapped ? " swapped" : "");
  for (int r = 0; r < rounds; r++) {
    scene(rand() % 5, w, h, swapped);
    if (rand() & 1) {
      pump(w, h, swapped);
    } else { /* (part of it, and changes while a rectangle is half out) */
      const uint8_t* m;
      uint32_t n;
      bs_rect rr;
      for (int k = rand() % 4; k-- && (n = bs_dev_next(&dev, now, &m));) {
        bs_view_sysex(&view, m, n, &rr);
      }
      continue;
    }
    now += 7;
    CHECK(same(w, h, swapped), "%dx%d%s: round %d", w, h, swapped ? " swapped" : "", r);
  }
  pump(w, h, swapped);
  CHECK(same(w, h, swapped), "%dx%d%s: at the end", w, h, swapped ? " swapped" : "");
}

int main(void) {
  srand(1);
  run(240, 240, 1, 400);
  run(240, 240, 0, 400);
  run(100, 70, 0, 300); /* not whole tiles */
  run(320, 240, 0, 200);
  v1_only = 1; /* old firmware: the viewer falls back to version 1 */
  run(240, 240, 1, 200);
  run(100, 70, 0, 200);
  /* ... and the firmware's updated: the viewer comes up to version 2 by itself */
  v1_only = 0;
  for (int i = 0; i < BS_REPROBE + 2; i++, now += 1000) {
    pump(100, 70, 0);
  }
  CHECK(view.ver == 2, "still version %d after the update", view.ver);
  CHECK(same(100, 70, 0), "after the update");
  /* nobody's looking: nothing goes out, and checking is skipped */
  now += BS_TIMEOUT + 1;
  const uint8_t* m;
  CHECK(!bs_dev_next(&dev, now, &m) && !bs_dev_active(&dev), "still sending with nobody looking");
  printf("%s (%u bytes, %u frames)\n", fails ? "FAILED" : "ok", sent_bytes, frames);
  return fails != 0;
}
