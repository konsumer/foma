/* foma: one MIDI clock for the app, 24 ticks a beat (FOMA_MIDI_CLOCK). It runs at the app's tempo, or follows the
 * clock coming in over MIDI; either way audio() gets each block's ticks, start, continue and stop at the frame they
 * fall on, and the clock can go out over USB MIDI.
 *
 * The platform defines CLOCK_MUL (its time stamps to frames: 44100 * 2^32 / their rate), passes clock_in() the
 * clock and transport messages that come in with when they came, and calls clock_block() before each audio().
 * Followed ticks are placed by when they came between the last block and this one: the sound is a block late, but
 * steady. clock_in() runs where MIDI comes in (an ISR on the FM-1), clock_block() where audio() runs. */
#include "fm1.h"
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" :: \
                                            : "memory")
#endif

#define CK_Q 64u
static struct {
  uint32_t t;
  uint8_t st, src;
} ck_q[CK_Q];
static volatile uint32_t ck_w, ck_r;
static volatile uint8_t ck_from, ck_out, ck_req; /* the app's (loop) */
static volatile uint32_t ck_bpm100 = 12000, ck_measured;
static clock_event_t ck_ev[CLOCK_EVENTS];
static int ck_n;
static uint32_t ck_next_q8;                      /* its own tempo: frames to the next tick, Q8 */
static uint32_t ck_t, ck_frames, ck_last, ck_q4; /* followed: the last block's time; a tick's length (frames, Q4) */
static uint8_t ck_on;                            /* followed ticks are coming */

static int clock_in(uint32_t st, uint32_t src, uint32_t t) {
  if (ck_w - ck_r < CK_Q) {
    ck_q[ck_w % CK_Q].t = t;
    ck_q[ck_w % CK_Q].st = (uint8_t)st;
    ck_q[ck_w % CK_Q].src = (uint8_t)src;
    RING_PUBLISH();
    ck_w++;
  }
  return 1;
}

static void ck_add(uint32_t at, int what) {
  static const uint8_t ST[5] = {0, 0xF8, 0xFA, 0xFB, 0xFC};
  if (ck_n < CLOCK_EVENTS) {
    ck_ev[ck_n].at = (uint16_t)at;
    ck_ev[ck_n].what = (uint8_t)what;
    ck_n++;
  }
  if (ck_out) {
    midi_send(ST[what], 0, 0);
  }
}

static void ck_tick(uint32_t at) { /* a followed tick: its length, smoothed (a pause is not one) */
  uint32_t f = ck_frames + at, d = f - ck_last;
  ck_last = f;
  if (ck_on && d < 44100u && (!ck_q4 || d << 4 < ck_q4 * 3u)) {
    ck_q4 = ck_q4 ? (uint32_t)((int32_t)ck_q4 + ((int32_t)(d << 4) - (int32_t)ck_q4) / 4) : d << 4;
  }
  ck_on = 1;
  ck_add(at, CLOCK_TICK);
}

static void clock_block(uint32_t n, uint32_t t) {
  uint32_t r = ck_r, from = ck_from, req = ck_req;
  ck_n = 0;
  ck_req = 0;
  if (req) { /* the app's own start / continue / stop, first (following too) */
    ck_add(0, (int)req);
    if (req != CLOCK_STOP) {
      ck_next_q8 = 0; /* (its own tempo: a tick on the start) */
    }
  }
  if (from) {
    while (r != ck_w) {
      RING_PUBLISH();
      uint32_t st = ck_q[r % CK_Q].st, src = ck_q[r % CK_Q].src;
      int32_t dt = (int32_t)(ck_q[r % CK_Q].t - ck_t);
      uint32_t at = dt <= 0 ? 0 : (uint32_t)(((uint64_t)(uint32_t)dt * CLOCK_MUL) >> 32);
      r++;
      if (!(src & from)) {
        continue;
      }
      at = at >= n ? n - 1 : at;
      if (st == 0xF8) {
        ck_tick(at);
      } else {
        ck_add(at, st == 0xFA ? CLOCK_START : st == 0xFB ? CLOCK_CONTINUE
                                                         : CLOCK_STOP);
      }
    }
    if (ck_frames + n - ck_last > 44100u) { /* none for 1 s */
      ck_q4 = 0;
      ck_on = 0;
    }
    ck_measured = ck_q4 ? 176400000u / ck_q4 : 0; /* 44100 * 60 / 24 * 100, Q4 */
  } else {
    uint32_t bpm = ck_bpm100 < 100u ? 100u : ck_bpm100, len = 2822400000u / bpm; /* 44100 * 60 / 24 * 100, Q8 */
    r = ck_w;                                                                    /* (what came in is not followed) */
    ck_on = 0;
    while ((ck_next_q8 >> 8) < n) {
      ck_add(ck_next_q8 >> 8, CLOCK_TICK);
      ck_next_q8 += len;
    }
    ck_next_q8 -= n << 8;
  }
  RING_PUBLISH();
  ck_r = r;
  ck_t = t;
  ck_frames += n;
}

void clock_follow(int from) { ck_from = (uint8_t)(from & MIDI_ANY); }
void clock_send(int on) { ck_out = (uint8_t) !!on; }
void clock_tempo(uint32_t bpm100) { ck_bpm100 = bpm100; }
void clock_start(void) { ck_req = CLOCK_START; }
void clock_continue(void) { ck_req = CLOCK_CONTINUE; }
void clock_stop(void) { ck_req = CLOCK_STOP; }
int clock_events(const clock_event_t **ev) {
  *ev = ck_ev;
  return ck_n;
}
uint32_t clock_bpm(void) { return ck_from ? ck_measured : ck_bpm100; }
