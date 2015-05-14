/*
 * Copyright (C) 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Controls the timings of the hardware's pixel valve.
 */

#include "drm_atomic_helper.h"
#include "drm_crtc_helper.h"
#include "linux/component.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

#define CRTC_WRITE(offset, val) writel(val, vc4_crtc->regs + (offset))
#define CRTC_READ(offset) readl(vc4_crtc->regs + (offset))

#define CRTC_REG(reg) { reg, #reg }
static const struct {
	u32 reg;
	const char *name;
} crtc_regs[] = {
	CRTC_REG(PV_CONTROL),
	CRTC_REG(PV_V_CONTROL),
	CRTC_REG(PV_VSYNCD),
	CRTC_REG(PV_HORZA),
	CRTC_REG(PV_HORZB),
	CRTC_REG(PV_VERTA),
	CRTC_REG(PV_VERTB),
	CRTC_REG(PV_VERTA_EVEN),
	CRTC_REG(PV_VERTB_EVEN),
	CRTC_REG(PV_INTEN),
	CRTC_REG(PV_INTSTAT),
	CRTC_REG(PV_STAT),
};

static void vc4_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
}

static bool vc4_crtc_mode_fixup(struct drm_crtc *crtc,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void vc4_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
}

static void vc4_crtc_disable(struct drm_crtc *crtc)
{
}

static void vc4_crtc_enable(struct drm_crtc *crtc)
{
}

static int vc4_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	return 0;
}

static void vc4_crtc_atomic_begin(struct drm_crtc *crtc)
{
}

static void vc4_crtc_atomic_flush(struct drm_crtc *crtc)
{
}

int vc4_enable_vblank(struct drm_device *dev, int crtc_id)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = vc4->crtc[crtc_id];

	CRTC_WRITE(PV_INTEN, PV_INT_VFP_START);

	return 0;
}

void vc4_disable_vblank(struct drm_device *dev, int crtc_id)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = vc4->crtc[crtc_id];

	CRTC_WRITE(PV_INTEN, 0);
}

static irqreturn_t vc4_crtc_irq_handler(int irq, void *data)
{
	struct vc4_crtc *vc4_crtc = data;
	u32 stat = CRTC_READ(PV_INTSTAT);
	irqreturn_t ret = IRQ_NONE;

	if (stat & PV_INT_VFP_START) {
		drm_crtc_handle_vblank(&vc4_crtc->base);
		CRTC_WRITE(PV_INTSTAT, PV_INT_VFP_START);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = vc4_crtc_destroy,
	.page_flip = drm_atomic_helper_page_flip,
	.set_property = NULL,
	.cursor_set = NULL, /* handled by drm_mode_cursor_universal */
	.cursor_move = NULL, /* handled by drm_mode_cursor_universal */
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.mode_fixup = vc4_crtc_mode_fixup,
	.mode_set_nofb = vc4_crtc_mode_set_nofb,
	.disable = vc4_crtc_disable,
	.enable = vc4_crtc_enable,
	.atomic_check = vc4_crtc_atomic_check,
	.atomic_begin = vc4_crtc_atomic_begin,
	.atomic_flush = vc4_crtc_atomic_flush,
};

static int vc4_crtc_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_crtc *vc4_crtc;
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane, *cursor_plane;
	int ret;

	primary_plane = vc4_plane_init(drm, DRM_PLANE_TYPE_PRIMARY);
	if (!primary_plane) {
		dev_err(dev, "failed to construct primary plane\n");
		ret = PTR_ERR(primary_plane);
		goto fail;
	}

	cursor_plane = vc4_plane_init(drm, DRM_PLANE_TYPE_CURSOR);
	if (!cursor_plane) {
		dev_err(dev, "failed to construct cursor_plane\n");
		ret = PTR_ERR(cursor_plane);
		goto fail;
	}

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	vc4_crtc->regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(vc4_crtc->regs))
		return PTR_ERR(vc4_crtc->regs);

	drm_crtc_init_with_planes(drm, crtc, primary_plane, cursor_plane,
				  &vc4_crtc_funcs);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);
	primary_plane->crtc = crtc;
	cursor_plane->crtc = crtc;
	vc4->crtc[drm_crtc_index(crtc)] = vc4_crtc;

	CRTC_WRITE(PV_INTEN, 0);
	CRTC_WRITE(PV_INTSTAT, PV_INT_VFP_START);
	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       vc4_crtc_irq_handler, 0, "vc4 crtc", vc4_crtc);

	platform_set_drvdata(pdev, vc4_crtc);

	return 0;

fail:
	return ret;
}

static void vc4_crtc_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc *vc4_crtc = dev_get_drvdata(dev);

	vc4_crtc_destroy(&vc4_crtc->base);

	CRTC_WRITE(PV_INTEN, 0);

	platform_set_drvdata(pdev, NULL);
}

static const struct component_ops vc4_crtc_ops = {
	.bind   = vc4_crtc_bind,
	.unbind = vc4_crtc_unbind,
};

static int vc4_crtc_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_crtc_ops);
}

static int vc4_crtc_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_crtc_ops);
	return 0;
}

static const struct of_device_id vc4_crtc_dt_match[] = {
	{ .compatible = "brcm,vc4-pixelvalve" },
	{}
};

static struct platform_driver vc4_crtc_driver = {
	.probe = vc4_crtc_dev_probe,
	.remove = vc4_crtc_dev_remove,
	.driver = {
		.name = "vc4_crtc",
		.of_match_table = vc4_crtc_dt_match,
	},
};

void __init vc4_crtc_register(void)
{
	platform_driver_register(&vc4_crtc_driver);
}

void __exit vc4_crtc_unregister(void)
{
	platform_driver_unregister(&vc4_crtc_driver);
}
