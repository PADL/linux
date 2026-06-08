// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Luminex Network Intelligence

#include <linux/bitfield.h>

#include <net/dscp.h>

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

/* The DCB priority maps the DSCP to an egress queue priority (QPri); the frame
 * priority (FPri) is left untouched.  These chips always map every DSCP, so
 * there is no per-entry "unmapped" state on the global (6352) table: deleting
 * an entry restores its reset-default QPri rather than removing it.
 */

/* G1 offset 0x19 is the IP Mapping Table only on these families; elsewhere it is
 * the Core Tag Type register, so report DSCP mapping as unsupported there.
 */
static bool mv88e6352_has_ip_pri_map(struct mv88e6xxx_chip *chip)
{
	switch (chip->info->family) {
	case MV88E6XXX_FAMILY_6320:
	case MV88E6XXX_FAMILY_6341:
	case MV88E6XXX_FAMILY_6352:
		return true;
	default:
		return false;
	}
}

static int mv88e6352_get_dscp_prio(struct mv88e6xxx_chip *chip, u8 dscp)
{
	u16 val;
	int err;

	if (!mv88e6352_has_ip_pri_map(chip))
		return -EOPNOTSUPP;

	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* load the table pointer (Update clear), then read the entry back */
	err = mv88e6xxx_g1_write(chip, MV88E6352_G1_IP_PRI_MAP,
				 FIELD_PREP(MV88E6352_G1_IP_PRI_MAP_PTR_MASK,
					    dscp));
	if (err)
		return err;

	err = mv88e6xxx_g1_read(chip, MV88E6352_G1_IP_PRI_MAP, &val);
	if (err)
		return err;

	return FIELD_GET(MV88E6352_G1_IP_PRI_MAP_QPRI_MASK, val);
}

/* Update only the QPri of a DSCP entry, preserving the global UseIpFPri bit and
 * the per-entry FPri so the frame priority handling is left untouched.
 */
static int mv88e6352_write_dscp_qpri(struct mv88e6xxx_chip *chip, u8 dscp,
				     u8 prio)
{
	u16 val;
	int err;

	/* load the table pointer (Update clear), then read the entry back */
	err = mv88e6xxx_g1_write(chip, MV88E6352_G1_IP_PRI_MAP,
				 FIELD_PREP(MV88E6352_G1_IP_PRI_MAP_PTR_MASK,
					    dscp));
	if (err)
		return err;

	err = mv88e6xxx_g1_read(chip, MV88E6352_G1_IP_PRI_MAP, &val);
	if (err)
		return err;

	val &= ~(MV88E6352_G1_IP_PRI_MAP_PTR_MASK |
		 MV88E6352_G1_IP_PRI_MAP_QPRI_MASK);
	val |= MV88E6352_G1_IP_PRI_MAP_UPDATE |
	       FIELD_PREP(MV88E6352_G1_IP_PRI_MAP_PTR_MASK, dscp) |
	       FIELD_PREP(MV88E6352_G1_IP_PRI_MAP_QPRI_MASK, prio);

	return mv88e6xxx_g1_write(chip, MV88E6352_G1_IP_PRI_MAP, val);
}

static int mv88e6352_set_dscp_prio(struct mv88e6xxx_chip *chip, u8 dscp,
				   u8 prio)
{
	if (!mv88e6352_has_ip_pri_map(chip))
		return -EOPNOTSUPP;

	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* QPri range as for the PCP-to-priority mapping */
	if (prio >= 4)
		return -EINVAL;

	return mv88e6352_write_dscp_qpri(chip, dscp, prio);
}

static int mv88e6352_del_dscp_prio(struct mv88e6xxx_chip *chip, u8 dscp)
{
	if (!mv88e6352_has_ip_pri_map(chip))
		return -EOPNOTSUPP;

	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* No per-entry disable exists, so restore the reset-default QPri, which
	 * is the top two DSCP bits (DSCP >> 4: 0x00-0x0f->0, 0x10-0x1f->1,
	 * 0x20-0x2f->2, 0x30-0x3f->3).
	 */
	return mv88e6352_write_dscp_qpri(chip, dscp, dscp >> 4);
}

static int mv88e6390_port_get_dscp_prio(struct mv88e6xxx_chip *chip, int port,
					u8 dscp)
{
	u16 val;
	int err;

	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* load the table pointer (Update clear), then read the entry back */
	err = mv88e6xxx_port_write(chip, port, MV88E6390_PORT_IP_PRI_MAP,
				   FIELD_PREP(MV88E6390_PORT_IP_PRI_MAP_PTR_MASK,
					      dscp));
	if (err)
		return err;

	err = mv88e6xxx_port_read(chip, port, MV88E6390_PORT_IP_PRI_MAP, &val);
	if (err)
		return err;

	if (val & MV88E6390_PORT_IP_PRI_MAP_DIS_IP_QPRI)
		return -EOPNOTSUPP;

	return FIELD_GET(MV88E6390_PORT_IP_PRI_MAP_QPRI_MASK, val);
}

static int mv88e6390_port_set_dscp_prio(struct mv88e6xxx_chip *chip, int port,
					u8 dscp, u8 prio)
{
	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* QPri range as for the PCP-to-priority mapping */
	if (prio >= 8)
		return -EINVAL;

	/* assign the QPri only; disable the FPri so it stays unchanged */
	return mv88e6xxx_port_write(chip, port, MV88E6390_PORT_IP_PRI_MAP,
				   MV88E6390_PORT_IP_PRI_MAP_UPDATE |
				   MV88E6390_PORT_IP_PRI_MAP_DIS_IP_FPRI |
				   FIELD_PREP(MV88E6390_PORT_IP_PRI_MAP_PTR_MASK,
					      dscp) |
				   FIELD_PREP(MV88E6390_PORT_IP_PRI_MAP_QPRI_MASK,
					      prio));
}

static int mv88e6390_port_del_dscp_prio(struct mv88e6xxx_chip *chip, int port,
					u8 dscp)
{
	if (dscp >= DSCP_MAX)
		return -EINVAL;

	/* Disable both the QPri and FPri overrides so the DSCP falls back to the
	 * port default; get_dscp_prio then reports the entry as removed.
	 */
	return mv88e6xxx_port_write(chip, port, MV88E6390_PORT_IP_PRI_MAP,
				   MV88E6390_PORT_IP_PRI_MAP_UPDATE |
				   MV88E6390_PORT_IP_PRI_MAP_DIS_IP_QPRI |
				   MV88E6390_PORT_IP_PRI_MAP_DIS_IP_FPRI |
				   FIELD_PREP(MV88E6390_PORT_IP_PRI_MAP_PTR_MASK,
					      dscp));
}

const struct mv88e6xxx_dcb_ops mv88e6352_dcb_ops = {
	.global_get_pcp_prio = mv88e6352_get_pcp_prio,
	.global_set_pcp_prio = mv88e6352_set_pcp_prio,
	.global_get_dscp_prio = mv88e6352_get_dscp_prio,
	.global_set_dscp_prio = mv88e6352_set_dscp_prio,
	.global_del_dscp_prio = mv88e6352_del_dscp_prio,
};

const struct mv88e6xxx_dcb_ops mv88e6390_dcb_ops = {
	.port_get_pcp_prio = mv88e6390_port_get_pcp_prio,
	.port_set_pcp_prio = mv88e6390_port_set_pcp_prio,
	.port_get_dscp_prio = mv88e6390_port_get_dscp_prio,
	.port_set_dscp_prio = mv88e6390_port_set_dscp_prio,
	.port_del_dscp_prio = mv88e6390_port_del_dscp_prio,
};

const struct mv88e6xxx_dcb_ops mv88e6393x_dcb_ops = {
	.port_get_pcp_prio = mv88e6393x_port_get_pcp_prio,
	.port_set_pcp_prio = mv88e6393x_port_set_pcp_prio,
	.port_get_dscp_prio = mv88e6390_port_get_dscp_prio,
	.port_set_dscp_prio = mv88e6390_port_set_dscp_prio,
	.port_del_dscp_prio = mv88e6390_port_del_dscp_prio,
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

static int mv88e6xxx_dcb_get_dscp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 dscp)
{
	const struct mv88e6xxx_dcb_ops *dcb_ops = chip->info->ops->dcb_ops;

	if (!dcb_ops)
		return -EOPNOTSUPP;

	if (dcb_ops->port_get_dscp_prio)
		return dcb_ops->port_get_dscp_prio(chip, port, dscp);

	if (dcb_ops->global_get_dscp_prio)
		return dcb_ops->global_get_dscp_prio(chip, dscp);

	return -EOPNOTSUPP;
}

static int mv88e6xxx_dcb_set_dscp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 dscp, u8 prio)
{
	const struct mv88e6xxx_dcb_ops *dcb_ops = chip->info->ops->dcb_ops;

	if (!dcb_ops)
		return -EOPNOTSUPP;

	if (dcb_ops->port_set_dscp_prio)
		return dcb_ops->port_set_dscp_prio(chip, port, dscp, prio);

	if (dcb_ops->global_set_dscp_prio)
		return dcb_ops->global_set_dscp_prio(chip, dscp, prio);

	return -EOPNOTSUPP;
}

static int mv88e6xxx_dcb_del_dscp_prio(struct mv88e6xxx_chip *chip, int port,
				       u8 dscp)
{
	const struct mv88e6xxx_dcb_ops *dcb_ops = chip->info->ops->dcb_ops;

	if (!dcb_ops)
		return -EOPNOTSUPP;

	if (dcb_ops->port_del_dscp_prio)
		return dcb_ops->port_del_dscp_prio(chip, port, dscp);

	if (dcb_ops->global_del_dscp_prio)
		return dcb_ops->global_del_dscp_prio(chip, dscp);

	return -EOPNOTSUPP;
}

int mv88e6xxx_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err;

	mv88e6xxx_reg_lock(chip);
	err = mv88e6xxx_dcb_get_dscp_prio(chip, port, dscp);
	mv88e6xxx_reg_unlock(chip);

	return err;
}

int mv88e6xxx_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				 u8 prio)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err;

	if (prio >= IEEE_8021Q_MAX_PRIORITIES)
		return -EINVAL;

	mv88e6xxx_reg_lock(chip);
	err = mv88e6xxx_dcb_set_dscp_prio(chip, port, dscp, prio);
	mv88e6xxx_reg_unlock(chip);

	return err;
}

int mv88e6xxx_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				 u8 prio)
{
	struct mv88e6xxx_chip *chip = ds->priv;
	int err;

	mv88e6xxx_reg_lock(chip);

	err = mv88e6xxx_dcb_get_dscp_prio(chip, port, dscp);
	if (err == -EOPNOTSUPP) {
		/* No explicit mapping for this DSCP, so nothing to remove */
		err = 0;
		goto unlock;
	}
	if (err < 0)
		goto unlock;
	if (err != prio) {
		/* Mapping was changed in the meantime; leave it alone */
		err = 0;
		goto unlock;
	}

	err = mv88e6xxx_dcb_del_dscp_prio(chip, port, dscp);

unlock:
	mv88e6xxx_reg_unlock(chip);
	return err;
}
