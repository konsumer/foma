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

/* ---- build options: firmware.json "options" turns them on or off (all on unless it says otherwise). What an
 * option adds is only there when it is on: using it otherwise fails the build */
#ifndef FOMA_USB_AUDIO
#define FOMA_USB_AUDIO 1 /* usb_audio: a computer or tablet plays into the FM-1 over USB: audio_in() */
#endif
#ifndef FOMA_TRS_MIDI
#define FOMA_TRS_MIDI 1 /* trs_midi: MIDI in on the 3.5 mm jack too, through midi_read() */
#endif
#ifndef FOMA_MIDI_CLOCK
#define FOMA_MIDI_CLOCK 1 /* midi_clock: one MIDI clock for audio(), its own or followed: clock_*() */
#endif
#ifndef FOMA_BLUETOOTH
#define FOMA_BLUETOOTH 0 /* bluetooth: BLE MIDI and audio from a phone, bt_*() / ble_*() (the "mvave" platform only) */
#endif
/* FOMA_MVAVE: 1 when built on M-VAVE's FM1 SDK ("platform": "mvave" in firmware.json), else Felucca's platform */
#ifndef FOMA_MVAVE
#define FOMA_MVAVE 0
#endif

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
void led_backlight(int level); /* every key and button glows dimly: 0 off, 1 low, 2 full */

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

/* ---- MIDI in over USB (and the TRS jack: trs_midi; BLE: bluetooth), out over USB (and BLE) (any channel; SysEx
 * belongs to the updater).
 * midi_read gives channel messages, and clock / start / continue / stop (0xF8 0xFA 0xFB 0xFC) unless the clock
 * helper (midi_clock) takes them. midi_send works from loop() and audio() */
enum { MIDI_USB = 1,
       MIDI_TRS = 2,
       MIDI_BLE = 4,
       MIDI_ANY = 7 };
int midi_read(uint8_t *status, uint8_t *d1, uint8_t *d2); /* 0: none; else where it came from: MIDI_USB, MIDI_TRS, MIDI_BLE */
void midi_send(uint8_t status, uint8_t d1, uint8_t d2);

#if FOMA_MIDI_CLOCK
/* ---- MIDI clock (midi_clock): one clock for the app, 24 ticks a beat. It runs at clock_tempo(), or follows the
 * clock coming in (clock_follow); either way, in audio(), clock_events() gives this block's ticks and start /
 * continue / stop, in order, each at the frame it falls on. Count ticks to keep time: a 16th note is 6.
 * clock_send(1) sends it out over USB MIDI too (a followed clock is passed on). Set it up from loop() */
enum { CLOCK_TICK = 1,
       CLOCK_START,
       CLOCK_CONTINUE,
       CLOCK_STOP };
typedef struct {
  uint16_t at;  /* the frame in this block */
  uint8_t what; /* CLOCK_* */
} clock_event_t;
#define CLOCK_EVENTS 32            /* at most this many a block */
void clock_follow(int from);       /* 0: its own tempo; MIDI_USB, MIDI_TRS or MIDI_ANY: the clock coming in */
void clock_send(int on);           /* 1: send it over USB MIDI */
void clock_tempo(uint32_t bpm100); /* its own tempo, BPM x 100 (12000: 120 BPM) */
void clock_start(void);            /* start, continue, stop from the app (following: the other device's too) */
void clock_continue(void);
void clock_stop(void);
int clock_events(const clock_event_t **ev); /* audio() only: this block's events; returns how many */
uint32_t clock_bpm(void);                   /* BPM x 100: its tempo, or the followed clock's (0: none coming) */
#endif

#if FOMA_BLUETOOTH
/* ---- Bluetooth (bluetooth: the "mvave" platform). Off until bt_enable(). BLE MIDI comes in through midi_read
 * (MIDI_BLE) and midi_send goes out over it too; a phone or computer can play into the FM-1 (bt_audio_in).
 * By default phones and computers connect to the FM-1 (it shows up as a BLE MIDI device); ble_central(1)
 * makes it connect to a BLE MIDI controller instead: ble_scan(), then ble_device() / ble_connect(). From loop();
 * the calls return 0 when taken, -2 busy (try again), -3 not ready */
enum { BT_AUDIO = 1,
       BT_MIDI = 2 };
enum { BLE_OFF,
       BLE_IDLE,
       BLE_ADVERTISING,
       BLE_SCANNING,
       BLE_CONNECTING,
       BLE_DISCOVERING,
       BLE_CONNECTED,
       BLE_READY,
       BLE_DISCONNECTING };
typedef struct {
  uint8_t ready;                          /* the stack is up */
  uint8_t audio_connected, audio_playing; /* a phone or computer, for audio */
  uint8_t ble_role, ble_state;            /* ble_central's, BLE_* (BLE_READY: MIDI flows) */
  uint8_t ble_count, ble_error;           /* devices found by ble_scan; the last failure (0 none) */
} bt_status_t;
int bt_enable(int features); /* BT_AUDIO | BT_MIDI, 0 off */
void bt_status(bt_status_t *s);
int ble_central(int on);                         /* 0: others connect to the FM-1; 1: it connects to a controller */
int ble_scan(void);                              /* central: 15 s of looking for BLE MIDI devices */
int ble_device(int i, char name[25], int *rssi); /* 0: device i of the scan */
int ble_connect(int i);
int ble_disconnect(void);
int ble_forget(void);             /* forget the BLE pairings */
const int32_t *bt_audio_in(void); /* in audio(): this block from Bluetooth (as audio_in), or 0 */
#endif

#if FOMA_USB_AUDIO
/* ---- audio from a computer or tablet over USB (usb_audio): the FM-1 is a 44.1 kHz stereo output it can pick.
 * In audio(): this block's frames, L R L R ..., +-32767 (process it, record it, add it to out), or 0 when nothing
 * plays. The platform doesn't play it itself */
const int32_t *audio_in(void);
#endif

/* ---- settings: up to SETTINGS_MAX bytes that survive power-off (three 4 KiB flash sectors, less a header). Saving stalls the sound for
 * ~50 ms a sector:
 * save when nothing plays, never in a loop (flash wears out) */
#define SETTINGS_MAX 12276u
int settings_load(void *p, uint32_t n); /* 0: loaded; else p is untouched */
int settings_save(const void *p, uint32_t n);

/* ---- helpers for sound without floats */
int32_t sine(uint32_t phase);          /* a full cycle over 2^32 -> -32767..32767 */
uint32_t note_inc(int note);           /* phase step per frame of a MIDI note (0..127) at SAMPLE_RATE */
uint32_t random32(void);               /* xorshift */
void format_int(char *buf, int32_t v); /* decimal, buf of 12 */
void print(const char *s);             /* to the emulator's console (nothing on the FM-1) */

/* big buffers (delay lines, samples): RAM is 96 KiB, the pool ~200 KiB more ("mvave": one RAM, POOL is ordinary)
 *   static int16_t delay[48000] POOL; */
#if defined(__wasm__) || FOMA_MVAVE
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif

#endif
