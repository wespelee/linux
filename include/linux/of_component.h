/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __OF_COMPONENT_H
#define __OF_COMPONENT_H

#include "linux/of.h"

void of_component_match_add_children(struct device *dev,
				     struct component_match **match,
				     struct device_node *node);

#endif /* __OF_COMPONENT_H */
