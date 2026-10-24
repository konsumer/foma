/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_device.h"
#define REG32(address) (*(volatile uint32_t *)(address))
static void (*input_scan)(void);
static FM1_AUDIO_CODE __attribute__((interrupt(""))) void fm1_panel_irq(void)
{
    REG32(0x10900U) |= 0x4000U;
    if (input_scan) input_scan();
}
void fm1_device_start(void (*scan)(void))
{
    /* TIMER4: free-running 24 MHz timebase; TIMER5: 10 kHz input scan. */
    REG32(0x10800U) = 0;
    REG32(0x10808U) = 0xffffffffU;
    REG32(0x10804U) = 0;
    REG32(0x10800U) = 0x4009U;
    fm1_device_adc_register(3U); /* PB01 battery */
    fm1_device_adc_register(4U); /* PB06 master */
    input_scan = scan;
    REG32(0x10900U) = 0x4000U;
    REG32(0x10904U) = 0;
    REG32(0x10908U) = 600U;
    fm1_device_irq_attach(63U, fm1_panel_irq, 5U);
    REG32(0x10900U) = 0x4019U;
}
