// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Marvell 88E6xxx Switch AVB support
 *
 * Copyright (c) 2024-2026 PADL Software Pty Ltd
 */

#include "avb.h"
#include "chip.h"
#include "global1.h"
#include "global2.h"
#include "port.h"

static int mv88e6xxx_qav_read(struct mv88e6xxx_chip *chip, int addr,
			      u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->qav_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->qav_read(chip, addr, data, len);
}

static int mv88e6xxx_qav_write(struct mv88e6xxx_chip *chip, int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->qav_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->qav_write(chip, addr, data);
}

static int mv88e6xxx_avb_write(struct mv88e6xxx_chip *chip, int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->avb_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->avb_write(chip, addr, data);
}

static int mv88e6xxx_port_avb_read(struct mv88e6xxx_chip *chip, int port,
				   int addr, u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->port_avb_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->port_avb_read(chip, port, addr, data,
						       len);
}

static int mv88e6xxx_port_avb_write(struct mv88e6xxx_chip *chip, int port,
				    int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->port_avb_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->port_avb_write(chip, port, addr, data);
}

static int mv88e6xxx_qav_set_iso_ptr(struct mv88e6xxx_chip *chip, u16 threshold)
{
	u16 data;
	int err;

	err = mv88e6xxx_qav_read(chip, MV88E6XXX_QAV_CFG, &data, 1);
	if (err)
		return err;

	data &= ~(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK);
	data |= MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_SET(threshold);

	return mv88e6xxx_qav_write(chip, MV88E6XXX_QAV_CFG, data);
}

static int mv88e6xxx_avb_set_port_avb_mode(struct mv88e6xxx_chip *chip,
					   int port, enum mv88e6xxx_avb_mode mode)
{
	u16 data;
	int err;

	err = mv88e6xxx_port_avb_read(chip, port, MV88E6XXX_PORT_AVB_CFG, &data, 1);
	if (err)
		return err;

	data &= ~(MV88E6XXX_PORT_AVB_CFG_AVB_MODE |
		  MV88E6XXX_PORT_AVB_CFG_AVB_FILTER_BAD_AVB |
		  MV88E6XXX_PORT_AVB_CFG_AVB_DISCARD_BAD);

	/* MV88E6XXX_AVB_MODE_ENHANCED is enabled dynamically once a MDB entry
	 * with MDB_FLAGS_STREAM_RESERVED is added, indicating that AVB traffic
	 * classes should be reserved for SRP-managed destination addresses.
	 */
	switch (mode) {
	case MV88E6XXX_AVB_MODE_DISABLED:
		data |= MV88E6XXX_PORT_AVB_CFG_AVB_MODE_LEGACY;
		break;
	case MV88E6XXX_AVB_MODE_STANDARD:
		data |= MV88E6XXX_PORT_AVB_CFG_AVB_MODE_STANDARD;
		break;
	case MV88E6XXX_AVB_MODE_ENHANCED:
		data |= MV88E6XXX_PORT_AVB_CFG_AVB_MODE_ENHANCED |
			MV88E6XXX_PORT_AVB_CFG_AVB_FILTER_BAD_AVB |
			MV88E6XXX_PORT_AVB_CFG_AVB_DISCARD_BAD;
		break;
	}

	return mv88e6xxx_port_avb_write(chip, port, MV88E6XXX_PORT_AVB_CFG, data);
}

static u8 mv88e6xxx_mqprio_tc_fpri(const struct tc_mqprio_qopt *qopt, int tc)
{
	u8 fpri;

	for (fpri = 0; fpri < IEEE_8021Q_MAX_PRIORITIES; fpri++)
		if (qopt->prio_tc_map[fpri] == tc)
			return fpri;

	return 0;
}

static u16 mv88e6xxx_avb_pri_map_to_reg(const struct tc_mqprio_qopt *qopt)
{
	u8 hi_fpri = mv88e6xxx_mqprio_tc_fpri(qopt, MV88E6XXX_AVB_TC_HI);
	u8 lo_fpri = mv88e6xxx_mqprio_tc_fpri(qopt, MV88E6XXX_AVB_TC_LO);
	u8 hi_qpri = qopt->offset[MV88E6XXX_AVB_TC_HI];
	u8 lo_qpri = qopt->offset[MV88E6XXX_AVB_TC_LO];

	return MV88E6XXX_AVB_CFG_AVB_HI_FPRI_SET(hi_fpri) |
	       MV88E6XXX_AVB_CFG_AVB_HI_QPRI_SET(hi_qpri) |
	       MV88E6XXX_AVB_CFG_AVB_LO_FPRI_SET(lo_fpri) |
	       MV88E6XXX_AVB_CFG_AVB_LO_QPRI_SET(lo_qpri);
}

int mv88e6xxx_avb_enable(struct mv88e6xxx_chip *chip,
			 struct tc_mqprio_qopt_offload *mqprio)
{
	const struct mv88e6xxx_qav_info *qav = chip->info->qav;
	enum mv88e6xxx_avb_mode mode;
	int err, port;

	if (!qav)
		return -EOPNOTSUPP;

	err = mv88e6xxx_qav_set_iso_ptr(chip, mv88e6xxx_num_ports(chip) << 6);
	if (err)
		return err;

	/* interpret AVB_NRL bits in the ATU as STREAM_RESERVED */
	err = mv88e6xxx_g1_atu_set_mac_avb(chip, true);
	if (err)
		goto err_iso_ptr;

	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB,
				  mv88e6xxx_avb_pri_map_to_reg(&mqprio->qopt));
	if (err)
		goto err_mac_avb;

	mode = refcount_read(&chip->tc_policy.avb_mdb_count) ?
		MV88E6XXX_AVB_MODE_ENHANCED : MV88E6XXX_AVB_MODE_STANDARD;

	for (port = 0; port < mv88e6xxx_num_ports(chip); port++) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6xxx_avb_set_port_avb_mode(chip, port, mode);
		if (err)
			goto err_port_mode;
	}

	return 0;

err_port_mode:
	while (port-- > 0) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		mv88e6xxx_avb_set_port_avb_mode(chip, port, MV88E6XXX_AVB_MODE_DISABLED);
	}
	mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB, qav->avb_pri_map);
err_mac_avb:
	mv88e6xxx_g1_atu_set_mac_avb(chip, false);
err_iso_ptr:
	mv88e6xxx_qav_set_iso_ptr(chip, 0);

	return err;
}

int mv88e6xxx_avb_disable(struct mv88e6xxx_chip *chip)
{
	const struct mv88e6xxx_qav_info *qav = chip->info->qav;
	int err, port;

	if (!qav)
		return -EOPNOTSUPP;

	for (port = 0; port < mv88e6xxx_num_ports(chip); port++) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6xxx_avb_set_port_avb_mode(chip, port,
						      MV88E6XXX_AVB_MODE_DISABLED);
		if (err)
			return err;
	}

	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB, qav->avb_pri_map);
	if (err)
		return err;

	err = mv88e6xxx_g1_atu_set_mac_avb(chip, false);
	if (err)
		return err;

	err = mv88e6xxx_qav_set_iso_ptr(chip, 0);
	if (err)
		return err;

	return 0;
}

int mv88e6xxx_avb_set_mode(struct mv88e6xxx_chip *chip,
			   enum mv88e6xxx_avb_mode mode)
{
	int err, port;

	for (port = 0; port < mv88e6xxx_num_ports(chip); port++) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6xxx_avb_set_port_avb_mode(chip, port, mode);
		if (err)
			return err;
	}

	return 0;
}
