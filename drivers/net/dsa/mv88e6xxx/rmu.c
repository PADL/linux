// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Marvell 88E6xxx Switch Remote Management Unit Support
 *
 * Copyright (c) 2022 Mattias Forsblad <mattias.forsblad@gmail.com>
 *
 * Copyright (c) 2025 PADL Software Pty Ltd
 */

#include <linux/dsa/mv88e6xxx.h>
#include <net/dsa.h>
#include "chip.h"
#include "global1.h"
#include "port.h"
#include "rmu.h"

static const u8 mv88e6xxx_rmu_dest_addr[ETH_ALEN] = {
	0x01, 0x50, 0x43, 0x00, 0x00, 0x00
};

static void mv88e6xxx_rmu_create_l2(struct dsa_switch *ds,
				    struct mv88e6xxx_chip *chip,
				    struct sk_buff *skb,
				    bool edsa)
{
	struct dsa_tagger_data *tagger_data = ds->tagger_data;
	struct ethhdr *eth;
	u8 *header;

	/* Create RMU L2 header. */
	header = skb_push(skb, 2);
	/* Two bytes of EtherType, which is ignored by the switch */
	header[0] = 0;
	header[1] = 0;

	/* Ask tagger to add {E}DSA header */
	tagger_data->rmu_reg2frame(ds, skb);

	/* Insert RMU MAC destination address */
	eth = skb_push(skb, ETH_ALEN * 2);

	memcpy(eth->h_dest, mv88e6xxx_rmu_dest_addr, ETH_ALEN);
	ether_addr_copy(eth->h_source, chip->rmu_conduit->dev_addr);
	skb_reset_network_header(skb);
}

static void mv88e6xxx_rmu_fill_seqno(struct sk_buff *skb, u32 seqno, int offset)
{
	u8 *dsa_header = skb->data + offset;

	dsa_header[3] = seqno;
}

/* 2 MAC address, 2 byte Ethertype, 2 bytes padding, to DSA header */
static void mv88e6xxx_rmu_fill_seqno_edsa(struct sk_buff *skb, u32 seqno)
{
	mv88e6xxx_rmu_fill_seqno(skb, seqno, ETH_ALEN * 2 + 2 + 2);
}

/* 2 MAC address, to DSA header */
static void mv88e6xxx_rmu_fill_seqno_dsa(struct sk_buff *skb, u32 seqno)
{
	mv88e6xxx_rmu_fill_seqno(skb, seqno, ETH_ALEN * 2);
}

static inline bool __mv88e6xxx_rmu_should_retry(struct mv88e6xxx_chip *chip,
						int err)
{
	/* if no MDIO, retry in case of timeout */
	return err == -ETIMEDOUT &&
		chip->rmu_state == MV88E6XXX_RMU_ONLY_ENABLED;
}

static int __mv88e6xxx_rmu_request_retry(struct mv88e6xxx_chip *chip,
					 struct sk_buff *skb, bool edsa,
					 void *resp, unsigned int resp_len,
					 int timeout_ms)
{
	unsigned long retry_timeout;
	ktime_t now, delay;
	int err;

	/* rate limit RMU requests */
	now = ktime_get();
	delay = ktime_sub(ktime_add_us(chip->rmu_last_resp,
				       MV88E6XXX_RMU_REQUEST_RATE_USEC_MIN), now);

	if (delay > 0)
		usleep_range(ktime_to_us(delay), MV88E6XXX_RMU_REQUEST_RATE_USEC_MAX);

	/* retry RMU requests if MDIO fallback unavailable */
	if (chip->rmu_state == MV88E6XXX_RMU_ONLY_ENABLED)
		retry_timeout = jiffies + msecs_to_jiffies(MV88E6XXX_RMU_RETRY_TIMEOUT_MS);
	else
		retry_timeout = 0;

	do {
		err = dsa_inband_request(&chip->rmu_inband, skb_get(skb),
					 (edsa ? mv88e6xxx_rmu_fill_seqno_edsa :
					  mv88e6xxx_rmu_fill_seqno_dsa),
					 resp, resp_len, timeout_ms);
	} while (__mv88e6xxx_rmu_should_retry(chip, err) &&
		 time_is_before_eq_jiffies(retry_timeout));

	dev_kfree_skb_any(skb);

	if (__mv88e6xxx_rmu_should_retry(chip, err) && !chip->ds->dst->setup) {
		dev_info(chip->dev, "RMU: request timed out during switch setup, deferring\n");
		err = -EPROBE_DEFER;
	}

	if (err != -ETIMEDOUT)
		chip->rmu_last_resp = ktime_get();

	return err;
}

static int mv88e6xxx_rmu_request(struct mv88e6xxx_chip *chip,
				 const void *req, int req_len,
				 void *resp, unsigned int resp_len,
				 int timeout_ms)
{
	struct sk_buff *skb;
	unsigned char *data;
	bool edsa;

	if (!chip->rmu_conduit) {
		dev_err(chip->dev, "RMU: conduit device uninitialized");
		return -EINVAL;
	}

	skb = dev_alloc_skb(64);
	if (!skb)
		return -ENOMEM;

	/* Insert RMU request message */
	data = skb_put(skb, req_len);
	memcpy(data, req, req_len);

	edsa = chip->tag_protocol == DSA_TAG_PROTO_EDSA;

	if (chip->ds->tagger_data == NULL)
		return -EOPNOTSUPP; /* can happen on teardown */

	mv88e6xxx_rmu_create_l2(chip->ds, chip, skb, edsa);
	skb->dev = chip->rmu_conduit;

	return __mv88e6xxx_rmu_request_retry(chip, skb, edsa,
					     resp, resp_len,
					     timeout_ms);
}

int mv88e6xxx_rmu_stats(struct mv88e6xxx_chip *chip, int port,
			uint64_t *data,
			const struct mv88e6xxx_hw_stat *hw_stats,
			int num_hw_stats)
{
	__be16 req[] = {
		MV88E6XXX_RMU_REQ_FORMAT_SOHO,
		MV88E6XXX_RMU_REQ_PAD,
		MV88E6XXX_RMU_REQ_CODE_MIB,
		htons(port),
	};
	const struct mv88e6xxx_hw_stat *stat;
	struct mv88e6xxx_rmu_mib_resp resp;
	int i, j, ret;
	int resp_len;
	u64 high;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED)
		return -EOPNOTSUPP;

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command MIB %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: MIB returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.rmu_header.code != MV88E6XXX_RMU_RESP_CODE_MIB) {
		dev_err(chip->dev, "RMU: MIB returned wrong code %d\n",
			be16_to_cpu(resp.rmu_header.code));
		return -EPROTO;
	}


	for (i = 0, j = 0; i < num_hw_stats; i++) {
		stat = &hw_stats[i];
		if (!(stat->type & chip->info->stats_type))
			continue;

		if (stat->type & STATS_TYPE_PORT) {
			switch (stat->reg) {
			case MV88E6XXX_PORT_IN_DISCARD_LO:
				data[j] = be16_to_cpu(resp.port[0]) << 16;
				data[j] |= be16_to_cpu(resp.port[1]);
				break;
			case MV88E6XXX_PORT_IN_FILTERED:
				data[j] = be16_to_cpu(resp.port[3]);
				break;
			case MV88E6XXX_PORT_OUT_FILTERED:
				data[j] = be16_to_cpu(resp.port[5]);
				break;
			default:
				return -EINVAL;
			}
		}

		if (stat->type & STATS_TYPE_BANK0) {
			data[j] = be32_to_cpu(resp.bank0[stat->reg]);
			if (stat->size == 8) {
				high = be32_to_cpu(resp.bank0[stat->reg + 1]);
				data[j] |= (high << 32);
			}
		}

		if (stat->type & STATS_TYPE_BANK1) {
			/* Not available via RMU, use SMI (if available) */
			mv88e6xxx_stats_get_stat(chip, port, stat, &data[j]);
		}
		j++;
	}

	return j;
}

int mv88e6xxx_rmu_write(struct mv88e6xxx_chip *chip, int addr, int reg, u16 val)
{
	__be16 req[] = {
		MV88E6XXX_RMU_REQ_FORMAT_SOHO,
		MV88E6XXX_RMU_REQ_PAD,
		MV88E6XXX_RMU_REQ_CODE_REG_RW,
		MV88E6XXX_RMU_REQ_RW_0_WRITE(addr, reg),
		htons(val),
		MV88E6XXX_RMU_REQ_RW_0_END,
		MV88E6XXX_RMU_REQ_RW_1_END,
	};
	struct mv88e6xxx_rmu_header resp;
	int resp_len;
	int ret = -1;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED ||
	    (chip->rmu_flags & MV88E6XXX_RMU_IS_SLOW))
		return -EOPNOTSUPP;

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command REQ_RW:WRITE %pe "
			"addr %d reg %d val %04x\n",
			ERR_PTR(ret), addr, reg, val);
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: write returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.code != MV88E6XXX_RMU_RESP_CODE_REG_RW) {
		dev_err(chip->dev, "RMU: write returned wrong code %d\n",
			be16_to_cpu(resp.code));
		return -EPROTO;
	}

	return 0;
}

static void mv88e6xxx_rmu_read_latency(struct mv88e6xxx_chip *chip,
				       ktime_t latency)
{
	ktime_t average = 0;
	int i;

	if (chip->rmu_state == MV88E6XXX_RMU_ONLY_ENABLED || /* no MDIO */
	    chip->rmu_samples > ARRAY_SIZE(chip->rmu_read_latencies))
		return;

	chip->rmu_read_latencies[chip->rmu_samples++] = latency;

	if (chip->rmu_samples == ARRAY_SIZE(chip->rmu_read_latencies)) {
		for (i = 0; i < ARRAY_SIZE(chip->rmu_read_latencies); i++)
			average += chip->rmu_read_latencies[i];
		average = average / ARRAY_SIZE(chip->rmu_read_latencies);

		dev_dbg(chip->dev, "RMU %lldus, smi %lldus\n",
			div_u64(average, 1000),
			div_u64(chip->smi_read_latency, 1000));

		if (chip->smi_read_latency < average)
			chip->rmu_flags |= MV88E6XXX_RMU_IS_SLOW;
		else
			chip->rmu_flags &= ~(MV88E6XXX_RMU_IS_SLOW);

		chip->rmu_samples = U32_MAX;
	}
}

int mv88e6xxx_rmu_read(struct mv88e6xxx_chip *chip, int addr, int reg,
		       u16 *val)
{
	__be16 req[] = {
		MV88E6XXX_RMU_REQ_FORMAT_SOHO,
		MV88E6XXX_RMU_REQ_PAD,
		MV88E6XXX_RMU_REQ_CODE_REG_RW,
		MV88E6XXX_RMU_REQ_RW_0_READ(addr, reg),
		0,
		MV88E6XXX_RMU_REQ_RW_0_END,
		MV88E6XXX_RMU_REQ_RW_1_END,
	};
	struct mv88e6xxx_rmu_rw_resp resp;
	int resp_len;
	ktime_t start;
	int ret;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED ||
	    (chip->rmu_flags & MV88E6XXX_RMU_IS_SLOW))
		return -EOPNOTSUPP;

	start = ktime_get();

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command REQ_RW:READ %pe "
			"addr %d reg %d\n",
			ERR_PTR(ret), addr, reg);
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: read returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.rmu_header.code != MV88E6XXX_RMU_RESP_CODE_REG_RW) {
		dev_err(chip->dev, "RMU: read returned wrong code %d\n",
			be16_to_cpu(resp.rmu_header.code));
		return -EPROTO;
	}

	mv88e6xxx_rmu_read_latency(chip, ktime_get() - start);

	*val = ntohs(resp.value);

	return 0;
}

int mv88e6xxx_rmu_wait_bit(struct mv88e6xxx_chip *chip, int addr, int reg,
			   int bit, int val)
{
	__be16 req[] = {
		MV88E6XXX_RMU_REQ_FORMAT_SOHO,
		MV88E6XXX_RMU_REQ_PAD,
		MV88E6XXX_RMU_REQ_CODE_REG_RW,
		val ? MV88E6XXX_RMU_REQ_RW_0_WAIT_1(addr, reg) :
		MV88E6XXX_RMU_REQ_RW_0_WAIT_0(addr, reg),
		htons(bit),
		MV88E6XXX_RMU_REQ_RW_0_END,
		MV88E6XXX_RMU_REQ_RW_1_END,
	};
	struct mv88e6xxx_rmu_rw_resp resp;
	int resp_len;
	int ret = -1;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED ||
	    (chip->rmu_flags & MV88E6XXX_RMU_IS_SLOW))
		return -EOPNOTSUPP;

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_WAIT_BIT_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command REQ_RW:WAIT %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: wait on bit returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.rmu_header.code != MV88E6XXX_RMU_RESP_CODE_REG_RW) {
		dev_err(chip->dev, "RMU: wait on bit returned wrong code %d\n",
			be16_to_cpu(resp.rmu_header.code));
		return -EPROTO;
	}

	if ((ntohs(resp.value) & 0xff) == 0xff) {
		dev_err(chip->dev, "RMU: wait on bit timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

int mv88e6xxx_rmu_atu_mac_data_write(struct mv88e6xxx_chip *chip,
				     const struct mv88e6xxx_atu_entry *entry)
{
	struct mv88e6xxx_rmu_rw_mac_data_resp req, resp;
	ktime_t start;
	u16 data;
	int resp_len;
	int ret;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED ||
	    (chip->rmu_flags & MV88E6XXX_RMU_IS_SLOW))
		return -EOPNOTSUPP;

	memset(&req, 0, sizeof(req));

	req.rmu_header.format = MV88E6XXX_RMU_REQ_FORMAT_SOHO;
	req.rmu_header.prodnr = MV88E6XXX_RMU_REQ_PAD;
	req.rmu_header.code = MV88E6XXX_RMU_REQ_CODE_REG_RW;

	data = mv88e6xxx_g1_atu_entry_to_data(chip, entry);

	req.mac[0].cmd = MV88E6XXX_RMU_REQ_RW_0_WRITE(chip->info->global1_addr,
						      MV88E6XXX_G1_ATU_MAC01);
	req.mac[0].value = htons((entry->mac[0] << 8) | entry->mac[1]);
	req.mac[1].cmd = MV88E6XXX_RMU_REQ_RW_0_WRITE(chip->info->global1_addr,
						      MV88E6XXX_G1_ATU_MAC23);
	req.mac[1].value = htons((entry->mac[2] << 8) | entry->mac[3]);
	req.mac[2].cmd = MV88E6XXX_RMU_REQ_RW_0_WRITE(chip->info->global1_addr,
						      MV88E6XXX_G1_ATU_MAC45);
	req.mac[2].value = htons((entry->mac[4] << 8) | entry->mac[5]);

	req.data.cmd = MV88E6XXX_RMU_REQ_RW_0_WRITE(chip->info->global1_addr,
						    MV88E6XXX_G1_ATU_DATA);
	req.data.value = htons(data);

	req.end.cmd = MV88E6XXX_RMU_REQ_RW_0_END;
	req.end.value = MV88E6XXX_RMU_REQ_RW_1_END;

	start = ktime_get();

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, &req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command REQ_RW:MAC_DATA_WRITE %pe ",
			ERR_PTR(ret));
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: write MAC DATA returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.rmu_header.code != MV88E6XXX_RMU_RESP_CODE_REG_RW) {
		dev_err(chip->dev, "RMU: write MAC DATA returned wrong code %d\n",
			be16_to_cpu(resp.rmu_header.code));
		return -EPROTO;
	}

	return 0;
}

int mv88e6xxx_rmu_atu_mac_data_read(struct mv88e6xxx_chip *chip,
				    struct mv88e6xxx_atu_entry *entry)
{
	struct mv88e6xxx_rmu_rw_mac_data_resp req, resp;
	ktime_t start;
	int resp_len;
	u16 tmp;
	int ret;

	if (chip->rmu_state == MV88E6XXX_RMU_DISABLED ||
	    (chip->rmu_flags & MV88E6XXX_RMU_IS_SLOW))
		return -EOPNOTSUPP;

	memset(&req, 0, sizeof(req));

	req.rmu_header.format = MV88E6XXX_RMU_REQ_FORMAT_SOHO;
	req.rmu_header.prodnr = MV88E6XXX_RMU_REQ_PAD;
	req.rmu_header.code = MV88E6XXX_RMU_REQ_CODE_REG_RW;

	req.mac[0].cmd = MV88E6XXX_RMU_REQ_RW_0_READ(chip->info->global1_addr,
						     MV88E6XXX_G1_ATU_MAC01);
	req.mac[1].cmd = MV88E6XXX_RMU_REQ_RW_0_READ(chip->info->global1_addr,
						     MV88E6XXX_G1_ATU_MAC23);
	req.mac[2].cmd = MV88E6XXX_RMU_REQ_RW_0_READ(chip->info->global1_addr,
						     MV88E6XXX_G1_ATU_MAC45);

	req.data.cmd = MV88E6XXX_RMU_REQ_RW_0_READ(chip->info->global1_addr,
						   MV88E6XXX_G1_ATU_DATA);

	req.end.cmd = MV88E6XXX_RMU_REQ_RW_0_END;
	req.end.value = MV88E6XXX_RMU_REQ_RW_1_END;

	start = ktime_get();

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, &req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command REQ_RW:MAC_DATA_READ %pe ",
			ERR_PTR(ret));
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: read MAC DATA returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.rmu_header.code != MV88E6XXX_RMU_RESP_CODE_REG_RW) {
		dev_err(chip->dev, "RMU: read MAC DATA returned wrong code %d\n",
			be16_to_cpu(resp.rmu_header.code));
		return -EPROTO;
	}

	mv88e6xxx_rmu_read_latency(chip, ktime_get() - start);

	tmp = ntohs(resp.mac[0].value);
	entry->mac[0] = tmp >> 8;
	entry->mac[1] = tmp & 0xff;

	tmp = ntohs(resp.mac[1].value);
	entry->mac[2] = tmp >> 8;
	entry->mac[3] = tmp & 0xff;

	tmp = ntohs(resp.mac[2].value);
	entry->mac[4] = tmp >> 8;
	entry->mac[5] = tmp & 0xff;

	mv88e6xxx_g1_atu_data_to_entry(chip, ntohs(resp.data.value), entry);

	return 0;
}

static int mv88e6xxx_rmu_get_id(struct mv88e6xxx_chip *chip)
{
	const __be16 req[4] = {
		MV88E6XXX_RMU_REQ_FORMAT_GET_ID,
		MV88E6XXX_RMU_REQ_PAD,
		MV88E6XXX_RMU_REQ_CODE_GET_ID,
		MV88E6XXX_RMU_REQ_DATA};
	struct mv88e6xxx_rmu_header resp;
	int resp_len;
	int ret = -1;

	resp_len = sizeof(resp);
	ret = mv88e6xxx_rmu_request(chip, req, sizeof(req),
				    &resp, resp_len,
				    MV88E6XXX_RMU_REQUEST_TIMEOUT_MS);
	if (ret < 0) {
		dev_dbg(chip->dev, "RMU: error for command GET_ID %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	if (ret < resp_len) {
		dev_err(chip->dev, "RMU: GET_ID returned wrong length: rx %d expecting %d\n",
			ret, resp_len);
		return -EPROTO;
	}

	if (resp.code != MV88E6XXX_RMU_RESP_CODE_GOT_ID) {
		dev_dbg(chip->dev, "RMU: GET_ID returned wrong code %d\n",
			be16_to_cpu(resp.code));
		return -EPROTO;
	}

	dev_dbg(chip->dev, "RMU: product ID %4x\n", be16_to_cpu(resp.prodnr));

	return 0;
}

void mv88e6xxx_rmu_conduit_state_change(struct dsa_switch *ds,
					const struct net_device *conduit,
					bool operational)
{
	struct dsa_port *cpu_dp = conduit->dsa_ptr;
	struct mv88e6xxx_chip *chip = ds->priv;
	ktime_t start;
	int port;
	int ret;
	u16 id;

	if (mv88e6xxx_rmu_disabled() ||
	    chip->rmu_state == MV88E6XXX_RMU_ONLY_ENABLED)
		return;

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

		/* Get the device ID to prove that the RMU works */
		ret = mv88e6xxx_rmu_get_id(chip);
		if (ret < 0) {
			dev_err(chip->dev, "RMU: initialization check failed %pe",
				ERR_PTR(ret));
			goto out;
		}

		start = ktime_get();
		ret = mv88e6xxx_port_read(chip, 0, MV88E6XXX_PORT_SWITCH_ID,
					  &id);
		chip->smi_read_latency = ktime_get() - start;
		chip->rmu_state = MV88E6XXX_RMU_ENABLED;

		dev_info(chip->dev, "RMU: enabled on port %d via conduit device %s",
			 port, chip->rmu_conduit->name);
	} else {
		if (chip->info->ops->rmu_disable)
			chip->info->ops->rmu_disable(chip);

		chip->rmu_state = MV88E6XXX_RMU_DISABLED;
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
	int resp_len;
	int err = 0;

	if (!chip->rmu_conduit)
		goto drop;

	/* Check received destination MAC is the conduit MAC address */
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

	rmu_header = (struct mv88e6xxx_rmu_header *)(skb->data + 4);
	resp_len = skb->len - 4;
	if (resp_len == 0) {
		dev_dbg_ratelimited(ds->dev, "RMU: zero length response\n");
		err = -ETIMEDOUT;
	} else if (rmu_header->format != MV88E6XXX_RMU_RESP_FORMAT_1 &&
	    rmu_header->format != MV88E6XXX_RMU_RESP_FORMAT_2) {
		dev_dbg_ratelimited(ds->dev, "RMU: invalid format: rx %d\n",
				    be16_to_cpu(rmu_header->format));
		err = -EPROTO;
	}

	dsa_inband_complete(&chip->rmu_inband, rmu_header, resp_len, err);
drop:
	return;
}

int mv88e6xxx_detect_rmu_only(struct mv88e6xxx_chip *chip)
{
	int err;

	if (mv88e6xxx_rmu_disabled())
		return -ENODEV;

	chip->tag_protocol = DSA_TAG_PROTO_EDSA; /* must be set for get_tag_protocol() */
	chip->rmu_state = MV88E6XXX_RMU_ONLY_ENABLED;

	err = mv88e6xxx_register_switch(chip);
	if (err) {
		dev_err(chip->dev, "RMU: failed RMU-only mode switch registration: %pe\n",
			ERR_PTR(err));
		goto error;
	}

	BUG_ON(chip->ds == NULL);

	err = mv88e6xxx_rmu_get_id(chip);
	if (err) {
		dev_err(chip->dev, "RMU: RMU-only mode validation failed: %pe\n",
			ERR_PTR(err));
		goto error;
	}

	err = mv88e6xxx_detect(chip);
	if (err)
		goto error;

	BUG_ON(chip->rmu_conduit == NULL);

	dev_info(chip->dev, "RMU: RMU-only mode enabled on conduit device %s\n",
		 chip->rmu_conduit->name);

	return 0;

error:
	chip->tag_protocol = DSA_TAG_PROTO_NONE;
	chip->rmu_conduit = NULL;
	chip->rmu_state = MV88E6XXX_RMU_DISABLED;

	return err;
}
