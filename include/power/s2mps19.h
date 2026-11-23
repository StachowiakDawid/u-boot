/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Samsung S2MPS19 PMIC, accessed through the ACPM firmware
 */

#ifndef __S2MPS19_H__
#define __S2MPS19_H__

/*
 * The registers are in pages ("access types" of the ACPM PMIC protocol).
 * The register numbers of the PMIC uclass are <page> << 8 | <register>.
 */
#define S2MPS19_PAGE_COMMON		0x000
#define S2MPS19_PAGE_PMIC		0x100

/* Common registers */
#define S2MPS19_REG_CHIP_ID		(S2MPS19_PAGE_COMMON | 0x00)

/* PMIC registers */
#define S2MPS19_REG_L1CTRL		(S2MPS19_PAGE_PMIC | 0x46)
#define S2MPS19_REG_ADC_DATA		(S2MPS19_PAGE_PMIC | 0xc2)

#define S2MPS19_REG_COUNT		(S2MPS19_REG_ADC_DATA + 1)

/* LDOs: LDO<n>M of the board schematics, "ldo<n>m" in the device tree */
#define S2MPS19_LDO_NUM			29
#define S2MPS19_LDO_ENABLE_MASK		0xc0
#define S2MPS19_LDO_VSEL_MASK		0x3f

#define S2MPS19_OF_LDO_PREFIX		"ldo"
#define S2MPS19_LDO_DRIVER		"s2mps19_ldo"

#endif /* __S2MPS19_H__ */
