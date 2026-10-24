/* foma on M-VAVE's FM1 SDK (sdk/lib/libfm1_engine.a: FreeRTOS, USB, Bluetooth, flash, the updater): the SDK side.
 * The SDK boots, runs its tasks and calls the application's hooks: groove_screen_init once and
 * groove_screen_control over and over (the control task: foma's setup / loop), groove_midi_poll, and the
 * board code in board/ (the panel, the screen, the audio interrupt, which renders foma's audio()). M-VAVE's
 * own instrument is in the SDK too; foma never starts it, so its hooks below are empty. */
#include "fm1.h" /* the SDK's */
#include "fm1_audio_io.h"
#include "fm1_device.h"
#include "bridge.h"

/* ---- the SDK's instrument hooks: unused (its sequencer and FM engine never run) */
struct groove_project;
static uint32_t project_scratch[4064 / 4]; /* sizeof(struct groove_project) in fm1_open V14 */
struct groove_project *groove_project_scratch(void) {
  return (struct groove_project *)project_scratch;
}
void groove_project_init(void) {}
void groove_project_poll(void) {}
void groove_project_name(char name[13]) { name[0] = 0; }
void groove_project_metadata(const struct groove_project *p) { (void)p; }
void groove_transfer_init(void) {}
void groove_transfer_poll(void) {}
void groove_samples_init(void) {}
void groove_samples_poll(void) {}
void groove_fm_library_init(void) {}
FM1_AUDIO_CODE void groove_fm_program(uint8_t patch[156], unsigned program) {
  (void)program;
  for (unsigned i = 0; i < 156; i++) {
    patch[i] = 0;
  }
}
void groove_midi_init(void) {}
void groove_midi_clock_reset(void) {}
void groove_midi_panic(void) {}
FM1_AUDIO_CODE unsigned groove_midi_mode(unsigned track) { return (void)track, 0; }
FM1_AUDIO_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity) { (void)track, (void)note, (void)velocity; }
FM1_AUDIO_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_AUDIO_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner, (void)status; }
FM1_AUDIO_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner, (void)tempo; }

/* ---- foma's: the control task, MIDI */
void groove_screen_init(void) {
  fm1_controls_init();
  for (unsigned id = 0; id < 41; id++) {
    fm1_led_set(id, 0);
  }
  fm1_screen_init();
  mv_setup();
}
void groove_screen_control(void) { mv_loop(); }
void groove_midi_poll(void) { mv_midi_flush(); }
void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n) { mv_midi_in(source, m, (unsigned)n); }

/* ---- what port.c asks for */
void mv_controls(struct mv_controls *c) {
  struct fm1_controls s;
  fm1_controls_read(&s);
  c->keys = s.keys;
  c->buttons = s.buttons;
  for (int e = 0; e < 7; e++) {
    c->enc[e] = s.encoders[e];
  }
  c->knob = s.knob;
}
void mv_led(unsigned id, int on) { fm1_led_set(id, on); }
uint32_t mv_millis(void) { return fm1_millis(); }
uint32_t mv_ticks(void) { return fm1_device_ticks(); }
uint32_t mv_irq_save(void) { return fm1_device_irq_save(); }
void mv_irq_restore(uint32_t s) { fm1_device_irq_restore(s); }
int mv_midi_send(const uint8_t *m, unsigned n) { return fm1_midi_send(m, n); }

uint32_t mv_flash_size(void) { return fm1_flash_size(); }
int mv_flash_read(uint32_t off, void *p, uint32_t n) { return fm1_flash_read(off, p, n); }
int mv_flash_write(uint32_t off, const void *p, uint32_t n) { return fm1_flash_write(off, p, n); }
int mv_flash_erase(uint32_t off) { return fm1_flash_erase(off); }

static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bt_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
int mv_usb_in(int32_t *q15) { /* Q23 at the host's volume (Q7) -> Q15 */
  uint16_t gain[2] = {0, 0};
  if (fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, gain) != FM1_BLOCK_FRAMES) {
    return 0;
  }
  for (unsigned i = 0; i < FM1_BLOCK_WORDS; i++) {
    q15[i] = (int32_t)(((int64_t)usb_pcm[i] * gain[i & 1]) >> 15);
  }
  return 1;
}
int mv_bt_in(int32_t *q15) {
  if (fm1_bluetooth_audio_read(bt_pcm, FM1_BLOCK_FRAMES) != FM1_BLOCK_FRAMES) {
    return 0;
  }
  for (unsigned i = 0; i < FM1_BLOCK_WORDS; i++) {
    q15[i] = bt_pcm[i];
  }
  return 1;
}
void mv_usb_out(const int32_t *q23) { (void)fm1_usb_audio_write(q23, FM1_BLOCK_FRAMES); }

int mv_bt_configure(unsigned features) { return fm1_bluetooth_configure(features); }
void mv_bt_status(uint8_t *features, uint8_t *ready, uint8_t *audio_connected, uint8_t *audio_playing) {
  struct fm1_bluetooth_status s = {0};
  fm1_bluetooth_get_status(&s);
  *features = s.features, *ready = s.ready, *audio_connected = s.audio_connected, *audio_playing = s.audio_playing;
}
int mv_ble_role(int central) { return fm1_ble_set_role(central ? FM1_BLE_CENTRAL : FM1_BLE_PERIPHERAL); }
int mv_ble_scan(void) { return fm1_ble_scan(); }
int mv_ble_connect(unsigned i) { return fm1_ble_connect(i); }
int mv_ble_disconnect(void) { return fm1_ble_disconnect(); }
int mv_ble_forget(void) { return fm1_ble_forget_bonds(); }
void mv_ble_status(uint8_t *role, uint8_t *state, uint8_t *count, uint8_t *error, uint8_t *pending) {
  struct fm1_ble_status s = {0};
  fm1_ble_get_status(&s);
  *role = s.role, *state = s.state, *count = s.count, *error = s.error, *pending = s.pending;
}
int mv_ble_device(unsigned i, char name[25], int8_t *rssi, uint8_t *midi) {
  struct fm1_ble_device d;
  if (fm1_ble_get_device(i, &d)) {
    return -1;
  }
  for (int k = 0; k < 25; k++) {
    name[k] = d.name[k];
  }
  *rssi = d.rssi, *midi = d.midi;
  return 0;
}
