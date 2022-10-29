// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Marvell 88E6xxx Switch Remote Management Unit Support
 *
 * Copyright (c) 2022 Mattias Forsblad <mattias.forsblad@gmail.com>
 *
 */

#include <net/dsa.h>
#include "chip.h"
#include "global1.h"
#include "rmu.h"

void mv88e6xxx_rmu_conduit_state_change(struct dsa_switch *ds,
					const struct net_device *conduit,
					bool operational)
{
	struct dsa_port *cpu_dp = conduit->dsa_ptr;
	struct mv88e6xxx_chip *chip = ds->priv;
	int port;
	int ret;

	port = dsa_towards_port(ds, cpu_dp->ds->index, cpu_dp->index);

	mv88e6xxx_reg_lock(chip);

	if (operational && chip->info->ops->rmu_enable) {
		ret = chip->info->ops->rmu_enable(chip, port);
		if (ret == -EOPNOTSUPP) {
			dev_info(chip->dev, "RMU: not usable on this board");
			goto out;
		} else if (ret < 0) {
			dev_err(chip->dev, "RMU: unable to enable on port %d %pe",
				port, ERR_PTR(ret));
			goto out;
		}

		chip->rmu_conduit = (struct net_device *)conduit;

		dev_dbg(chip->dev, "RMU: Enabled on port %d", port);
	} else {
		if (chip->info->ops->rmu_disable)
			chip->info->ops->rmu_disable(chip);

		chip->rmu_conduit = NULL;
	}

out:
	mv88e6xxx_reg_unlock(chip);
}

void mv88e6xxx_rmu_frame2reg_handler(struct dsa_switch *ds,
				     struct sk_buff *skb,
				     u8 seqno)
{
	struct mv88e6xxx_rmu_header *rmu_header;
	struct mv88e6xxx_chip *chip = ds->priv;
	unsigned char *ethhdr;
	u8 expected_seqno;

	/* Check received destination MAC is the conduit MAC address */
	if (!chip->rmu_conduit)
		goto drop;

	ethhdr = skb_mac_header(skb);
	if (!ether_addr_equal(chip->rmu_conduit->dev_addr, ethhdr)) {
		dev_dbg_ratelimited(ds->dev, "RMU: mismatched MAC address for request: rx %pM expecting %pM\n",
				    ethhdr, chip->rmu_conduit->dev_addr);
		goto drop;
	}

	expected_seqno = dsa_inband_seqno(&chip->rmu_inband);
	if (seqno != expected_seqno) {
		dev_dbg_ratelimited(ds->dev, "RMU: mismatched seqno for request: rx %d expecting %d\n",
				    seqno, expected_seqno);
		goto drop;
	}

	if (skb->len < 4 + sizeof(*rmu_header)) {
		dev_dbg_ratelimited(ds->dev, "RMU: response too short (%d bytes)\n",
				    skb->len);
		dsa_inband_complete(&chip->rmu_inband, NULL, 0, -ETIMEDOUT);
		goto drop;
	}

	rmu_header = (struct mv88e6xxx_rmu_header *)(skb->data + 4);
	if (rmu_header->format != MV88E6XXX_RMU_RESP_FORMAT_1 &&
	    rmu_header->format != MV88E6XXX_RMU_RESP_FORMAT_2) {
		dev_dbg_ratelimited(ds->dev, "RMU: invalid format: rx %d\n",
				    be16_to_cpu(rmu_header->format));
		goto drop;
	}

drop:
	return;
}
