/*
 * Implements a clock provider for the clocks controlled by the
 * firmware on Raspberry Pi.
 *
 * These clocks are controlled by the CLOCKMAN peripheral in the
 * hardware, but the ARM doesn't have access to the registers for
 * them.  As a result, we have to call into the firmware to get it to
 * enable, disable, and set their frequencies.
 *
 * We don't have an interface for getting the set of frequencies
 * available from the hardware.  We can request a min/max, but other
 * than that we have to request a frequency and take what it gives us.
 *
 * Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <dt-bindings/clk/raspberrypi.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define RPI_FIRMWARE_CLOCK_STATE_ENABLED		(1 << 0)
#define RPI_FIRMWARE_CLOCK_STATE_ERROR			(1 << 1)
#define RPI_FIRMWARE_SET_CLOCK_RATE_ERROR		0

static const struct {
	const char *name;
	int flags;
} rpi_clocks[] = {
	[RPI_CLOCK_EMMC] = { "emmc", CLK_IS_ROOT },
	[RPI_CLOCK_UART0] = { "uart0", CLK_IS_ROOT },
	[RPI_CLOCK_ARM] = { "arm", CLK_IS_ROOT | CLK_IGNORE_UNUSED },
	[RPI_CLOCK_CORE] = { "core", CLK_IS_ROOT | CLK_IGNORE_UNUSED },
	[RPI_CLOCK_V3D] = { "v3d", CLK_IS_ROOT },
	[RPI_CLOCK_H264] = { "h264", CLK_IS_ROOT },
	[RPI_CLOCK_ISP] = { "isp", CLK_IS_ROOT },
	[RPI_CLOCK_SDRAM] = { "sdram", CLK_IS_ROOT | CLK_IGNORE_UNUSED },
	[RPI_CLOCK_PIXEL] = { "pixel", CLK_IS_ROOT | CLK_IGNORE_UNUSED },
	[RPI_CLOCK_PWM] = { "pwm", CLK_IS_ROOT },
};

struct rpi_firmware_clock {
	/* Clock definitions in our static struct. */
	const char *name;
	int flags;

	/* The rest are filled in at init time. */
	struct clk_hw hw;
	struct device *dev;
	struct rpi_firmware *firmware;
	uint32_t last_rate;
	int id;
};

static int rpi_clk_is_on(struct clk_hw *hw)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	packet[0] = rpi_clk->id;
	packet[1] = 0;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_GET_CLOCK_STATE,
				    &packet, sizeof(packet));
	/*
	 * The second packet field has the new clock state returned in
	 * the low bit, or an error flag in the second bit.
	 */
	if (ret || (packet[1] & RPI_FIRMWARE_CLOCK_STATE_ERROR)) {
		dev_err(rpi_clk->dev, "Failed to get clock state\n");
		return ret ? ret : -EINVAL;
	}
	dev_err(rpi_clk->dev, "%s: %s\n", rpi_clk->name, packet[1] ? "on" : "off");

	return packet[1] & RPI_FIRMWARE_CLOCK_STATE_ENABLED;
}

static int rpi_clk_set_state(struct clk_hw *hw, bool on)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	if (on == (rpi_clk->last_rate != 0))
		return 0;

	dev_err(rpi_clk->dev, "Setting %s %s\n", rpi_clk->name,
		on ? "on" : "off");

	packet[0] = rpi_clk->id;
	packet[1] = on;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_SET_CLOCK_STATE,
				    &packet, sizeof(packet));
	/*
	 * The second packet field has the new clock state returned in
	 * the low bit, or an error flag in the second bit.
	 */
	if (ret || (packet[1] & RPI_FIRMWARE_CLOCK_STATE_ERROR)) {
		dev_err(rpi_clk->dev, "Failed to set clock state\n");
		return ret ? ret : -EINVAL;
	}
	dev_err(rpi_clk->dev, "Checking...\n");
	rpi_clk_is_on(&rpi_clk->hw);

	return 0;
}

static int rpi_clk_on(struct clk_hw *hw)
{
	return rpi_clk_set_state(hw, true);
}

static void rpi_clk_off(struct clk_hw *hw)
{
	rpi_clk_set_state(hw, false);
}

static unsigned long rpi_clk_get_rate(struct clk_hw *hw,
				      unsigned long parent_rate)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	packet[0] = rpi_clk->id;
	packet[1] = 0;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_GET_CLOCK_RATE,
				    &packet, sizeof(packet));
	/*
	 * Note that the second packet field returns 0 on an unknown
	 * clock error, which would also be a reasonable value for a
	 * clock that's off.
	 */
	if (ret) {
		dev_err(rpi_clk->dev, "Failed to get clock rate\n");
		return 0;
	}

	rpi_clk->last_rate = packet[1];

	dev_err(rpi_clk->dev, "%s rate: %d\n", rpi_clk->name, packet[1]);

	return packet[1];
}

static int rpi_clk_set_rate(struct clk_hw *hw,
			    unsigned long rate, unsigned long parent_rate)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	if (rate == rpi_clk->last_rate)
		return 0;

	packet[0] = rpi_clk->id;
	packet[1] = rate;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_SET_CLOCK_RATE,
				    &packet, sizeof(packet));
	/*
	 * The second packet field has the new clock rate returned, or
	 * 0 on error.
	 */
	if (ret || packet[1] == RPI_FIRMWARE_SET_CLOCK_RATE_ERROR) {
		dev_err(rpi_clk->dev, "Failed to set clock rate\n");
		return ret;
	}

	rpi_clk->last_rate = packet[1];

	/*
	 * The firmware will have adjusted our requested rate and
	 * returned it in packet[1].  The clk core code will call
	 * rpi_clk_get_rate() to get the adjusted rate.
	 */
	dev_err(rpi_clk->dev, "Set %s clock rate to %d\n", rpi_clk->name, packet[1]);

	return 0;
}

static long rpi_clk_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	/*
	 * The firmware will end up rounding our rate to something,
	 * but we don't have an interface for it.  Just return the
	 * requested value, and it'll get updated after the clock gets
	 * set.
	 */
	return rate;
}

static struct clk_ops rpi_clk_ops = {
	.is_prepared = rpi_clk_is_on,
	.prepare = rpi_clk_on,
	.unprepare = rpi_clk_off,
	.recalc_rate = rpi_clk_get_rate,
	.set_rate = rpi_clk_set_rate,
	.round_rate = rpi_clk_round_rate,
};

static int rpi_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *firmware_node;
	struct rpi_firmware *firmware;
	struct clk_init_data init;
	int i;
	struct clk_onecell_data *onecell;

	firmware_node = of_parse_phandle(dev->of_node,
					 "raspberrypi,firmware", 0);
	if (!firmware_node) {
		dev_err(dev, "Missing firmware node\n");
		return -ENODEV;
	}
	firmware = rpi_firmware_get(firmware_node);
	if (!firmware)
		return -EPROBE_DEFER;

	onecell = devm_kmalloc(dev, sizeof(*onecell), GFP_KERNEL);
	if (!onecell)
		return -ENOMEM;
	onecell->clk_num = ARRAY_SIZE(rpi_clocks);
	onecell->clks = devm_kzalloc(dev, sizeof(*onecell->clks), GFP_KERNEL);
	if (!onecell->clks)
		return -ENOMEM;

	memset(&init, 0, sizeof(init));
	init.ops = &rpi_clk_ops;

	for (i = 0; i < ARRAY_SIZE(rpi_clocks); i++) {
		struct rpi_firmware_clock *rpi_clk;

		if (!rpi_clocks[i].name)
			continue;

		rpi_clk = devm_kzalloc(dev, sizeof(*rpi_clk), GFP_KERNEL);
		if (!rpi_clk)
			return -ENOMEM;

		rpi_clk->name = rpi_clocks[i].name;
		rpi_clk->firmware = firmware;
		rpi_clk->dev = dev;
		rpi_clk->hw.init = &init;
		rpi_clk->id = i;
		init.name = rpi_clocks[i].name;
		init.flags = rpi_clocks[i].flags;

		onecell->clks[i] = devm_clk_register(&pdev->dev, &rpi_clk->hw);
		if (IS_ERR(onecell->clks[i]))
			return PTR_ERR(onecell->clks[i]);

		/* Get the current clock rate/state, to avoid extra on
		 * to on transitions at boot.
		 */
		rpi_clk_get_rate(&rpi_clk->hw, 0);
	}

	return of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
				   onecell);
}

static int rpi_clk_remove(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);

	return 0;
}

static const struct of_device_id rpi_clk_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-firmware-clocks", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_clk_of_match);

static struct platform_driver rpi_clk_driver = {
	.driver = {
		.name = "raspberrypi-clk",
		.of_match_table = rpi_clk_of_match,
	},
	.probe		= rpi_clk_probe,
	.remove		= rpi_clk_remove,
};
module_platform_driver(rpi_clk_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi clock driver");
MODULE_LICENSE("GPL v2");
