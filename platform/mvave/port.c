/* SPDX-License-Identifier: GPL-3.0-only */
/* foma on M-VAVE's FM1 SDK: the API of fm1.h (foma's) over bridge.h (sdk.c and board/: the SDK's side).
 *   control task: groove_screen_init -> setup(); groove_screen_control -> one pass of loop(), then the screen
 *   audio ISR:    a block of 128 frames: the clock, the audio coming in (USB, Bluetooth), audio(), out (and to
 *                 the computer's recording, over USB)
 *   routing task: MIDI in (USB, TRS, BLE) into a ring loop() reads; clock and transport to the clock helper */
#include <stdint.h>

#include "fm1.h"
#include "bridge.h"

/* Felucca's tiny libc (the number formatting common.c uses); the SDK's libc has the mem* functions */
#define memset foma_memset
#define memcpy foma_memcpy
#define memcmp foma_memcmp
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp

typedef struct { /* as gfx.c */
  uint8_t h;
  uint8_t pad;
  uint8_t first, last;
  const uint8_t *adv;
  const uint8_t *bw;
  const uint16_t *off;
  const uint8_t *data;
} felucca_font_t;
#include "felucca_font.h"
#include "common.c"

#define TICKS_PER_US (MV_TICK_HZ / 1000000u)
uint32_t millis(void) { return mv_millis(); }
uint32_t micros(void) { return mv_ticks() / TICKS_PER_US; }

#if FOMA_MIDI_CLOCK
#define CLOCK_MUL ((uint64_t)44100u * 4294967296u / 1000000u) /* micros -> frames */
#include "clock.c"
#endif

/* ---- the panel, as one pass of loop() sees it */
static uint32_t in_keys, in_kp, in_kr, in_btn, in_bp, in_br, in_master = 512;
static int32_t in_enc[ENC_COUNT];
static volatile int32_t master_q12 = 2048;

static void input_snapshot(void) {
  static int32_t knob_avg = 512 * 16;
  struct mv_controls c;
  mv_controls(&c);
  in_kp = c.keys & ~in_keys;
  in_kr = in_keys & ~c.keys;
  in_keys = c.keys;
  in_bp = c.buttons & ~in_btn;
  in_br = in_btn & ~c.buttons;
  in_btn = c.buttons;
  for (int e = 0; e < ENC_COUNT; e++) {
    in_enc[e] = c.enc[e];
  }
  knob_avg += ((int32_t)c.knob * 16 - knob_avg) / 8;
  in_master = (uint32_t)(knob_avg / 16);
  master_q12 = (int32_t)((in_master * in_master) >> 8);
}
uint32_t keys(void) { return in_keys; }
int key_held(int k) { return (unsigned)k < KEY_COUNT && (in_keys >> k & 1u); }
int key_pressed(int k) { return (unsigned)k < KEY_COUNT && (in_kp >> k & 1u); }
int key_released(int k) { return (unsigned)k < KEY_COUNT && (in_kr >> k & 1u); }
uint32_t buttons(void) { return in_btn; }
int button_held(int b) { return (unsigned)b < BTN_COUNT && (in_btn >> b & 1u); }
int button_pressed(int b) { return (unsigned)b < BTN_COUNT && (in_bp >> b & 1u); }
int button_released(int b) { return (unsigned)b < BTN_COUNT && (in_br >> b & 1u); }
int32_t encoder(int e) { return (unsigned)e < ENC_COUNT ? in_enc[e] : 0; }
uint32_t master_knob(void) { return in_master; }

static uint32_t led_k, led_b; /* what is lit: the SDK is told only of changes */
void led_key(int k, int on) {
  if ((unsigned)k < KEY_COUNT && ((led_k >> k & 1u) != (uint32_t) !!on)) {
    led_k ^= 1u << k;
    mv_led(14u + (unsigned)k, on);
  }
}
void led_button(int b, int on) {
  if ((unsigned)b < BTN_COUNT && ((led_b >> b & 1u) != (uint32_t) !!on)) {
    led_b ^= 1u << b;
    mv_led((unsigned)b, on);
  }
}
void led_backlight(int level) { (void)level; } /* (the SDK's panel has no dim layer yet) */
void print(const char *s) { (void)s; }

/* ---- MIDI in: the routing task fills the ring, loop() reads it */
#define MQ 64u
static uint32_t mi_q[MQ];
static volatile uint32_t mi_w, mi_r;
void mv_midi_in(unsigned src, const uint8_t *m, unsigned n) {
  static const uint8_t SRC[3] = {MIDI_TRS, MIDI_USB, MIDI_BLE};
  if (src >= 3 || !n || n > 3) {
    return;
  }
#if FOMA_MIDI_CLOCK
  if (m[0] == 0xF8 || m[0] == 0xFA || m[0] == 0xFB || m[0] == 0xFC) {
    clock_in(m[0], SRC[src], micros());
    return;
  }
#endif
  if (mi_w - mi_r < MQ) {
    mi_q[mi_w % MQ] = SRC[src] | (uint32_t)m[0] << 8 | (uint32_t)(n > 1 ? m[1] : 0) << 16 | (uint32_t)(n > 2 ? m[2] : 0) << 24;
    __asm__ volatile("" ::
                         : "memory");
    mi_w++;
  }
}
int midi_read(uint8_t *status, uint8_t *d1, uint8_t *d2) {
  if (mi_r == mi_w) {
    return 0;
  }
  uint32_t pk = mi_q[mi_r % MQ];
  __asm__ volatile("" ::
                       : "memory");
  mi_r++;
  *status = (uint8_t)(pk >> 8);
  *d1 = (uint8_t)(pk >> 16);
  *d2 = (uint8_t)(pk >> 24);
  return (int)(pk & 0xFFu);
}

/* ---- MIDI out: midi_send (loop() or audio()) queues, the control task sends (the SDK's rule) */
static uint32_t mo_q[MQ];
static volatile uint32_t mo_w, mo_r;
void midi_send(uint8_t status, uint8_t d1, uint8_t d2) {
  uint32_t s = mv_irq_save();
  if (mo_w - mo_r < MQ) {
    mo_q[mo_w % MQ] = status | (uint32_t)(d1 & 0x7Fu) << 8 | (uint32_t)(d2 & 0x7Fu) << 16;
    mo_w++;
  }
  mv_irq_restore(s);
}
void mv_midi_flush(void) {
  while (mo_r != mo_w) {
    uint32_t pk = mo_q[mo_r % MQ];
    uint8_t m[3] = {(uint8_t)pk, (uint8_t)(pk >> 8), (uint8_t)(pk >> 16)};
    unsigned n = m[0] >= 0xF8 || m[0] == 0xF6 ? 1 : (m[0] & 0xE0) == 0xC0 || m[0] == 0xF1 || m[0] == 0xF3 ? 2
                                                                                                          : 3;
    if (mv_midi_send(m, n) == -2) {
      return; /* full: the rest next time */
    }
    mo_r++;
  }
}

/* ---- the sound: a block of 128 frames */
#if FOMA_USB_AUDIO
static int32_t ain[2 * MV_BLOCK];
static int ain_on;
const int32_t *audio_in(void) { return ain_on ? ain : 0; }
#endif
#if FOMA_BLUETOOTH
static int32_t btin[2 * MV_BLOCK];
static int btin_on;
const int32_t *bt_audio_in(void) { return btin_on ? btin : 0; }
#endif
void mv_render(int32_t *q23) {
  static int32_t blk[2 * MV_BLOCK];
#if FOMA_USB_AUDIO
  ain_on = mv_usb_in(ain);
#endif
#if FOMA_BLUETOOTH
  btin_on = mv_bt_in(btin);
#endif
#if FOMA_MIDI_CLOCK
  clock_block(MV_BLOCK, micros());
#endif
  for (unsigned i = 0; i < 2 * MV_BLOCK; i++) {
    blk[i] = 0;
  }
  audio(blk, MV_BLOCK);
  int32_t g = master_q12;
  for (unsigned i = 0; i < 2 * MV_BLOCK; i++) { /* MASTER, then Q15 -> Q23 at -6 dBFS (as on Felucca's platform) */
    int32_t v = (blk[i] * g) >> 12;
    v = v > 32767 ? 32767 : v < -32767 ? -32767
                                       : v;
    q23[i] = v << 7;
  }
  mv_usb_out(q23);
}

/* ---- settings: [magic][length][crc32] then the bytes, at the start of the user partition */
#define SETTINGS_MAGIC 0x414D4F46u /* "FOMA" */
static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n) {
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) {
      c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
  }
  return c;
}
int settings_load(void *p, uint32_t n) {
  uint8_t buf[256];
  uint32_t h[3], c = 0xFFFFFFFFu;
  if (n > SETTINGS_MAX || mv_flash_size() < 12u + n || mv_flash_read(0, h, sizeof h) || h[0] != SETTINGS_MAGIC || h[1] != n) {
    return -1;
  }
  for (uint32_t at = 0; at < n; at += sizeof buf) { /* check it all before p is touched */
    uint32_t k = n - at < sizeof buf ? n - at : sizeof buf;
    if (mv_flash_read(12u + at, buf, k)) {
      return -1;
    }
    c = crc32(c, buf, k);
  }
  if (~c != h[2]) {
    return -1;
  }
  return mv_flash_read(12u, p, n) ? -1 : 0;
}
int settings_save(const void *p, uint32_t n) {
  uint32_t h[3] = {SETTINGS_MAGIC, n, ~crc32(0xFFFFFFFFu, p, n)};
  int rc;
  if (n > SETTINGS_MAX || mv_flash_size() < 12u + n) {
    return -1;
  }
  for (uint32_t at = 0; at < 12u + n; at += 4096u) {
    if ((rc = mv_flash_erase(at)) != 0) {
      return rc;
    }
  }
  if ((rc = mv_flash_write(12u, p, n)) != 0) {
    return rc;
  }
  return mv_flash_write(0, h, sizeof h); /* the header last: a cut save stays invalid */
}

#if FOMA_BLUETOOTH
/* ---- Bluetooth */
int bt_enable(int features) { return mv_bt_configure((unsigned)features & 3u); }
void bt_status(bt_status_t *s) {
  uint8_t f, pending;
  mv_bt_status(&f, &s->ready, &s->audio_connected, &s->audio_playing);
  mv_ble_status(&s->ble_role, &s->ble_state, &s->ble_count, &s->ble_error, &pending);
}
int ble_central(int on) { return mv_ble_role(on); }
int ble_scan(void) { return mv_ble_scan(); }
int ble_device(int i, char name[25], int *rssi) {
  int8_t r;
  uint8_t midi;
  if (i < 0 || mv_ble_device((unsigned)i, name, &r, &midi)) {
    return -1;
  }
  *rssi = r;
  return 0;
}
int ble_connect(int i) { return i < 0 ? -1 : mv_ble_connect((unsigned)i); }
int ble_disconnect(void) { return mv_ble_disconnect(); }
int ble_forget(void) { return mv_ble_forget(); }
#endif

/* ---- the control task */
static void screen_flush(void) {
  if (fb_y0 < fb_y1) {
    mv_screen(fb + fb_y0 * SCREEN_W, (unsigned)fb_y0, (unsigned)(fb_y1 - fb_y0));
  }
  fb_y0 = SCREEN_H;
  fb_y1 = 0;
}
void mv_setup(void) {
  input_snapshot();
  setup();
  screen_flush();
}
void mv_loop(void) {
  input_snapshot();
  loop();
  screen_flush();
  mv_midi_flush();
}
