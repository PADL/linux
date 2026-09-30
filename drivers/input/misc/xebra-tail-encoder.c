// SPDX-License-Identifier: GPL-2.0
/*
 * Rotary encoder driver for the XEBRA tail microcontroller
 *
 * Copyright (C) 2026 PADL Software Pty Ltd.
 *
 * The tail is an RP2040 that answers on two I2C addresses: 0x45 for the
 * display panel (see xebra-panel-regulator.c) and 0x44 for the rotary
 * encoders, which this driver handles. Each encoder reports a signed 8-bit
 * delta since it was last read, and has a push switch.
 *
 * The tail can pull an interrupt line low while it has unread events,
 * releasing it when ENCODER_STATES is read. Where that line is wired up, it
 * is used as a level-triggered interrupt; otherwise the tail is polled.
 * Either way, reads continue at the poll interval for a while after the
 * last activity, as the tail can take an event after snapshotting the
 * deltas but before releasing the line, which would otherwise leave that
 * event unread until the next one.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/xebra-tail.h>

/* the switch bitmask is 8 bits wide */
#define XT_MAX_ENCODERS		8

#define XT_POLL_INTERVAL_MS	33	/* while active */
#define XT_IDLE_INTERVAL_MS	100	/* while idle, without an interrupt */
#define XT_ACTIVITY_HOLD_MS	1000	/* how long "active" lasts */

struct xebra_tail_encoder {
	bool present;
	unsigned int axis;
	unsigned int code;	/* KEY_RESERVED if the switch is unused */
};

struct xebra_tail {
	struct i2c_client *client;
	struct input_dev *input;

	/* serialises reads, and protects the fields below */
	struct mutex lock;
	struct delayed_work work;
	bool running;
	u8 switches;
	unsigned long last_activity;

	unsigned int poll_interval;
	unsigned int idle_interval;
	unsigned int activity_hold;

	unsigned int nencoders;
	struct xebra_tail_encoder encoders[XT_MAX_ENCODERS];
};

static int xebra_tail_read_states(struct xebra_tail *xt, u8 *buf)
{
	int len = 1 + xt->nencoders;
	int ret;

	ret = i2c_smbus_read_i2c_block_data(xt->client, XEBRA_TAIL_ENCODER_STATES,
					    len, buf);
	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;

	return 0;
}

/*
 * Reads the tail and reports any changes. Returns 1 if there were some, 0 if
 * not, or a negative error code.
 */
static int xebra_tail_process(struct xebra_tail *xt)
{
	u8 buf[1 + XT_MAX_ENCODERS];
	bool active = false;
	unsigned int i;
	int ret;

	lockdep_assert_held(&xt->lock);

	ret = xebra_tail_read_states(xt, buf);
	if (ret) {
		dev_err_ratelimited(&xt->client->dev,
				    "Failed to read encoder states: %d\n", ret);
		return ret;
	}

	for (i = 0; i < xt->nencoders; i++) {
		const struct xebra_tail_encoder *enc = &xt->encoders[i];
		s8 delta = (s8)buf[1 + i];

		if (!enc->present)
			continue;

		if ((buf[0] ^ xt->switches) & BIT(i)) {
			if (enc->code != KEY_RESERVED)
				input_report_key(xt->input, enc->code,
						 buf[0] & BIT(i));
			active = true;
		}

		if (delta) {
			input_report_rel(xt->input, enc->axis, delta);
			active = true;
		}
	}

	xt->switches = buf[0];

	if (active)
		input_sync(xt->input);

	return active;
}

static void xebra_tail_work(struct work_struct *work)
{
	struct xebra_tail *xt = container_of(to_delayed_work(work),
					     struct xebra_tail, work);
	unsigned long interval;

	guard(mutex)(&xt->lock);

	if (!xt->running)
		return;

	if (xebra_tail_process(xt) > 0)
		xt->last_activity = jiffies;

	if (time_before(jiffies, xt->last_activity + xt->activity_hold)) {
		interval = xt->poll_interval;
	} else {
		/* idle: with an interrupt, wait for the next one */
		if (xt->client->irq > 0)
			return;
		interval = xt->idle_interval;
	}

	queue_delayed_work(system_freezable_wq, &xt->work, interval);
}

static irqreturn_t xebra_tail_irq(int irq, void *dev_id)
{
	struct xebra_tail *xt = dev_id;
	int ret;

	scoped_guard(mutex, &xt->lock) {
		if (!xt->running)
			return IRQ_HANDLED;

		ret = xebra_tail_process(xt);
		if (ret > 0) {
			xt->last_activity = jiffies;
			mod_delayed_work(system_freezable_wq, &xt->work,
					 xt->poll_interval);
		}
	}

	/*
	 * Only a successful read releases the line, so after a failed one
	 * the interrupt fires again as soon as it is unmasked; wait a poll
	 * interval before retrying rather than spin on a broken bus.
	 */
	if (ret < 0)
		msleep(jiffies_to_msecs(xt->poll_interval));

	return IRQ_HANDLED;
}

static void xebra_tail_start(struct xebra_tail *xt)
{
	u8 buf[1 + XT_MAX_ENCODERS];
	unsigned int i;

	scoped_guard(mutex, &xt->lock) {
		/*
		 * Deltas accumulate on the tail while nobody is listening, so
		 * drop them rather than deliver a stale jump; take the switch
		 * states as they are now.
		 */
		if (!xebra_tail_read_states(xt, buf)) {
			xt->switches = buf[0];
			for (i = 0; i < xt->nencoders; i++) {
				const struct xebra_tail_encoder *enc =
					&xt->encoders[i];

				if (enc->present && enc->code != KEY_RESERVED)
					input_report_key(xt->input, enc->code,
							 buf[0] & BIT(i));
			}
			input_sync(xt->input);
		}

		xt->running = true;
		xt->last_activity = jiffies;
	}

	if (xt->client->irq > 0)
		enable_irq(xt->client->irq);
	else
		queue_delayed_work(system_freezable_wq, &xt->work,
				   xt->poll_interval);
}

static void xebra_tail_stop(struct xebra_tail *xt)
{
	scoped_guard(mutex, &xt->lock) {
		if (!xt->running)
			return;
		xt->running = false;
	}

	if (xt->client->irq > 0)
		disable_irq(xt->client->irq);
	cancel_delayed_work_sync(&xt->work);
}

static int xebra_tail_open(struct input_dev *input)
{
	xebra_tail_start(input_get_drvdata(input));

	return 0;
}

static void xebra_tail_close(struct input_dev *input)
{
	xebra_tail_stop(input_get_drvdata(input));
}

static int xebra_tail_set_interrupt(struct xebra_tail *xt, bool enable)
{
	return i2c_smbus_write_byte_data(xt->client, XEBRA_TAIL_ENABLE_INTERRUPT,
					 enable ? 1 : 0);
}

static void xebra_tail_disable_interrupt(void *data)
{
	xebra_tail_set_interrupt(data, false);
}

static int xebra_tail_parse_encoders(struct xebra_tail *xt)
{
	struct device *dev = &xt->client->dev;
	unsigned int count = 0;
	int ret;

	device_for_each_child_node_scoped(dev, child) {
		struct xebra_tail_encoder *enc;
		u32 reg, axis, code = KEY_RESERVED;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%pfwP: missing reg\n", child);

		if (reg >= xt->nencoders) {
			dev_warn(dev, "%pfwP: tail has only %u encoders\n",
				 child, xt->nencoders);
			continue;
		}

		ret = fwnode_property_read_u32(child, "linux,axis", &axis);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%pfwP: missing linux,axis\n",
					     child);
		if (axis > REL_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "%pfwP: invalid linux,axis %u\n",
					     child, axis);

		fwnode_property_read_u32(child, "linux,code", &code);
		if (code > KEY_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "%pfwP: invalid linux,code %u\n",
					     child, code);

		enc = &xt->encoders[reg];
		if (enc->present)
			return dev_err_probe(dev, -EINVAL,
					     "%pfwP: duplicate encoder %u\n",
					     child, reg);

		enc->present = true;
		enc->axis = axis;
		enc->code = code;

		input_set_capability(xt->input, EV_REL, axis);
		if (code != KEY_RESERVED)
			input_set_capability(xt->input, EV_KEY, code);

		count++;
	}

	if (!count)
		return dev_err_probe(dev, -ENODEV, "no encoders described\n");

	return 0;
}

static int xebra_tail_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct xebra_tail *xt;
	u32 poll_interval = XT_POLL_INTERVAL_MS;
	int device_type, firmware, count;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA |
				     I2C_FUNC_SMBUS_WORD_DATA |
				     I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return -ENODEV;

	xt = devm_kzalloc(dev, sizeof(*xt), GFP_KERNEL);
	if (!xt)
		return -ENOMEM;

	xt->client = client;
	i2c_set_clientdata(client, xt);

	ret = devm_mutex_init(dev, &xt->lock);
	if (ret)
		return ret;

	INIT_DELAYED_WORK(&xt->work, xebra_tail_work);

	device_type = i2c_smbus_read_word_swapped(client, XEBRA_TAIL_DEVICE_TYPE);
	if (device_type < 0)
		return dev_err_probe(dev, device_type,
				     "Failed to read device type\n");

	if (device_type != XEBRA_TAIL_DEVICE_TYPE_ID)
		return dev_err_probe(dev, -ENODEV,
				     "Unknown device type 0x%04x\n",
				     device_type);

	firmware = i2c_smbus_read_word_swapped(client, XEBRA_TAIL_DEVICE_FIRMWARE);
	if (firmware < 0)
		return dev_err_probe(dev, firmware,
				     "Failed to read firmware version\n");

	count = i2c_smbus_read_byte_data(client, XEBRA_TAIL_ENCODER_COUNT);
	if (count < 0)
		return dev_err_probe(dev, count,
				     "Failed to read encoder count\n");
	if (count > XT_MAX_ENCODERS)
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported encoder count %d\n", count);
	xt->nencoders = count;

	device_property_read_u32(dev, "poll-interval", &poll_interval);
	xt->poll_interval = msecs_to_jiffies(poll_interval) ?: 1;
	xt->idle_interval = max(msecs_to_jiffies(XT_IDLE_INTERVAL_MS),
				xt->poll_interval);
	xt->activity_hold = msecs_to_jiffies(XT_ACTIVITY_HOLD_MS);

	xt->input = devm_input_allocate_device(dev);
	if (!xt->input)
		return -ENOMEM;

	xt->input->name = "XEBRA Tail Encoders";
	xt->input->id.bustype = BUS_I2C;
	xt->input->id.product = device_type;
	xt->input->id.version = firmware;
	xt->input->open = xebra_tail_open;
	xt->input->close = xebra_tail_close;
	input_set_drvdata(xt->input, xt);

	ret = xebra_tail_parse_encoders(xt);
	if (ret)
		return ret;

	/*
	 * Tell the tail whether to drive the interrupt line; without one it
	 * would hold the line low after the first event and never release it
	 * until read, which is harmless but pointless.
	 */
	ret = xebra_tail_set_interrupt(xt, client->irq > 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to set interrupt mode\n");

	if (client->irq > 0) {
		ret = devm_add_action_or_reset(dev,
					       xebra_tail_disable_interrupt,
					       xt);
		if (ret)
			return ret;

		/* enabled on open */
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						xebra_tail_irq,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						client->name, xt);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to request interrupt\n");
	}

	ret = input_register_device(xt->input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register input device\n");

	dev_info(dev, "XEBRA tail 0x%04x firmware %u, %u encoders, %s\n",
		 device_type, firmware, xt->nencoders,
		 client->irq > 0 ? "interrupt driven" : "polled");

	return 0;
}

/*
 * xebra-panel-regulator asks the tail to cut power from the power-off and
 * restart prepare handlers, which run after device_shutdown() and assume
 * nothing else is using the bus by then.
 */
static void xebra_tail_shutdown(struct i2c_client *client)
{
	xebra_tail_stop(i2c_get_clientdata(client));
}

static const struct of_device_id xebra_tail_of_match[] = {
	{ .compatible = "padl,xebra-tail-encoder" },
	{ }
};
MODULE_DEVICE_TABLE(of, xebra_tail_of_match);

static const struct i2c_device_id xebra_tail_id[] = {
	{ "xebra-tail-encoder" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, xebra_tail_id);

static struct i2c_driver xebra_tail_driver = {
	.driver = {
		.name = "xebra-tail-encoder",
		.of_match_table = xebra_tail_of_match,
	},
	.probe = xebra_tail_probe,
	.shutdown = xebra_tail_shutdown,
	.id_table = xebra_tail_id,
};
module_i2c_driver(xebra_tail_driver);

MODULE_AUTHOR("Luke Howard <lukeh@padl.com>");
MODULE_DESCRIPTION("XEBRA tail rotary encoder driver");
MODULE_LICENSE("GPL");
