// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 PADL Software Pty Ltd.
 * Copyright (C) 2022 Raspberry Pi Ltd.
 *
 * Based on rpi-panel-v2-regulator.c by Dave Stevenson <dave.stevenson@raspberrypi.com>
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>

/*
 * Regulator driver for the XEBRA board
 */

/* I2C registers of the microcontroller. */
#define DEVICE_TYPE		0x00	/* device type identifier */
#define DEVICE_FIRMWARE		0x01	/* device firmware identifier */
#define ENABLE_INTERRUPT 	0x04	/* encoder interrupt enable */
#define LCD_BACKLIGHT		0x05	/* PWM level, deprecated */
#define GPIO_BITMASK		0x06	/* GPIO bitmask */
#define ENCODER_COUNT		0x07	/* number of configured encoders */
#define ENCODER_STATES		0x08	/* switch states and deltas */
#define ENCODER_SWITCHES	0x09	/* switch states */
#define ENCODER_POSITION	0x10	/* current encoder position */
#define SHUTDOWN_DEVICE		0xFE	/* set with device type to shutdown */
#define RESET_DEVICE		0xFF	/* set with device type to reset */

/* Device type register */
#define DT_XEBRA_TAIL		0xEB7A

/* GPIO register */
#define LCD_STBY_N_BIT		BIT(0)	/* LCD power */
#define LCD_RST_BIT		BIT(1)	/* LCD reset */
#define CTP_RESET_BIT		BIT(4)	/* Touchscreen reset */

#define NUM_GPIO		5

/* how long to wait for the microcontroller to cut power on shutdown/reset */
#define XEBRA_PANEL_SYS_OFF_TIMEOUT_MS	1000

struct xebra_panel_regulator {
	/* lock to serialise overall accesses to the Atmel */
	struct mutex		lock;
	struct i2c_client	*i2c;
	struct regmap		*regmap;

	u8			poweron_state;

	struct gpio_chip	gc;
};

static const struct regmap_config xebra_panel_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = GPIO_BITMASK,
};

static int xebra_panel_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

static int xebra_panel_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct xebra_panel_regulator *state = gpiochip_get_data(gc);
	u8 last_val;
	int ret;

	if (off >= NUM_GPIO)
		return -EINVAL;

	mutex_lock(&state->lock);

	last_val = state->poweron_state;
	if (val)
		last_val |= (1 << off);
	else
		last_val &= ~(1 << off);

	state->poweron_state = last_val;

	ret = regmap_write(state->regmap, GPIO_BITMASK, last_val);

	mutex_unlock(&state->lock);

	return ret;
}

static int xebra_panel_i2c_read(struct i2c_client *client, u8 reg, u16 *value)
{
	struct i2c_msg msgs[1];
	u8 addr_buf[1] = { reg };
	u8 data_buf[2] = { 0, 0 };
	int ret;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	usleep_range(5000, 10000);

	/* Read data from register */
	msgs[0].addr = client->addr;
	msgs[0].flags = I2C_M_RD;
	msgs[0].len = ARRAY_SIZE(data_buf);
	msgs[0].buf = data_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*value = (data_buf[0] << 8) | data_buf[1];

	return 0;
}

/*
 * Ask the microcontroller to power off or power-cycle the whole board
 * (XEBRA board rev3). It holds off for a short while after receiving the
 * command before cutting CM4 power.
 */
static int xebra_panel_reset_device(struct i2c_client *client, u8 reg)
{
	u8 addr_buf[3];
	struct i2c_msg msgs[1];
	int ret;

	addr_buf[0] = reg;
	addr_buf[1] = (DT_XEBRA_TAIL >> 8) & 0xff;
	addr_buf[2] = (DT_XEBRA_TAIL >> 0) & 0xff;

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

/*
 * Runs in the power-off/restart "prepare" phase: after device_shutdown()
 * has completed (so the panel has been unprepared and MMC caches flushed)
 * but before interrupts are disabled, so the I2C bus is still usable.
 *
 * A plain reboot notifier is too early: it fires before device_shutdown(),
 * and the microcontroller would cut our power part way through it.
 */
static int xebra_panel_sys_off(struct sys_off_data *data)
{
	struct xebra_panel_regulator *state = data->cb_data;
	bool power_off = data->mode == SYS_OFF_MODE_POWER_OFF_PREPARE;
	int ret;

	ret = xebra_panel_reset_device(state->i2c,
				       power_off ? SHUTDOWN_DEVICE : RESET_DEVICE);
	if (ret) {
		dev_err(&state->i2c->dev, "Failed to request %s: %d\n",
			power_off ? "shutdown" : "reset", ret);
		return NOTIFY_DONE;
	}

	/*
	 * The microcontroller cuts CM4 power shortly after the request; on a
	 * reset it also powers us back up, so there is no point letting the
	 * SoC reset itself first. Wait here for that to happen; if it does
	 * not, fall through to the kernel's own power-off/restart path.
	 */
	msleep(XEBRA_PANEL_SYS_OFF_TIMEOUT_MS);
	dev_warn(&state->i2c->dev,
		 "Microcontroller did not cut power within %u ms\n",
		 XEBRA_PANEL_SYS_OFF_TIMEOUT_MS);

	return NOTIFY_DONE;
}

/*
 * I2C driver interface functions
 */
static int xebra_panel_i2c_probe(struct i2c_client *i2c)
{
	struct xebra_panel_regulator *state;
	struct regmap *regmap;
	u16 device_type;
	int ret;

	state = devm_kzalloc(&i2c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);
	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &xebra_panel_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}

	ret = xebra_panel_i2c_read(i2c, DEVICE_TYPE, &device_type);
	if (ret < 0) {
		dev_err(&i2c->dev, "Failed to read DEVICE_TYPE reg: %d\n", ret);
		goto error;
	}

	if (device_type != DT_XEBRA_TAIL) {
		dev_err(&i2c->dev, "Unknown device type: 0x%04x\n", device_type);
		ret = -ENODEV;
		goto error;
	}

	regmap_write(regmap, GPIO_BITMASK, 0);

	state->regmap = regmap;
	state->i2c = i2c;

	state->gc.parent = &i2c->dev;
	state->gc.label = i2c->name;
	state->gc.owner = THIS_MODULE;
	state->gc.base = -1;
	state->gc.ngpio = NUM_GPIO;

	state->gc.set = xebra_panel_gpio_set;
	state->gc.get_direction = xebra_panel_gpio_get_direction;
	state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &state->gc, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}

	ret = devm_register_sys_off_handler(&i2c->dev,
					    SYS_OFF_MODE_POWER_OFF_PREPARE,
					    SYS_OFF_PRIO_DEFAULT,
					    xebra_panel_sys_off, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to register power-off handler: %d\n",
			ret);
		goto error;
	}

	ret = devm_register_sys_off_handler(&i2c->dev,
					    SYS_OFF_MODE_RESTART_PREPARE,
					    SYS_OFF_PRIO_DEFAULT,
					    xebra_panel_sys_off, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to register restart handler: %d\n",
			ret);
		goto error;
	}

	dev_info(&i2c->dev, "Initialized XEBRA display panel regulator\n");

	return 0;

error:
	mutex_destroy(&state->lock);
	return ret;
}

static void xebra_panel_i2c_remove(struct i2c_client *client)
{
	struct xebra_panel_regulator *state = i2c_get_clientdata(client);

	mutex_destroy(&state->lock);
}

static void xebra_panel_i2c_shutdown(struct i2c_client *client)
{
	struct xebra_panel_regulator *state = i2c_get_clientdata(client);

	regmap_write(state->regmap, GPIO_BITMASK, 0);
}

static const struct of_device_id xebra_panel_dt_ids[] = {
	{ .compatible = "padl,xebra-touchscreen-panel-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, xebra_panel_dt_ids);

static struct i2c_driver xebra_panel_regulator_driver = {
	.driver = {
		.name = "xebra_touchscreen",
		.of_match_table = of_match_ptr(xebra_panel_dt_ids),
	},
	.probe = xebra_panel_i2c_probe,
	.remove	= xebra_panel_i2c_remove,
	.shutdown = xebra_panel_i2c_shutdown,
};

module_i2c_driver(xebra_panel_regulator_driver);

MODULE_AUTHOR("Luke Howard <lukeh@padl.com>");
MODULE_DESCRIPTION("Regulator device driver for XEBRA touchscreen unit");
MODULE_LICENSE("GPL");
