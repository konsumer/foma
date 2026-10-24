/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_FIRMWARE_LAYOUT_H
#define FM1_FIRMWARE_LAYOUT_H
#include <stdint.h>
/* Firmware layout supplied by Build.ps1; must match the SDK. */
struct fm1_firmware_layout {
    uint32_t user_bytes;
    uint32_t update_family;
};
extern const struct fm1_firmware_layout fm1_layout;
_Static_assert(sizeof(struct fm1_firmware_layout) == 8, "FM1 layout ABI");
#endif
