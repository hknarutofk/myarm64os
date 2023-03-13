/* Hisilicon Hibmc SoC drm driver
 *
 * Based on the bochs drm driver.
 *
 * Copyright (c) 2016 Huawei Limited.
 *
 * Author:
 *	Rongrong Zou <zourongrong@huawei.com>
 *	Rongrong Zou <zourongrong@gmail.com>
 *	Jianhua Li <lijianhua@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>

#include "hibmc_drm_drv.h"
#include "hibmc_drm_regs.h"

struct hibmc_resolution {
	int w;
	int h;
};

static const struct hibmc_resolution hibmc_modetables[] = {
	{640, 480},
	{800, 600},
	{1024, 768},
	{1152, 864},
	{1280, 768},
	{1280, 720},
	{1280, 960},
	{1280, 1024},
	{1440, 900},
	{1600, 900},
	{1600, 1200},
	{1920, 1080},
	{1920, 1200}
};

static int hibmc_valid_mode(int w, int h)
{
	int i;

	for (i = 0; i < sizeof(hibmc_modetables) /
		sizeof(struct hibmc_resolution); i++) {
		if (hibmc_modetables[i].w == w && hibmc_modetables[i].h == h)
			return 0;
	}

	return -1;
}

static int hibmc_connector_get_modes(struct drm_connector *connector)
{
	int count;
	void *edid;
	struct hibmc_connector *hibmc_connector = to_hibmc_connector(connector);

	edid = drm_get_edid(connector, &hibmc_connector->adapter);
	if (edid) {
		drm_connector_update_edid_property(connector, edid);
		count = drm_add_edid_modes(connector, edid);
		if (count)
			goto out;
	}

	drm_connector_update_edid_property(connector, NULL);
	count = drm_add_modes_noedid(connector, 1920, 1200);
	drm_set_preferred_mode(connector, 1024, 768);

out:
	kfree(edid);
	return count;
}

static enum drm_mode_status hibmc_connector_mode_valid(struct drm_connector *connector,
				      struct drm_display_mode *mode)
{
	int vrefresh = drm_mode_vrefresh(mode);

	if (vrefresh < 59 || vrefresh > 61)
		return MODE_NOMODE;
	else if (hibmc_valid_mode(mode->hdisplay, mode->vdisplay) != 0)
		return MODE_NOMODE;
	else
		return MODE_OK;
}

static struct drm_encoder *
hibmc_connector_best_encoder(struct drm_connector *connector)
{
	return drm_encoder_find(connector->dev, NULL, connector->encoder_ids[0]);
}

static const struct drm_connector_helper_funcs
	hibmc_connector_helper_funcs = {
	.get_modes = hibmc_connector_get_modes,
	.mode_valid = hibmc_connector_mode_valid,
	.best_encoder = hibmc_connector_best_encoder,
};

static void hibmc_connector_destroy(struct drm_connector *connector)
{
	struct hibmc_connector *hibmc_connector = to_hibmc_connector(connector);

	i2c_del_adapter(&hibmc_connector->adapter);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs hibmc_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = hibmc_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int
hibmc_connector_init(struct hibmc_drm_private *priv)
{
	struct drm_device *dev = priv->dev;
	struct drm_connector *connector = &priv->connector.base;
	int ret;

	ret = drm_connector_init(dev, connector,
				 &hibmc_connector_funcs,
				 DRM_MODE_CONNECTOR_VGA);
	if (ret) {
		DRM_ERROR("failed to init connector: %d\n", ret);
		return ret;
	}
	drm_connector_helper_add(connector,
				 &hibmc_connector_helper_funcs);
	drm_connector_register(connector);

	return 0;
}

static void hibmc_encoder_mode_set(struct drm_encoder *encoder,
				   struct drm_display_mode *mode,
				   struct drm_display_mode *adj_mode)
{
	u32 reg;
	struct drm_device *dev = encoder->dev;
	struct hibmc_drm_private *priv = dev->dev_private;

	reg = readl(priv->mmio + HIBMC_DISPLAY_CONTROL_HISILE);
	reg |= HIBMC_DISPLAY_CONTROL_FPVDDEN(1);
	reg |= HIBMC_DISPLAY_CONTROL_PANELDATE(1);
	reg |= HIBMC_DISPLAY_CONTROL_FPEN(1);
	reg |= HIBMC_DISPLAY_CONTROL_VBIASEN(1);
	writel(reg, priv->mmio + HIBMC_DISPLAY_CONTROL_HISILE);
}

static const struct drm_encoder_helper_funcs hibmc_encoder_helper_funcs = {
	.mode_set = hibmc_encoder_mode_set,
};

static const struct drm_encoder_funcs hibmc_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

int hibmc_vdac_init(struct hibmc_drm_private *priv)
{
	struct drm_device *dev = priv->dev;
	struct hibmc_connector *hibmc_connector = &priv->connector;
	struct drm_encoder *encoder;
	struct drm_connector *connector = &hibmc_connector->base;
	int ret;

	ret = hibmc_connector_init(priv);
	if (ret) {
		DRM_ERROR("failed to init connector: %d\n", ret);
		return ret;
	}

	ret = hibmc_ddc_create(dev, hibmc_connector);
	if (ret) {
		DRM_ERROR("failed to create ddc: %d\n", ret);
		return ret;
	}

	encoder = devm_kzalloc(dev->dev, sizeof(*encoder), GFP_KERNEL);
	if (!encoder) {
		DRM_ERROR("failed to alloc memory when init encoder\n");
		return -ENOMEM;
	}

	encoder->possible_crtcs = 0x1;
	ret = drm_encoder_init(dev, encoder, &hibmc_encoder_funcs,
			       DRM_MODE_ENCODER_DAC, NULL);
	if (ret) {
		DRM_ERROR("failed to init encoder: %d\n", ret);
		return ret;
	}

	drm_encoder_helper_add(encoder, &hibmc_encoder_helper_funcs);
	drm_connector_attach_encoder(connector, encoder);

	return 0;
}
