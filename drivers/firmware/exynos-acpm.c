// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Exynos ACPM (Alive Clock and Power Manager) protocol
 *
 * The APM co-processor owns, among others, the bus to the PMICs. The
 * application processor talks to its firmware through message queues in a
 * shared SRAM and rings a mailbox doorbell for each message. The replies are
 * polled for.
 *
 * Based on the Linux driver (drivers/firmware/samsung/exynos-acpm.c,
 * exynos-acpm-pmic.c):
 * Copyright 2020 Samsung Electronics Co., Ltd.
 * Copyright 2020 Google LLC.
 * Copyright 2024 Linaro Ltd.
 */

#define LOG_CATEGORY UCLASS_FIRMWARE

#include <dm.h>
#include <exynos-acpm.h>
#include <log.h>
#include <mailbox.h>
#include <time.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <dm/devres.h>
#include <dm/ofnode.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/sizes.h>

#define ACPM_PROTOCOL_SEQNUM		GENMASK(21, 16)
/* The sequence numbers of the messages are 1 ... ACPM_SEQNUM_MAX - 1 */
#define ACPM_SEQNUM_MAX			64

#define ACPM_POLL_TIMEOUT_US		100000
#define ACPM_POLL_DELAY_US		20

#define ACPM_MAX_CHANS			16
#define ACPM_MAX_QLEN			256
#define ACPM_MAX_MLEN			SZ_4K

#define ACPM_EXYNOS9820_INITDATA_BASE	0x7000

/* PMIC protocol */
#define ACPM_PMIC_CHANNEL		GENMASK(15, 12)
#define ACPM_PMIC_TYPE			GENMASK(11, 8)
#define ACPM_PMIC_REG			GENMASK(7, 0)

#define ACPM_PMIC_RETURN		GENMASK(31, 24)
#define ACPM_PMIC_VALUE			GENMASK(15, 8)
#define ACPM_PMIC_FUNC			GENMASK(7, 0)

#define ACPM_PMIC_BULK_READ		3
#define ACPM_PMIC_BULK_WRITE		4

/**
 * struct acpm_shmem - shared memory configuration information
 * @reserved:	unused fields
 * @chans:	offset of the array of struct acpm_chan_shmem
 * @reserved1:	unused fields
 * @num_chans:	number of channels
 */
struct acpm_shmem {
	u32 reserved[2];
	u32 chans;
	u32 reserved1[3];
	u32 num_chans;
};

/**
 * struct acpm_chan_shmem - descriptor of a shared memory channel
 * @id:			channel ID
 * @reserved:		unused fields
 * @rx_rear:		rear pointer of the APM RX queue (TX for the AP)
 * @rx_front:		front pointer of the APM RX queue (TX for the AP)
 * @rx_base:		base address of the APM RX queue (TX for the AP)
 * @reserved1:		unused fields
 * @tx_rear:		rear pointer of the APM TX queue (RX for the AP)
 * @tx_front:		front pointer of the APM TX queue (RX for the AP)
 * @tx_base:		base address of the APM TX queue (RX for the AP)
 * @qlen:		queue length, of both queues
 * @mlen:		message length, of both queues
 * @reserved2:		unused fields
 * @poll_completion:	true when the channel works on polling
 */
struct acpm_chan_shmem {
	u32 id;
	u32 reserved[3];
	u32 rx_rear;
	u32 rx_front;
	u32 rx_base;
	u32 reserved1[3];
	u32 tx_rear;
	u32 tx_front;
	u32 tx_base;
	u32 qlen;
	u32 mlen;
	u32 reserved2[2];
	u32 poll_completion;
};

/**
 * struct acpm_queue - message queue in the shared memory
 * @rear:	address of the rear index
 * @front:	address of the front index
 * @base:	address of the first message
 */
struct acpm_queue {
	void __iomem *rear;
	void __iomem *front;
	void __iomem *base;
};

/**
 * struct acpm_chan - ACPM channel
 * @tx:		queue to the firmware: the AP advances the front index, the
 *		firmware the rear index
 * @rx:		queue from the firmware: the firmware advances the front
 *		index, the AP the rear index
 * @qlen:	number of messages the queues hold
 * @mlen:	message length in bytes
 * @seqnum:	sequence number of the last message sent
 * @usable:	the descriptor is that of a polled channel with both queues
 *		inside the shared memory
 */
struct acpm_chan {
	struct acpm_queue tx;
	struct acpm_queue rx;
	u32 qlen;
	u32 mlen;
	u8 seqnum;
	bool usable;
};

/**
 * struct acpm_priv - driver private data
 * @sram:	base address of the shared memory
 * @sram_size:	size of the shared memory
 * @mbox:	doorbell of the firmware
 * @chans:	the channels
 * @num_chans:	number of channels
 */
struct acpm_priv {
	void __iomem *sram;
	fdt_size_t sram_size;
	struct mbox_chan mbox;
	struct acpm_chan *chans;
	u32 num_chans;
};

/*
 * Offset 0 is rejected as well: it is what an unused (zero-filled) channel
 * descriptor holds, and reading the start of the SRAM takes a synchronous
 * external abort on Exynos 9820.
 */
static bool acpm_sram_ok(struct acpm_priv *priv, u32 off, u32 len)
{
	return off && off < priv->sram_size && len <= priv->sram_size - off;
}

/* Take all replies off the RX queue, keep the one to the message @seqnum */
static int acpm_get_rx(struct acpm_chan *achan, u32 seqnum, u32 *reply,
		       unsigned int count, bool *found)
{
	const void __iomem *addr;
	u32 front, i;
	unsigned int n;

	front = readl(achan->rx.front);
	i = readl(achan->rx.rear);
	if (front >= achan->qlen || i >= achan->qlen)
		return -EIO;

	if (i == front)
		return 0;

	do {
		addr = achan->rx.base + achan->mlen * i;
		if (FIELD_GET(ACPM_PROTOCOL_SEQNUM, readl(addr)) == seqnum) {
			for (n = 0; n < count; n++)
				reply[n] = readl(addr + n * sizeof(u32));
			*found = true;
		}
		i = (i + 1) % achan->qlen;
	} while (i != front);

	/* Mark the RX queue empty */
	writel(front, achan->rx.rear);

	return 0;
}

/* Wait until the firmware has room for one more message */
static int acpm_wait_for_queue_slot(struct acpm_chan *achan, u32 next_front)
{
	ulong start = timer_get_us();

	/*
	 * The RX front must keep up with the TX front, with at least one
	 * element between them, and the TX queue must not be full.
	 */
	while (readl(achan->rx.front) == next_front ||
	       readl(achan->tx.rear) == next_front) {
		if (timer_get_us() - start > ACPM_POLL_TIMEOUT_US)
			return -EBUSY;
		udelay(ACPM_POLL_DELAY_US);
	}

	return 0;
}

int exynos_acpm_xfer(struct udevice *dev, unsigned int chan, u32 *cmd,
		     unsigned int count)
{
	struct acpm_priv *priv = dev_get_priv(dev);
	struct acpm_chan *achan;
	u32 front, next_front, mbox_chan = chan;
	bool found = false;
	unsigned int n;
	ulong start;
	int ret;

	if (chan >= priv->num_chans)
		return -EINVAL;

	achan = &priv->chans[chan];
	if (!achan->usable)
		return -ENODEV;

	if (!count || count * sizeof(u32) > achan->mlen)
		return -EINVAL;

	front = readl(achan->tx.front);
	if (front >= achan->qlen)
		return -EIO;
	next_front = (front + 1) % achan->qlen;

	ret = acpm_wait_for_queue_slot(achan, next_front);
	if (ret)
		return ret;

	/* Drop stale replies, e.g. to messages of the previous boot stage */
	ret = acpm_get_rx(achan, 0, NULL, 0, &found);
	if (ret)
		return ret;
	found = false;

	if (++achan->seqnum >= ACPM_SEQNUM_MAX)
		achan->seqnum = 1;
	cmd[0] &= ~ACPM_PROTOCOL_SEQNUM;
	cmd[0] |= FIELD_PREP(ACPM_PROTOCOL_SEQNUM, achan->seqnum);

	for (n = 0; n < count; n++)
		writel(cmd[n],
		       achan->tx.base + achan->mlen * front + n * sizeof(u32));

	/* Advance the TX front and ring the doorbell */
	writel(next_front, achan->tx.front);

	ret = mbox_send(&priv->mbox, &mbox_chan);
	if (ret)
		return ret;

	start = timer_get_us();
	do {
		ret = acpm_get_rx(achan, achan->seqnum, cmd, count, &found);
		if (ret)
			return ret;
		if (found)
			return 0;
		udelay(ACPM_POLL_DELAY_US);
	} while (timer_get_us() - start < ACPM_POLL_TIMEOUT_US);

	dev_err(dev, "timeout on channel %u, message %u\n", chan,
		achan->seqnum);

	return -ETIMEDOUT;
}

static int acpm_pmic_to_errno(u32 reply)
{
	switch (FIELD_GET(ACPM_PMIC_RETURN, reply)) {
	case 0:
		return 0;
	case 1:	/* the register cannot be read */
	case 2:	/* the register cannot be written */
		return -EACCES;
	default:
		return -EIO;
	}
}

static void acpm_pmic_init_cmd(u32 cmd[4], u8 func, u8 type, u8 reg, u8 chan,
			       u8 count)
{
	cmd[0] = FIELD_PREP(ACPM_PMIC_TYPE, type) |
		 FIELD_PREP(ACPM_PMIC_REG, reg) |
		 FIELD_PREP(ACPM_PMIC_CHANNEL, chan);
	cmd[1] = FIELD_PREP(ACPM_PMIC_FUNC, func) |
		 FIELD_PREP(ACPM_PMIC_VALUE, count);
	cmd[2] = 0;
	cmd[3] = 0;
}

int exynos_acpm_pmic_bulk_read(struct udevice *dev, unsigned int acpm_chan,
			       u8 type, u8 reg, u8 chan, u8 count, u8 *buf)
{
	u32 cmd[4];
	int i, ret;

	if (!count || count > EXYNOS_ACPM_PMIC_BULK_MAX)
		return -EINVAL;

	acpm_pmic_init_cmd(cmd, ACPM_PMIC_BULK_READ, type, reg, chan, count);

	ret = exynos_acpm_xfer(dev, acpm_chan, cmd, ARRAY_SIZE(cmd));
	if (ret)
		return ret;

	ret = acpm_pmic_to_errno(cmd[1]);
	if (ret)
		return ret;

	for (i = 0; i < count; i++)
		buf[i] = cmd[2 + i / 4] >> (8 * (i % 4));

	return 0;
}

int exynos_acpm_pmic_bulk_write(struct udevice *dev, unsigned int acpm_chan,
				u8 type, u8 reg, u8 chan, u8 count,
				const u8 *buf)
{
	u32 cmd[4];
	int i, ret;

	if (!count || count > EXYNOS_ACPM_PMIC_BULK_MAX)
		return -EINVAL;

	acpm_pmic_init_cmd(cmd, ACPM_PMIC_BULK_WRITE, type, reg, chan, count);
	for (i = 0; i < count; i++)
		cmd[2 + i / 4] |= (u32)buf[i] << (8 * (i % 4));

	ret = exynos_acpm_xfer(dev, acpm_chan, cmd, ARRAY_SIZE(cmd));
	if (ret)
		return ret;

	return acpm_pmic_to_errno(cmd[1]);
}

/* Get the queues of a channel from its descriptor in the shared memory */
static void acpm_chan_init(struct acpm_priv *priv, struct acpm_chan *achan,
			   const struct acpm_chan_shmem __iomem *shmem)
{
	u32 tx_base, tx_rear, tx_front, rx_base, rx_rear, rx_front, qsize;

	achan->qlen = readl(&shmem->qlen);
	achan->mlen = readl(&shmem->mlen);

	/* The RX queue of the firmware is the TX queue of the AP */
	tx_base = readl(&shmem->rx_base);
	tx_rear = readl(&shmem->rx_rear);
	tx_front = readl(&shmem->rx_front);
	rx_base = readl(&shmem->tx_base);
	rx_rear = readl(&shmem->tx_rear);
	rx_front = readl(&shmem->tx_front);

	achan->tx.base = priv->sram + tx_base;
	achan->tx.rear = priv->sram + tx_rear;
	achan->tx.front = priv->sram + tx_front;
	achan->rx.base = priv->sram + rx_base;
	achan->rx.rear = priv->sram + rx_rear;
	achan->rx.front = priv->sram + rx_front;

	if (!achan->qlen || achan->qlen > ACPM_MAX_QLEN ||
	    achan->mlen < sizeof(u32) || achan->mlen > ACPM_MAX_MLEN)
		return;

	qsize = achan->qlen * achan->mlen;

	/* Interrupt driven channels are not supported */
	achan->usable = readl(&shmem->poll_completion) &&
			acpm_sram_ok(priv, tx_rear, sizeof(u32)) &&
			acpm_sram_ok(priv, tx_front, sizeof(u32)) &&
			acpm_sram_ok(priv, rx_rear, sizeof(u32)) &&
			acpm_sram_ok(priv, rx_front, sizeof(u32)) &&
			acpm_sram_ok(priv, tx_base, qsize) &&
			acpm_sram_ok(priv, rx_base, qsize);
}

static int acpm_probe(struct udevice *dev)
{
	struct acpm_priv *priv = dev_get_priv(dev);
	const struct acpm_chan_shmem __iomem *chans;
	const struct acpm_shmem __iomem *shmem;
	struct ofnode_phandle_args args;
	fdt_addr_t addr;
	u32 initdata_base = dev_get_driver_data(dev);
	u32 chans_off, i;
	int ret;

	ret = dev_read_phandle_with_args(dev, "shmem", NULL, 0, 0, &args);
	if (ret)
		return ret;

	addr = ofnode_get_addr_size_index(args.node, 0, &priv->sram_size);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->sram = map_physmem(addr, priv->sram_size, MAP_NOCACHE);

	if (!acpm_sram_ok(priv, initdata_base, sizeof(*shmem)))
		return -EINVAL;
	shmem = priv->sram + initdata_base;

	priv->num_chans = readl(&shmem->num_chans);
	chans_off = readl(&shmem->chans);
	if (!priv->num_chans || priv->num_chans > ACPM_MAX_CHANS ||
	    !acpm_sram_ok(priv, chans_off, priv->num_chans * sizeof(*chans))) {
		dev_err(dev, "no channels in the shared memory\n");
		return -ENODEV;
	}
	chans = priv->sram + chans_off;

	priv->chans = devm_kcalloc(dev, priv->num_chans, sizeof(*priv->chans),
				   GFP_KERNEL);
	if (!priv->chans)
		return -ENOMEM;

	for (i = 0; i < priv->num_chans; i++)
		acpm_chan_init(priv, &priv->chans[i], &chans[i]);

	return mbox_get_by_index(dev, 0, &priv->mbox);
}

static const struct udevice_id acpm_ids[] = {
	{
		.compatible = "samsung,exynos9820-acpm-ipc",
		.data = ACPM_EXYNOS9820_INITDATA_BASE,
	},
	{ }
};

U_BOOT_DRIVER(exynos_acpm) = {
	.name = "exynos-acpm",
	.id = UCLASS_FIRMWARE,
	.of_match = acpm_ids,
	.probe = acpm_probe,
	.priv_auto = sizeof(struct acpm_priv),
};
