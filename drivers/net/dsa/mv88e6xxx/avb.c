// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Marvell 88E6xxx Switch AVB support
 *
 * Copyright (c) 2008 Marvell Semiconductor
 *
 * Copyright (c) 2024 PADL Software Pty Ltd
 */

#include "avb.h"
#include "chip.h"
#include "global1.h"
#include "global2.h"
#include "port.h"

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP

/* AVB operation wrappers */

static int mv88e6xxx_port_avb_read(struct mv88e6xxx_chip *chip, int port,
				   int addr, u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->port_avb_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->port_avb_read(chip, port, addr,
						       data, len);
}

static int mv88e6xxx_port_avb_write(struct mv88e6xxx_chip *chip, int port,
				    int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->port_avb_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->port_avb_write(chip, port, addr, data);
}

static int mv88e6xxx_avb_read(struct mv88e6xxx_chip *chip, int addr,
			      u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->avb_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->avb_read(chip, addr, data, len);
}

static int mv88e6xxx_avb_write(struct mv88e6xxx_chip *chip, int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->avb_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->avb_write(chip, addr, data);
}

/* 802.1Qav operation wrappers */

static int mv88e6xxx_qav_read(struct mv88e6xxx_chip *chip, int addr,
			      u16 *data, int len)
{
	if (!chip->info->ops->avb_ops->port_qav_read)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->qav_read(chip, addr, data, len);
}

static int mv88e6xxx_qav_write(struct mv88e6xxx_chip *chip, int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->port_qav_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->qav_write(chip, addr, data);
}
#endif /* CONFIG_NET_DSA_MV88E6XXX_SRP */

static int mv88e6xxx_port_qav_write(struct mv88e6xxx_chip *chip, int port,
				    int addr, u16 data)
{
	if (!chip->info->ops->avb_ops->port_qav_write)
		return -EOPNOTSUPP;

	return chip->info->ops->avb_ops->port_qav_write(chip, port, addr, data);
}

static int mv88e6xxx_tc_enable(struct mv88e6xxx_chip *chip,
			       const struct mv88e6xxx_avb_tc_policy *policy)
{
	if (!chip->info->ops->tc_ops->tc_enable)
		return -EOPNOTSUPP;

	return chip->info->ops->tc_ops->tc_enable(chip, policy);
}

static int mv88e6xxx_tc_disable(struct mv88e6xxx_chip *chip)
{
	if (!chip->info->ops->tc_ops->tc_disable)
		return -EOPNOTSUPP;

	return chip->info->ops->tc_ops->tc_disable(chip);
}

/* MQPRIO and CBS helpers */

static int mv88e6xxx_map_tc_cbs_qopt(struct mv88e6xxx_chip *chip,
				     const struct tc_cbs_qopt_offload *cbs_qopt,
				     u16 *rate, u16 *hilimit)
{
	if (!chip->info->ops->tc_ops->map_tc_cbs_qopt)
		return -EOPNOTSUPP;

	return chip->info->ops->tc_ops->map_tc_cbs_qopt(chip, cbs_qopt, rate, hilimit);
}

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
/* Set the AVB global policy limit registers. Caller must acquired register
 * lock.
 *
 * @param chip		Marvell switch chip instance
 * @param hilimit	Maximum frame size allowed for AVB Class A frames
 *
 * @return		0 on success, or a negative error value otherwise
 */
static int mv88e6xxx_avb_set_hilimit(struct mv88e6xxx_chip *chip, u16 hilimit)
{
	u16 data;
	int err;

	if (hilimit > MV88E6XXX_AVB_CFG_HI_LIMIT_MASK)
		return -EINVAL;

	err = mv88e6xxx_avb_read(chip, MV88E6XXX_AVB_CFG_HI_LIMIT, &data, 1);
	if (err)
		return err;

	data &= ~(MV88E6XXX_AVB_CFG_HI_LIMIT_MASK);
	data |= MV88E6XXX_AVB_CFG_HI_LIMIT_SET(hilimit);

	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_HI_LIMIT, hilimit);
	if (err)
		return err;

	return 0;
}

/* Set the global isochronous queue pointer threshold. Caller must acquire
 * register lock.
 *
 * @param chip		Marvell switch chip instance
 * @param threshold	Total number of pointers reserved for isochronous streams
 *
 * @return		0 on success, or a negative error value otherwise
 */
static int mv88e6xxx_qav_set_iso_ptr(struct mv88e6xxx_chip *chip, u16 threshold)
{
	u16 data;
	int err;

	err = mv88e6xxx_qav_read(chip, MV88E6XXX_QAV_CFG, &data, 1);
	if (err)
		return err;

	data &= ~(MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_MASK);
	data |= MV88E6XXX_QAV_CFG_GLOBAL_ISO_PTR_SET(threshold);

	err = mv88e6xxx_qav_write(chip, MV88E6XXX_QAV_CFG, data);
	if (err)
		return err;

	return 0;
}
#endif /* CONFIG_NET_DSA_MV88E6XXX_SRP */

/* Enable or disable a port for AVB. Caller must acquire register lock.
 *
 * @param chip		Marvell switch chip instance
 * @param port		Switch port
 * @param enable	If true, will enable AVB queues on this port.
 *
 * @return		0 on success, or a negative error value otherwise
 */
static int mv88e6xxx_avb_set_port_avb_mode(struct mv88e6xxx_chip *chip,
					   int port, bool enable)
{
	u16 cfg;

	if (enable) {
#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
		/* Require static FDB/MDB entries to admit streams */
		cfg = MV88E6XXX_PORT_AVB_CFG_AVB_MODE_ENHANCED |
			MV88E6XXX_PORT_AVB_CFG_AVB_FILTER_BAD_AVB |
			MV88E6XXX_PORT_AVB_CFG_AVB_DISCARD_BAD;
#else
		/* All frames with AVB priorities are acceptable */
		cfg = MV88E6XXX_PORT_AVB_CFG_AVB_MODE_STANDARD;
#endif
	} else {
		cfg = MV88E6XXX_PORT_AVB_CFG_AVB_MODE_LEGACY;
	}

	return mv88e6xxx_port_avb_write(chip, port, MV88E6XXX_PORT_AVB_CFG, cfg);
}

static int mv88e6xxx_avb_set_avb_mode(struct mv88e6xxx_chip *chip, bool enable)
{
	int port, err;

	for (port = 0, err = 0; port < mv88e6xxx_num_ports(chip); ++port) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6xxx_avb_set_port_avb_mode(chip, port, enable);
		if (err)
			break;
	}

	return err;
}

int mv88e6xxx_avb_tc_enable(struct mv88e6xxx_chip *chip,
			    const struct mv88e6xxx_avb_tc_policy *policy)
{
	int err;

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	err = mv88e6xxx_g1_atu_set_mac_avb(chip, true);
	if (err)
		return err;

	err = mv88e6xxx_qav_set_iso_ptr(chip, mv88e6xxx_num_ports(chip) << 6);
	if (err)
		return err;
#endif

	err = mv88e6xxx_tc_enable(chip, policy);
	if (err)
		return err;

	err = mv88e6xxx_avb_set_avb_mode(chip, true);
	if (err)
		return err;

	return 0;
}

int mv88e6xxx_avb_tc_disable(struct mv88e6xxx_chip *chip)
{
	int err;

	err = mv88e6xxx_avb_set_avb_mode(chip, false);
	if (err)
		return err;

	err = mv88e6xxx_tc_disable(chip);
	if (err)
		return err;

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	err = mv88e6xxx_qav_set_iso_ptr(chip, 0);
	if (err)
		return err;

	err = mv88e6xxx_g1_atu_set_mac_avb(chip, false);
	if (err)
		return err;
#endif

	return 0;
}

int mv88e6xxx_qav_set_port_cbs_qopt(struct mv88e6xxx_chip *chip, int port,
				    const struct tc_cbs_qopt_offload *cbs_qopt)
{
	u16 rate, hilimit;
	int err;

	err = mv88e6xxx_map_tc_cbs_qopt(chip, cbs_qopt, &rate, &hilimit);
	if (err)
		return err;

	err = mv88e6xxx_port_qav_write(chip, port,
				       MV88E6XXX_PORT_QAV_CFG_RATE(cbs_qopt->queue),
				       rate);
	if (err)
		return err;

	err = mv88e6xxx_port_qav_write(chip, port,
				       MV88E6XXX_PORT_QAV_CFG_HI_LIMIT(cbs_qopt->queue),
				       hilimit);
	if (err)
		return err;

	return 0;
}

/* Assign FPri to QPri mappings for each traffic class
 *
 * @param chip		Marvell switch chip instance
 * @param policy	AVB policy settings
 * @param map		Callback for setting individual FPri:QPri mapping
 * @param context	Opaque context passed to callback function
 *
 * @return		0 on success, or error returned by callback
 */
static int mv88e6xxx_qav_assign_qpri(struct mv88e6xxx_chip *chip,
				     const struct mv88e6xxx_avb_tc_policy *policy,
				     int (*map)(u8 fpri, u8 qpri, void *context),
				     void *context)
{
	int tc0_qcount, tc0_base_qpri;
	size_t tc0_fpri_per_qpri;
	int err, fpri;

	tc0_base_qpri = policy->map[MV88E6XXX_AVB_TC_LEGACY].qpri;
	tc0_fpri_per_qpri =
		DIV_ROUND_UP(IEEE_8021Q_MAX_PRIORITIES - 2,
			     policy->map[MV88E6XXX_AVB_TC_LEGACY].count);

	/* Match TC1/TC2 (AVB) FPri to QPri mappings to avoid needing to
	 * configure legacy AVB registers, which map non-AVB frame FPri/QPris
	 * to non-conflicting values. The also enables prioritization when
	 * CONFIG_NET_DSA_MV88E6XXX_SRP is unset.
	 *
	 * Distribute TC0 (non-AVB) queues amongst remaining FPris.
	 */
	for (fpri = 0, tc0_qcount = 0; fpri < IEEE_8021Q_MAX_PRIORITIES; fpri++) {
		if (policy->map[MV88E6XXX_AVB_TC_LO].fpri == fpri) {
			err = map(fpri, policy->map[MV88E6XXX_AVB_TC_LO].qpri, context);
		} else if (policy->map[MV88E6XXX_AVB_TC_HI].fpri == fpri) {
			err = map(fpri, policy->map[MV88E6XXX_AVB_TC_HI].qpri, context);
		} else {
			int qpri = tc0_base_qpri + (tc0_qcount++ / tc0_fpri_per_qpri);

			err = map(fpri, qpri, context);
		}

		if (err)
			break;
	}

	return err;
}

/* Family-specific 802.1Qav support */

static inline u16 mv88e6352_avb_pri_map_to_reg(const struct mv88e6xxx_avb_priority_map map[])
{
	return MV88E6352_AVB_CFG_AVB_HI_FPRI_SET(map[MV88E6XXX_AVB_TC_HI].fpri) |
		MV88E6352_AVB_CFG_AVB_HI_QPRI_SET(map[MV88E6XXX_AVB_TC_HI].qpri) |
		MV88E6352_AVB_CFG_AVB_LO_FPRI_SET(map[MV88E6XXX_AVB_TC_LO].fpri) |
		MV88E6352_AVB_CFG_AVB_LO_QPRI_SET(map[MV88E6XXX_AVB_TC_LO].qpri);
}

static int mv88e6352_qav_map_fpri_qpri(u8 fpri, u8 qpri, void *reg)
{
	mv88e6352_g1_ieee_pri_set(fpri, qpri, (u16 *)reg);
	return 0;
}

static int mv88e6352_tc_enable(struct mv88e6xxx_chip *chip,
			       const struct mv88e6xxx_avb_tc_policy *policy)
{
	u16 reg = 0;
	int err;
#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	int tc;

	/* Validate TC to QPri mapping */
	for (tc = MV88E6XXX_AVB_TC_LO; tc <= MV88E6XXX_AVB_TC_HI; tc++) {
		if (policy->map[tc].qpri < MV88E6352_AVB_QUEUE_MIN(tc) ||
		    policy->map[tc].qpri > MV88E6352_AVB_QUEUE_MAX(tc)) {
			dev_err(chip->dev, "%s: bad QPri %d for TC %d\n",
				__func__, policy->map[tc].qpri, tc);
			return -EOPNOTSUPP;
		}
	}

	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB,
				  mv88e6352_avb_pri_map_to_reg(policy->map));
	if (err)
		return err;
#endif

	err = mv88e6xxx_qav_assign_qpri(chip, policy, mv88e6352_qav_map_fpri_qpri, &reg);
	if (err)
		return err;

	err = mv88e6xxx_g1_set_ieee_pri_map(chip, reg);
	if (err)
		return err;

	return 0;
}

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
static struct mv88e6xxx_avb_priority_map
mv88e6352_init_avb_pri_map[MV88E6XXX_AVB_TC_MAX + 1] = {
	[MV88E6XXX_AVB_TC_LO] = {
		/* VI, queue 2 */
		.fpri = 0x4,
		.qpri = 0x2
	},
	[MV88E6XXX_AVB_TC_HI] = {
		/* VO, queue 3 */
		.fpri = 0x5,
		.qpri = 0x3
	},
};
#endif

static int mv88e6352_tc_disable(struct mv88e6xxx_chip *chip)
{
	int err;

	err = mv88e6250_g1_ieee_pri_map(chip);
	if (err)
		return err;

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB,
				  mv88e6352_avb_pri_map_to_reg(mv88e6352_init_avb_pri_map));
	if (err)
		return err;
#endif

	return 0;
}

static int mv88e6341_map_tc_cbs_qopt(struct mv88e6xxx_chip *chip,
				     const struct tc_cbs_qopt_offload *cbs_qopt,
				     u16 *rate, u16 *hilimit)
{
	if (cbs_qopt->enable) {
		*rate = DIV_ROUND_UP(cbs_qopt->idleslope, MV88E6341_AVB_CFG_RATE_UNITS);
		*rate = clamp_t(u16, *rate, 1, MV88E6341_AVB_CFG_RATE_MASK);

		*hilimit = cbs_qopt->hicredit;
		*hilimit = clamp_t(u16, *hilimit, 1, MV88E6341_AVB_CFG_HI_LIMIT_MASK);
	} else {
		*rate = 0;
		*hilimit = MV88E6341_AVB_CFG_HI_LIMIT_MASK;
	}

	return 0;
}

const struct mv88e6xxx_tc_ops mv88e6341_tc_ops = {
	.tc_enable		= mv88e6352_tc_enable,
	.tc_disable		= mv88e6352_tc_disable,
	.map_tc_cbs_qopt	= mv88e6341_map_tc_cbs_qopt,
};

static int mv88e6352_map_tc_cbs_qopt(struct mv88e6xxx_chip *chip,
				     const struct tc_cbs_qopt_offload *cbs_qopt,
				     u16 *rate, u16 *hilimit)
{
	if (cbs_qopt->enable) {
		*rate = DIV_ROUND_UP(cbs_qopt->idleslope, MV88E6352_AVB_CFG_RATE_UNITS);
		*rate = clamp_t(u16, *rate, 1, MV88E6352_AVB_CFG_RATE_MASK);

		*hilimit = cbs_qopt->hicredit;
		*hilimit = clamp_t(u16, *hilimit, 1, MV88E6352_AVB_CFG_HI_LIMIT_MASK);
	} else {
		*rate = 0;
		*hilimit = MV88E6352_AVB_CFG_HI_LIMIT_MASK;
	}

	return 0;
}

const struct mv88e6xxx_tc_ops mv88e6352_tc_ops = {
	.tc_enable		= mv88e6352_tc_enable,
	.tc_disable		= mv88e6352_tc_disable,
	.map_tc_cbs_qopt	= mv88e6352_map_tc_cbs_qopt,
};

static inline u16 mv88e6390_avb_pri_map_to_reg(const struct mv88e6xxx_avb_priority_map map[])
{
	return MV88E6390_AVB_CFG_AVB_HI_FPRI_SET(map[MV88E6XXX_AVB_TC_HI].fpri) |
		MV88E6390_AVB_CFG_AVB_HI_QPRI_SET(map[MV88E6XXX_AVB_TC_HI].qpri) |
		MV88E6390_AVB_CFG_AVB_LO_FPRI_SET(map[MV88E6XXX_AVB_TC_LO].fpri) |
		MV88E6390_AVB_CFG_AVB_LO_QPRI_SET(map[MV88E6XXX_AVB_TC_LO].qpri);
}

static int mv88e6390_qav_map_fpri_qpri(u8 fpri, u8 qpri, void *context)
{
	int err, port;
	struct mv88e6xxx_chip *chip = (struct mv88e6xxx_chip *)context;

	for (port = 0, err = 0; port < mv88e6xxx_num_ports(chip); port++) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6390_port_set_ieeepmt_ingress_pcp(chip, port, fpri,
							     fpri, qpri);
		if (err)
			break;
	}

	return err;
}

static int mv88e6390_tc_enable(struct mv88e6xxx_chip *chip,
			       const struct mv88e6xxx_avb_tc_policy *policy)
{
	int err;

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB,
				  mv88e6390_avb_pri_map_to_reg(policy->map));
	if (err)
		return err;
#endif

	err = mv88e6xxx_qav_assign_qpri(chip, policy, mv88e6390_qav_map_fpri_qpri, chip);
	if (err)
		return err;

	return 0;
}

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
static struct mv88e6xxx_avb_priority_map
mv88e6390_init_avb_pri_map[MV88E6XXX_AVB_TC_MAX + 1] = {
	[MV88E6XXX_AVB_TC_LO] = {
		/* EE, queue 6 */
		.fpri = 0x2,
		.qpri = 0x6
	},
	[MV88E6XXX_AVB_TC_HI] = {
		/* CA, queue 7 */
		.fpri = 0x3,
		.qpri = 0x7
	},
};
#endif

static int mv88e6390_tc_disable(struct mv88e6xxx_chip *chip)
{
	int err, port;

	for (port = 0, err = 0; port < mv88e6xxx_num_ports(chip); port++) {
		if (!dsa_is_user_port(chip->ds, port))
			continue;

		err = mv88e6390_port_tag_remap(chip, port);
		if (err)
			break;
	}

#ifdef CONFIG_NET_DSA_MV88E6XXX_SRP
	err = mv88e6xxx_avb_write(chip, MV88E6XXX_AVB_CFG_AVB,
				  mv88e6390_avb_pri_map_to_reg(mv88e6390_init_avb_pri_map));
	if (err)
		return err;
#endif

	return err;
}

static int mv88e6390_map_tc_cbs_qopt(struct mv88e6xxx_chip *chip,
				     const struct tc_cbs_qopt_offload *cbs_qopt,
				     u16 *rate, u16 *hilimit)
{
	if (cbs_qopt->enable) {
		*rate = DIV_ROUND_UP(cbs_qopt->idleslope, MV88E6390_AVB_CFG_RATE_UNITS);
		*rate = clamp_t(u16, *rate, 1, MV88E6390_AVB_CFG_RATE_MASK);

		*hilimit = cbs_qopt->hicredit;
		*hilimit = clamp_t(u16, *hilimit, 1, MV88E6390_AVB_CFG_HI_LIMIT_MASK);
	} else {
		*rate = 0;
		*hilimit = MV88E6390_AVB_CFG_HI_LIMIT_MASK;
	}

	return 0;
}

const struct mv88e6xxx_tc_ops mv88e6390_tc_ops = {
	.tc_enable		= mv88e6390_tc_enable,
	.tc_disable		= mv88e6390_tc_disable,
	.map_tc_cbs_qopt	= mv88e6390_map_tc_cbs_qopt,
};
