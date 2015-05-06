/*
 * SMP support for BCM2836 (Raspberry Pi 2)
 *
 * Copyright (C) 2015 Andrea Merello <andrea.merello@gmail.com>
 *
 * Partially based on sunxi/platsmp.c
 * Partially based on mach-bcm2709 code
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

#include <linux/smp.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>

#define ARM_LOCAL_MAILBOX3_SET0 0x8c
#define ARM_LOCAL_MAILBOX3_CLR0 0xcc

static void __iomem *local_mbox;

void __init bcm2836_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *node;

	node = of_find_compatible_node(NULL, NULL, "brcm,bcm2836-local-mbox");
	if (!node) {
		pr_err("Missing 'brcm,bcm2836-local-mbox' OF node\n");
		return;
	}

	local_mbox = of_iomap(node, 0);
	if (!local_mbox)
		pr_err("Can't map 'brcm,bcm2836-local-mbox' regs. SMP won't work\n");
}

int __init bcm2836_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	void __iomem *mbox_set;
	void __iomem *mbox_clr;
	int timeout = 20;
	unsigned long secondary_startup_phys =
		(unsigned long) virt_to_phys((void *)secondary_startup);

	/* failure cause already notified via pr_debug, just give up here */
	if(!local_mbox)
		return -ENOSYS;

	mbox_set = local_mbox + ARM_LOCAL_MAILBOX3_SET0 + 16 * cpu;
	mbox_clr = local_mbox + ARM_LOCAL_MAILBOX3_CLR0 + 16 * cpu;
	dsb();
	writel(secondary_startup_phys, mbox_set);

	while (readl(mbox_clr) != 0) {
		if (timeout-- == 0)
			return -ETIMEDOUT;
		cpu_relax();
	}

	return 0;
}

struct smp_operations bcm2836_smp_ops __initdata = {
	.smp_prepare_cpus	= bcm2836_smp_prepare_cpus,
	.smp_boot_secondary	= bcm2836_smp_boot_secondary,
};
