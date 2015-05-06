/*
 * Copyright (C) 2010 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/irqchip.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/clk/bcm2835.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#define ARM_LOCAL_MAILBOX3_SET0 0x8c
#define ARM_LOCAL_MAILBOX3_CLR0 0xcc

#ifdef CONFIG_SMP
int __init bcm2836_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	int timeout = 20;
	unsigned long secondary_startup_phys =
		(unsigned long) virt_to_phys((void *)secondary_startup);
	struct regmap *regmap =
		syscon_regmap_lookup_by_compatible("brcm,bcm2836-arm-local");

	if (IS_ERR(regmap)) {
		pr_err("Faild to get local register map for SMP\n");
		return -ENOSYS;
	}

	dsb();
	regmap_write(regmap, ARM_LOCAL_MAILBOX3_SET0 + 16 * cpu,
		     secondary_startup_phys);

	while (true) {
		int val;
		int ret = regmap_read(regmap,
				      ARM_LOCAL_MAILBOX3_CLR0 + 16 * cpu, &val);
		if (ret)
			return ret;
		if (val == 0)
			return 0;
		if (timeout-- == 0)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

struct smp_operations bcm2836_smp_ops __initdata = {
	.smp_boot_secondary	= bcm2836_smp_boot_secondary,
};
#endif

static void __init bcm2835_init(void)
{
	int ret;

	bcm2835_init_clocks();

	ret = of_platform_populate(NULL, of_default_bus_match_table, NULL,
				   NULL);
	if (ret) {
		pr_err("of_platform_populate failed: %d\n", ret);
		BUG();
	}
}

static const char * const bcm2835_compat[] = {
	"brcm,bcm2835",
	NULL
};

static const char * const bcm2836_compat[] = {
	"brcm,bcm2836",
	NULL
};

DT_MACHINE_START(BCM2835, "BCM2835")
	.init_machine = bcm2835_init,
	.dt_compat = bcm2835_compat
MACHINE_END

DT_MACHINE_START(BCM2836, "BCM2836")
#ifdef CONFIG_SMP
	.smp = smp_ops(bcm2836_smp_ops),
#endif
	.init_machine = bcm2835_init,
	.dt_compat = bcm2836_compat
MACHINE_END
