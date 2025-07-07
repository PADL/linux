// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2018-2019, Bridge Systems BV
 * Copyright (C) 2018-2019, Bootlin
 * Copyright (C) 2017, Free Electrons
 * Copyright (C) 2025, PADL Software Pty Ltd
 *
 * This file based on panel-ronbo-rb070d30
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define B70HM30_GAMMA_1		0x80
#define B70HM30_GAMMA_2		0x81
#define B70HM30_GAMMA_3		0x82
#define B70HM30_GAMMA_4		0x83
#define B70HM30_GAMMA_5		0x84
#define B70HM30_GAMMA_6		0x85
#define B70HM30_GAMMA_7		0x86

#define B70HM30_PWR_EN		0xB0
#define B70HM30_CONTROL_1	0xB1
#define B70HM30_CONTROL_2	0xB2

struct b70hm30_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *supply;

	struct {
		struct gpio_desc *standby;
		struct gpio_desc *reset;
		struct gpio_desc *updn;
		struct gpio_desc *shlr;
	} gpios;
};

static inline struct b70hm30_panel *panel_to_b70hm30_panel(struct drm_panel *panel)
{
	return container_of(panel, struct b70hm30_panel, panel);
}

static int b70hm30_init_sequence(struct b70hm30_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_msleep(&dsi_ctx, 55); /* 10.1.1 of datasheet */

	/* Configure gamma voltages */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_1, 0xAC);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_2, 0xB8);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_3, 0x09);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_4, 0x78);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_5, 0x7F);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_6, 0xBB);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_GAMMA_7, 0x70);

	/* Enable internal power management (PWM, charge pump, VCOM buffer) */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_PWR_EN, 0x80);

	/* Set resolution to 1024x600 (RES[1:0]=00) and other control bits */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, B70HM30_CONTROL_1, 0x00);

	mipi_dsi_msleep(&dsi_ctx, 30);

	return dsi_ctx.accum_err;
}

static int b70hm30_panel_prepare(struct drm_panel *panel)
{
	struct b70hm30_panel *ctx = panel_to_b70hm30_panel(panel);
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to enable supply: %d\n", ret);
		return ret;
	}

	/* GPIO_ACTIVE_LOW: value 1 = pin driven low (active), value 0 = pin driven high (inactive) */
	gpiod_set_value_cansleep(ctx->gpios.standby, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->gpios.reset, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->gpios.reset, 1);
	msleep(30);
	gpiod_set_value_cansleep(ctx->gpios.reset, 0);

	ret = b70hm30_init_sequence(ctx);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to write registers: %d\n", ret);
		return ret;
	}

	return 0;
}

static int b70hm30_panel_unprepare(struct drm_panel *panel)
{
	struct b70hm30_panel *ctx = panel_to_b70hm30_panel(panel);

	gpiod_set_value_cansleep(ctx->gpios.reset, 1);
	gpiod_set_value_cansleep(ctx->gpios.standby, 1);

	regulator_disable(ctx->supply);

	return 0;
}

static int b70hm30_panel_enable(struct drm_panel *panel)
{
	struct b70hm30_panel *ctx = panel_to_b70hm30_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0)
		return ret;

	return 0;
}

static int b70hm30_panel_disable(struct drm_panel *panel)
{
	struct b70hm30_panel *ctx = panel_to_b70hm30_panel(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		return ret;

	msleep(20);

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		return ret;

	return 0;
}

/* Default timings */
static const struct drm_display_mode default_mode = {
	.clock		= 51200,
	.hdisplay	= 1024,
	.hsync_start	= 1024 + 160,
	.hsync_end	= 1024 + 160 + 10,
	.htotal		= 1024 + 160 + 10 + 160,
	.vdisplay	= 600,
	.vsync_start	= 600 + 12,
	.vsync_end	= 600 + 12 + 2,
	.vtotal		= 600 + 12 + 2 + 22,

	/* Active area is 154.2144×85.92 */
	.width_mm	= 154,
	.height_mm	= 86,
};

static int b70hm30_panel_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	struct b70hm30_panel *ctx = panel_to_b70hm30_panel(panel);
	struct drm_display_mode *mode;
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(&ctx->dsi->dev, "Failed to add mode " DRM_MODE_FMT "\n",
			DRM_MODE_ARG(&default_mode));
		return -EINVAL;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_display_info_set_bus_formats(&connector->display_info,
					 &bus_format, 1);

	return 1;
}

static const struct drm_panel_funcs b70hm30_panel_funcs = {
	.get_modes	= b70hm30_panel_get_modes,
	.prepare	= b70hm30_panel_prepare,
	.enable		= b70hm30_panel_enable,
	.disable	= b70hm30_panel_disable,
	.unprepare	= b70hm30_panel_unprepare,
};

static int b70hm30_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct b70hm30_panel *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supply = devm_regulator_get(&dsi->dev, "vcc-lcd");
	if (IS_ERR(ctx->supply))
		return PTR_ERR(ctx->supply);

	mipi_dsi_set_drvdata(dsi, ctx);
	ctx->dsi = dsi;
	ctx->panel.prepare_prev_first = true;

	drm_panel_init(&ctx->panel, &dsi->dev, &b70hm30_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	/* Global reset pin */
	ctx->gpios.reset = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.reset)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->gpios.reset);
	}

	/* Standby mode */
	ctx->gpios.standby = devm_gpiod_get(&dsi->dev, "standby", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.standby)) {
		dev_err(&dsi->dev, "Couldn't get our standby GPIO\n");
		return PTR_ERR(ctx->gpios.standby);
	}

	/*
	 * We don't change the state of that GPIO later on but we need
	 * to force it into a low state.
	 */
	ctx->gpios.updn = devm_gpiod_get(&dsi->dev, "updn", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.updn)) {
		dev_err(&dsi->dev, "Couldn't get our updn GPIO\n");
		return PTR_ERR(ctx->gpios.updn);
	}

	/*
	 * We don't change the state of that GPIO later on but we need
	 * to force it into a low state.
	 */
	ctx->gpios.shlr = devm_gpiod_get(&dsi->dev, "shlr", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->gpios.shlr)) {
		dev_err(&dsi->dev, "Couldn't get our shlr GPIO\n");
		return PTR_ERR(ctx->gpios.shlr);
	}

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST | MIPI_DSI_MODE_LPM;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;
	dsi->hs_rate = 170000000; /* 170MHz HS clock (340 Mbps per lane max), supplied by vendor */
	dsi->lp_rate = 10000000; /* 10MHz LP clock */

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void b70hm30_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct b70hm30_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id b70hm30_panel_of_match[] = {
	{ .compatible = "maxen,b70hm30" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, b70hm30_panel_of_match);

static struct mipi_dsi_driver b70hm30_panel_driver = {
	.probe = b70hm30_panel_dsi_probe,
	.remove = b70hm30_panel_dsi_remove,
	.driver = {
		.name = "panel-maxen-b70hm30",
		.of_match_table	= b70hm30_panel_of_match,
	},
};
module_mipi_dsi_driver(b70hm30_panel_driver);

MODULE_AUTHOR("Boris Brezillon <boris.brezillon@bootlin.com>");
MODULE_AUTHOR("Konstantin Sudakov <k.sudakov@integrasources.com>");
MODULE_AUTHOR("Luke Howard <lukeh@padl.com>");
MODULE_DESCRIPTION("Maxen B70HM30 Panel Driver");
MODULE_LICENSE("GPL");
