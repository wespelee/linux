/*
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "drm_atomic_helper.h"
#include "drm_crtc_helper.h"
#include "drm_edid.h"
#include "linux/clk.h"
#include "linux/component.h"
#include "linux/of_gpio.h"
#include "linux/of_platform.h"
#include "vc4_drv.h"
#include "vc4_regs.h"

/* General HDMI hardware state. */
struct vc4_hdmi {
	struct platform_device *pdev;
	struct i2c_adapter *ddc;
	void __iomem *hdmicore_regs;
	void __iomem *hd_regs;
	int hpd_gpio;

	/* Probed video mode at boot time, used to filter display
	 * modes to only allow equivalent ones (since we can't set the
	 * pixel clock yet).
	 */
	struct drm_display_mode boot_mode;
};
#define HDMI_READ(offset) readl(vc4->hdmi->hdmicore_regs + offset)
#define HDMI_WRITE(offset, val) writel(val, vc4->hdmi->hdmicore_regs + offset)
#define HD_READ(offset) readl(vc4->hdmi->hd_regs + offset)
#define HD_WRITE(offset, val) writel(val, vc4->hdmi->hd_regs + offset)

/* VC4 HDMI encoder KMS struct */
struct vc4_hdmi_encoder {
	struct drm_encoder base;
	bool hdmi_monitor;
};
static inline struct vc4_hdmi_encoder *
to_vc4_hdmi_encoder(struct drm_encoder *encoder)
{
	return container_of(encoder, struct vc4_hdmi_encoder, base);
}

/* VC4 HDMI connector KMS struct */
struct vc4_hdmi_connector {
	struct drm_connector base;

	/* Since the connector is attached to just the one encoder,
	 * this is the reference to it so we can do the best_encoder()
	 * hook.
	 */
	struct drm_encoder *encoder;
};
static inline struct vc4_hdmi_connector *
to_vc4_hdmi_connector(struct drm_connector *connector)
{
	return container_of(connector, struct vc4_hdmi_connector, base);
}

#define HDMI_REG(reg) { reg, #reg }
static const struct {
	u32 reg;
	const char *name;
} hdmi_regs[] = {
	HDMI_REG(VC4_HDMI_CORE_REV),
	HDMI_REG(VC4_HDMI_SW_RESET_CONTROL),
	HDMI_REG(VC4_HDMI_HOTPLUG_INT),
	HDMI_REG(VC4_HDMI_HOTPLUG),
	HDMI_REG(VC4_HDMI_HORZA),
	HDMI_REG(VC4_HDMI_HORZB),
	HDMI_REG(VC4_HDMI_FIFO_CTL),
	HDMI_REG(VC4_HDMI_SCHEDULER_CONTROL),
	HDMI_REG(VC4_HDMI_VERTA0),
	HDMI_REG(VC4_HDMI_VERTA1),
	HDMI_REG(VC4_HDMI_VERTB0),
	HDMI_REG(VC4_HDMI_VERTB1),
	HDMI_REG(VC4_HDMI_TX_PHY_RESET_CTL),
};
static const struct {
	u32 reg;
	const char *name;
} hd_regs[] = {
	HDMI_REG(VC4_HD_M_CTL),
	HDMI_REG(VC4_HD_MAI_CTL),
	HDMI_REG(VC4_HD_VID_CTL),
	HDMI_REG(VC4_HD_CSC_CTL),
	HDMI_REG(VC4_HD_FRAME_COUNT),
};

#ifdef CONFIG_DEBUG_FS
int vc4_hdmi_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_regs); i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   hdmi_regs[i].name, hdmi_regs[i].reg,
			   HDMI_READ(hdmi_regs[i].reg));
	}

	for (i = 0; i < ARRAY_SIZE(hd_regs); i++) {
		seq_printf(m, "%s (0x%04x): 0x%08x\n",
			   hd_regs[i].name, hd_regs[i].reg,
			   HD_READ(hd_regs[i].reg));
	}

	return 0;
}
#endif /* CONFIG_DEBUG_FS */

static void vc4_hdmi_dump_regs(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(hdmi_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 hdmi_regs[i].reg, hdmi_regs[i].name,
			 HDMI_READ(hdmi_regs[i].reg));
	}
	for (i = 0; i < ARRAY_SIZE(hd_regs); i++) {
		DRM_INFO("0x%04x (%s): 0x%08x\n",
			 hd_regs[i].reg, hd_regs[i].name,
			 HD_READ(hd_regs[i].reg));
	}
}

static enum drm_connector_status
vc4_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	struct drm_device *dev = connector->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	if (vc4->hdmi->hpd_gpio) {
		if (gpio_get_value(vc4->hdmi->hpd_gpio))
			return connector_status_connected;
		else
			return connector_status_disconnected;
	}

	if (HDMI_READ(VC4_HDMI_HOTPLUG) & VC4_HDMI_HOTPLUG_CONNECTED)
		return connector_status_connected;
	else
		return connector_status_disconnected;
}

static void vc4_hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int vc4_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct vc4_hdmi_connector *vc4_connector =
		to_vc4_hdmi_connector(connector);
	struct drm_encoder *encoder = vc4_connector->encoder;
	struct vc4_hdmi_encoder *vc4_encoder = to_vc4_hdmi_encoder(encoder);
	struct drm_device *dev = connector->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret = 0;
	struct edid *edid;

	edid = drm_get_edid(connector, vc4->hdmi->ddc);
	if (!edid)
		return -ENODEV;

	vc4_encoder->hdmi_monitor = drm_detect_hdmi_monitor(edid);
	drm_mode_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);

	return ret;
}

/* Since we can't set the pixel clock yet, filter out all the EDID
 * modes that don't match what was set up by the firmware.
 */
int vc4_hdmi_mode_valid(struct drm_connector *connector,
			struct drm_display_mode *mode)
{
	struct drm_device *dev = connector->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_display_mode *boot_mode = &vc4->hdmi->boot_mode;

	if (mode->hdisplay != boot_mode->hdisplay ||
	    mode->hsync_start != boot_mode->hsync_start ||
	    mode->hsync_end != boot_mode->hsync_end ||
	    mode->htotal != boot_mode->htotal) {
		return MODE_ERROR;
	}

	return MODE_OK;
}

static struct drm_encoder *
vc4_hdmi_connector_best_encoder(struct drm_connector *connector)
{
	struct vc4_hdmi_connector *hdmi_connector =
		to_vc4_hdmi_connector(connector);
	return hdmi_connector->encoder;
}

static const struct drm_connector_funcs vc4_hdmi_connector_funcs = {
	.dpms = drm_atomic_helper_connector_dpms,
	.detect = vc4_hdmi_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_hdmi_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs vc4_hdmi_connector_helper_funcs = {
	.get_modes = vc4_hdmi_connector_get_modes,
	.mode_valid = vc4_hdmi_mode_valid,
	.best_encoder = vc4_hdmi_connector_best_encoder,
};

struct drm_connector *vc4_hdmi_connector_init(struct drm_device *dev,
					      struct drm_encoder *encoder)
{
	struct drm_connector *connector = NULL;
	struct vc4_hdmi_connector *hdmi_connector;
	int ret = 0;

	hdmi_connector = devm_kzalloc(dev->dev, sizeof(*hdmi_connector),
				      GFP_KERNEL);
	if (!hdmi_connector) {
		ret = -ENOMEM;
		goto fail;
	}
	connector = &hdmi_connector->base;

	hdmi_connector->encoder = encoder;

	drm_connector_init(dev, connector, &vc4_hdmi_connector_funcs,
			   DRM_MODE_CONNECTOR_HDMIA);
	drm_connector_helper_add(connector, &vc4_hdmi_connector_helper_funcs);

	connector->polled = (DRM_CONNECTOR_POLL_CONNECT |
			     DRM_CONNECTOR_POLL_DISCONNECT);

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	drm_connector_register(connector);

	drm_mode_connector_attach_encoder(connector, encoder);

	return connector;

 fail:
	if (connector)
		vc4_hdmi_connector_destroy(connector);

	return ERR_PTR(ret);
}

static void vc4_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs vc4_hdmi_encoder_funcs = {
	.destroy = vc4_encoder_destroy,
};

static void vc4_hdmi_encoder_mode_set(struct drm_encoder *encoder,
				      struct drm_display_mode *unadjusted_mode,
				      struct drm_display_mode *mode)
{
	struct drm_device *dev = encoder->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	bool debug_dump_regs = false;
	bool hsync_pos = !(mode->flags & DRM_MODE_FLAG_NHSYNC);
	bool vsync_pos = !(mode->flags & DRM_MODE_FLAG_NVSYNC);
	u32 vactive = (mode->vdisplay >>
		       ((mode->flags & DRM_MODE_FLAG_INTERLACE) ? 1 : 0));
	u32 verta = (VC4_SET_FIELD(mode->vsync_end - mode->vsync_start,
				   VC4_HDMI_VERTA_VSP) |
		     VC4_SET_FIELD(mode->vsync_start - mode->vdisplay,
				   VC4_HDMI_VERTA_VFP) |
		     VC4_SET_FIELD(vactive, VC4_HDMI_VERTA_VAL));
	u32 vertb = (VC4_SET_FIELD(0, VC4_HDMI_VERTB_VSPO) |
		     VC4_SET_FIELD(mode->vtotal - mode->vsync_end,
				   VC4_HDMI_VERTB_VBP));

	if (debug_dump_regs) {
		DRM_INFO("HDMI regs before:\n");
		vc4_hdmi_dump_regs(dev);
	}

	HD_WRITE(VC4_HD_VID_CTL, 0);

	/* XXX: This is where we would set the HDMI state machine
	 * clock, if we had an interface for it.
	 */

	HDMI_WRITE(VC4_HDMI_SCHEDULER_CONTROL,
		   HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) |
		   VC4_HDMI_SCHEDULER_CONTROL_MANUAL_FORMAT |
		   VC4_HDMI_SCHEDULER_CONTROL_IGNORE_VSYNC_PREDICTS);

	HDMI_WRITE(VC4_HDMI_HORZA,
		   (vsync_pos ? VC4_HDMI_HORZA_VPOS : 0) |
		   (hsync_pos ? VC4_HDMI_HORZA_HPOS : 0) |
		   VC4_SET_FIELD(mode->hdisplay, VC4_HDMI_HORZA_HAP));

	HDMI_WRITE(VC4_HDMI_HORZB,
		   VC4_SET_FIELD(mode->htotal - mode->hsync_end,
				 VC4_HDMI_HORZB_HBP) |
		   VC4_SET_FIELD(mode->hsync_end - mode->hsync_start,
				 VC4_HDMI_HORZB_HSP) |
		   VC4_SET_FIELD(mode->hsync_start - mode->hdisplay,
				 VC4_HDMI_HORZB_HFP));

	HDMI_WRITE(VC4_HDMI_VERTA0, verta);
	HDMI_WRITE(VC4_HDMI_VERTA1, verta);

	HDMI_WRITE(VC4_HDMI_VERTB0, vertb);
	HDMI_WRITE(VC4_HDMI_VERTB1, vertb);

	HD_WRITE(VC4_HD_VID_CTL,
		 (vsync_pos ? 0 : VC4_HD_VID_CTL_VSYNC_LOW) |
		 (hsync_pos ? 0 : VC4_HD_VID_CTL_HSYNC_LOW));

	/* The RGB order applies even when CSC is disabled. */
	HD_WRITE(VC4_HD_CSC_CTL, VC4_SET_FIELD(VC4_HD_CSC_CTL_ORDER_BGR,
					       VC4_HD_CSC_CTL_ORDER));

	HDMI_WRITE(VC4_HDMI_FIFO_CTL, VC4_HDMI_FIFO_CTL_MASTER_SLAVE_N);

	if (debug_dump_regs) {
		DRM_INFO("HDMI regs after:\n");
		vc4_hdmi_dump_regs(dev);
	}
}

static void vc4_hdmi_encoder_disable(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	HDMI_WRITE(VC4_HDMI_TX_PHY_RESET_CTL, 0xf << 16);
	HD_WRITE(VC4_HD_VID_CTL,
		 HD_READ(VC4_HD_VID_CTL) & ~VC4_HD_VID_CTL_ENABLE);
}

static void vc4_hdmi_encoder_enable(struct drm_encoder *encoder)
{
	struct vc4_hdmi_encoder *vc4_encoder = to_vc4_hdmi_encoder(encoder);
	struct drm_device *dev = encoder->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	HDMI_WRITE(VC4_HDMI_TX_PHY_RESET_CTL, 0);

	HD_WRITE(VC4_HD_VID_CTL,
		 HD_READ(VC4_HD_VID_CTL) |
		 VC4_HD_VID_CTL_ENABLE |
		 VC4_HD_VID_CTL_UNDERFLOW_ENABLE |
		 VC4_HD_VID_CTL_FRAME_COUNTER_RESET);

	if (vc4_encoder->hdmi_monitor) {
		HDMI_WRITE(VC4_HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) |
			   VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		while (!(HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) &
			 VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE)) {
			cpu_relax();
		}
	} else {
		HDMI_WRITE(VC4_HDMI_RAM_PACKET_CONFIG,
			   HDMI_READ(VC4_HDMI_RAM_PACKET_CONFIG) &
			   ~(VC4_HDMI_RAM_PACKET_ENABLE));
		HDMI_WRITE(VC4_HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) &
			   ~VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		while (HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) &
		       VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE) {
			cpu_relax();
		}
	}

	if (vc4_encoder->hdmi_monitor) {
		u32 drift;

		WARN_ON(!(HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) &
			  VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE));
		HDMI_WRITE(VC4_HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(VC4_HDMI_SCHEDULER_CONTROL) |
			   VC4_HDMI_SCHEDULER_CONTROL_VERT_ALWAYS_KEEPOUT);

		/* XXX: Set HDMI_RAM_PACKET_CONFIG (1 << 16) and set
		 * up the infoframe.
		 */

		drift = HDMI_READ(VC4_HDMI_FIFO_CTL);
		drift &= VC4_HDMI_FIFO_VALID_WRITE_MASK;

		HDMI_WRITE(VC4_HDMI_FIFO_CTL,
			   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
		HDMI_WRITE(VC4_HDMI_FIFO_CTL,
			   drift | VC4_HDMI_FIFO_CTL_RECENTER);
		udelay(1000);
		HDMI_WRITE(VC4_HDMI_FIFO_CTL,
			   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
		HDMI_WRITE(VC4_HDMI_FIFO_CTL,
			   drift | VC4_HDMI_FIFO_CTL_RECENTER);

		while ((HDMI_READ(VC4_HDMI_FIFO_CTL) &
			VC4_HDMI_FIFO_CTL_RECENTER_DONE) == 0) {
			cpu_relax();
		}
	}
}

static const struct drm_encoder_helper_funcs vc4_hdmi_encoder_helper_funcs = {
	.mode_set = vc4_hdmi_encoder_mode_set,
	.disable = vc4_hdmi_encoder_disable,
	.enable = vc4_hdmi_encoder_enable,
};

static struct drm_crtc *
vc4_get_crtc_node(struct platform_device *pdev)
{
	struct device_node *crtc_node;
	struct platform_device *crtc_pdev;

	crtc_node = of_parse_phandle(pdev->dev.of_node, "crtc", 0);
	if (!crtc_node) {
		DRM_ERROR("No CRTC for hdmi in DT\n");
		return ERR_PTR(-EINVAL);
	}

	crtc_pdev = of_find_device_by_node(crtc_node);
	if (!crtc_pdev) {
		DRM_ERROR("No CRTC device attached to OF node\n");
		return ERR_PTR(-EINVAL);
	}

	return platform_get_drvdata(crtc_pdev);
}

struct drm_encoder *vc4_hdmi_encoder_init(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_encoder *encoder = NULL;
	struct vc4_hdmi_encoder *vc4_hdmi_encoder;
	struct drm_crtc *crtc;
	int ret;

	vc4_hdmi_encoder = devm_kzalloc(dev->dev, sizeof(*vc4_hdmi_encoder),
					GFP_KERNEL);
	if (!vc4_hdmi_encoder) {
		ret = -ENOMEM;
		goto fail;
	}
	encoder = &vc4_hdmi_encoder->base;

	crtc = vc4_get_crtc_node(vc4->hdmi->pdev);
	if (IS_ERR(crtc)) {
		ret = PTR_ERR(crtc);
		goto fail;
	}

	drm_encoder_init(dev, encoder, &vc4_hdmi_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS);
	drm_encoder_helper_add(encoder, &vc4_hdmi_encoder_helper_funcs);

	encoder->possible_crtcs = drm_crtc_mask(crtc);

	return encoder;

fail:
	if (encoder)
		vc4_encoder_destroy(encoder);

	return ERR_PTR(ret);
}

/* Reads out the current HDMI mode progreamming at driver load time.
 *
 * This is currently used for later filtering out of the DDC-probed
 * video modes, since we can't actually change modes due to not having
 * control of the necessary clocks.  Later on, we may end up reusing
 * this for skipping modesets at boot time.
 */
static void
vc4_hdmi_get_boot_display_mode(struct vc4_dev *vc4)
{
	struct drm_display_mode *mode = &vc4->hdmi->boot_mode;
	u32 horza = HDMI_READ(VC4_HDMI_HORZA);
	u32 horzb = HDMI_READ(VC4_HDMI_HORZB);
	u32 verta = HDMI_READ(VC4_HDMI_VERTA0);
	u32 vertb = HDMI_READ(VC4_HDMI_VERTB0);

	memset(mode, 0, sizeof(*mode));

	if (!(horza & VC4_HDMI_HORZA_VPOS))
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	if (!(horza & VC4_HDMI_HORZA_HPOS))
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	mode->hdisplay = VC4_GET_FIELD(horza, VC4_HDMI_HORZA_HAP);
	mode->hsync_start = (mode->hdisplay +
			     VC4_GET_FIELD(horzb, VC4_HDMI_HORZB_HFP));
	mode->hsync_end = (mode->hsync_start +
			   VC4_GET_FIELD(horzb, VC4_HDMI_HORZB_HSP));
	mode->htotal = (mode->hsync_end +
			VC4_GET_FIELD(horzb, VC4_HDMI_HORZB_HBP));

	mode->vdisplay = VC4_GET_FIELD(verta, VC4_HDMI_VERTA_VAL);
	mode->vsync_start = (mode->vdisplay +
			     VC4_GET_FIELD(verta, VC4_HDMI_VERTA_VFP));
	mode->vsync_end = (mode->vsync_start +
			   VC4_GET_FIELD(verta, VC4_HDMI_VERTA_VSP));
	mode->vtotal = (mode->vsync_end +
			VC4_GET_FIELD(vertb, VC4_HDMI_VERTB_VBP));
}

static int vc4_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = drm->dev_private;
	struct vc4_hdmi *hdmi;
	struct device_node *ddc_node;
	u32 value;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->pdev = pdev;
	hdmi->hdmicore_regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(hdmi->hdmicore_regs))
		return PTR_ERR(hdmi->hdmicore_regs);

	hdmi->hd_regs = vc4_ioremap_regs(pdev, 1);
	if (IS_ERR(hdmi->hd_regs))
		return PTR_ERR(hdmi->hd_regs);

	/* DDC i2c driver */
	ddc_node = of_parse_phandle(dev->of_node, "ddc", 0);
	if (!ddc_node) {
		DRM_ERROR("Failed to find ddc node in device tree\n");
		return -ENODEV;
	}

	hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
	if (!hdmi->ddc) {
		DRM_ERROR("Failed to get ddc i2c adapter by node\n");
		return -EPROBE_DEFER;
	}

	/* Only use the GPIO HPD pin if present in the DT, otherwise
	 * we'll use the HDMI core's register.
	 */
	if (of_find_property(dev->of_node, "hpd-gpio", &value)) {
		hdmi->hpd_gpio = of_get_named_gpio(dev->of_node, "hpd-gpio", 0);
		if (hdmi->hpd_gpio < 0)
			return hdmi->hpd_gpio;
	}

	vc4->hdmi = hdmi;

	/* HDMI core must be enabled. */
	WARN_ON_ONCE((HD_READ(VC4_HD_M_CTL) & VC4_HD_M_ENABLE) == 0);

	vc4_hdmi_get_boot_display_mode(vc4);

	return 0;
}

static void vc4_hdmi_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = drm->dev_private;

	put_device(&vc4->hdmi->ddc->dev);

	vc4->hdmi = NULL;
}

static const struct component_ops vc4_hdmi_ops = {
	.bind   = vc4_hdmi_bind,
	.unbind = vc4_hdmi_unbind,
};

static int vc4_hdmi_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hdmi_ops);
}

static int vc4_hdmi_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hdmi_ops);
	return 0;
}

static const struct of_device_id vc4_hdmi_dt_match[] = {
	{ .compatible = "brcm,vc4-hdmi" },
	{}
};

struct platform_driver vc4_hdmi_driver = {
	.probe = vc4_hdmi_dev_probe,
	.remove = vc4_hdmi_dev_remove,
	.driver = {
		.name = "vc4_hdmi",
		.of_match_table = vc4_hdmi_dt_match,
	},
};
