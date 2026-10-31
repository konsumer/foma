/* SPDX-License-Identifier: GPL-3.0-only */
/* foma: Bluetooth MIDI on Felucca's platform (the bluetooth option). Part of fm1.c (the unity build).
 *
 * The stack is Henri David's, from Optimist (platform/ble, from the Bluetooth Core and BLE-MIDI specifications), with
 * felucca-bsp's radio drivers (hal/fm1_ble.h, fm1_ble_rf.h) and its start-up calibration (hal/fm1_rf_cal.h), so
 * nothing stored on the FM-1 is needed. No JieLi library, no tasks: the stack runs in the radio's two interrupts
 * (IRQ 45 and 29, priority 2: below the audio and TIMER5) and talks to the rest through single-producer rings:
 *   in:  the BLE interrupts write, the TIMER5 tick moves it to the MIDI ring (and the clock) as cable 2;
 *   out: midi_send writes (interrupts off), the BLE interrupts send it.
 * The FM-1 is a BLE MIDI peripheral: a computer or phone connects to it. No pairing (macOS and iOS connect anyway),
 * no central role, no audio. */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME APP_NAME /* advertised as "NAME XXXX", XXXX from this FM-1's address */
#endif
#define FELUCCA_BLE_DIAG 0
#define BLE_CENTRAL 0
#include "ble/ble_stack.c"

/* the SDK board's constant records (crystal load, PA bias; the Wi-Fi rates' gains left as they are): as fm1-base's
 * bt_own.c. The radio's own record (187) is measured at every start (fm1_rf_cal.h) */
static const struct ble_rf_trims BT_TRIMS = {{0x0B, 0x0B}, {1, 7, 4, 7, 11, 1, 7}, {0}, {0}};

static void bt_audio_hold(int hold); /* fm1.c: the audio stopped (1) or going again (0) */

#define BQ 64u
static struct {
  uint32_t pkt, t;
} bt_in[BQ];
static volatile uint32_t bi_w, bi_r;
static struct {
  uint32_t pkt;
  uint16_t ms;
} bt_out[BQ];
static volatile uint32_t bo_w, bo_r;
static uint8_t bt_addr[6];
static volatile uint8_t bt_up, bt_on, bt_live; /* the radio started; switched on; a central listens for MIDI */
static uint8_t bt_held[16][16];                /* notes a central holds on: ended when the link goes */

/* ---- the stack's side: the BLE interrupts */
BLE_API int ble_app_midi_peek(uint32_t *pkt, uint32_t *t_ms) {
  if (bo_r == bo_w) {
    return 0;
  }
  *pkt = bt_out[bo_r % BQ].pkt;
  *t_ms = bt_out[bo_r % BQ].ms;
  return 1;
}
BLE_API void ble_app_midi_pop(void) {
  if (bo_r != bo_w) {
    bo_r++;
  }
}
BLE_API void ble_app_midi_in(uint32_t pkt, uint16_t ts, uint16_t last_ts) {
  (void)ts, (void)last_ts;
  if (bi_w - bi_r < BQ) {
    bt_in[bi_w % BQ].pkt = pkt & ~0xF0u;
    bt_in[bi_w % BQ].t = fm1_ticks();
    RING_PUBLISH();
    bi_w++;
  }
}
BLE_API void ble_app_state(void) { bt_live = (uint8_t)(bt_on && ble_midi_ready()); }

/* ---- the platform's side */
static void bt_put(uint32_t pkt) { /* TIMER5: into the MIDI ring, cable 2 */
  if (mi_w - mi_r < MQ) {
    midi_in_q[mi_w % MQ] = (pkt & ~0xF0u) | 0x20u;
    RING_PUBLISH();
    mi_w++;
  }
}

static void bt_poll(void) { /* TIMER5, beside usb_poll */
  static uint8_t was_live;
  uint8_t live = bt_live;
  while (bi_r != bi_w) {
    uint32_t pk = bt_in[bi_r % BQ].pkt, t = bt_in[bi_r % BQ].t, cin = pk & 15u, st = (pk >> 8) & 0xFFu;
    bi_r++;
    if (cin == 0xFu && (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)) {
#if FOMA_MIDI_CLOCK
      if (clock_in(st, MIDI_BLE, t)) {
        continue;
      }
#else
      (void)t;
#endif
      bt_put(0xFu | st << 8);
    } else if (cin >= 8u && cin <= 0xEu && (st >> 4) == cin) {
      uint32_t ch = st & 15u, n = (pk >> 16) & 0x7Fu;
      if (cin == 9u && (pk >> 24)) {
        bt_held[ch][n >> 3] |= (uint8_t)(1u << (n & 7u));
      } else if (cin == 8u || cin == 9u) {
        bt_held[ch][n >> 3] &= (uint8_t) ~(1u << (n & 7u));
      }
      bt_put(pk);
    }
  }
  if (was_live && !live) { /* the link went: no stuck notes */
    for (uint32_t ch = 0; ch < 16u; ch++) {
      for (uint32_t n = 0; n < 128u; n++) {
        if (bt_held[ch][n >> 3] >> (n & 7u) & 1u) {
          bt_put(8u | (0x80u | ch) << 8 | n << 16);
        }
      }
      for (uint32_t i = 0; i < 16u; i++) {
        bt_held[ch][i] = 0;
      }
    }
  }
  was_live = live;
}

static void bt_midi_out(uint32_t pkt, uint32_t ms) { /* midi_send, interrupts off; ms: when it falls (midi_ms) */
  static uint16_t last;
  if ((int16_t)((uint16_t)ms - last) < 0) { /* never before the last one: a packet's time stamps only go forward */
    ms = last;
  }
  last = (uint16_t)ms;
  if (bt_live && bo_w - bo_r < BQ) {
    bt_out[bo_w % BQ].pkt = pkt;
    bt_out[bo_w % BQ].ms = (uint16_t)ms;
    RING_PUBLISH();
    bo_w++;
  }
}

/* the address: static random, from the flash's unique ID (the same on every boot) */
static void bt_addr_make(void) {
  static uint8_t uid[16]; /* (in RAM: the flash driver writes it with the flash off) */
  uint32_t h = 2166136261u, g = 0x9E3779B9u, i, same = 1, f = irq_save();
  if (flash_ok && FL_FAR(fl_uid_ram)(uid) == 0) {
    for (i = 1; i < 16u; i++) {
      same &= uid[i] == uid[0];
    }
  }
  irq_restore(f);
  if (!same) {
    for (i = 0; i < 16u; i++) {
      h = (h ^ uid[i]) * 16777619u;
      g = (g ^ uid[i]) * 2246822519u;
    }
  } else { /* no ID: a new one each boot */
    h ^= fm1_ticks();
    g ^= fm1_ticks() * 2654435761u;
  }
  bt_addr[0] = (uint8_t)h;
  bt_addr[1] = (uint8_t)(h >> 8);
  bt_addr[2] = (uint8_t)g;
  bt_addr[3] = (uint8_t)(g >> 8);
  bt_addr[4] = (uint8_t)(g >> 16);
  bt_addr[5] = (uint8_t)((g >> 24) | 0xC0u); /* static random: the top two bits set */
}

/* the first bt_enable: the two BLE vectors (masked), the radio and its calibration, then advertising. Nothing of
 * this runs at boot: a firmware that never turns Bluetooth on never touches the radio (attaching the vectors at
 * boot crashed the FM-1) */
static void bt_start(void) {
  uint32_t f;
  bt_addr_make();
  f = irq_save();
  fm1_guard_unlock_top();
  ble_hw_wl82_attach();
  fm1_guard_lock_top();
  irq_restore(f);
  ble_init(bt_addr, 1);
  fm1_rf_cal_on = 1;
  fm1_wdt_arm(0x0F); /* (the radio's start-up and calibration take a while) */
  bt_audio_hold(1);  /* the audio interrupt paused: a long audio() in the start-up's timed sequences hangs it */
  ble_hw_wl82_start(&BT_TRIMS);
  bt_audio_hold(0);
  fm1_wdt_arm(0x0D); /* (as fm1_cstart arms it) */
  bt_up = 1;
}

int bt_enable(int features) {
  uint8_t on = (uint8_t)((features & BT_MIDI) != 0);
  if (on && !bt_up) {
    bt_start();
  }
  if (bt_up) {
    bt_on = on;
    fm1_ble_irqs_hold(1);
    ble_enable(on);
    bt_live = (uint8_t)(on && ble_midi_ready());
    fm1_ble_irqs_hold(0);
  }
  return 0;
}

void bt_status(bt_status_t *s) {
  *s = (bt_status_t){0};
  s->ready = bt_up;
  s->ble_state = !bt_up || !bt_on ? BLE_OFF : bt_live       ? BLE_READY
                                          : ble_connected() ? BLE_CONNECTED
                                                            : BLE_ADVERTISING;
}

/* not on this platform: the central role (connecting to a controller) and audio from Bluetooth */
int ble_central(int on) { return on ? -3 : 0; }
int ble_scan(void) { return -3; }
int ble_device(int i, char name[25], int *rssi) { return (void)i, (void)name, (void)rssi, -1; }
int ble_connect(int i) { return (void)i, -3; }
int ble_disconnect(void) { return -3; }
int ble_forget(void) { return -3; }
const int32_t *bt_audio_in(void) { return 0; }
