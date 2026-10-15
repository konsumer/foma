/* Your firmware. Change anything; `./fm1 dev` rebuilds it and the emulator reloads on save.
 *
 *   keys        4 voices, also sent over USB-MIDI
 *   OCT- OCT+   the octave (kept in flash)
 *   PRESETS     the waveform: sine, triangle, saw, square
 *   KNOB1..4    a value each, 0..127, on the screen (yours to use)
 *   USB-MIDI    notes in play too
 *
 * What you can call: platform/fm1.h. The rules (no float, keep audio() short): README.md. */
#include "fm1.h"

#define VOICES 4

static struct {
  volatile uint32_t inc; /* phase step per frame; 0 = free */
  volatile int32_t gate; /* target level, Q15: 0 = released */
  int32_t amp;           /* the level now: glides to gate, no clicks */
  uint32_t phase;
  uint8_t note;
} voice[VOICES];

static struct {
  int8_t octave;
  uint8_t wave;
} saved = {0, 0};

static volatile uint8_t wave;
static const char *const WAVES[] = {"SINE", "TRIANGLE", "SAW", "SQUARE"};
static int knob[4] = {64, 64, 64, 64}; /* KNOB1..4: nothing uses them yet */

/* ---- the sound: runs in the audio interrupt. Keep it short: no screen, no flash */
static int32_t osc(uint32_t ph) {
  switch (wave) {
    case 1:
      return (int32_t)((ph >> 31) ? ~ph : ph) / 32768 - 32767;
    case 2:
      return (int32_t)ph / 65536;
    case 3:
      return (int32_t)ph < 0 ? -20000 : 20000;
    default:
      return sine(ph);
  }
}

void audio(int32_t *out, uint32_t frames) {
  for (int v = 0; v < VOICES; v++) {
    int32_t amp = voice[v].amp, gate = voice[v].gate;
    uint32_t ph = voice[v].phase, inc = voice[v].inc;
    if (!amp && !gate) {
      continue;
    }
    for (uint32_t i = 0; i < frames; i++) {
      if (amp < gate) {
        amp = amp + 48 > gate ? gate : amp + 48; /* attack ~5 ms */
      } else if (amp > gate) {
        amp = amp - 6 < gate ? gate : amp - 6; /* release ~45 ms */
      }
      ph += inc;
      int32_t s = (osc(ph) * amp) >> 15;
      out[2 * i] += s;
      out[2 * i + 1] += s;
    }
    voice[v].phase = ph;
    voice[v].amp = amp;
  }
}

/* ---- notes */
static void note_on(int note, int velocity) {
  int pick = 0;
  for (int v = 0; v < VOICES; v++) { /* the same note, else a silent voice, else the quietest */
    if (voice[v].note == note && voice[v].gate) {
      pick = v;
      break;
    }
    if (voice[v].amp + voice[v].gate < voice[pick].amp + voice[pick].gate) {
      pick = v;
    }
  }
  voice[pick].note = (uint8_t)note;
  voice[pick].inc = note_inc(note);
  voice[pick].gate = velocity * 90; /* 127 -> 11430: four voices stay under full scale */
}

static void note_off(int note) {
  for (int v = 0; v < VOICES; v++) {
    if (voice[v].note == note) {
      voice[v].gate = 0;
    }
  }
}

/* ---- the screen */
#define ACCENT RGB(255, 170, 60)
#define DIM RGB(90, 90, 90)

static void draw_centered(int y, const char *s, uint16_t color, int font) {
  rect(0, y, SCREEN_W, text_height(font), BLACK);
  text((SCREEN_W - text_width(s, font)) / 2, y, s, color, font);
}

static void draw_status(void) {
  char s[24] = "OCTAVE ";
  format_int(s + 7, saved.octave);
  draw_centered(134, WAVES[saved.wave], WHITE, FONT_SMALL);
  draw_centered(154, s, DIM, FONT_SMALL);
}

static void draw_knob(int k) { /* four columns of 60 px: the label, the value big */
  static const char *const NAME[4] = {"KNOB1", "KNOB2", "KNOB3", "KNOB4"};
  char v[12];
  int x = k * 60;
  format_int(v, knob[k]);
  rect(x, 180, 60, 52, BLACK);
  text(x + (60 - text_width(NAME[k], FONT_SMALL)) / 2, 180, NAME[k], DIM, FONT_SMALL);
  text(x + (60 - text_width(v, FONT_BIG)) / 2, 198, v, ACCENT, FONT_BIG);
}

/* detents turned this pass -> steps: a slow turn is 1 a detent, a fast spin up to 4 */
static int accel(int e, int32_t detents) {
  static uint32_t last[ENC_COUNT];
  uint32_t now = millis(), dt = now - last[e];
  if (!detents) {
    return 0;
  }
  last[e] = now;
  return detents * (dt < 25 ? 4 : dt < 60 ? 2
                                          : 1);
}

static void draw_keys(uint32_t lit) {
  static const uint8_t BLACK_KEY[12] = {0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0}; /* from F */
  int x = 8;
  for (int k = 0; k < KEY_COUNT; k++) {
    if (!BLACK_KEY[k % 12]) {
      rect(x, 80, 13, 50, lit >> k & 1 ? ACCENT : RGB(210, 210, 210));
      x += 14;
    }
  }
  x = 8;
  for (int k = 0; k < KEY_COUNT; k++) {
    if (BLACK_KEY[k % 12]) {
      rect(x - 5, 80, 9, 30, lit >> k & 1 ? ACCENT : BLACK);
    } else {
      x += 14;
    }
  }
}

void setup(void) {
  settings_load(&saved, sizeof saved);
  if (saved.octave < -2 || saved.octave > 2) {
    saved.octave = 0;
  }
  wave = saved.wave &= 3;
  clear(BLACK);
  draw_centered(20, "FOMA", ACCENT, FONT_BIG);
  draw_centered(56, "a synth to start from", DIM, FONT_SMALL);
  draw_status();
  for (int k = 0; k < 4; k++) {
    draw_knob(k);
  }
  print("hello from setup()");
}

void loop(void) {
  static uint8_t held[KEY_COUNT];
  static uint32_t lit_drawn = ~0u, changed_at;
  static int dirty;
  int base = KEY_FIRST_NOTE + 12 * saved.octave;
  uint8_t st, d1, d2;

  for (int k = 0; k < KEY_COUNT; k++) {
    if (key_pressed(k)) {
      held[k] = (uint8_t)(base + k); /* a key stops the note it started, whatever the octave is now */
      note_on(held[k], 100);
      midi_send(0x90, held[k], 100);
    }
    if (key_released(k)) {
      note_off(held[k]);
      midi_send(0x80, held[k], 0);
    }
  }

  while (midi_read(&st, &d1, &d2)) {
    if ((st & 0xF0) == 0x90 && d2) {
      note_on(d1, d2);
    } else if ((st & 0xF0) == 0x80 || (st & 0xF0) == 0x90) {
      note_off(d1);
    }
  }

  int oct = saved.octave + button_pressed(BTN_OCT_UP) - button_pressed(BTN_OCT_DOWN);
  int w = (saved.wave + encoder(ENC_PRESETS)) & 3;
  if ((oct != saved.octave && oct >= -2 && oct <= 2) || w != saved.wave) {
    saved.octave = (int8_t)(oct < -2 ? -2 : oct > 2 ? 2
                                                    : oct);
    wave = saved.wave = (uint8_t)w;
    draw_status();
    dirty = 1;
    changed_at = millis();
  }
  for (int k = 0; k < 4; k++) {
    int v = knob[k] + accel(ENC_KNOB1 + k, encoder(ENC_KNOB1 + k));
    v = v < 0 ? 0 : v > 127 ? 127
                            : v;
    if (v != knob[k]) {
      knob[k] = v;
      draw_knob(k);
    }
  }
  led_button(BTN_OCT_DOWN, saved.octave < 0);
  led_button(BTN_OCT_UP, saved.octave > 0);

  uint32_t lit = 0;
  base = KEY_FIRST_NOTE + 12 * saved.octave;
  for (int v = 0; v < VOICES; v++) {
    if (voice[v].gate && voice[v].note >= base && voice[v].note < base + KEY_COUNT) {
      lit |= 1u << (voice[v].note - base);
    }
  }
  for (int k = 0; k < KEY_COUNT; k++) {
    led_key(k, lit >> k & 1);
  }
  if (lit != lit_drawn) { /* draw only what changed */
    lit_drawn = lit;
    draw_keys(lit);
  }

  int quiet = 1;
  for (int v = 0; v < VOICES; v++) {
    quiet &= !voice[v].gate && !voice[v].amp;
  }
  if (dirty && quiet && millis() - changed_at > 2000) { /* saving stalls the sound: wait for silence */
    settings_save(&saved, sizeof saved);
    dirty = 0;
  }
}
