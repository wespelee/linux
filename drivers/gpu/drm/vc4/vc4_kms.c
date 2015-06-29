/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/**
 * DOC: VC4 KMS
 *
 * This is the general code for implementing KMS mode setting that
 * doesn't clearly associate with any of the other objects (plane,
 * crtc, HDMI encoder).
 */

#include "drm_crtc.h"
#include "drm_atomic.h"
#include "drm_atomic_helper.h"
#include "drm_crtc_helper.h"
#include "drm_plane_helper.h"
#include "drm_fb_cma_helper.h"
#include "vc4_drv.h"

static void vc4_output_poll_changed(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (vc4->fbdev)
		drm_fbdev_cma_hotplug_event(vc4->fbdev);
}

/**
 * vc4_atomic_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @async: asynchronous commit
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails. For
 * now this doesn't implement asynchronous commits.
 *
 * RETURNS
 * Zero for success or -errno.
 */
static int vc4_atomic_commit(struct drm_device *dev,
			     struct drm_atomic_state *state,
			     bool async)
{
	int ret;
	int i;
	uint64_t wait_seqno = 0;

	if (async) {
		DRM_ERROR("async\n");
		return -EBUSY;
	}

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret)
		return ret;

	for (i = 0; i < dev->mode_config.num_total_plane; i++) {
		struct drm_plane *plane = state->planes[i];
		struct drm_plane_state *new_state = state->plane_states[i];

		if (!plane)
			continue;

		if ((plane->state->fb != new_state->fb) && new_state->fb) {
			struct drm_gem_cma_object *cma_bo;
			struct vc4_bo *bo;
			cma_bo = drm_fb_cma_get_gem_obj(plane->state->fb, 0);
			bo = to_vc4_bo(&cma_bo->base);
			wait_seqno = max(bo->seqno, wait_seqno);
		}
	}

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	drm_atomic_helper_swap_state(dev, state);

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	drm_atomic_helper_commit_modeset_disables(dev, state);

	drm_atomic_helper_commit_planes(dev, state);

	drm_atomic_helper_commit_modeset_enables(dev, state);

	drm_atomic_helper_wait_for_vblanks(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);

	drm_atomic_state_free(state);

	return 0;
}

static const struct drm_mode_config_funcs vc4_mode_funcs = {
	.output_poll_changed = vc4_output_poll_changed,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = vc4_atomic_commit,
	.fb_create = drm_fb_cma_create,
};

int vc4_kms_load(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret;

	ret = drm_vblank_init(dev, dev->mode_config.num_crtc);
	if (ret < 0) {
		dev_err(dev->dev, "failed to initialize vblank\n");
		return ret;
	}

	dev->mode_config.max_width = 2048;
	dev->mode_config.max_height = 2048;
	dev->mode_config.funcs = &vc4_mode_funcs;
	dev->mode_config.preferred_depth = 24;
	dev->vblank_disable_allowed = true;

	drm_mode_config_reset(dev);

	vc4->fbdev = drm_fbdev_cma_init(dev, 32,
					dev->mode_config.num_crtc,
					dev->mode_config.num_connector);
	if (IS_ERR(vc4->fbdev))
		vc4->fbdev = NULL;

	drm_kms_helper_poll_init(dev);

	return 0;
}
