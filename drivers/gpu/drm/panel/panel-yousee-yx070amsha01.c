// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025, PADL Software Pty Ltd
 */

#include <linux/backlight.h>
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

#include <video/mipi_display.h>

#define YX070AMSHA01_DEFAULT_BRIGHTNESS 2047
#define YX070AMSHA01_MAX_BRIGHTNESS 4095

struct yx070amsha01_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct backlight_device *bl;
	struct regulator *supply;
	struct gpio_desc *reset_gpio;
};

static int yx070amsha01_get_brightness(struct backlight_device *bl)
{
	struct yx070amsha01_panel *ctx = bl_get_data(bl);
	u16 brightness;
	int ret;

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(ctx->dsi, &brightness);
	if (ret < 0)
		return ret;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static int yx070amsha01_update_status(struct backlight_device *bl)
{
	struct yx070amsha01_panel *ctx = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(ctx->dsi, brightness);
	if (ret < 0)
		return ret;

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret;
}

static const struct backlight_ops yx070amsha01_bl_ops = {
	.get_brightness = yx070amsha01_get_brightness,
	.update_status = yx070amsha01_update_status,
};

static inline struct yx070amsha01_panel *
panel_to_yx070amsha01_panel(struct drm_panel *panel)
{
	return container_of(panel, struct yx070amsha01_panel, panel);
}

static inline void yx070amsha01_select_group(struct mipi_dsi_multi_context *dsi,
					     u8 group)
{
	u8 data[] = { 0x9F, group };

	mipi_dsi_generic_write_multi(dsi, data, sizeof(data));
}

static int yx070amsha01_init_sequence(struct drm_panel *panel)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* ICNA3512 specific commands */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x9C, 0xA5, 0xA5);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xFD, 0x5A, 0x5A);

	/* DCS: Set display brightness control, backlight, and TE */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x03);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x53, 0xE0);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	/* DCS: Exit sleep mode and wait for panel to stabilize */
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	/* Configure default brightness */
	mipi_dsi_generic_write_seq_multi(
		&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(
		&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
		YX070AMSHA01_DEFAULT_BRIGHTNESS >> 8,
		YX070AMSHA01_DEFAULT_BRIGHTNESS & 0xFF);

	/* Group 0x0F: TCON configuration */
	yx070amsha01_select_group(&dsi_ctx, 0x0F);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xCE, 0x22);

	/* Group 0x01: Display timing parameters */
	yx070amsha01_select_group(&dsi_ctx, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB3, 0x00, 0xE0, 0xA0, 0x10,
					 0xC8, 0x00);

	/* Group 0x07: Source timing control */
	yx070amsha01_select_group(&dsi_ctx, 0x07);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xB2, 0x04, 0x18, 0x08, 0x0C,
					 0x02, 0x00, 0xC4);

	/* 0xD3: Gamma correction parameters */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xD3, 0x88, 0x4A, 0x4A, 0x88,
					 0x4A, 0x4A, 0x00, 0xEB, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00);

	/* 0xCB: Power control settings */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xCB, 0x01, 0x01, 0x01, 0x01,
					 0x04, 0x09, 0x2C);

	/* 0x48: DSI mode */
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x33);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x48, 0x03);

	/* Group 0x01: Additional control register */
	yx070amsha01_select_group(&dsi_ctx, 0x01);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xC5, 0x01);

	/* yx070amsha01_panel_enable() will turn display on */

	return dsi_ctx.accum_err;
}

static int yx070amsha01_reset(struct yx070amsha01_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	mipi_dsi_msleep(&dsi_ctx, 10); /* T1 */

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	mipi_dsi_msleep(&dsi_ctx, 5);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	mipi_dsi_msleep(&dsi_ctx, 1); /* T3 */

	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	mipi_dsi_msleep(&dsi_ctx, 15); /* SLPIN */

	return dsi_ctx.accum_err;
}

static int yx070amsha01_panel_prepare(struct drm_panel *panel)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to enable supply: %d\n", ret);
		return ret;
	}

	ret = yx070amsha01_reset(ctx);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to toggle reset GPIO: %d\n",
			ret);
		return ret;
	}

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	ret = yx070amsha01_init_sequence(panel);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "Failed to write registers: %d\n", ret);
		return ret;
	}

	return 0;
}

static int yx070amsha01_panel_unprepare(struct drm_panel *panel)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~(MIPI_DSI_MODE_LPM);

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);

	return 0;
}

static int yx070amsha01_panel_enable(struct drm_panel *panel)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 80);

	return dsi_ctx.accum_err;
}

static int yx070amsha01_panel_disable(struct drm_panel *panel)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 80);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode default_mode = {
	.clock = 150000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 165, // HFP 4-200
	.hsync_end = 1080 + 165 + 8, // Hsync 1-30
	.htotal = 1080 + 165 + 8 + 23, // HBP 4-200
	.vdisplay = 1920,
	.vsync_start = 1920 + 20, // VFP 4-150
	.vsync_end = 1920 + 20 + 4, // Vsync 1-30
	.vtotal = 1920 + 20 + 4 + 15, // VBP 4-150
	.width_mm = 87,
	.height_mm = 155,
};

static int yx070amsha01_panel_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	struct yx070amsha01_panel *ctx = panel_to_yx070amsha01_panel(panel);
	static const u32 bus_format = MEDIA_BUS_FMT_RGB888_1X24;
	struct drm_display_mode *mode;

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
	drm_display_info_set_bus_formats(&connector->display_info, &bus_format,
					 1);

	return 1;
}

static const struct drm_panel_funcs yx070amsha01_panel_funcs = {
	.get_modes = yx070amsha01_panel_get_modes,
	.prepare = yx070amsha01_panel_prepare,
	.enable = yx070amsha01_panel_enable,
	.disable = yx070amsha01_panel_disable,
	.unprepare = yx070amsha01_panel_unprepare,
};

static int yx070amsha01_panel_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct yx070amsha01_panel *ctx;
	int ret;

	ctx = devm_kzalloc(&dsi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dsi = dsi;

	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 4;
	dsi->hs_rate = 1000000000;
	dsi->lp_rate = 10000000;

	ctx->supply = devm_regulator_get(&dsi->dev, "vcc-lcd");
	if (IS_ERR(ctx->supply))
		return PTR_ERR(ctx->supply);

	/* Global reset pin */
	ctx->reset_gpio = devm_gpiod_get(&dsi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(&dsi->dev, "Couldn't get our reset GPIO\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	ctx->bl = backlight_device_register("yx070amsha01", &dsi->dev, ctx,
					    &yx070amsha01_bl_ops, NULL);
	if (IS_ERR(ctx->bl)) {
		dev_err(&dsi->dev, "Failed to register backlight device\n");
		return PTR_ERR(ctx->bl);
	}

	ctx->bl->props.max_brightness = YX070AMSHA01_MAX_BRIGHTNESS;
	ctx->bl->props.brightness = YX070AMSHA01_DEFAULT_BRIGHTNESS;
	ctx->bl->props.type = BACKLIGHT_RAW;

	ctx->panel.prepare_prev_first = true;
	drm_panel_init(&ctx->panel, &dsi->dev, &yx070amsha01_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		goto remove_panel;

	return 0;

remove_panel:
	drm_panel_remove(&ctx->panel);
	backlight_device_unregister(ctx->bl);

	return ret;
}

static void yx070amsha01_panel_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct yx070amsha01_panel *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
	backlight_device_unregister(ctx->bl);
}

static const struct of_device_id yx070amsha01_panel_of_match[] = {
	{ .compatible = "yousee,yx070amsha01" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, yx070amsha01_panel_of_match);

static struct mipi_dsi_driver yx070amsha01_panel_driver = {
	.probe = yx070amsha01_panel_dsi_probe,
	.remove = yx070amsha01_panel_dsi_remove,
	.driver = {
		.name = "panel-yousee-yx070amsha01",
		.of_match_table	= yx070amsha01_panel_of_match,
	},
};
module_mipi_dsi_driver(yx070amsha01_panel_driver);

MODULE_AUTHOR("Luke Howard <lukeh@padl.com>");
MODULE_DESCRIPTION("Yousee YX070AMSHA01 Panel Driver");
MODULE_LICENSE("GPL");
