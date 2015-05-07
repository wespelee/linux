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

#include <dt-bindings/clk/raspberrypi.h>
#include <linux/clk-provider.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define RPI_FIRMWARE_CLOCK_STATE_ENABLED		(1 << 0)
#define RPI_FIRMWARE_CLOCK_STATE_ERROR			(1 << 1)
#define RPI_FIRMWARE_SET_CLOCK_RATE_ERROR		0

struct rpi_firmware_clock {
	/* Clock definitions in our static struct. */
	const char *name;

	/* The rest are filled in at init time. */
	struct clk_hw hw;
	struct device *dev;
	struct rpi_firmware *firmware;
};

static struct rpi_firmware_clock rpi_clocks[] = {
	[RPI_CLOCK_EMMC] = { "emmc" },
	[RPI_CLOCK_UART0] = { "uart0" },
	[RPI_CLOCK_ARM] = { "arm" },
	[RPI_CLOCK_CORE] = { "core" },
	[RPI_CLOCK_V3D] = { "v3d" },
	[RPI_CLOCK_H264] = { "h264" },
	[RPI_CLOCK_ISP] = { "isp" },
	[RPI_CLOCK_SDRAM] = { "sdram" },
	[RPI_CLOCK_PIXEL] = { "pixel" },
	[RPI_CLOCK_PWM] = { "pwm" },
};

static int rpi_clock_id(struct rpi_firmware_clock *rpi_clk)
{
	return rpi_clk - rpi_clocks;
}

static int rpi_clk_is_on(struct clk_hw *hw)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	packet[0] = rpi_clock_id(rpi_clk);
	packet[1] = 0;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_GET_CLOCK_STATE,
				    &packet, sizeof(packet));
	/* The second packet field has the new clock state returned in
	 * the low bit, or an error flag in the second bit.
	 */
	if (ret || (packet[1] & RPI_FIRMWARE_CLOCK_STATE_ERROR)) {
		dev_err(rpi_clk->dev, "Failed to get clock state\n");
		return ret ? ret : -EINVAL;
	}

	return packet[1] & RPI_FIRMWARE_CLOCK_STATE_ENABLED;
}

static int rpi_clk_set_state(struct clk_hw *hw, bool on)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	packet[0] = rpi_clock_id(rpi_clk);
	packet[1] = on;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_SET_CLOCK_STATE,
				    &packet, sizeof(packet));
	/* The second packet field has the new clock state returned in
	 * the low bit, or an error flag in the second bit.
	 */
	if (ret || (packet[1] & RPI_FIRMWARE_CLOCK_STATE_ERROR)) {
		dev_err(rpi_clk->dev, "Failed to set clock state\n");
		return ret ? ret : -EINVAL;
	}

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

	packet[0] = rpi_clock_id(rpi_clk);
	packet[1] = 0;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_GET_CLOCK_RATE,
				    &packet, sizeof(packet));
	/* Note that the second packet field returns 0 on an unknown
	 * clock error, which would also be a reasonable value for a
	 * clock that's off.
	 */
	if (ret) {
		dev_err(rpi_clk->dev, "Failed to get clock rate\n");
		return 0;
	}

	return packet[1];
}

static int rpi_clk_set_rate(struct clk_hw *hw,
			    unsigned long rate, unsigned long parent_rate)
{
	struct rpi_firmware_clock *rpi_clk =
		container_of(hw, struct rpi_firmware_clock, hw);
	u32 packet[2];
	int ret;

	packet[0] = rpi_clock_id(rpi_clk);
	packet[1] = rate;
	ret = rpi_firmware_property(rpi_clk->firmware,
				    RPI_FIRMWARE_SET_CLOCK_RATE,
				    &packet, sizeof(packet));
	/* The second packet field has the new clock rate returned, or
	 * 0 on error.
	 */
	if (ret || packet[1] == RPI_FIRMWARE_SET_CLOCK_RATE_ERROR) {
		dev_err(rpi_clk->dev, "Failed to set clock rate\n");
		return ret;
	}

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

static DEFINE_MUTEX(delayed_clock_init);
static struct clk *rpi_firmware_delayed_get_clk(struct of_phandle_args *clkspec,
						void *_data)
{
	struct device_node *of_node = _data;
	struct platform_device *pdev = of_find_device_by_node(of_node);
	struct device *dev = &pdev->dev;
	struct device_node *firmware_node;
	struct rpi_firmware *firmware;
	struct clk_init_data init;
	struct rpi_firmware_clock *rpi_clk;
	struct clk *ret_clk;

	if (clkspec->args_count != 1) {
		dev_err(dev, "clock phandle should have 1 argument\n");
		return ERR_PTR(-ENODEV);
	}

	if (clkspec->args[0] >= ARRAY_SIZE(rpi_clocks)) {
		dev_err(dev, "clock phandle index %d out of range\n",
			clkspec->args[0]);
		return ERR_PTR(-ENODEV);
	}

	rpi_clk = &rpi_clocks[clkspec->args[0]];
	if (!rpi_clk->name) {
		dev_err(dev, "clock phandle index %d invalid\n",
			clkspec->args[0]);
		return ERR_PTR(-ENODEV);
	}

	firmware_node = of_parse_phandle(of_node, "raspberrypi,firmware", 0);
	if (!firmware_node) {
		dev_err(dev, "%s: Missing firmware node\n", rpi_clk->name);
		return ERR_PTR(-ENODEV);
	}
	firmware = rpi_firmware_get(firmware_node);
	if (!firmware)
		return ERR_PTR(-EPROBE_DEFER);

	mutex_lock(&delayed_clock_init);
	if (rpi_clk->hw.clk) {
		mutex_unlock(&delayed_clock_init);
		return rpi_clk->hw.clk;
	}
	memset(&init, 0, sizeof(init));
	init.ops = &rpi_clk_ops;

	rpi_clk->firmware = firmware;
	rpi_clk->dev = dev;
	rpi_clk->hw.init = &init;
	init.name = rpi_clk->name;
	init.flags = CLK_IS_ROOT;

	ret_clk = clk_register(dev, &rpi_clk->hw);
	mutex_unlock(&delayed_clock_init);
	if (!IS_ERR(ret_clk))
		dev_info(dev, "registered %s clock\n", rpi_clk->name);
	else {
		dev_err(dev, "%s clock failed to init: %ld\n", rpi_clk->name,
			PTR_ERR(ret_clk));
	}
	return ret_clk;
}

void __init rpi_firmware_init_clock_provider(struct device_node *node)
{
	/* We delay construction of our struct clks until get time,
	 * because we need to be able to return -EPROBE_DEFER if the
	 * firmware driver isn't up yet.  clk core doesn't support
	 * re-probing on -EPROBE_DEFER, but callers of clk_get can.
	 */
	of_clk_add_provider(node, rpi_firmware_delayed_get_clk, node);
}

CLK_OF_DECLARE(rpi_firmware_clocks, "raspberrypi,bcm2835-firmware-clocks",
	       rpi_firmware_init_clock_provider);
