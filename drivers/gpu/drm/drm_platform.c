/*
 * Derived from drm_pci.c
 *
 * Copyright 2003 José Fonseca.
 * Copyright 2003 Leif Delgass.
 * Copyright (c) 2009, Code Aurora Forum.
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/component.h>
#include <linux/export.h>
#include <drm/drmP.h>

/*
 * Register.
 *
 * \param platdev - Platform device struture
 * \return zero on success or a negative number on failure.
 *
 * Attempt to gets inter module "drm" information. If we are first
 * then register the character device and inter module information.
 * Try and register, if we fail to register, backout previous work.
 */

static int drm_get_platform_dev(struct platform_device *platdev,
				struct drm_driver *driver)
{
	struct drm_device *dev;
	int ret;

	DRM_DEBUG("\n");

	dev = drm_dev_alloc(driver, &platdev->dev);
	if (!dev)
		return -ENOMEM;

	dev->platformdev = platdev;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	DRM_INFO("Initialized %s %d.%d.%d %s on minor %d\n",
		 driver->name, driver->major, driver->minor, driver->patchlevel,
		 driver->date, dev->primary->index);

	return 0;

err_free:
	drm_dev_unref(dev);
	return ret;
}

int drm_platform_set_busid(struct drm_device *dev, struct drm_master *master)
{
	int id;

	id = dev->platformdev->id;
	if (id < 0)
		id = 0;

	master->unique = kasprintf(GFP_KERNEL, "platform:%s:%02d",
						dev->platformdev->name, id);
	if (!master->unique)
		return -ENOMEM;

	master->unique_len = strlen(master->unique);
	return 0;
}
EXPORT_SYMBOL(drm_platform_set_busid);

/**
 * drm_platform_init - Register a platform device with the DRM subsystem
 * @driver: DRM device driver
 * @platform_device: platform device to register
 *
 * Registers the specified DRM device driver and platform device with the DRM
 * subsystem, initializing a drm_device structure and calling the driver's
 * .load() function.
 *
 * Return: 0 on success or a negative error code on failure.
 */
int drm_platform_init(struct drm_driver *driver, struct platform_device *platform_device)
{
	DRM_DEBUG("\n");

	return drm_get_platform_dev(platform_device, driver);
}
EXPORT_SYMBOL(drm_platform_init);

/**
 * drm_platform_register_drivers - Helper to register an array of
 * struct platform_drivers.
 */
int drm_platform_register_drivers(struct platform_driver *const *drv,
				  int count)
{
	int i, ret;

	for (i = 0; i < count; ++i) {
		ret = platform_driver_register(drv[i]);
		if (!ret)
			continue;

		while (--i >= 0)
			platform_driver_unregister(drv[i]);

		return ret;
	}

	return 0;
}

/**
 * drm_platform_register_drivers - Helper to unregister an array of
 * struct platform_drivers.
 */
void drm_platform_unregister_drivers(struct platform_driver *const *drv,
				     int count)
{
	while (--count >= 0)
		platform_driver_unregister(drv[count]);
}

static int compare_dev(struct device *dev, void *data)
{
	return dev == (struct device *)data;
}

/**
 * drm_platform_component_match_add_drivers - For each driver passed
 * in, finds each device that matched to it and adds it as a component
 * driver to the match list.
 */
void drm_platform_component_match_add_drivers(struct device *dev,
					      struct component_match **match,
					      struct platform_driver *const *drivers,
					      int count)
{
	int i;

	for (i = 0; i < count; i++) {
		struct device_driver *drv = &drivers[i]->driver;
		struct device *p = NULL, *d;

		while ((d = bus_find_device(&platform_bus_type, p, drv,
					    (void *)platform_bus_type.match))) {
			put_device(p);
			component_match_add(dev, match, compare_dev, d);
			p = d;
		}
		put_device(p);
	}
}
EXPORT_SYMBOL_GPL(drm_platform_component_match_add_drivers);
