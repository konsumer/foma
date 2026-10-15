/* SPDX-License-Identifier: GPL-3.0-only */
/* foma: everything your firmware can use. The same calls work on the FM-1 and in the browser
 * emulator. Your code supplies three functions:
 *
 *   void setup(void)                           once, at power-on
 *   void loop(void)                            over and over; keep each pass short (a few ms)
 *   void audio(int32_t *out, uint32_t frames)  the sound: add to out, L R L R ..., +-32767
 *
 * The chip has no FPU: integers only (no float, no double, no 64-bit division). */
#ifndef FM1_H
#define FM1_H
#include <stdint.h>

void setup(void);
void loop(void);
void audio(int32_t *out, uint32_t frames);

/* ---- time */
#define SAMPLE_RATE 44100u
uint32_t millis(void); /* ms since power-on */
uint32_t micros(void); /* us, wraps every ~179 s: use differences */

/* ---- keys: 27, bit 0 = F3 .. bit 26 = G5. "pressed" / "released" are for this pass of loop() */
#define KEY_COUNT 27
#define KEY_FIRST_NOTE 53 /* MIDI note of key 0 (F3) */
uint32_t keys(void);      /* bit k: key k held */
int key_held(int k);
int key_pressed(int k);
int key_released(int k);

/* ---- buttons */
enum {
  BTN_OCT_DOWN,
  BTN_OCT_UP,
  BTN_FX,
  BTN_SEL,
  BTN_ENV,
  BTN_LFO,
  BTN_EDIT,
  BTN_GLO,
  BTN_HOME,
  BTN_SAVE,
  BTN_ARP,
  BTN_SEQ,
  BTN_PLAY,
  BTN_REC,
  BTN_COUNT
};
uint32_t buttons(void); /* bit b: button b held */
int button_held(int b);
int button_pressed(int b);
int button_released(int b);

/* ---- encoders: detents turned since the last pass of loop(), + = clockwise */
enum {
  ENC_SELECT,
  ENC_ALGORITHM,
  ENC_KNOB1,
  ENC_KNOB2,
  ENC_KNOB3,
  ENC_KNOB4,
  ENC_PRESETS,
  ENC_COUNT
};
int32_t encoder(int e);
uint32_t master_knob(void); /* the MASTER knob 0..1023; it is also the volume */

/* ---- LEDs */
void led_key(int k, int on);
void led_button(int b, int on);

/* ---- screen: 240 x 240, RGB565. Draw anywhere, any time in loop(); what changed is sent
 * after loop() returns. Sending is ~0.3 ms per row: draw only what changed. */
#define SCREEN_W 240
#define SCREEN_H 240
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define BLACK 0x0000u
#define WHITE 0xFFFFu
enum { FONT_SMALL,
       FONT_BIG }; /* 8 x 16 (ASCII); 16 x 32 (capitals, digits, signs) */
void clear(uint16_t color);
void pixel(int x, int y, uint16_t color);
void rect(int x, int y, int w, int h, uint16_t color);
void frame(int x, int y, int w, int h, uint16_t color); /* outline */
void line(int x0, int y0, int x1, int y1, uint16_t color);
int text(int x, int y, const char *s, uint16_t color, int font); /* returns the x after it */
int text_width(const char *s, int font);
int text_height(int font);

/* ---- MIDI over USB (any channel; SysEx belongs to the updater) */
int midi_read(uint8_t *status, uint8_t *d1, uint8_t *d2); /* 1: got one */
void midi_send(uint8_t status, uint8_t d1, uint8_t d2);

/* ---- settings: up to 256 bytes that survive power-off. Saving stalls the sound for ~50 ms:
 * save when nothing plays, never in a loop (flash wears out) */
int settings_load(void *p, uint32_t n); /* 0: loaded; else p is untouched */
int settings_save(const void *p, uint32_t n);

/* ---- helpers for sound without floats */
int32_t sine(uint32_t phase);          /* a full cycle over 2^32 -> -32767..32767 */
uint32_t note_inc(int note);           /* phase step per frame of a MIDI note (0..127) at SAMPLE_RATE */
uint32_t random32(void);               /* xorshift */
void format_int(char *buf, int32_t v); /* decimal, buf of 12 */
void print(const char *s);             /* to the emulator's console (nothing on the FM-1) */

/* big buffers (delay lines, samples): RAM is 96 KiB, the pool ~200 KiB more
 *   static int16_t delay[48000] POOL; */
#ifdef __wasm__
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif

#endif
