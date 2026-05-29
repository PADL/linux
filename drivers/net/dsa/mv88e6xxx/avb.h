/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Marvell 88E6xxx Switch AVB support
 *
 * Copyright (c) 2024-2026 PADL Software Pty Ltd
 */

#ifndef _MV88E6XXX_AVB_H
#define _MV88E6XXX_AVB_H

#include "chip.h"

/* Global AVB registers */

/* Offset 0x00: AVB Global Config */

#define MV88E6XXX_AVB_CFG_AVB			0x00

#define MV88E6XXX_AVB_CFG_AVB_HI_FPRI_MASK	GENMASK(14, 12)
#define MV88E6XXX_AVB_CFG_AVB_HI_FPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_HI_FPRI_MASK, p)

#define MV88E6XXX_AVB_CFG_AVB_LO_FPRI_MASK	GENMASK(6, 4)
#define MV88E6XXX_AVB_CFG_AVB_LO_FPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_LO_FPRI_MASK, p)

#define MV88E6XXX_AVB_CFG_AVB_HI_QPRI_MASK	GENMASK(10, 8)
#define MV88E6XXX_AVB_CFG_AVB_HI_QPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_HI_QPRI_MASK, p)

#define MV88E6XXX_AVB_CFG_AVB_LO_QPRI_MASK	GENMASK(2, 0)
#define MV88E6XXX_AVB_CFG_AVB_LO_QPRI_SET(p)	FIELD_PREP(MV88E6XXX_AVB_CFG_AVB_LO_QPRI_MASK, p)

/* Global Qav registers */
#define MV88E6XXX_QAV_CFG			0x00

#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK	GENMASK(9, 0)
#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_GET(x)	FIELD_GET(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK, x)
#define MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_SET(x)	FIELD_PREP(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK, x)

/* allow mgmt frames in isochronous pointer pool */
#define MV88E6XXX_QAV_CFG_ADMIT_MGMT		0x8000

/* Per-port AVB registers */

/* Offset 0x00: AVB Port Config */
#define MV88E6XXX_PORT_AVB_CFG				0x00
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE			GENMASK(15, 14)
/* all frames legacy (non-AVB) unless overridden */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_LEGACY		0x0000
/* AVB frames indicated by priority */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_STANDARD	0x4000
/* STANDARD && ATU has STATIC_AVB_NRL bit set */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_ENHANCED	0x8000
/* ENHANCED && source port in destination port vector */
#define MV88E6XXX_PORT_AVB_CFG_AVB_MODE_SECURE		0xc000

#define MV88E6XXX_PORT_AVB_CFG_AVB_OVERRIDE		0x2000
#define MV88E6XXX_PORT_AVB_CFG_AVB_FILTER_BAD_AVB	0x1000
#define MV88E6XXX_PORT_AVB_CFG_AVB_TUNNEL		0x0800
#define MV88E6XXX_PORT_AVB_CFG_AVB_DISCARD_BAD		0x0400

int mv88e6xxx_avb_enable(struct mv88e6xxx_chip *chip,
			 struct tc_mqprio_qopt_offload *mqprio);
int mv88e6xxx_avb_disable(struct mv88e6xxx_chip *chip);

/**
 * enum mv88e6xxx_avb_mode - Current AVB mode
 * @MV88E6XXX_AVB_MODE_DISABLED: No AVB TCs (DCB Qdisc) configured
 * @MV88E6XXX_AVB_MODE_STANDARD: AVB configured, BR_FILTER_STREAM_RESERVED unset
 * @MV88E6XXX_AVB_MODE_ENHANCED: AVB configured, BR_FILTER_STREAM_RESERVED set
 */
enum mv88e6xxx_avb_mode {
	MV88E6XXX_AVB_MODE_DISABLED = 0,
	MV88E6XXX_AVB_MODE_STANDARD,
	MV88E6XXX_AVB_MODE_ENHANCED,
};

int mv88e6xxx_avb_set_port_avb_mode(struct mv88e6xxx_chip *chip, int port,
				    enum mv88e6xxx_avb_mode mode);

#endif /* _MV88E6XXX_AVB_H */
