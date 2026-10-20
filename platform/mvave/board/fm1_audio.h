/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_BOARD_AUDIO_H
#define FM1_BOARD_AUDIO_H
#include "fm1.h"
#include "fm1_device.h"

/* WL82 ALNK0: 24-bit stereo, two DMA halves, 128 frames per interrupt.
 * PC0 MCLK, PC2 LRCLK, PC1 SCLK, PC3 clock/RX, PC6 DAC.
 * RX supplies the interrupt clock; FM1 has no connected analogue input. */
#define FM1_ALNK_CON0 (*(volatile uint16_t *)0x12e00U)
#define FM1_ALNK_CON1 (*(volatile uint16_t *)0x12e04U)
#define FM1_ALNK_CON2 (*(volatile uint8_t  *)0x12e08U)
#define FM1_ALNK_CON3 (*(volatile uint8_t  *)0x12e0cU)
#define FM1_ALNK_ADR0 (*(volatile uint32_t *)0x12e10U)
#define FM1_ALNK_ADR3 (*(volatile uint32_t *)0x12e1cU)
#define FM1_ALNK_LEN  (*(volatile uint16_t *)0x12e20U)
#define FM1_AUDIO_TIMER_COUNT  (*(volatile uint32_t *)0x1eef0f0U)
#define FM1_AUDIO_TIMER_PERIOD (*(volatile uint32_t *)0x1eef0f4U)
#define FM1_AUDIO_HARDWARE_RATE 44118U
#define FM1_AUDIO_IRQ 11U
#define FM1_AUDIO_PRIORITY 4U

int fm1_audio_start(void);
void fm1_audio_suspend(void);
void fm1_audio_resume(void);
#define FM1_AUDIO_INLINE static inline __attribute__((always_inline))
FM1_AUDIO_INLINE uint8_t fm1_audio_pending(void) { return (FM1_ALNK_CON2 >> 4) & 15U; }
FM1_AUDIO_INLINE void fm1_audio_ack(uint8_t pending) { FM1_ALNK_CON2 = pending & 15U; }
FM1_AUDIO_INLINE unsigned fm1_audio_free_half(void) { return !(FM1_ALNK_CON0 & (1U << 15)); }
FM1_AUDIO_INLINE unsigned fm1_audio_half_owned(unsigned half)
{
    unsigned before = fm1_audio_free_half();
    uint8_t pending = fm1_audio_pending();
    unsigned after = fm1_audio_free_half();
    return before == half && after == half && !(pending & (1U << 3));
}
FM1_AUDIO_INLINE uint32_t fm1_audio_irq_save(void)
{
    uint32_t flags;
    __asm__ volatile("%0 = icfg" : "=r"(flags) :: "memory");
    uint32_t masked = flags & ~(1U << 8);
    __asm__ volatile("icfg = %0" :: "r"(masked) : "memory");
    return flags;
}
FM1_AUDIO_INLINE void fm1_audio_irq_restore(uint32_t flags)
{
    __asm__ volatile("icfg = %0" :: "r"(flags) : "memory");
}
#endif
