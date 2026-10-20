/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* From M-VAVE fm1_open app/board/audio.c; foma: renders mv_render (port.c) instead of the SDK instrument. */
#include "fm1_audio.h"
#include "fm1_gpio.h"
#include "fm1_engine_control.h"
#include "bridge.h"
#include <string.h>

#define DMA_ALIGNED __attribute__((aligned(32)))
static int32_t receive[2][FM1_BLOCK_WORDS] DMA_ALIGNED;
static int32_t transmit[2][FM1_BLOCK_WORDS] DMA_ALIGNED;
static int32_t rendered[FM1_BLOCK_WORDS] DMA_ALIGNED;
static struct fm1_audio_stats statistics;
static uint32_t block_ticks, previous_tick;
static uint8_t initialized, previous_valid;
static volatile unsigned mute_depth;

static inline __attribute__((always_inline)) uint32_t elapsed(uint32_t start, uint32_t end)
{
    return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
}
/* No libc calls in the DMA commit window, including during Flash writes. */
static FM1_AUDIO_CODE void fm1_audio_copy(volatile int32_t *out, const int32_t *in)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) out[i] = in[i];
    __asm__ volatile("csync" ::: "memory");
}
FM1_AUDIO_CODE __attribute__((interrupt(""))) void fm1_audio_irq(void)
{
    uint8_t pending = fm1_audio_pending();
    unsigned rx = pending & 1U;
    uint32_t start = FM1_AUDIO_TIMER_COUNT, interval, used;
    unsigned half, committed = 0;
    fm1_audio_ack(pending);
    if (!rx || mute_depth) return;
    if (!previous_valid) { previous_tick = start; previous_valid = 1; return; }
    interval = elapsed(previous_tick, start);
    if (interval < 80000U) { ++statistics.short_intervals; return; }
    previous_tick = start;
    if (interval < block_ticks - (block_ticks >> 3)) { ++statistics.short_intervals; return; }
    if (interval > block_ticks + (block_ticks >> 3)) { ++statistics.overruns; return; }
    half = fm1_audio_free_half();
    /* foma: the app's audio() (port.c), not the SDK's instrument */
    mv_render(rendered);
    uint32_t flags = fm1_audio_irq_save();
    if (fm1_audio_half_owned(half) &&
        elapsed(start, FM1_AUDIO_TIMER_COUNT) < block_ticks - (block_ticks >> 3)) {
        fm1_audio_copy(transmit[half], rendered);
        committed = fm1_audio_half_owned(half);
    }
    fm1_audio_irq_restore(flags);
    if (!committed) ++statistics.overruns;
    pending = fm1_audio_pending();
    if (pending) {
        fm1_audio_ack(pending);
        if (pending & 1U) ++statistics.overruns;
    }
    used = elapsed(start, FM1_AUDIO_TIMER_COUNT);
    if (used > statistics.peak_ticks) statistics.peak_ticks = used;
    ++statistics.blocks;
}
void fm1_audio_suspend(void)
{
    uint32_t flags = fm1_device_irq_save();
    if (mute_depth++ == 0) {
        for (unsigned half = 0; half < 2; ++half)
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) transmit[half][i] = 0;
        fm1_device_fence();
    }
    fm1_device_irq_restore(flags);
}
void fm1_audio_resume(void)
{
    uint32_t flags = fm1_device_irq_save();
    if (mute_depth && --mute_depth == 0) previous_valid = 0;
    fm1_device_irq_restore(flags);
}
int fm1_audio_start(void)
{
    if (initialized) return 0;
    uint32_t clock = fm1_device_clock();
    if (!clock) return -1;
    block_ticks = (uint32_t)((uint64_t)clock * FM1_BLOCK_FRAMES / FM1_AUDIO_HARDWARE_RATE);
    if (!block_ticks) return -1;
    memset(receive, 0, sizeof receive);
    memset(transmit, 0, sizeof transmit);
    FM1_ALNK_CON0 = 0; FM1_ALNK_CON1 = 0; FM1_ALNK_CON2 = 0; FM1_ALNK_CON3 = 0;
    *(volatile uint32_t *)0x51030U &= ~(3U << 6); /* fixed port C route */
    const uint32_t outputs = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 6);
    FM1_PR(FM1_PC, FM1_OUT) |= outputs;
    FM1_PR(FM1_PC, FM1_DIE) |= 1U << 3;
    FM1_PR(FM1_PC, FM1_DIR) = (FM1_PR(FM1_PC, FM1_DIR) & ~outputs) | (1U << 3);
    FM1_PR(FM1_PC, FM1_PD) &= ~(1U << 3);
    FM1_PR(FM1_PC, FM1_PU) |= 1U << 3;
    *(volatile uint32_t *)0x10014U &= ~(15U << 8); /* 11.2896 MHz, 256 fs */
    FM1_ALNK_CON3 = 3U | (4U << 5);
    FM1_ALNK_CON0 = (1U << 8) | (1U << 7);
    FM1_ALNK_LEN = FM1_BLOCK_WORDS;
    FM1_ALNK_ADR0 = (uint32_t)(uintptr_t)receive;
    FM1_ALNK_ADR3 = (uint32_t)(uintptr_t)transmit;
    FM1_ALNK_CON1 = 1U | (1U << 2) | (1U << 3) | (1U << 12) | (1U << 14);
    fm1_device_fence();
    fm1_device_irq_attach(FM1_AUDIO_IRQ, fm1_audio_irq, FM1_AUDIO_PRIORITY);
    FM1_ALNK_CON0 |= 1U << 11;
    initialized = 1;
    return 0;
}
void fm1_audio_stats_read(struct fm1_audio_stats *out)
{
    if (!out) return;
    uint32_t flags = fm1_device_irq_save();
    *out = statistics;
    fm1_device_irq_restore(flags);
}
