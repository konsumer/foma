/* foma on M-VAVE's FM1 SDK: what the two halves of the port share. sdk.c talks to the SDK (its own fm1.h),
 * port.c implements foma's fm1.h on top of it; the two headers can't meet in one file (same name, same guard). */
#ifndef MV_BRIDGE_H
#define MV_BRIDGE_H
#include <stdint.h>

#define MV_BLOCK 128u        /* frames a block (the SDK's FM1_BLOCK_FRAMES) */
#define MV_TICK_HZ 24000000u /* fm1_device_ticks(): TIMER4 */

/* ---- port.c, called by sdk.c */
void mv_setup(void);                                         /* control task, once: after the screen and the panel are up */
void mv_loop(void);                                          /* control task, over and over */
void mv_render(int32_t *q23);                                /* audio ISR: a block of output, L R, signed Q23 */
void mv_midi_in(unsigned src, const uint8_t *m, unsigned n); /* the SDK's routing task: a complete short message */
void mv_midi_flush(void);                                    /* control task: what midi_send queued, out */

/* ---- sdk.c, called by port.c */
struct mv_controls {
  uint32_t keys;    /* bit k: note key k (F3 ..) */
  uint32_t buttons; /* bit b: button b, foma's BTN_* order */
  int16_t enc[7];   /* detents since the last read */
  uint16_t knob;    /* MASTER, 0..1023 */
};
void mv_controls(struct mv_controls *c);
void mv_led(unsigned id, int on); /* 0..13 buttons, 14..40 keys */
uint32_t mv_millis(void);
uint32_t mv_ticks(void);
uint32_t mv_irq_save(void);
void mv_irq_restore(uint32_t s);
void mv_screen(const uint16_t *rows, unsigned y, unsigned h); /* 240-wide rows, already in the panel's byte order */
int mv_midi_send(const uint8_t *m, unsigned n);               /* control task: 0 queued, else busy / offline */
/* the user partition (fm1_flash_*): 0 ok */
uint32_t mv_flash_size(void);
int mv_flash_read(uint32_t off, void *p, uint32_t n);
int mv_flash_write(uint32_t off, const void *p, uint32_t n);
int mv_flash_erase(uint32_t off); /* one 4 KiB sector */
/* the audio coming in, for one block (audio ISR): 1 when there is a block (L R, Q15) */
int mv_usb_in(int32_t *q15);
int mv_bt_in(int32_t *q15);
void mv_usb_out(const int32_t *q23); /* what the computer records */
/* Bluetooth (control task) */
int mv_bt_configure(unsigned features); /* 1 audio, 2 MIDI */
void mv_bt_status(uint8_t *features, uint8_t *ready, uint8_t *audio_connected, uint8_t *audio_playing);
int mv_ble_role(int central);
int mv_ble_scan(void);
int mv_ble_connect(unsigned i);
int mv_ble_disconnect(void);
int mv_ble_forget(void);
void mv_ble_status(uint8_t *role, uint8_t *state, uint8_t *count, uint8_t *error, uint8_t *pending);
int mv_ble_device(unsigned i, char name[25], int8_t *rssi, uint8_t *midi);
#endif
