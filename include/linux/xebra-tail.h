/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XEBRA tail microcontroller I2C interface
 *
 * Copyright (C) 2026 PADL Software Pty Ltd.
 *
 * The tail answers on two I2C addresses, each served by its own peripheral
 * with its own register pointer: one for the rotary encoders
 * (xebra-tail-encoder) and one for the display panel
 * (xebra-panel-regulator). Registers are as xt_register_t in the tail's
 * firmware; multi-byte values are big-endian.
 */

#ifndef __LINUX_XEBRA_TAIL_H
#define __LINUX_XEBRA_TAIL_H

#define XEBRA_TAIL_ENCODER_ADDR		0x44
#define XEBRA_TAIL_PANEL_ADDR		0x45

/* Registers answered on both addresses */
#define XEBRA_TAIL_DEVICE_TYPE		0x00	/* 16-bit device type (get) */
#define XEBRA_TAIL_DEVICE_FIRMWARE	0x01	/* 16-bit firmware version (get) */
#define XEBRA_TAIL_SHUTDOWN_DEVICE	0xFE	/* device type ID: power off (set) */
#define XEBRA_TAIL_RESET_DEVICE		0xFF	/* device type ID: power cycle (set) */

#define XEBRA_TAIL_DEVICE_TYPE_ID	0xEB7A

/* Encoder registers */
#define XEBRA_TAIL_ENABLE_INTERRUPT	0x04	/* 8-bit interrupt enable (get, set) */
#define XEBRA_TAIL_ENCODER_COUNT	0x07	/* 8-bit encoder count (get) */
#define XEBRA_TAIL_ENCODER_STATES	0x08	/* switch bitmask, 8-bit deltas (get) */
#define XEBRA_TAIL_ENCODER_SWITCHES	0x09	/* 8-bit switch bitmask (get) */
#define XEBRA_TAIL_ENCODER_POSITION	0x10	/* 16-bit position, per encoder (get) */

/* Panel registers */
#define XEBRA_TAIL_LCD_CONTROL		0x06	/* 8-bit GPIO bitmask (get, set) */

#endif /* __LINUX_XEBRA_TAIL_H */
