/* Helpers for componentized device handling with devicetree nodes.
 *
 * Copyright (C) STMicroelectronics SA 2014
 * Copyright (C) 2015 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "linux/device.h"
#include "linux/component.h"
#include "linux/of_component.h"

static int compare_of_node(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/* Given a node, add all of its enabled direct children as component matches*/
void of_component_match_add_children(struct device *dev,
				     struct component_match **match,
				     struct device_node *node)
{
	struct device_node *child_np;

	child_np = of_get_next_available_child(node, NULL);

	while (child_np) {
		component_match_add(dev, match, compare_of_node, child_np);
		of_node_put(child_np);
		child_np = of_get_next_available_child(node, child_np);
	}
}
