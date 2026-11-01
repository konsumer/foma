/* SPDX-License-Identifier: GPL-3.0-only */
/* foma: Bluetooth MIDI on Felucca's platform (the bluetooth option). Part of fm1.c (the unity build).
 *
 * The stack is Henri David's, from Optimist (platform/ble, from the Bluetooth Core and BLE-MIDI specifications), with
 * felucca-bsp's radio drivers (hal/fm1_ble.h, fm1_ble_rf.h) and its start-up calibration (hal/fm1_rf_cal.h), so
 * nothing stored on the FM-1 is needed. No JieLi library, no tasks: the stack runs in the radio's two interrupts
 * (IRQ 45 and 29, priority 2: below the audio and TIMER5) and talks to the rest through single-producer rings:
 *   in:  the BLE interrupts write, the TIMER5 tick moves it to the MIDI ring (and the clock) as cable 2;
 *   out: midi_send writes (interrupts off), the BLE interrupts send it.
 * The FM-1 is a BLE MIDI peripheral (a computer or phone connects to it), and it can connect to one itself: ble_scan,
 * ble_device, ble_connect (the central role, at the stack's minimal level: no pairing, so a controller or another
 * FM-1, not a device that insists on pairing). One link at a time. No pairing as a peripheral either (macOS and
 * iOS connect anyway), no audio. */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME APP_NAME /* advertised as "NAME XXXX", XXXX from this FM-1's address */
#endif
#define FELUCCA_BLE_DIAG 0
#ifndef BLE_CENTRAL
#define BLE_CENTRAL 3 /* the central role without pairing (ble_cfg.h) */
#endif
#include "ble/ble_stack.c"
#include "ble/ble_vm.c" /* the stock firmware's stored radio records (read only) */

/* the radio's four records (106, 107, 108, 187): this FM-1's own from the stock firmware's VM when it's still there
 * (ble_vm.c: the factory's, read only), else the SDK board's constant ones with the captured start-up's 187 fields
 * (as fm1-base's bt_own.c, without its measuring). Our own calibration (hal/fm1_rf_cal.h) is not run: the radio's
 * writes after it, in group 10 of the start-up, left the chip hanging or faulting in most builds (fomajam with or
 * without the central role, the code in flash or in RAM, 2026-10-31), and ran only in some, by their layout */
static struct ble_rf_trims bt_trims = {{0x0B, 0x0B}, {1, 7, 4, 7, 11, 1, 7}, {0}, {0}};
static volatile struct { /* which records the last start used, kept over a reset (fm1.c platform_frame 0x14) */
  uint32_t magic, src, area, nrec;
  uint8_t x187[68], pad[44];
} bt_rf_kept __attribute__((section(".noinit.ble")));

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
static uint8_t bt_held[16][16];
/* notes a central holds on: ended when the link goes */
#if BLE_CENTRAL
static struct ble_scan_tab bt_scan; /* the BLE MIDI devices around (ble_scan) */
static uint32_t bt_scan_end, bt_aged;
static uint32_t bt_adv_at; /* when our scan's end or a cancel started advertising again (fm1_ms) */
/* advertising stopped right after it started leaves the engine deaf in the next mode (nothing heard while scanning or
 * initiating, 2026-10-31): a scan or a connect waits until advertising has run BT_SETTLE_MS */
#define BT_SETTLE_MS 250u
static int bt_settled(void) { return fm1_ms - bt_adv_at >= BT_SETTLE_MS; }
static uint8_t bt_role; /* ble_central */
#endif

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
/* when a message was meant, from its BLE-MIDI time stamp (the sender's ms, 13 bits): our ms minus its stamp is the
 * two clocks' offset plus the trip; its smallest (the quickest trip) is kept, creeping up 1 ms every 2 s to follow the
 * clocks' drift. A message's time is its arrival less what it was late past that, plus BT_CLOCK_DELAY_MS, so a clock
 * comes out as evenly as it was sent (clock.c holds a tick that isn't due yet) */
#define BT_CLOCK_DELAY_MS 6u
static uint32_t bt_when(uint16_t ts) {
  static uint32_t off, off_at;
  static uint8_t have;
  uint32_t now = fm1_ticks(), ms = fm1_ms, d = (ms - ts) & 8191u, late = (d - off) & 8191u;
  if (!have || late > 4096u) { /* the quickest trip yet (or the first) */
    off = d, have = 1, late = 0, off_at = ms;
  } else if (ms - off_at > 2000u) {
    off = (off + 1u) & 8191u, off_at = ms, late = late ? late - 1u : 0u;
  }
  late = late > 50u ? 50u : late; /* (a stamp far off: as it came) */
  return now - late * 1000u * FM1_TICKS_PER_US + BT_CLOCK_DELAY_MS * 1000u * FM1_TICKS_PER_US;
}

BLE_API void ble_app_midi_in(uint32_t pkt, uint16_t ts, uint16_t last_ts) {
  (void)last_ts;
  if (bi_w - bi_r < BQ) {
    bt_in[bi_w % BQ].pkt = pkt & ~0xF0u;
    bt_in[bi_w % BQ].t = bt_when(ts);
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

static int bt_vm_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t n) { /* ble_vm_scan's flash reads */
  (void)ctx;
  return st_read(off, dst, n);
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
  {
    struct ble_vm_info vi;
    struct ble_rf_trims vm;
    uint32_t i, src = flash_ok && ble_vm_scan(bt_vm_read, 0, &vi, &vm, 0, 0) ? 1u : 0u;
    if (src) {
      bt_trims = vm;
    }
    bt_rf_kept.magic = 0x52464B54u; /* "TKFR" */
    bt_rf_kept.src = src;
    bt_rf_kept.area = src ? vi.area : 0u;
    bt_rf_kept.nrec = src ? vi.nrec : 0u;
    for (i = 0; i < 68u; i++) {
      bt_rf_kept.x187[i] = bt_trims.x187[i];
    }
  }
  fm1_rf_cal_on = 2; /* this unit's own VCO bands (the replay's are another FM-1's: reception went bad seconds
                      * after the start), the stored 187 for the rest (our trim hung the start-up: hal/fm1_ble_rf.h) */
  fm1_wdt_arm(0x0F); /* (the radio's start-up takes a while) */
  bt_audio_hold(1);  /* the audio interrupt paused: a long audio() in the start-up's timed sequences hangs it */
  ble_hw_wl82_start(&bt_trims);
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

#if BLE_CENTRAL
void bt_status(bt_status_t *s) {
  uint8_t idx[BLE_SCAN_N], cs = bt_up ? ble_central_state() : BLE_CS_IDLE;
  *s = (bt_status_t){0};
  s->ready = bt_up;
  s->ble_role = bt_role;
  s->ble_count = (uint8_t)(bt_up ? ble_scan_list(&bt_scan, idx) : 0u);
  s->ble_error = bt_up ? ble_central_fail() : 0;
  s->ble_state = !bt_up || !bt_on                   ? BLE_OFF
                 : bt_live                          ? BLE_READY
                 : cs == BLE_CS_CONNECTING          ? BLE_CONNECTING
                 : cs == BLE_CS_SETUP               ? BLE_DISCOVERING
                 : ble_connected()                  ? BLE_CONNECTED
                 : bt_scan_end && ble_ll_scanning() ? BLE_SCANNING
                                                    : BLE_ADVERTISING;
}

/* the main loop (platform_poll): the scan's reports into the list, and its end */
static void bt_loop(void) {
  uint8_t pdu[40];
  uint16_t rssi;
  uint8_t n;
  if (!bt_up) {
    return;
  }
  while ((n = ble_ll_scan_take(pdu, &rssi)) != 0) {
    ble_scan_add(&bt_scan, pdu, n, rssi, fm1_ms);
  }
  if (fm1_ms - bt_aged > 1000u) {
    bt_aged = fm1_ms;
    ble_scan_age(&bt_scan, fm1_ms);
  }
  if (bt_scan_end && (int32_t)(fm1_ms - bt_scan_end) >= 0) {
    bt_scan_end = 0;
    fm1_ble_irqs_hold(1);
    ble_ll_scan(0);
    fm1_ble_irqs_hold(0);
    bt_adv_at = fm1_ms;
  }
}

/* ---- the central role: the FM-1 connects to a BLE MIDI device (a controller, another FM-1) */
int ble_central(int on) {
  bt_role = (uint8_t)(on != 0);
  if (!on) {
    ble_disconnect();
  }
  return 0;
}
int ble_scan(void) {
  if (!bt_up || !bt_on) {
    return -3;
  }
  if (!bt_settled()) {
    return -2; /* (busy: ask again) */
  }
  ble_scan_clear(&bt_scan);
  bt_scan_end = (fm1_ms + 15000u) | 1u;
  fm1_ble_irqs_hold(1);
  ble_ll_scan(1);
  fm1_ble_irqs_hold(0);
  return 0;
}
int ble_device(int i, char name[25], int *rssi) {
  uint8_t idx[BLE_SCAN_N];
  uint32_t n = bt_up ? ble_scan_list(&bt_scan, idx) : 0u, k;
  if (i < 0 || (uint32_t)i >= n) {
    return -1;
  }
  const struct ble_found *e = &bt_scan.e[idx[i]];
  for (k = 0; k < 24u && e->name[k]; k++) {
    name[k] = e->name[k];
  }
  name[k] = 0;
  if (rssi) { /* (no gain table: the list's three bars as a rough dBm) */
    *rssi = -100 + 20 * (int)ble_scan_bars(&bt_scan, idx[i]);
  }
  return 0;
}
int ble_device_addr(int i, uint8_t addr[6], int *random) {
  uint8_t idx[BLE_SCAN_N];
  uint32_t n = bt_up ? ble_scan_list(&bt_scan, idx) : 0u;
  if (i < 0 || (uint32_t)i >= n) {
    return -1;
  }
  ble_cpy(addr, bt_scan.e[idx[i]].addr, 6);
  if (random) {
    *random = bt_scan.e[idx[i]].addr_rand;
  }
  return 0;
}
int ble_find(const uint8_t addr[6], int random, const char *name) {
  uint8_t idx[BLE_SCAN_N];
  uint32_t n = bt_up ? ble_scan_list(&bt_scan, idx) : 0u, i, k;
  for (i = 0; i < n; i++) { /* its address */
    const struct ble_found *e = &bt_scan.e[idx[i]];
    if (e->addr_rand == (uint8_t)(random != 0) && ble_eq(e->addr, addr, 6)) {
      return (int)i;
    }
  }
  for (i = 0; name && name[0] && i < n; i++) { /* else its name (an address that changes) */
    const struct ble_found *e = &bt_scan.e[idx[i]];
    for (k = 0; name[k] && e->name[k] == name[k]; k++) {
    }
    if (!name[k] && !e->name[k]) {
      return (int)i;
    }
  }
  return -1;
}
int ble_connect(int i) {
  uint8_t idx[BLE_SCAN_N];
  uint32_t n = bt_up && bt_on ? ble_scan_list(&bt_scan, idx) : 0u;
  struct ble_peer p;
  int ok;
  if (i < 0 || (uint32_t)i >= n) {
    return -1;
  }
  if (!bt_settled()) {
    return -2;
  }
  ble_zero((uint8_t *)&p, sizeof p);
  ble_cpy(p.addr, bt_scan.e[idx[i]].addr, 6);
  p.addr_rand = bt_scan.e[idx[i]].addr_rand;
  bt_scan_end = 0;
  fm1_ble_irqs_hold(1);
  ok = ble_central_connect(&p); /* (from the scan: it stops it. A scan stopped first starts advertising again, and
                                 * initiating right after that left the engine deaf: nothing heard, no link) */
  ble_ll_scan(0);               /* (initiating: only the wish to scan goes, so the scan doesn't come back after) */
  fm1_ble_irqs_hold(0);
  return ok ? 0 : -2;
}
int ble_disconnect(void) {
  if (!bt_up) {
    return -3;
  }
  fm1_ble_irqs_hold(1);
  ble_central_cancel(); /* (a computer connected to us stays) */
  fm1_ble_irqs_hold(0);
  bt_adv_at = fm1_ms;
  return 0;
}
int ble_forget(void) { return 0; } /* (no pairings are kept) */

#else
/* (BLE_CENTRAL 0: a peripheral only) */
void bt_status(bt_status_t *s) {
  *s = (bt_status_t){0};
  s->ready = bt_up;
  s->ble_state = !bt_up || !bt_on ? BLE_OFF : bt_live ? BLE_READY
                                          : ble_connected() ? BLE_CONNECTED
                                                            : BLE_ADVERTISING;
}
static void bt_loop(void) {}
int ble_central(int on) { return on ? -3 : 0; }
int ble_scan(void) { return -3; }
int ble_device(int i, char name[25], int *rssi) { return (void)i, (void)name, (void)rssi, -1; }
int ble_device_addr(int i, uint8_t addr[6], int *random) { return (void)i, (void)addr, (void)random, -1; }
int ble_find(const uint8_t addr[6], int random, const char *name) { return (void)addr, (void)random, (void)name, -1; }
int ble_connect(int i) { return (void)i, -3; }
int ble_disconnect(void) { return -3; }
int ble_forget(void) { return -3; }
#endif

#if BLE_CENTRAL
/* for ./fm1 flash --bt (fm1.c platform_frame): 0x15 the state, 0x16 scan entry i. 32 bytes each */
static void bt_report(uint32_t cmd, uint32_t i, uint8_t o[32]) {
  uint8_t idx[BLE_SCAN_N];
  uint32_t n = bt_up ? ble_scan_list(&bt_scan, idx) : 0u, k;
  bt_status_t b;
  for (k = 0; k < 32u; k++) {
    o[k] = 0;
  }
  if (cmd == 0x15u) {
    bt_status(&b);
    o[0] = bt_up, o[1] = bt_on, o[2] = bt_live, o[3] = bt_role, o[4] = b.ble_state;
    o[5] = bt_up ? ble_central_state() : 0u, o[6] = bt_up ? ble_central_fail() : 0u, o[7] = bt_up ? ble_central_code() : 0u;
    o[8] = (uint8_t)(bt_up && ble_connected()), o[9] = (uint8_t)n, o[10] = (uint8_t)(bt_up && ble_ll_scanning()), o[11] = drv.state;
    ble_cpy(o + 12, bcen.peer.addr, 6);
    o[18] = bcen.peer.addr_rand, o[19] = drv.master;
    for (k = 0; k < 4u; k++) {
      o[20 + k] = (uint8_t)(ble_hw_stat.connects >> 8 * k), o[24 + k] = (uint8_t)(ble_hw_stat.rx >> 8 * k);
      o[28 + k] = (uint8_t)(bt_scan.reports >> 8 * k);
    }
  } else if (cmd == 0x19u) { /* our own address */
    ble_cpy(o, bt_addr, 6);
  } else if (i < n) {
    const struct ble_found *e = &bt_scan.e[idx[i]];
    o[0] = 1, o[1] = e->midi, o[2] = e->addr_rand, o[3] = e->hits;
    ble_cpy(o + 4, e->addr, 6);
    o[10] = (uint8_t)e->rssi, o[11] = (uint8_t)(e->rssi >> 8);
    for (k = 0; k < 20u && e->name[k]; k++) {
      o[12 + k] = (uint8_t)e->name[k];
    }
  }
}
#endif

/* not on this platform: audio from Bluetooth */
const int32_t *bt_audio_in(void) { return 0; }
