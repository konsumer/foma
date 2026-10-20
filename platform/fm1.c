/* SPDX-License-Identifier: GPL-3.0-only */
/* foma on the FM-1: boot, the boot guard, the USB rescue, the updater, USB-MIDI, USB audio, the
 * panel, the screen, settings, the crash screen. Taken from Dinghy's platform.c (zvenson), which
 * is Felucca's (Leo Kuroshita, Hügelton Instruments) as SLOOP (isod89) carries it, and wired to
 * the API in fm1.h. Your code never needs to touch this file.
 *
 *   OCT- held at power-on   the USB rescue: no sound, only the updater
 *   crashes twice at boot   the USB rescue by itself
 *
 * APP_NAME, APP_VER, FELUCCA_ID come from tools/build.py (firmware.json). */
#include "fm1.h"

#include <stdint.h>

#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_lcd_hw.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "bootguard.h"

#ifndef APP_NAME
#define APP_NAME "FOMA"
#endif
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_950"
#endif
#ifndef APP_VER
#define APP_VER "0.0"
#endif

bootguard_t bootguard __attribute__((section(".noinit")));
static volatile uint32_t fm1_ms; /* milliseconds since power-on (TIMER5) */

// clang-format off
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
// clang-format on
#include "libc.c"
#include "lcd.c"
#define CV_MAX (240u * 32u) /* the platform's own one-line screens (update, rescue, crash) */
#include "gfx.c"

#define HALF_FRAMES 256u /* I2S half buffer: 5.8 ms */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define FELUCCA_OTA 1
#if FOMA_MIDI_CLOCK
#define CLOCK_MUL ((uint64_t)44100u * 4294967296u / (1000000u * FM1_TICKS_PER_US)) /* TIMER4 ticks -> frames */
#include "clock.c"
#define MIDI_RT(status, source) clock_in(status, source, fm1_ticks()) /* clock and transport: the helper's */
#endif
#include "usb.c"
#if FOMA_TRS_MIDI
#include "midi_uart.c"
#endif

/* ---- flash: read anywhere; erase / program only the update's staging area and the settings sectors
 * (Felucca's globals, 0xFC000..0xFF000: below them are the update records, above the MAC key) */
#define SETTINGS_AT 0x000FC000u
#define SETTINGS_END 0x000FF000u
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, SETTINGS_AT, SETTINGS_END))
#include "fm1_flash.h"
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n) {
  uint8_t *d = dst;
  while (n) {
    uint32_t k = n > 256u ? 256u : n, f = irq_save();
    int rc = FL_FAR(fl_read_ram)(off, d, k);
    irq_restore(f);
    if (rc) {
      return rc;
    }
    off += k;
    d += k;
    n -= k;
  }
  return 0;
}

static uint8_t recovery_active;
#define OTA_IDENTITY (recovery_active ? "FM-1_000" : FELUCCA_ID)
#include "ota.c"
static void recovery_poll(void);
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) {
  fm1_wdt_feed();
  if (recovery_active) {
    recovery_poll();
  }
}
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off) {
  uint32_t took;
  if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu)) {
    return -8;
  }
  return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n) {
  if (!ota_in_area(off, n)) {
    return -8;
  }
  return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code) {
  static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
  if (recovery_active) {
    return;
  }
  lcd_fill(0, 0, 240, 240, C_BLACK);
  draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
  if (step < 9u) {
    draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_WHITE, 1);
  } else {
    draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_WHITE, 1);
  }
}
static void ota_commit(const uint8_t *parm) {
  bootguard_clear(&bootguard);
  usb_detach();
  fm1_delay_ms(30);
  fm1_enter_update(parm);
}
#include "recovery.c"

/* ---- the screen, the sound helpers (shared with the emulator) */
#include "common.c"

static void screen_flush(void) {
  int y;
  if (fb_y0 >= fb_y1) {
    return;
  }
  for (y = fb_y0; y < fb_y1; y += 64) { /* strips of <= 30 KB per DMA */
    int h = fb_y1 - y > 64 ? 64 : fb_y1 - y;
    lcd_blit(0, (uint32_t)y, SCREEN_W, (uint32_t)h, fb + y * SCREEN_W);
  }
  fb_y0 = SCREEN_H;
  fb_y1 = 0;
}

/* ---- settings: [magic][length][crc32] then the bytes, across up to three 4 KiB sectors */
#define SETTINGS_MAGIC 0x414D4F46u /* "FOMA" */
_Static_assert(SETTINGS_MAX + 12u <= SETTINGS_END - SETTINGS_AT, "the settings must fit their sectors");
static uint32_t crc32(uint32_t c, const uint8_t *p, uint32_t n) {
  uint32_t k;
  while (n--) {
    c ^= *p++;
    for (k = 0; k < 8u; k++) {
      c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
  }
  return c;
}
int settings_load(void *p, uint32_t n) {
  uint8_t buf[256]; /* the flash driver wants RAM */
  uint32_t h[3], c = 0xFFFFFFFFu;
  if (!flash_ok || n > SETTINGS_MAX || st_read(SETTINGS_AT, h, sizeof h) || h[0] != SETTINGS_MAGIC || h[1] != n) {
    return -1;
  }
  for (uint32_t at = 0; at < n; at += sizeof buf) { /* check it all before p is touched */
    uint32_t k = n - at < sizeof buf ? n - at : sizeof buf;
    if (st_read(SETTINGS_AT + sizeof h + at, buf, k)) {
      return -1;
    }
    c = crc32(c, buf, k);
  }
  if (~c != h[2]) {
    return -1;
  }
  return st_read(SETTINGS_AT + sizeof h, p, n) ? -1 : 0;
}
int settings_save(const void *p, uint32_t n) { /* p is in RAM (the flash driver wants it) */
  uint32_t h[3] = {SETTINGS_MAGIC, n, 0}, took;
  int rc;
  if (!flash_ok || n > SETTINGS_MAX) {
    return -1;
  }
  h[2] = ~crc32(0xFFFFFFFFu, p, n);
  for (uint32_t at = SETTINGS_AT; at < SETTINGS_AT + sizeof h + n; at += 0x1000u) { /* only the sectors it needs */
    if ((rc = fl_erase4k(at, &took)) != 0) {
      return rc;
    }
  }
  if ((rc = fl_write(SETTINGS_AT + sizeof h, p, n)) != 0) {
    return rc;
  }
  return fl_write(SETTINGS_AT, (const uint8_t *)h, sizeof h); /* the header last: a cut save stays invalid */
}

/* ---- MIDI in from USB (cable 0) and the TRS jack (cable 1); out over USB, from loop() or audio() */
int midi_read(uint8_t *status, uint8_t *d1, uint8_t *d2) {
  uint32_t pk;
  if (mi_r == mi_w) {
    return 0;
  }
  pk = midi_in_q[mi_r % MQ];
  mi_r++;
  *status = (uint8_t)(pk >> 8);
  *d1 = (uint8_t)(pk >> 16);
  *d2 = (uint8_t)(pk >> 24);
  return pk & 0x10u ? MIDI_TRS : MIDI_USB;
}
void midi_send(uint8_t status, uint8_t d1, uint8_t d2) {
  uint32_t st = status, f = irq_save(); /* (loop() and audio() may both send) */
  midi_out_event((st >> 4) | st << 8 | (uint32_t)(d1 & 0x7Fu) << 16 | (uint32_t)(d2 & 0x7Fu) << 24);
  irq_restore(f);
}

/* ---- time */
uint32_t millis(void) { return fm1_ms; }
uint32_t micros(void) { return fm1_micros(); }

/* ---- the panel, as seen by one pass of loop() */
static uint32_t in_keys, in_kp, in_kr, in_btn, in_bp, in_br, in_master = 512;
static int32_t in_enc[ENC_COUNT];
static void input_snapshot(void) {
  uint32_t now = fm1_in.notes, taps = fm1_input_note_edges(), rel, e;
  in_kp = taps | (now & ~in_keys);
  in_kr = (in_keys & ~now) | (taps & ~now);
  in_keys = now;
  in_bp = fm1_input_edges(&rel);
  in_br = rel;
  in_btn = fm1_in.buttons;
  for (e = 0; e < ENC_COUNT; e++) {
    in_enc[e] = fm1_enc_take(e);
  }
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
void led_key(int k, int on) {
  if ((unsigned)k < KEY_COUNT) {
    fm1_led_key(14u + (uint32_t)k, on);
  }
}
void led_button(int b, int on) {
  if ((unsigned)b < BTN_COUNT) {
    fm1_led_key((uint32_t)b, on);
  }
}
void led_backlight(int level) {
  static const uint16_t NS[3] = {0, 1000, 4000}; /* the pulse a frame; a lit LED is ~95 us */
  static int8_t was = -1;
  level = level < 0 ? 0 : level > 2 ? 2
                                    : level;
  if (level == was) {
    return;
  }
  if (was < 0) { /* once: every LED behind a key or button */
    for (uint32_t p = 0; p < FM1_NCOL; p++) {
      for (uint32_t r = 1; r < 5u; r++) {
        if (FM1_KEYMAP[r][p] >= 0) {
          fm1_led_bg[p] |= (uint8_t)(1u << r);
        }
      }
    }
  }
  was = (int8_t)level;
  fm1_led_bg_ns = NS[level];
}
void print(const char *s) { (void)s; }

/* ---- the timer: 10 kHz key scan, milliseconds, USB at 2 kHz. It outranks the audio ISR; nested in a
 * render it only scans, counts and feeds the USB audio stream */
static volatile uint8_t in_audio;
void fm1_timer5_irq(void) {
  static uint32_t sub, owed, last, acc;
  uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
  fm1_timer5_ack();
  fm1_input_tick();
  acc += t0 - last;
  last = t0;
  while (acc >= 1000u * FM1_TICKS_PER_US) {
    acc -= 1000u * FM1_TICKS_PER_US;
    fm1_ms++;
  }
  if (usb_due) {
    owed = 1;
  }
  if (++sub == 10u) {
    sub = 0;
  }
  if (in_audio) { /* (the audio from the host keeps coming, and a clock on the TRS jack keeps its time) */
#if FOMA_USB_AUDIO
    if (usb_due) {
      uo_service();
    }
#endif
#if FOMA_TRS_MIDI
    uart_midi_poll();
#endif
    return;
  }
  if (owed) {
    usb_poll();
#if FOMA_TRS_MIDI
    uart_midi_poll();
#endif
  }
  owed = 0;
}
extern void isr_timer5(void);

/* ---- the sound: ALNK0 I2S, two halves; the app renders, MASTER scales, Q15 -> 24 bit (-6 dBFS ceiling) */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));
static volatile int32_t master_q12 = 2048;
#if FOMA_USB_AUDIO
static int32_t ain[HALF_WORDS]; /* this block of the host's audio (audio_in) */
static int ain_on;
const int32_t *audio_in(void) { return ain_on ? ain : 0; }
#endif

void fm1_alnk0_irq(void) { /* via isr_alnk0 (hal/fm1_isr.S) */
  uint8_t p;
  in_audio = 1;
  p = fm1_audio_pending();
  fm1_audio_ack_aux(p);
  if (p & FM1_AUDIO_HALF) {
    int32_t *o = &abuf[fm1_audio_free_half() * HALF_WORDS];
    uint32_t i;
    for (i = 0; i < HALF_WORDS; i++) {
      o[i] = 0;
    }
#if FOMA_USB_AUDIO
    ain_on = usb_audio_take(ain, HALF_FRAMES);
#endif
#if FOMA_MIDI_CLOCK
    clock_block(HALF_FRAMES, fm1_ticks());
#endif
    audio(o, HALF_FRAMES);
    for (i = 0; i < HALF_WORDS; i++) {
      int32_t v = (o[i] * master_q12) >> 12;
      o[i] = v > 32767 ? 32767 : v < -32767 ? -32767
                                            : v;
    }
    for (i = 0; i < HALF_WORDS; i++) {
      o[i] <<= 7;
    }
    fm1_audio_ack_half();
  }
  in_audio = 0;
}
extern void isr_alnk0(void);

/* ---- the main loop's duties: watchdog, USB, the installer's update request, the MASTER knob */
static void platform_poll(void) {
  static int32_t knob_avg = 512 * 16;
  int32_t a;
  fm1_wdt_feed();
  usb_retry(fm1_ms);
  ota_service();
  if (usb.ota_req) {
    usb.ota_req = 0;
    if (flash_ok) {
      ota_session(); /* returns only if nothing was committed */
      fb_dirty(0, SCREEN_H);
    }
  }
  if (usb.uboot_req) {
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, "UBOOT (USB)", C_WHITE, 1);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;
    fm1_enter_uboot();
  }
  if (fm1_ms >= 30000u && bootguard.pending) {
    bootguard_clear(&bootguard); /* 30 s up: this firmware boots */
  }
  if ((a = fm1_adc_read(FM1_ADC_MASTER)) >= 0) {
    uint32_t k10;
    knob_avg += (a * 16 - knob_avg) / 8;
    k10 = (uint32_t)(knob_avg / 16);
    in_master = k10;
    master_q12 = (int32_t)((k10 * k10) >> 8);
  }
}

static void fm1_fault(const fm1_crash_t *c) {
  char b[12];
  uint32_t i, t0, v[3] = {c->vec, c->pc, c->rets};
  fm1_audio_stop();
  lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
  draw_text_box(0, 20, 240, &FONT_S, APP_NAME " CRASH", C_WHITE, 1);
  for (i = 0; i < 3u; i++) {
    uint32_t k;
    for (k = 0; k < 8u; k++) {
      b[k] = "0123456789ABCDEF"[(v[i] >> (28u - 4u * k)) & 15u];
    }
    b[8] = 0;
    draw_text_box(10, 60 + i * 20, 220, &FONT_S, b, C_WHITE, 0);
  }
  t0 = fm1_ticks();
  while ((uint32_t)(fm1_ticks() - t0) < 8000u * 1000u * FM1_TICKS_PER_US) {
    fm1_wdt_feed();
  }
  fm1_reboot();
}

void fm1_cstart(void) {
  extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
  extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
  uint32_t *s, *d, p3, boot_mode;
  fm1_time_init();
  fm1_reset_reason();
  p3 = fm1_boot.p3_rst;
  fm1_wdt_arm(0x0D);
  if ((p3 & 1u) && !(p3 & (4u | 0x40u))) {
    bootguard_clear(&bootguard);
  }
  boot_mode = bootguard_begin(&bootguard);
  if (boot_mode == BOOT_ROM) {
    fm1_enter_uboot();
  }
  fm1_irq_init();
  for (d = _bss_start; d < _bss_end; d++) {
    *d = 0;
  }
  for (d = _pool_start; d < _pool_end; d++) {
    *d = 0;
  }
  for (s = _data_load, d = _data_start; d < _data_end; s++, d++) {
    *d = *s;
  }
  for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) {
    *d = *s;
  }
  fm1_mailbox_clear();
  fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
  fm1_boot.p3_rst = (uint8_t)p3;
  if (boot_mode == BOOT_RECOVERY || recovery_key()) {
    recovery_main(); /* OCT- at power-on: the USB rescue */
  }
  flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;
  ota_boot_cleanup();
  lcd_init();
  lcd_fill(0, 0, 240, 240, C_BLACK);
  fm1_input_init();
  fm1_adc_init();
  fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3); /* (abuf is zero: .bss) */
  usb_start();
#if FOMA_TRS_MIDI
  uart_midi_init();
#endif
  fm1_timer5_start(isr_timer5, 4);
  fm1_guard_lock_top();
  fm1_irq_enable_all();
  setup();
  screen_flush();
  for (;;) {
    platform_poll();
    input_snapshot();
    loop();
    screen_flush();
  }
}
