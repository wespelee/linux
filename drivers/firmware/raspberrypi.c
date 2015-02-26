/*
 *  Copyright © 2015 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Defines interfaces for interacting wtih the Raspberry Pi firmware,
 * and registers some of those services with the kernel.
 */

#include <dt-bindings/arm/raspberrypi-firmware-power.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <soc/bcm2835/raspberrypi-firmware-property.h>

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_CHAN_PROPERTY		8

struct raspberrypi_firmware {
	struct genpd_onecell_data genpd_xlate;
	struct mbox_client cl;
	struct mbox_chan *chan; /* The property channel. */
	struct completion c;
	u32 enabled;
};

static struct raspberrypi_firmware *firmware;
static DEFINE_MUTEX(transaction_lock);

static void response_callback(struct mbox_client *cl, void *msg)
{
	complete(&firmware->c);
}

/*
 * Sends a request to the firmware through the BCM2835 mailbox driver,
 * and synchronously waits for the reply.
 */
static int
raspberrypi_firmware_transaction(u32 chan, u32 data)
{
	u32 message = MBOX_MSG(chan, data);
	int ret;

	WARN_ON(data & 0xf);

	mutex_lock(&transaction_lock);
	reinit_completion(&firmware->c);
	ret = mbox_send_message(firmware->chan, &message);
	if (ret >= 0) {
		wait_for_completion(&firmware->c);
		ret = 0;
	} else {
		dev_err(firmware->cl.dev, "mbox_send_message returned %d\n",
			ret);
	}
	mutex_unlock(&transaction_lock);

	return ret;
}


/*
 * Submits a set of concatenated tags to the VPU firmware through the
 * mailbox property interface.
 *
 * The buffer header and the ending tag are added by this function and
 * don't need to be supplied, just the actual tags for your operation.
 * See struct raspberrypi_firmware_property_tag_header for the per-tag
 * structure.
 */
int raspberrypi_firmware_property(void *data, size_t tag_size)
{
	size_t size = tag_size + 12;
	u32 *buf;
	dma_addr_t bus_addr;
	int ret = 0;

	if (!firmware)
		return -EPROBE_DEFER;

	/* Packets are processed a dword at a time. */
	if (size & 3)
		return -EINVAL;

	buf = dma_alloc_coherent(firmware->cl.dev, PAGE_ALIGN(size), &bus_addr,
				 GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	/* The firmware will error out without parsing in this case. */
	WARN_ON(size >= 1024 * 1024);

	buf[0] = size;
	buf[1] = RASPBERRYPI_FIRMWARE_STATUS_REQUEST;
	memcpy(&buf[2], data, tag_size);
	buf[size / 4 - 1] = RASPBERRYPI_FIRMWARE_PROPERTY_END;
	wmb();

	ret = raspberrypi_firmware_transaction(MBOX_CHAN_PROPERTY, bus_addr);

	rmb();
	memcpy(data, &buf[2], tag_size);
	if (ret == 0 && buf[1] != RASPBERRYPI_FIRMWARE_STATUS_SUCCESS) {
		/*
		 * The tag name here might not be the one causing the
		 * error, if there were multiple tags in the request.
		 * But single-tag is the most common, so go with it.
		 */
		dev_err(firmware->cl.dev,
			"Request 0x%08x returned status 0x%08x\n",
			buf[2], buf[1]);
		ret = -EINVAL;
	}

	dma_free_coherent(NULL, PAGE_ALIGN(size), buf, bus_addr);

	return ret;
}
EXPORT_SYMBOL_GPL(raspberrypi_firmware_property);

struct raspberrypi_power_domain {
	u32 domain;
	struct generic_pm_domain base;
};

/*
 * Asks the firmware to enable or disable power on a specific power
 * domain.
 */
static int raspberrypi_firmware_set_power(uint32_t domain, bool on)
{
	struct {
		struct raspberrypi_firmware_property_tag_header header;
		u32 domain;
		u32 on;
	} packet;
	int ret;

	memset(&packet, 0, sizeof(packet));
	packet.header.tag = RASPBERRYPI_FIRMWARE_SET_POWER_STATE;
	packet.header.buf_size = 8;
	packet.domain = domain;
	packet.on = on;
	ret = raspberrypi_firmware_property(&packet, sizeof(packet));
	if (!ret && !packet.on)
		ret = -EINVAL;
	return ret;
}

static int raspberrypi_domain_off(struct generic_pm_domain *domain)
{
	struct raspberrypi_power_domain *raspberrpi_domain =
		container_of(domain, struct raspberrypi_power_domain, base);

	return raspberrypi_firmware_set_power(raspberrpi_domain->domain, false);
}

static int raspberrypi_domain_on(struct generic_pm_domain *domain)
{
	struct raspberrypi_power_domain *raspberrpi_domain =
		container_of(domain, struct raspberrypi_power_domain, base);

	return raspberrypi_firmware_set_power(raspberrpi_domain->domain, true);
}

static struct raspberrypi_power_domain raspberrypi_power_domain_sdcard = {
	.domain = 0,
	.base = {
		.name = "SDCARD",
		.power_off = raspberrypi_domain_off,
		.power_on = raspberrypi_domain_on,
	}
};

static struct raspberrypi_power_domain raspberrypi_power_domain_usb = {
	.domain = 3,
	.base = {
		.name = "USB",
		.power_off = raspberrypi_domain_off,
		.power_on = raspberrypi_domain_on,
	}
};

static struct raspberrypi_power_domain raspberrypi_power_domain_dsi = {
	.domain = 9,
	.base = {
		.name = "DSI",
		.power_off = raspberrypi_domain_off,
		.power_on = raspberrypi_domain_on,
	}
};

static struct generic_pm_domain *raspberrypi_power_domains[] = {
	[POWER_DOMAIN_SDCARD] = &raspberrypi_power_domain_sdcard.base,
	[POWER_DOMAIN_USB] = &raspberrypi_power_domain_usb.base,
	[POWER_DOMAIN_DSI] = &raspberrypi_power_domain_dsi.base,
};

static int raspberrypi_firmware_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;
	int i;

	firmware = devm_kzalloc(dev, sizeof(*firmware), GFP_KERNEL);
	if (!firmware)
		return -ENOMEM;

	firmware->cl.dev = dev;
	firmware->cl.rx_callback = response_callback;
	firmware->cl.tx_block = true;

	firmware->chan = mbox_request_channel(&firmware->cl, 0);
	if (IS_ERR(firmware->chan)) {
		ret = PTR_ERR(firmware->chan);
		/* An -EBUSY from the core means it couldn't find our
		 * channel, because the mailbox driver hadn't
		 * registered yet.
		 */
		if (ret == -EBUSY)
			ret = -EPROBE_DEFER;
		else
			dev_err(dev, "Failed to get mbox channel: %d\n", ret);
		goto fail;
	}

	init_completion(&firmware->c);

	platform_set_drvdata(pdev, firmware);

	firmware->genpd_xlate.domains =
		raspberrypi_power_domains;
	firmware->genpd_xlate.num_domains =
		ARRAY_SIZE(raspberrypi_power_domains);

	for (i = 0; i < ARRAY_SIZE(raspberrypi_power_domains); i++)
		pm_genpd_init(raspberrypi_power_domains[i], NULL, true);

	of_genpd_add_provider_onecell(dev->of_node, &firmware->genpd_xlate);

	return 0;

fail:
	firmware = NULL;
	return ret;
}

static int raspberrypi_firmware_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	of_genpd_del_provider(dev->of_node);
	mbox_free_channel(firmware->chan);
	firmware = NULL;

	return 0;
}

static const struct of_device_id raspberrypi_firmware_of_match[] = {
	{ .compatible = "raspberrypi,firmware", },
	{},
};
MODULE_DEVICE_TABLE(of, raspberrypi_firmware_of_match);

static struct platform_driver raspberrypi_firmware_driver = {
	.driver = {
		.name = "raspberrypi-firmware",
		.owner = THIS_MODULE,
		.of_match_table = raspberrypi_firmware_of_match,
	},
	.probe		= raspberrypi_firmware_probe,
	.remove		= raspberrypi_firmware_remove,
};
module_platform_driver(raspberrypi_firmware_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi firmware driver");
MODULE_LICENSE("GPL v2");
