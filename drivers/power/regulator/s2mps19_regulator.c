// SPDX-License-Identifier: GPL-2.0+
/*
 * Regulators of the Samsung S2MPS19 PMIC: the LDOs
 *
 * Each LDO has one control register, with the output voltage selector in
 * the bits 5:0 and the enable mode in the bits 7:6.
 *
 * The voltage tables are those of the Linux driver
 * (drivers/regulator/s2mps11.c).
 */

#include <dm.h>
#include <errno.h>
#include <linux/kernel.h>
#include <power/pmic.h>
#include <power/regulator.h>
#include <power/s2mps19.h>
#include <vsprintf.h>

/**
 * struct s2mps19_ldo_desc - output voltage range of an LDO
 * @min_uV:	voltage of the selector 0, or 0 if there is no such LDO
 * @step_uV:	voltage step
 */
struct s2mps19_ldo_desc {
	int min_uV;
	int step_uV;
};

/**
 * struct s2mps19_ldo_priv - LDO private data
 * @desc:	output voltage range
 * @reg:	control register
 */
struct s2mps19_ldo_priv {
	const struct s2mps19_ldo_desc *desc;
	uint reg;
};

#define S2MPS19_LDO(num, min, step)	\
	[num] = { .min_uV = min, .step_uV = step }

static const struct s2mps19_ldo_desc s2mps19_ldo_desc[S2MPS19_LDO_NUM + 1] = {
	S2MPS19_LDO(1, 700000, 12500),
	S2MPS19_LDO(2, 1800000, 25000),
	S2MPS19_LDO(3, 700000, 12500),
	S2MPS19_LDO(4, 700000, 25000),
	S2MPS19_LDO(5, 300000, 25000),
	S2MPS19_LDO(6, 300000, 25000),
	S2MPS19_LDO(7, 300000, 25000),
	S2MPS19_LDO(8, 300000, 25000),
	S2MPS19_LDO(9, 700000, 12500),
	S2MPS19_LDO(10, 700000, 12500),
	S2MPS19_LDO(11, 700000, 25000),
	S2MPS19_LDO(12, 1800000, 25000),
	S2MPS19_LDO(15, 1800000, 25000),
	S2MPS19_LDO(16, 1800000, 25000),
	S2MPS19_LDO(17, 700000, 25000),
	S2MPS19_LDO(18, 700000, 12500),
	S2MPS19_LDO(19, 700000, 25000),
	S2MPS19_LDO(20, 1800000, 25000),
	S2MPS19_LDO(21, 700000, 25000),
	S2MPS19_LDO(22, 700000, 25000),
	S2MPS19_LDO(23, 700000, 12500),
	S2MPS19_LDO(24, 1800000, 25000),
	S2MPS19_LDO(25, 700000, 25000),
	S2MPS19_LDO(26, 1800000, 25000),
	S2MPS19_LDO(27, 1800000, 25000),
	S2MPS19_LDO(28, 700000, 25000),
	S2MPS19_LDO(29, 1800000, 25000),
};

static int s2mps19_ldo_get_value(struct udevice *dev)
{
	struct s2mps19_ldo_priv *priv = dev_get_priv(dev);
	int ret;

	ret = pmic_reg_read(dev->parent, priv->reg);
	if (ret < 0)
		return ret;

	return priv->desc->min_uV +
	       (ret & S2MPS19_LDO_VSEL_MASK) * priv->desc->step_uV;
}

static int s2mps19_ldo_uV_to_sel(const struct s2mps19_ldo_priv *priv, int uV)
{
	int sel;

	if (uV < priv->desc->min_uV)
		return -EINVAL;

	sel = (uV - priv->desc->min_uV) / priv->desc->step_uV;
	if (sel > S2MPS19_LDO_VSEL_MASK ||
	    priv->desc->min_uV + sel * priv->desc->step_uV != uV)
		return -EINVAL;

	return sel;
}

static int s2mps19_ldo_set_value(struct udevice *dev, int uV)
{
	struct s2mps19_ldo_priv *priv = dev_get_priv(dev);
	int ret, sel;

	sel = s2mps19_ldo_uV_to_sel(priv, uV);
	if (sel < 0)
		return sel;

	ret = pmic_reg_read(dev->parent, priv->reg);
	if (ret < 0)
		return ret;

	/* Every write is a transfer through the firmware: skip the no-ops */
	if ((ret & S2MPS19_LDO_VSEL_MASK) == sel)
		return 0;

	return pmic_reg_write(dev->parent, priv->reg,
			      (ret & ~S2MPS19_LDO_VSEL_MASK) | sel);
}

static int s2mps19_ldo_get_enable(struct udevice *dev)
{
	struct s2mps19_ldo_priv *priv = dev_get_priv(dev);
	int ret;

	ret = pmic_reg_read(dev->parent, priv->reg);
	if (ret < 0)
		return ret;

	return !!(ret & S2MPS19_LDO_ENABLE_MASK);
}

static int s2mps19_ldo_set_enable(struct udevice *dev, bool enable)
{
	struct dm_regulator_uclass_plat *uc_pdata = dev_get_uclass_plat(dev);
	struct s2mps19_ldo_priv *priv = dev_get_priv(dev);
	int ret, val;

	ret = pmic_reg_read(dev->parent, priv->reg);
	if (ret < 0)
		return ret;

	val = ret & ~S2MPS19_LDO_ENABLE_MASK;
	if (enable) {
		val |= S2MPS19_LDO_ENABLE_MASK;

		/*
		 * Nobody sets the voltage of a fixed voltage supply (equal
		 * minimum and maximum in the device tree) that is not on
		 * from the boot on, and the selector is not known to hold
		 * that voltage: switch it on with the right one.
		 */
		if (uc_pdata->flags & REGULATOR_FLAG_AUTOSET_UV) {
			int sel = s2mps19_ldo_uV_to_sel(priv, uc_pdata->min_uV);

			if (sel < 0)
				return sel;
			val = (val & ~S2MPS19_LDO_VSEL_MASK) | sel;
		}
	}

	/* Every write is a transfer through the firmware: skip the no-ops */
	if (val == ret)
		return 0;

	return pmic_reg_write(dev->parent, priv->reg, val);
}

static int s2mps19_ldo_probe(struct udevice *dev)
{
	struct dm_regulator_uclass_plat *uc_pdata = dev_get_uclass_plat(dev);
	struct s2mps19_ldo_priv *priv = dev_get_priv(dev);
	const char *name = dev->name + strlen(S2MPS19_OF_LDO_PREFIX);
	char *end;
	ulong num;

	/* The LDO number is in the node name: "ldo<n>m" */
	num = simple_strtoul(name, &end, 10);
	if (end == name || strcmp(end, "m") || num > S2MPS19_LDO_NUM ||
	    !s2mps19_ldo_desc[num].step_uV)
		return -EINVAL;

	priv->desc = &s2mps19_ldo_desc[num];
	priv->reg = S2MPS19_REG_L1CTRL + num - 1;

	uc_pdata->type = REGULATOR_TYPE_LDO;
	uc_pdata->mode_count = 0;

	return 0;
}

static const struct dm_regulator_ops s2mps19_ldo_ops = {
	.get_value = s2mps19_ldo_get_value,
	.set_value = s2mps19_ldo_set_value,
	.get_enable = s2mps19_ldo_get_enable,
	.set_enable = s2mps19_ldo_set_enable,
};

U_BOOT_DRIVER(s2mps19_ldo) = {
	.name = S2MPS19_LDO_DRIVER,
	.id = UCLASS_REGULATOR,
	.ops = &s2mps19_ldo_ops,
	.probe = s2mps19_ldo_probe,
	.priv_auto = sizeof(struct s2mps19_ldo_priv),
};
