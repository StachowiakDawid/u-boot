// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos mailbox between the application processor and the APM
 * (Active Power Management) co-processor.
 *
 * Only the doorbell towards the APM is supported: the messages themselves
 * are exchanged through shared memory by the ACPM protocol driver, which
 * polls for the replies.
 *
 * Based on the Linux driver (drivers/mailbox/exynos-mailbox.c):
 * Copyright 2024 Linaro Ltd.
 */

#include <dm.h>
#include <mailbox-uclass.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define EXYNOS_MBOX_INTGR1		0x08	/* Interrupt Generation Register 1 */

#define EXYNOS_MBOX_CHAN_COUNT		16

/**
 * struct exynos_mbox_data - SoC specific mailbox configuration
 * @intgr:		offset of the register that rings the APM doorbell
 * @intgr_shift:	position of the channel 0 doorbell bit in @intgr
 */
struct exynos_mbox_data {
	u32 intgr;
	u32 intgr_shift;
};

struct exynos_mbox_priv {
	void __iomem *regs;
	const struct exynos_mbox_data *data;
};

static int exynos_mbox_of_xlate(struct mbox_chan *chan,
				struct ofnode_phandle_args *args)
{
	/*
	 * The mailbox specifier has no cells: the ACPM channel to ring is
	 * passed with each message.
	 */
	if (args->args_count)
		return -EINVAL;

	chan->id = 0;

	return 0;
}

/* @data points to the number (u32) of the ACPM channel to ring */
static int exynos_mbox_send(struct mbox_chan *chan, const void *data)
{
	struct exynos_mbox_priv *priv = dev_get_priv(chan->dev);
	u32 acpm_chan = *(const u32 *)data;

	if (acpm_chan >= EXYNOS_MBOX_CHAN_COUNT)
		return -EINVAL;

	writel(BIT(acpm_chan) << priv->data->intgr_shift,
	       priv->regs + priv->data->intgr);

	return 0;
}

static int exynos_mbox_probe(struct udevice *dev)
{
	struct exynos_mbox_priv *priv = dev_get_priv(dev);

	priv->regs = dev_read_addr_ptr(dev);
	if (!priv->regs)
		return -EINVAL;

	priv->data = (const struct exynos_mbox_data *)dev_get_driver_data(dev);

	return 0;
}

static const struct mbox_ops exynos_mbox_ops = {
	.of_xlate = exynos_mbox_of_xlate,
	.send = exynos_mbox_send,
};

/*
 * The Exynos 9820 mailbox is described as "google,gs101-mbox" in the Linux
 * device tree, but rings the APM with the bits 31:16 of INTGR1.
 */
static const struct exynos_mbox_data exynos9820_mbox_data = {
	.intgr = EXYNOS_MBOX_INTGR1,
	.intgr_shift = 16,
};

static const struct udevice_id exynos_mbox_ids[] = {
	{
		.compatible = "samsung,exynos9820-mbox",
		.data = (ulong)&exynos9820_mbox_data,
	},
	{ }
};

U_BOOT_DRIVER(exynos_mbox) = {
	.name = "exynos-mbox",
	.id = UCLASS_MAILBOX,
	.of_match = exynos_mbox_ids,
	.probe = exynos_mbox_probe,
	.priv_auto = sizeof(struct exynos_mbox_priv),
	.ops = &exynos_mbox_ops,
};
