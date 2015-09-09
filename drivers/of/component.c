/* Helpers for componentized device handling with devicetree nodes.
 *
 * Copyright (C) STMicroelectronics SA 2014
 * Copyright (C) 2015 Broadcom Corporation
 * Copyright (C) 2013 Red Hat
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

/* Given a node, add all the phandles in the list under "name" as
 * component matches.
 */
void of_component_match_add_phandles(struct device *dev,
				     struct component_match **match,
				     struct device_node *node,
				     const char *name)
{
	unsigned i;

	for (i = 0; ; i++) {
		struct device_node *child;

		child = of_parse_phandle(node, name, i);
		if (!child)
			break;

		component_match_add(dev, match, compare_of_node, child);
	}
}
EXPORT_SYMBOL_GPL(of_component_match_add_phandles);
