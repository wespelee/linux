/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Controls an individual layer of pixels being scanned out by the HVS.
 */

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "drm_atomic_helper.h"
#include "drm_fb_cma_helper.h"
#include "drm_plane_helper.h"
#include <linux/platform_data/mailbox-bcm2708.h>
#include <soc/bcm2835/raspberrypi-firmware-property.h>

/* Firmware's structure for making an FB mbox call. */
struct fbinfo_s {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch, bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
};

struct vc4_plane_state {
	struct drm_plane_state base;
};

static inline struct vc4_plane_state *
to_vc4_plane_state(struct drm_plane_state *state)
{
	return (struct vc4_plane_state *)state;
}

static const struct hvs_format {
	u32 drm; /* DRM_FORMAT_* */
	u32 hvs; /* HVS_FORMAT_* */
	u32 pixel_order;
	bool has_alpha;
} hvs_formats[] = {
	{
		.drm = DRM_FORMAT_XRGB8888, .hvs = HVS_PIXEL_FORMAT_RGBA8888,
		.pixel_order = HVS_PIXEL_ORDER_ABGR, .has_alpha = false,
	},
	{
		.drm = DRM_FORMAT_ARGB8888, .hvs = HVS_PIXEL_FORMAT_RGBA8888,
		.pixel_order = HVS_PIXEL_ORDER_ABGR, .has_alpha = true,
	},
};

static const struct hvs_format *
vc4_get_hvs_format(u32 drm_format)
{
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(hvs_formats); i++) {
		if (hvs_formats[i].drm == drm_format)
			return &hvs_formats[i];
	}

	return NULL;
}

static bool plane_enabled(struct drm_plane_state *state)
{
	return state->fb && state->crtc;
}

struct drm_plane_state *vc4_plane_duplicate_state(struct drm_plane *plane)
{
	struct vc4_plane_state *vc4_state;

	if (WARN_ON(!plane->state))
		return NULL;

	vc4_state = kmemdup(plane->state, sizeof(*vc4_state), GFP_KERNEL);
	if (!vc4_state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &vc4_state->base);

	return &vc4_state->base;
}

void vc4_plane_destroy_state(struct drm_plane *plane,
			     struct drm_plane_state *state)
{
	struct vc4_plane_state *vc4_state = to_vc4_plane_state(state);

	__drm_atomic_helper_plane_destroy_state(plane, &vc4_state->base);
	kfree(state);
}

/* Called during init to allocate the plane's atomic state. */
void vc4_plane_reset(struct drm_plane *plane)
{
	struct vc4_plane_state *vc4_state;

	WARN_ON(plane->state);

	vc4_state = kzalloc(sizeof(*vc4_state), GFP_KERNEL);
	if (!vc4_state)
		return;

	plane->state = &vc4_state->base;
	vc4_state->base.plane = plane;
}

/*
 * If a modeset involves changing the setup of a plane, the atomic
 * infrastructure will call this to validate a proposed plane setup.
 * However, if a plane isn't getting updated, this (and the
 * corresponding vc4_plane_atomic_update) won't get called.  Thus, we
 * compute the dlist here and have all active plane dlists get updated
 * in the CRTC's flush.
 */
static int vc4_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	return 0;
}

/* Turns the display on/off. */
static int
vc4_plane_set_primary_blank(struct drm_plane *plane, bool blank)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	u32 packet = blank;
	return rpi_firmware_property(vc4->firmware_node,
				     RPI_FIRMWARE_FRAMEBUFFER_BLANK,
				     &packet, sizeof(packet));
}

/*
 * Submits the current vc4_plane->fbinfo setup to the VPU firmware to
 * set up the primary plane.
 */
static int
vc4_mbox_submit_fb(struct drm_plane *plane)
{
	struct vc4_plane *vc4_plane = to_vc4_plane(plane);
	int ret;
	u32 val;

	wmb();

	ret = bcm_mailbox_write(MBOX_CHAN_FB, vc4_plane->fbinfo_bus_addr);
	if (ret) {
		DRM_ERROR("MBOX_CHAN_FB write failed: %d\n", ret);
		return ret;
	}

	ret = bcm_mailbox_read(MBOX_CHAN_FB, &val);
	if (ret) {
		DRM_ERROR("MBOX_CHAN_FB read failed: %d\n", ret);
		return ret;
	}

	rmb();

	return 0;
}

static void vc4_plane_atomic_update_primary(struct drm_plane *plane,
					    struct drm_plane_state *old_state)
{
	struct vc4_plane *vc4_plane = to_vc4_plane(plane);
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	volatile struct fbinfo_s *fbinfo = vc4_plane->fbinfo;
	u32 bpp = 32;
	int ret;

	vc4_plane_set_primary_blank(plane, false);

	fbinfo->xres = state->crtc_w;
	fbinfo->yres = state->crtc_h;
	fbinfo->xres_virtual = state->crtc_w;
	fbinfo->yres_virtual = state->crtc_h;
	fbinfo->bpp = bpp;
	fbinfo->xoffset = state->crtc_x;
	fbinfo->yoffset = state->crtc_y;
	fbinfo->base = bo->paddr + fb->offsets[0];
	fbinfo->pitch = fb->pitches[0];

	/* A bug in the firmware makes it so that if the fb->base is
	 * set to nonzero, the configured pitch gets overwritten with
	 * the previous pitch.  So, to get the configured pitch
	 * recomputed, we have to make it allocate itself a new buffer
	 * in VC memory, first.
	 */
	if (vc4_plane->pitch != fb->pitches[0]) {
		u32 saved_base = fbinfo->base;
		fbinfo->base = 0;
		ret = vc4_mbox_submit_fb(plane);
		fbinfo->base = saved_base;

		vc4_plane->pitch = fbinfo->pitch;
		WARN_ON_ONCE(vc4_plane->pitch != fb->pitches[0]);
	}

	vc4_mbox_submit_fb(plane);
	WARN_ON_ONCE(fbinfo->pitch != fb->pitches[0]);
	WARN_ON_ONCE(fbinfo->base != bo->paddr + fb->offsets[0]);
}

static void vc4_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	vc4_plane_set_primary_blank(plane, true);
}

static void vc4_plane_atomic_update_cursor(struct drm_plane *plane,
					   struct drm_plane_state *old_state)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	int ret;
	u32 packet_state[] = { true, state->crtc_x, state->crtc_y, 0 };
	u32 packet_info[] = { state->crtc_w, state->crtc_h,
			      0, /* unused */
			      bo->paddr + fb->offsets[0],
			      0, 0, /* hotx, hoty */};
	WARN_ON_ONCE(fb->pitches[0] != state->crtc_w * 4);
	WARN_ON_ONCE(fb->bits_per_pixel != 32);

	ret = rpi_firmware_property(vc4->firmware_node,
				    RPI_FIRMWARE_SET_CURSOR_STATE,
				    &packet_state,
				    sizeof(packet_state));
	if (ret || packet_state[0] != 0)
		DRM_ERROR("Failed to set cursor state: 0x%08x\n", packet_state[0]);

	ret = rpi_firmware_property(vc4->firmware_node,
				    RPI_FIRMWARE_SET_CURSOR_INFO,
				    &packet_info,
				    sizeof(packet_info));
	if (ret || packet_info[0] != 0)
		DRM_ERROR("Failed to set cursor info: 0x%08x\n", packet_info[0]);
}

static void vc4_plane_cursor_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
#if 0
	/* This seems to break something in the FW -- we end up
	 * failing at CMA allocation.
	 */
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	u32 packet[] = { false, 0, 0, 0 };
	int ret = rpi_firmware_property(vc4->firmware_node,
					RPI_FIRMWARE_SET_CURSOR_STATE,
					&packet, sizeof(packet));
	if (ret)
		DRM_ERROR("Failed to disable cursor: 0x%08x\n", packet[0]);
#endif
}

static void vc4_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	if (plane->type == DRM_PLANE_TYPE_CURSOR) {
		if (plane_enabled(plane->state))
			vc4_plane_atomic_update_cursor(plane, old_state);
		else
			vc4_plane_cursor_disable(plane, old_state);
	} else {
		if (plane_enabled(plane->state))
			vc4_plane_atomic_update_primary(plane, old_state);
		else
			vc4_plane_atomic_disable(plane, old_state);
	}
}

static const struct drm_plane_helper_funcs vc4_plane_helper_funcs = {
	.prepare_fb = NULL,
	.cleanup_fb = NULL,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_plane_atomic_update,
};

static void vc4_plane_destroy(struct drm_plane *plane)
{
	drm_plane_helper_disable(plane);
	drm_plane_cleanup(plane);
}

static const struct drm_plane_funcs vc4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = vc4_plane_destroy,
	.set_property = NULL,
	.reset = vc4_plane_reset,
	.atomic_duplicate_state = vc4_plane_duplicate_state,
	.atomic_destroy_state = vc4_plane_destroy_state,
};

struct drm_plane *vc4_plane_init(struct drm_device *dev,
				 enum drm_plane_type type)
{
	struct drm_plane *plane = NULL;
	struct vc4_plane *vc4_plane;
	u32 formats[ARRAY_SIZE(hvs_formats)];
	int ret = 0;
	unsigned i;

	vc4_plane = devm_kzalloc(dev->dev, sizeof(*vc4_plane),
				 GFP_KERNEL);
	if (!vc4_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	if (type == DRM_PLANE_TYPE_PRIMARY) {
		vc4_plane->fbinfo =
			dma_alloc_coherent(dev->dev,
					   sizeof(*vc4_plane->fbinfo),
					   &vc4_plane->fbinfo_bus_addr,
					   GFP_KERNEL);
		memset(vc4_plane->fbinfo, 0, sizeof(*vc4_plane->fbinfo));
	}

	for (i = 0; i < ARRAY_SIZE(hvs_formats); i++)
		formats[i] = hvs_formats[i].drm;
	plane = &vc4_plane->base;
	ret = drm_universal_plane_init(dev, plane, 0xff,
				       &vc4_plane_funcs,
				       formats, ARRAY_SIZE(formats),
				       type);

	drm_plane_helper_add(plane, &vc4_plane_helper_funcs);

	return plane;
fail:
	if (plane)
		vc4_plane_destroy(plane);

	return ERR_PTR(ret);
}
