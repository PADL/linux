// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Luminex Network Intelligence

#include "chip.h"
#include "dcb.h"
#include "global1.h"
#include "port.h"

static int mv88e6352_get_pcp_prio(struct mv88e6xxx_chip *chip, u8 pcp)
{
	u16 val;
	int err;

	/* These devices have no mapping table for frames with DEI bit set */
	if (pcp >= 8)
		return -EOPNOTSUPP;

	err = mv88e6xxx_g1_read(chip, MV88E6XXX_G1_IEEE_PRI, &val);
	if (err)
		return err;

	return (val >> (pcp * 2)) & 3;
}

static int mv88e6352_set_pcp_prio(struct mv88e6xxx_chip *chip, u8 pcp, u8 prio)
{
	u16 val;
	int err;

	/* These devices have no mapping table for frames with DEI bit set */
	if (pcp >= 8)
		return -EOPNOTSUPP;

	if (prio >= 4)
		return -EINVAL;

	err = mv88e6xxx_g1_read(chip, MV88E6XXX_G1_IEEE_PRI, &val);
	if (err)
		return err;

	val &= ~(3 << (pcp * 2));
	val |= prio << (pcp * 2);

	return mv88e6xxx_g1_write(chip, MV88E6XXX_G1_IEEE_PRI, val);
}

static int mv88e639x_port_ieeepmt_pcp_entry(u8 pcp, bool has_dei,
					    u16 *table, u8 *ptr)
{
	if (pcp >= 16)
		return -EINVAL;

	/* not all devices have a table for mapping with DEI bit set */
	if (pcp >= 8 && !has_dei)
		return -EOPNOTSUPP;

	*table = pcp >= 8 ?
		MV88E6393X_PORT_IEEE_PRIO_MAP_TABLE_INGRESS_PCP_DEI :
		MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_INGRESS_PCP;
	*ptr = pcp & 7;

	return 0;
}

static int mv88e639x_port_get_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 pcp, bool has_dei)
{
	u16 val, table;
	u8 ptr;
	int err;

	err = mv88e639x_port_ieeepmt_pcp_entry(pcp, has_dei, &table, &ptr);
	if (err)
		return err;

	err = mv88e6xxx_port_ieeepmt_read(chip, port, table, ptr, &val);
	if (err)
		return err;

	if (val & MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_DIS)
		return -EOPNOTSUPP;

	return (val & MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_MASK)
		>> MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_SHIFT;
}

static int mv88e639x_port_set_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 pcp, u8 prio, bool has_dei)
{
	u16 val, table;
	u8 ptr;
	int err;

	if (prio >= 8)
		return -EINVAL;

	err = mv88e639x_port_ieeepmt_pcp_entry(pcp, has_dei, &table, &ptr);
	if (err)
		return err;

	err = mv88e6xxx_port_ieeepmt_read(chip, port, table, ptr, &val);
	if (err)
		return err;

	val &= ~MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_DIS;
	val &= ~MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_MASK;
	val |= (prio << MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_SHIFT) &
		MV88E6390_PORT_IEEE_PRIO_MAP_TABLE_QPRI_MASK;

	return mv88e6xxx_port_ieeepmt_write(chip, port, table, ptr, val);
}

static int mv88e6390_port_get_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 pcp)
{
	return mv88e639x_port_get_pcp_prio(chip, port, pcp, false);
}

static int mv88e6390_port_set_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 pcp, u8 prio)
{
	return mv88e639x_port_set_pcp_prio(chip, port, pcp, prio, false);
}

static int mv88e6393x_port_get_pcp_prio(struct mv88e6xxx_chip *chip, int port,
					u8 pcp)
{
	return mv88e639x_port_get_pcp_prio(chip, port, pcp, true);
}

static int mv88e6393x_port_set_pcp_prio(struct mv88e6xxx_chip *chip, int port,
					u8 pcp, u8 prio)
{
	return mv88e639x_port_set_pcp_prio(chip, port, pcp, prio, true);
}

const struct mv88e6xxx_dcb_ops mv88e6352_dcb_ops = {
	.global_get_pcp_prio = mv88e6352_get_pcp_prio,
	.global_set_pcp_prio = mv88e6352_set_pcp_prio,
};

const struct mv88e6xxx_dcb_ops mv88e6390_dcb_ops = {
	.port_get_pcp_prio = mv88e6390_port_get_pcp_prio,
	.port_set_pcp_prio = mv88e6390_port_set_pcp_prio,
};

const struct mv88e6xxx_dcb_ops mv88e6393x_dcb_ops = {
	.port_get_pcp_prio = mv88e6393x_port_get_pcp_prio,
	.port_set_pcp_prio = mv88e6393x_port_set_pcp_prio,
};

static int mv88e6xxx_dcb_get_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				      u8 pcp)
{
	const struct mv88e6xxx_dcb_ops *dcb_ops = chip->info->ops->dcb_ops;

	if (!dcb_ops)
		return -EOPNOTSUPP;

	if (dcb_ops->port_get_pcp_prio)
		return dcb_ops->port_get_pcp_prio(chip, port, pcp);

	if (dcb_ops->global_get_pcp_prio)
		return dcb_ops->global_get_pcp_prio(chip, pcp);

	return -EOPNOTSUPP;
}

static int mv88e6xxx_dcb_set_pcp_prio(struct mv88e6xxx_chip *chip, int port,
				      u8 pcp, u8 prio)
{
	const struct mv88e6xxx_dcb_ops *dcb_ops = chip->info->ops->dcb_ops;

	if (!dcb_ops)
		return -EOPNOTSUPP;

	if (dcb_ops->port_set_pcp_prio)
		return dcb_ops->port_set_pcp_prio(chip, port, pcp, prio);

	if (dcb_ops->global_set_pcp_prio)
		return dcb_ops->global_set_pcp_prio(chip, pcp, prio);

	return -EOPNOTSUPP;
}

int mv88e6xxx_port_get_pcp_prio(struct dsa_switch *ds, int port, u8 pcp)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err;

	mv88e6xxx_reg_lock(chip);
	err = mv88e6xxx_dcb_get_pcp_prio(chip, port, pcp);
	mv88e6xxx_reg_unlock(chip);

	return err;
}

int mv88e6xxx_port_add_pcp_prio(struct dsa_switch *ds, int port, u8 pcp,
				u8 prio)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err;

	if (prio >= 8)
		return -EINVAL;

	mv88e6xxx_reg_lock(chip);
	err = mv88e6xxx_dcb_set_pcp_prio(chip, port, pcp, prio);
	mv88e6xxx_reg_unlock(chip);

	return err;
}

int mv88e6xxx_port_del_pcp_prio(struct dsa_switch *ds, int port, u8 pcp,
				u8 prio)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err = 0;

	mv88e6xxx_reg_lock(chip);
	if (mv88e6xxx_dcb_get_pcp_prio(chip, port, pcp) != prio)
		goto unlock;

	err = mv88e6xxx_dcb_set_pcp_prio(chip, port, pcp, 0);

unlock:
	mv88e6xxx_reg_unlock(chip);
	return err;
}
