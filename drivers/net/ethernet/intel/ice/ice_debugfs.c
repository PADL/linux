// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, Intel Corporation. */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include "ice.h"
#include "ice_lib.h"

static struct dentry *ice_debugfs_root;

#define ICE_CGU_I2C_MAX_NODES		10
#define ICE_CGU_I2C_OFFSET_SIZE_M	GENMASK(6, 5)
#define ICE_CGU_I2C_MAX_LEN		FIELD_MAX(ICE_AQC_I2C_DATA_SIZE_M)

/**
 * struct ice_cgu_i2c_probe - state of the read-only CGU I2C probe
 * @pf: PF that owns the debugfs file
 * @lock: serializes reads of the CGU and access to the stored result
 * @valid: a read has been issued since the file was created
 * @bus_addr: 7-bit I2C address passed to the last read
 * @offset: register offset of the last read
 * @offset_size: register offset width of the last read (0, 1 or 2 bytes)
 * @len: number of bytes requested by the last read
 * @err: return value of the last read
 * @aq_err: admin queue status after the last read
 * @data: bytes returned by the last read
 */
struct ice_cgu_i2c_probe {
	struct ice_pf *pf;
	struct mutex lock; /* protects the fields below */
	bool valid;
	u16 bus_addr;
	u16 offset;
	u8 offset_size;
	u8 len;
	int err;
	enum libie_aq_err aq_err;
	u8 data[sizeof_field(struct ice_aqc_read_i2c_resp, i2c_data)];
};

/**
 * ice_cgu_i2c_node - read one clock controller node from the netlist
 * @hw: pointer to the HW struct
 * @idx: index of the node among the global clock controller nodes
 * @node: the Get Link Topology Handle response, on success
 *
 * Return: 0 on success, negative error code otherwise.
 */
static int ice_cgu_i2c_node(struct ice_hw *hw, u8 idx,
			    struct ice_aqc_get_link_topo *node)
{
	struct ice_aqc_get_link_topo *cmd;
	struct libie_aq_desc desc;
	int err;

	ice_fill_dflt_direct_cmd_desc(&desc, ice_aqc_opc_get_link_topo);
	cmd = libie_aq_raw(&desc);
	cmd->addr.topo_params.node_type_ctx =
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_TYPE_M,
			   ICE_AQC_LINK_TOPO_NODE_TYPE_CLK_CTRL) |
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_CTX_M,
			   ICE_AQC_LINK_TOPO_NODE_CTX_GLOBAL);
	cmd->addr.topo_params.index = idx;

	err = ice_aq_send_cmd(hw, &desc, NULL, 0, NULL);
	if (err)
		return err;

	*node = *cmd;
	return 0;
}

/**
 * ice_cgu_i2c_handle - find the netlist handle of the CGU
 * @hw: pointer to the HW struct
 * @handle: the node handle of the CGU, on success
 *
 * Return: 0 on success, -ENOENT if no node carries the CGU part number.
 */
static int ice_cgu_i2c_handle(struct ice_hw *hw, u16 *handle)
{
	struct ice_aqc_get_link_topo node;
	u8 idx;

	for (idx = 0; idx < ICE_CGU_I2C_MAX_NODES; idx++) {
		if (ice_cgu_i2c_node(hw, idx, &node))
			break;
		if (node.node_part_num == hw->cgu_part_number) {
			*handle = le16_to_cpu(node.addr.handle);
			return 0;
		}
	}

	return -ENOENT;
}

static void ice_cgu_i2c_show_read(struct seq_file *s,
				  struct ice_cgu_i2c_probe *probe)
{
	mutex_lock(&probe->lock);
	if (!probe->valid) {
		seq_puts(s, "read: none\n");
	} else {
		seq_printf(s, "read: bus 0x%02x offset 0x%04x size %u len %u: ",
			   probe->bus_addr, probe->offset, probe->offset_size,
			   probe->len);
		if (probe->err)
			seq_printf(s, "error %d aq %s\n", probe->err,
				   libie_aq_str(probe->aq_err));
		else
			seq_printf(s, "%*ph\n", probe->len, probe->data);
	}
	mutex_unlock(&probe->lock);
}

static int ice_cgu_i2c_show(struct seq_file *s, void *unused)
{
	struct ice_cgu_i2c_probe *probe = s->private;
	struct ice_hw *hw = &probe->pf->hw;
	struct ice_aqc_get_link_topo node;
	u8 idx;

	seq_printf(s, "cgu part number: 0x%02x timer owner: %d\n",
		   hw->cgu_part_number, ice_pf_src_tmr_owned(probe->pf));
	for (idx = 0; idx < ICE_CGU_I2C_MAX_NODES; idx++) {
		int err = ice_cgu_i2c_node(hw, idx, &node);

		if (err) {
			seq_printf(s, "clk_ctrl[%u]: error %d aq %s\n", idx,
				   err, libie_aq_str(hw->adminq.sq_last_status));
			break;
		}
		seq_printf(s, "clk_ctrl[%u]: handle 0x%04x part 0x%02x raw %*ph\n",
			   idx, le16_to_cpu(node.addr.handle),
			   node.node_part_num, (int)sizeof(node), &node);
	}
	ice_cgu_i2c_show_read(s, probe);
	return 0;
}

static int ice_cgu_i2c_open(struct inode *inode, struct file *file)
{
	return single_open(file, ice_cgu_i2c_show, inode->i_private);
}

/**
 * ice_cgu_i2c_parse - parse a CGU read request
 * @probe: the probe state to fill in
 * @buf: "<bus addr> <offset> <offset size> <len>", NUL terminated
 *
 * Return: 0 on success, -EINVAL if the request is malformed.
 */
static int ice_cgu_i2c_parse(struct ice_cgu_i2c_probe *probe, char *buf)
{
	int bus_addr, offset, offset_size, len, n = -1;

	buf = strim(buf);
	if (sscanf(buf, "%i %i %i %i%n", &bus_addr, &offset, &offset_size,
		   &len, &n) != 4 || n < 0 || buf[n])
		return -EINVAL;
	if (bus_addr < 0 || bus_addr > 0x7f || offset < 0 ||
	    offset > U16_MAX || offset_size < 0 || offset_size > 2 ||
	    len < 1 || len > ICE_CGU_I2C_MAX_LEN)
		return -EINVAL;
	if (offset_size < 2 && offset > U8_MAX)
		return -EINVAL;

	probe->bus_addr = bus_addr;
	probe->offset = offset;
	probe->offset_size = offset_size;
	probe->len = len;
	return 0;
}

/**
 * ice_cgu_i2c_read - read CGU registers through the Read I2C command
 * @probe: the probe state holding the request, receives the result
 * @handle: netlist handle of the CGU
 *
 * Issues a single Read I2C (0x06E2) with a repeated start. Nothing is
 * written to the CGU: the register offset travels in the read command.
 */
static void ice_cgu_i2c_read(struct ice_cgu_i2c_probe *probe, u16 handle)
{
	struct ice_aqc_link_topo_addr topo = {};
	struct ice_hw *hw = &probe->pf->hw;
	u8 params;

	topo.handle = cpu_to_le16(handle);
	topo.topo_params.node_type_ctx =
		FIELD_PREP(ICE_AQC_LINK_TOPO_NODE_CTX_M,
			   ICE_AQC_LINK_TOPO_NODE_CTX_PROVIDED);
	params = FIELD_PREP(ICE_AQC_I2C_DATA_SIZE_M, probe->len) |
		 FIELD_PREP(ICE_CGU_I2C_OFFSET_SIZE_M, probe->offset_size) |
		 ICE_AQC_I2C_USE_REPEATED_START;

	memset(probe->data, 0, sizeof(probe->data));
	probe->err = ice_aq_read_i2c(hw, topo, probe->bus_addr,
				     cpu_to_le16(probe->offset), params,
				     probe->data, NULL);
	probe->aq_err = hw->adminq.sq_last_status;
	probe->valid = true;
}

static ssize_t ice_cgu_i2c_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct ice_cgu_i2c_probe *probe = s->private;
	char buf[64];
	u16 handle;
	int err;

	if (!ice_is_feature_supported(probe->pf, ICE_F_CGU))
		return -EOPNOTSUPP;
	if (!count || count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	err = ice_cgu_i2c_handle(&probe->pf->hw, &handle);
	if (err)
		return err;

	mutex_lock(&probe->lock);
	err = ice_cgu_i2c_parse(probe, buf);
	if (!err)
		ice_cgu_i2c_read(probe, handle);
	mutex_unlock(&probe->lock);

	return err ? err : count;
}

static const struct file_operations ice_cgu_i2c_fops = {
	.owner = THIS_MODULE,
	.open = ice_cgu_i2c_open,
	.read = seq_read,
	.write = ice_cgu_i2c_write,
	.llseek = seq_lseek,
	.release = single_release,
};

/**
 * ice_debugfs_cgu_i2c_init - create the read-only CGU I2C probe file
 * @pf: pointer to the PF struct
 *
 * Reading cgu_i2c lists the clock controller nodes of the netlist and the
 * result of the last request; writing "<bus addr> <offset> <offset size>
 * <len>" reads up to 15 bytes from the CGU. Requests are refused unless the
 * PF owns the timer and the netlist has a CGU.
 */
static void ice_debugfs_cgu_i2c_init(struct ice_pf *pf)
{
	struct device *dev = ice_pf_to_dev(pf);
	struct ice_cgu_i2c_probe *probe;

	probe = devm_kzalloc(dev, sizeof(*probe), GFP_KERNEL);
	if (!probe || devm_mutex_init(dev, &probe->lock))
		return;

	probe->pf = pf;
	debugfs_create_file("cgu_i2c", 0600, pf->ice_debugfs_pf, probe,
			    &ice_cgu_i2c_fops);
}

int ice_debugfs_pf_init(struct ice_pf *pf)
{
	const char *name = pci_name(pf->pdev);

	pf->ice_debugfs_pf = debugfs_create_dir(name, ice_debugfs_root);
	if (IS_ERR(pf->ice_debugfs_pf))
		return PTR_ERR(pf->ice_debugfs_pf);

	ice_debugfs_cgu_i2c_init(pf);
	return 0;
}

/**
 * ice_debugfs_pf_deinit - cleanup PF's debugfs
 * @pf: pointer to the PF struct
 */
void ice_debugfs_pf_deinit(struct ice_pf *pf)
{
	debugfs_remove_recursive(pf->ice_debugfs_pf);
	pf->ice_debugfs_pf = NULL;
}

/**
 * ice_debugfs_init - create root directory for debugfs entries
 */
void ice_debugfs_init(void)
{
	ice_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (IS_ERR(ice_debugfs_root))
		pr_info("init of debugfs failed\n");
}

/**
 * ice_debugfs_exit - remove debugfs entries
 */
void ice_debugfs_exit(void)
{
	debugfs_remove_recursive(ice_debugfs_root);
	ice_debugfs_root = NULL;
}
