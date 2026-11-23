/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Samsung Exynos ACPM (Alive Clock and Power Manager) protocol
 */

#ifndef __EXYNOS_ACPM_H
#define __EXYNOS_ACPM_H

#include <linux/types.h>

struct udevice;

/* Most register values one PMIC bulk transfer carries */
#define EXYNOS_ACPM_PMIC_BULK_MAX	8

/**
 * exynos_acpm_xfer() - send a message to the APM firmware and get the reply
 *
 * @dev:	ACPM device
 * @chan:	ACPM channel
 * @cmd:	message, replaced by the reply of the firmware. The bits 21:16
 *		of the first word are reserved for the sequence number.
 * @count:	length of the message and of the reply in 32-bit words
 *
 * Return: 0 on success, -ETIMEDOUT if the firmware did not answer, other
 * -errno on failure
 */
int exynos_acpm_xfer(struct udevice *dev, unsigned int chan, u32 *cmd,
		     unsigned int count);

/**
 * exynos_acpm_pmic_bulk_read() - read consecutive registers of a PMIC
 *
 * @dev:	ACPM device
 * @acpm_chan:	ACPM channel of the PMIC protocol
 * @type:	register page (access type) of the PMIC
 * @reg:	first register
 * @chan:	SPEEDY channel of the PMIC
 * @count:	number of registers, at most EXYNOS_ACPM_PMIC_BULK_MAX
 * @buf:	buffer for the register values
 *
 * Return: 0 on success, -errno on failure
 */
int exynos_acpm_pmic_bulk_read(struct udevice *dev, unsigned int acpm_chan,
			       u8 type, u8 reg, u8 chan, u8 count, u8 *buf);

/**
 * exynos_acpm_pmic_bulk_write() - write consecutive registers of a PMIC
 *
 * @dev:	ACPM device
 * @acpm_chan:	ACPM channel of the PMIC protocol
 * @type:	register page (access type) of the PMIC
 * @reg:	first register
 * @chan:	SPEEDY channel of the PMIC
 * @count:	number of registers, at most EXYNOS_ACPM_PMIC_BULK_MAX
 * @buf:	register values
 *
 * Return: 0 on success, -errno on failure
 */
int exynos_acpm_pmic_bulk_write(struct udevice *dev, unsigned int acpm_chan,
				u8 type, u8 reg, u8 chan, u8 count,
				const u8 *buf);

#endif /* __EXYNOS_ACPM_H */
