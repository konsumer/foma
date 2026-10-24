/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Copyright (c) 2026 珠海笙科智能科技有限公司 (M-VAVE).
 * AC791N/WL82 port addresses and byte offsets. */
#ifndef FM1_GPIO_H
#define FM1_GPIO_H
#include <stdint.h>

#define FM1_PA 0U
#define FM1_PB 1U
#define FM1_PC 2U
#define FM1_PH 7U
#define FM1_OUT 0U
#define FM1_IN 4U
#define FM1_DIR 8U
#define FM1_DIE 12U
#define FM1_PU 16U
#define FM1_PD 20U
#define FM1_HD0 24U
#define FM1_HD 28U
#define FM1_PR(port, offset) (((volatile uint32_t *)(uintptr_t)0x50000U)[16U * (port) + (offset) / 4U])
#endif
