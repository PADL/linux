/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Marvell 88E6xxx Switch DCB support
 */

#ifndef _MV88E6XXX_DCB_H_
#define _MV88E6XXX_DCB_H_

#include "chip.h"

extern const struct mv88e6xxx_dcb_ops mv88e6352_dcb_ops;
extern const struct mv88e6xxx_dcb_ops mv88e6390_dcb_ops;
extern const struct mv88e6xxx_dcb_ops mv88e6393x_dcb_ops;

int mv88e6xxx_port_get_pcp_prio(struct dsa_switch *ds, int port, u8 pcp);
int mv88e6xxx_port_add_pcp_prio(struct dsa_switch *ds, int port, u8 pcp,
				u8 prio);
int mv88e6xxx_port_del_pcp_prio(struct dsa_switch *ds, int port, u8 pcp,
				u8 prio);

int mv88e6xxx_port_get_dscp_prio(struct dsa_switch *ds, int port, u8 dscp);
int mv88e6xxx_port_add_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				 u8 prio);
int mv88e6xxx_port_del_dscp_prio(struct dsa_switch *ds, int port, u8 dscp,
				 u8 prio);

#endif /* _MV88E6XXX_DCB_H_ */
