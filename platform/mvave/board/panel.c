/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE).
 * Connect the FM1 control API to the board scanner.
 * (foma: the TRS input follows FOMA_TRS_MIDI; mv_screen at the end.) */
#include "fm1.h"
#include "fm1_device.h"
#include "fm1_input.h"
#include "fm1_lcd_hw.h"
#include "lcd.inc"

static uint8_t controls_started;
#ifndef FOMA_TRS_MIDI
#define FOMA_TRS_MIDI 1
#endif
uint8_t fm1_io_trs_midi_enabled(void) { return FOMA_TRS_MIDI ? 1U : 0U; }

void fm1_controls_init(void)
{
    if (!controls_started) {
        /* PB1: battery divider; PB6: master potentiometer. */
        const uint32_t analog = (1U << 1) | (1U << 6);
        FM1_PR(FM1_PB, FM1_DIR) |= analog;
        FM1_PR(FM1_PB, FM1_DIE) &= ~analog;
        FM1_PR(FM1_PB, FM1_PU) &= ~analog;
        FM1_PR(FM1_PB, FM1_PD) &= ~analog;
        fm1_input_init();
        fm1_device_start(fm1_input_tick);
        controls_started = 1U;
    }
}

void fm1_controls_read(struct fm1_controls *state)
{
    if (!state) return;
    struct fm1_controls snapshot;
    uint32_t irq_state = fm1_device_irq_save();
    snapshot.keys = fm1_in.notes;
    snapshot.buttons = fm1_in.buttons;
    unsigned index = FM1_NENC;
    while (index) {
        --index;
        snapshot.encoders[index] = fm1_in.enc_steps[index];
        fm1_in.enc_steps[index] = 0;
    }
    fm1_device_irq_restore(irq_state);
    snapshot.knob = (uint16_t)fm1_device_adc(4U);
    snapshot.battery = (uint16_t)fm1_device_adc(3U);
    *state = snapshot;
}

void fm1_led_set(unsigned key, int on)
{
    fm1_led_key(key, on);
}

/* foma: rows of the frame buffer, already in the panel's byte order, straight to the screen */
#include "bridge.h"
void mv_screen(const uint16_t *rows, unsigned y, unsigned h)
{
    display_finish();
    display_window(0, y, 240, h);
    for (unsigned r = 0; r < h; r += 16) {
        unsigned n = h - r < 16 ? h - r : 16;
        display_data(rows + r * 240, 2 * 240 * n);
    }
}
