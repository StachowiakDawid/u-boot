// SPDX-License-Identifier: GPL-2.0+
/*
 * Samsung S2MPS19 PMIC, the main PMIC of the Exynos 9820 boards
 *
 * The PMIC is on a bus (SPEEDY) owned by the APM co-processor of the SoC and
 * is accessed through the ACPM firmware.
 */

#include <dm.h>
#include <exynos-acpm.h>
#include <log.h>
#include <linux/kernel.h>
#include <power/pmic.h>
#include <power/s2mps19.h>

/* ACPM channel of the PMIC protocol */
#define S2MPS19_ACPM_CHAN		2
/* SPEEDY channel of the main PMIC */
#define S2MPS19_SPEEDY_CHAN		0

static const struct pmic_child_info s2mps19_children_info[] = {
	{ .prefix = S2MPS19_OF_LDO_PREFIX, .driver = S2MPS19_LDO_DRIVER },
	{ },
};

static int s2mps19_reg_count(struct udevice *dev)
{
	return S2MPS19_REG_COUNT;
}

static int s2mps19_write(struct udevice *dev, uint reg, const uint8_t *buff,
			 int len)
{
	int ret, n;

	if (reg + len > S2MPS19_REG_COUNT)
		return -EINVAL;

	for (; len > 0; len -= n, reg += n, buff += n) {
		n = min(len, EXYNOS_ACPM_PMIC_BULK_MAX);
		ret = exynos_acpm_pmic_bulk_write(dev->parent,
						  S2MPS19_ACPM_CHAN, reg >> 8,
						  reg & 0xff,
						  S2MPS19_SPEEDY_CHAN, n, buff);
		if (ret) {
			log_err("write error to device: %p register: %#x!\n",
				dev, reg);
			return ret;
		}
	}

	return 0;
}

static int s2mps19_read(struct udevice *dev, uint reg, uint8_t *buff, int len)
{
	int ret, n;

	if (reg + len > S2MPS19_REG_COUNT)
		return -EINVAL;

	for (; len > 0; len -= n, reg += n, buff += n) {
		n = min(len, EXYNOS_ACPM_PMIC_BULK_MAX);
		ret = exynos_acpm_pmic_bulk_read(dev->parent,
						 S2MPS19_ACPM_CHAN, reg >> 8,
						 reg & 0xff,
						 S2MPS19_SPEEDY_CHAN, n, buff);
		if (ret) {
			log_err("read error from device: %p register: %#x!\n",
				dev, reg);
			return ret;
		}
	}

	return 0;
}

static int s2mps19_bind(struct udevice *dev)
{
	ofnode regulators_node;
	int children;

	regulators_node = dev_read_subnode(dev, "regulators");
	if (!ofnode_valid(regulators_node)) {
		log_debug("%s regulators subnode not found!\n", dev->name);
		return 0;
	}

	children = pmic_bind_children(dev, regulators_node,
				      s2mps19_children_info);
	if (!children)
		log_debug("%s - no child found\n", dev->name);

	return 0;
}

static struct dm_pmic_ops s2mps19_ops = {
	.reg_count = s2mps19_reg_count,
	.read = s2mps19_read,
	.write = s2mps19_write,
};

static const struct udevice_id s2mps19_ids[] = {
	{ .compatible = "samsung,s2mps19-pmic" },
	{ }
};

U_BOOT_DRIVER(pmic_s2mps19) = {
	.name = "s2mps19_pmic",
	.id = UCLASS_PMIC,
	.of_match = s2mps19_ids,
	.ops = &s2mps19_ops,
	.bind = s2mps19_bind,
};
