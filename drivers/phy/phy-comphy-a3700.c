/*
 * Marvell comphy driver
 *
 * Copyright (C) 2016 Marvell
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <dt-bindings/phy/phy-comphy-mvebu.h>

#include "phy-comphy-mvebu.h"
#include "phy-comphy-a3700.h"


/* PHY selector configures with corresponding modes */
static void mvebu_a3700_comphy_set_phy_selector(struct mvebu_comphy_priv *priv,
						struct mvebu_comphy *comphy)
{
	u32 reg;
	int mode = COMPHY_GET_MODE(priv->lanes[comphy->index].mode);


	reg = readl(priv->comphy_regs + COMPHY_SELECTOR_PHY_REG_OFFSET);
	switch (mode) {
	case (COMPHY_SATA_MODE):
		/* SATA must be in Lane2 */
		if (comphy->index == COMPHY_LANE2)
			reg &= ~COMPHY_SELECTOR_USB3_PHY_SEL_BIT;
		else
			dev_err(priv->dev, "COMPHY[%d] mode[%d] is invalid\n", comphy->index, mode);
		break;

	case (COMPHY_SGMII_MODE):
	case (COMPHY_HS_SGMII_MODE):
		if (comphy->index == COMPHY_LANE1)
			reg &= ~COMPHY_SELECTOR_USB3_GBE1_SEL_BIT;
		else if (comphy->index == COMPHY_LANE0)
			reg &= ~COMPHY_SELECTOR_PCIE_GBE0_SEL_BIT;
		else
			dev_err(priv->dev, "COMPHY[%d] mode[%d] is invalid\n", comphy->index, mode);
		break;

	case (COMPHY_USB3H_MODE):
	case (COMPHY_USB3D_MODE):
	case (COMPHY_USB3_MODE):
		if (comphy->index == COMPHY_LANE2)
			reg |= COMPHY_SELECTOR_USB3_PHY_SEL_BIT;
		else if (comphy->index == COMPHY_LANE1)
			reg |= COMPHY_SELECTOR_USB3_GBE1_SEL_BIT;
		else
			dev_err(priv->dev, "COMPHY[%d] mode[%d] is invalid\n", comphy->index, mode);
		break;

	case (COMPHY_PCIE_MODE):
		/* PCIE must be in Lane0 */
		if (comphy->index == COMPHY_LANE0)
			reg |= COMPHY_SELECTOR_PCIE_GBE0_SEL_BIT;
		else
			dev_err(priv->dev, "COMPHY[%d] mode[%d] is invalid\n", comphy->index, mode);
		break;

	default:
		dev_err(priv->dev, "COMPHY[%d] mode[%d] is invalid\n", comphy->index, mode);
		break;
	}

	writel(reg, priv->comphy_regs + COMPHY_SELECTOR_PHY_REG_OFFSET);
}

/***************************************************************************************************
  * mvebu_comphy_reg_set_indirect
  * It is only used for SATA and USB3 on comphy lane2.
  * return: void
 ***************************************************************************************************/
static void mvebu_comphy_reg_set_indirect(void __iomem *addr, u32 reg_offset, u16 data, u16 mask, int mode)
{
	/*
	 * When Lane 2 PHY is for USB3, access the PHY registers
	 * through indirect Address and Data registers INDIR_ACC_PHY_ADDR (RD00E0178h [31:0]) and
	 * INDIR_ACC_PHY_DATA (RD00E017Ch [31:0]) within the SATA Host Controller registers, Lane 2
	 * base register offset is 0x200
	 */
	if (mode == COMPHY_UNUSED)
		return;

	if (mode == COMPHY_SATA_MODE)
		writel(reg_offset, addr + COMPHY_LANE2_INDIR_ADDR_OFFSET);
	else
		writel(reg_offset + USB3PHY_LANE2_REG_BASE_OFFSET, addr + COMPHY_LANE2_INDIR_ADDR_OFFSET);

	reg_set(addr + COMPHY_LANE2_INDIR_DATA_OFFSET, data, mask);
}

/***************************************************************************************************
  * mvebu_comphy_usb3_reg_set_direct
  * It is only used USB3 direct access not on comphy lane2.
  * return: void
 ***************************************************************************************************/
static void mvebu_comphy_usb3_reg_set_direct(void __iomem *addr, u32 reg_offset, u16 data, u16 mask, int mode)
{
	reg_set16((reg_offset * PHY_SHFT(USB3) + addr), data, mask);
}

static int mvebu_a3700_comphy_sata_power_on(struct mvebu_comphy_priv *priv,
					    struct mvebu_comphy *comphy)
{
	int ret = 0;
	u32 reg_offset, data = 0;
	void __iomem *comphy_indir_regs;
	struct resource *res;
	struct platform_device *pdev = container_of(priv->dev, struct platform_device, dev);
	int mode = COMPHY_GET_MODE(priv->lanes[comphy->index].mode);
	int invert = COMPHY_GET_POLARITY_INVERT(priv->lanes[comphy->index].mode);

	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	/* Get the indirect access register resource and map */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "indirect");
	if (res) {
		comphy_indir_regs = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(comphy_indir_regs))
			return PTR_ERR(comphy_indir_regs);
	} else {
		dev_err(priv->dev, "no inirect register resource\n");
		return -ENOTSUPP;
	}

	/* Configure phy selector for SATA */
	mvebu_a3700_comphy_set_phy_selector(priv, comphy);

	/*
	 * 0. Check the Polarity invert bits
	 */
	if (invert & COMPHY_POLARITY_TXD_INVERT)
		data |= TXD_INVERT_BIT;
	if (invert & COMPHY_POLARITY_RXD_INVERT)
		data |= RXD_INVERT_BIT;

	reg_offset = COMPHY_SYNC_PATTERN_REG + SATAPHY_LANE2_REG_BASE_OFFSET;
	mvebu_comphy_reg_set_indirect(comphy_indir_regs,
				      reg_offset,
				      data,
				      TXD_INVERT_BIT | RXD_INVERT_BIT,
				      mode);

	/*
	 * 1. Select 40-bit data width width
	 */
	reg_offset = COMPHY_LOOPBACK_REG0 + SATAPHY_LANE2_REG_BASE_OFFSET;
	mvebu_comphy_reg_set_indirect(comphy_indir_regs,
				      reg_offset,
				      DATA_WIDTH_40BIT,
				      SEL_DATA_WIDTH_MASK,
				      mode);

	/*
	 * 2. Select reference clock(25M) and PHY mode (SATA)
	 */
	reg_offset = COMPHY_POWER_PLL_CTRL + SATAPHY_LANE2_REG_BASE_OFFSET;
	mvebu_comphy_reg_set_indirect(comphy_indir_regs,
				      reg_offset,
				      REF_CLOCK_SPEED_25M | PHY_MODE_SATA,
				      REF_FREF_SEL_MASK | PHY_MODE_MASK,
				      mode);

	/*
	 * 3. Use maximum PLL rate (no power save)
	 */
	reg_offset = COMPHY_KVCO_CAL_CTRL + SATAPHY_LANE2_REG_BASE_OFFSET;
	mvebu_comphy_reg_set_indirect(comphy_indir_regs,
				      reg_offset,
				      USE_MAX_PLL_RATE_BIT,
				      USE_MAX_PLL_RATE_BIT,
				      mode);

	/*
	 * 4. Reset reserved bit
	 */
	mvebu_comphy_reg_set_indirect(comphy_indir_regs,
				      COMPHY_RESERVED_REG,
				      0,
				      PHYCTRL_FRM_PIN_BIT,
				      mode);

	/*
	 * 5. Set vendor-specific configuration (It is done in sata driver)
	 */

	/* Wait for > 55 us to allow PLL be enabled */
	udelay(PLL_SET_DELAY_US);

	/* Polling status */
	writel(COMPHY_LOOPBACK_REG0 + SATAPHY_LANE2_REG_BASE_OFFSET,
	       comphy_indir_regs + COMPHY_LANE2_INDIR_ADDR_OFFSET);
	ret = polling_with_timeout(comphy_indir_regs + COMPHY_LANE2_INDIR_DATA_OFFSET,
				   PLL_READY_TX_BIT,
				   PLL_READY_TX_BIT,
				   A3700_COMPHY_PLL_LOCK_TIMEOUT,
				   REG_32BIT);

	/* Unmap resource */
	devm_iounmap(&pdev->dev, comphy_indir_regs);
	devm_release_mem_region(&pdev->dev, res->start, resource_size(res));

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	return ret;
}

static int mvebu_a3700_comphy_sgmii_power_on(struct mvebu_comphy_priv *priv,
					     struct mvebu_comphy *comphy)
{
	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	dev_err(priv->dev, "SGMII mode is not implemented\n");

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	return -ENOTSUPP;
}

static int mvebu_a3700_comphy_usb3_power_on(struct mvebu_comphy_priv *priv,
					    struct mvebu_comphy *comphy)
{
	int ret = 0;
	void __iomem *usb3_gbe1_phy_regs = NULL, *comphy_indir_regs = NULL;
	void __iomem *reg_base = NULL;
	struct resource *res = NULL, *res_indirect = NULL;
	struct platform_device *pdev = container_of(priv->dev, struct platform_device, dev);
	void (*usb3_reg_set)(void __iomem *addr, u32 reg_offset, u16 data, u16 mask, int mode);
	int mode = COMPHY_GET_MODE(priv->lanes[comphy->index].mode);
	int invert = COMPHY_GET_POLARITY_INVERT(priv->lanes[comphy->index].mode);

	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	/* Set phy seclector */
	mvebu_a3700_comphy_set_phy_selector(priv, comphy);

	/* Set usb3 reg access func, Lane2 is indirect access */
	if (comphy->index == COMPHY_LANE2) {
		usb3_reg_set = &mvebu_comphy_reg_set_indirect;
		/* Get the indirect access register resource and map */
		res_indirect = platform_get_resource_byname(pdev, IORESOURCE_MEM, "indirect");
		if (res_indirect) {
			comphy_indir_regs = devm_ioremap_resource(&pdev->dev, res_indirect);
			if (IS_ERR(comphy_indir_regs))
				return PTR_ERR(comphy_indir_regs);
		} else {
			dev_err(priv->dev, "no inirect register resource\n");
			return -ENOTSUPP;
		}
		reg_base = comphy_indir_regs;
	} else {
		/* Get the direct access register resource and map */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "usb3_gbe1_phy");
		if (res) {
			usb3_gbe1_phy_regs = devm_ioremap_resource(&pdev->dev, res);
			if (IS_ERR(usb3_gbe1_phy_regs))
				return PTR_ERR(usb3_gbe1_phy_regs);
		} else {
			dev_err(priv->dev, "no usb3_gbe1_phy register resource\n");
			return -ENOTSUPP;
		}
		usb3_reg_set = &mvebu_comphy_usb3_reg_set_direct;
		reg_base = usb3_gbe1_phy_regs;
	}

	/*
	 * 0. Set PHY OTG Control(0x5d034), bit 4, Power up OTG module
	 *    The register belong to UTMI module, so it is set
	 *    in UTMI phy driver.
	 */

	/*
	 * 1. Set PRD_TXDEEMPH (3.5db de-emph)
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_LANE_CFG0_ADDR,
		     PRD_TXDEEMPH_MASK,
		     (PRD_TXDEEMPH_MASK | PRD_TXMARGIN_MASK |
		      PRD_TXSWING_MASK | CFG_TX_ALIGN_POS_MASK),
		     mode);

	/*
	 * 2. Unset BIT0: set Tx Electrical Idle Mode
	 *    unset BIT4: set G2 Tx Datapath with no Delayed Latency
	 *    unset BIT6: set Tx Detect Rx Mode at LoZ mode
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_LANE_CFG1_ADDR,
		     0x0,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 3. Set Spread Spectrum Clock Enabled
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_LANE_CFG4_ADDR,
		     SPREAD_SPECTRUM_CLK_EN,
		     SPREAD_SPECTRUM_CLK_EN,
		     mode);

	/*
	 * 4. Set Override Margining Controls From the MAC:
	 *    Use margining signals from lane configuration
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_TEST_MODE_CTRL_ADDR,
		     MODE_MARGIN_OVERRIDE,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 5. Set Lane-to-Lane Bundle Clock Sampling Period = per PCLK cycles
	 *    set Mode Clock Source = PCLK is generated from REFCLK
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_GLOB_CLK_SRC_LO_ADDR,
		     0x0,
		     (MODE_CLK_SRC | BUNDLE_PERIOD_SEL | BUNDLE_PERIOD_SCALE |
		      BUNDLE_SAMPLE_CTRL | PLL_READY_DLY),
		     mode);

	/*
	 * 6. Set G2 Spread Spectrum Clock Amplitude at 4K
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_GEN2_SETTINGS_2,
		     G2_TX_SSC_AMP_VALUE_20,
		     G2_TX_SSC_AMP_MASK,
		     mode);

	/*
	 * 7. Unset G3 Spread Spectrum Clock Amplitude
	 *    set G3 TX and RX Register Master Current Select
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_GEN2_SETTINGS_3,
		     G3_VREG_RXTX_MAS_ISET_60U,
		     G3_TX_SSC_AMP_MASK | G3_VREG_RXTX_MAS_ISET_MASK | RSVD_PH03FH_6_0_MASK,
		     mode);

	/*
	 * 8. Check crystal jumper setting and program the Power and PLL Control accordingly
	 *    Change RX wait
	 */
	usb3_reg_set(reg_base,
		     COMPHY_POWER_PLL_CTRL,
		     (PU_IVREF_BIT | PU_PLL_BIT | PU_RX_BIT |
		      PU_TX_BIT | PU_TX_INTP_BIT | PU_DFE_BIT |
		      PHY_MODE_USB3 | USB3_REF_CLOCK_SPEED_25M),
		     (PU_IVREF_BIT | PU_PLL_BIT | PU_RX_BIT |
		      PU_TX_BIT | PU_TX_INTP_BIT | PU_DFE_BIT |
		      PLL_LOCK_BIT | PHY_MODE_MASK | REF_FREF_SEL_MASK),
		     mode);
	usb3_reg_set(reg_base,
		     COMPHY_REG_PWR_MGM_TIM1_ADDR,
		     CFG_PM_RXDEN_WAIT_1_UNIT | CFG_PM_RXDLOZ_WAIT_7_UNIT,
		     CFG_PM_OSCCLK_WAIT_MASK | CFG_PM_RXDEN_WAIT_MASK | CFG_PM_RXDLOZ_WAIT_MASK,
		     mode);

	/*
	 * 9. Enable idle sync
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_UNIT_CTRL_ADDR,
		     UNIT_CTRL_DEFAULT_VALUE | IDLE_SYNC_EN,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 10. Enable the output of 500M clock
	 */
	usb3_reg_set(reg_base,
		     COMPHY_MISC_REG0_ADDR,
		     MISC_REG0_DEFAULT_VALUE | CLK500M_EN,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 11. Set 20-bit data width
	 */
	usb3_reg_set(reg_base,
		     COMPHY_LOOPBACK_REG0,
		     DATA_WIDTH_20BIT,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 12. Override Speed_PLL value and use MAC PLL
	 */
	usb3_reg_set(reg_base,
		     COMPHY_KVCO_CAL_CTRL,
		     SPEED_PLL_VALUE_16 | USE_MAX_PLL_RATE_BIT,
		     REG_16_BIT_MASK,
		     mode);

	/*
	 * 13. Check the Polarity invert bit
	 */
	if (invert & COMPHY_POLARITY_TXD_INVERT)
		usb3_reg_set(reg_base,
			     COMPHY_SYNC_PATTERN_REG,
			     TXD_INVERT_BIT,
			     TXD_INVERT_BIT,
			     mode);
	if (invert & COMPHY_POLARITY_RXD_INVERT)
		usb3_reg_set(reg_base,
			     COMPHY_SYNC_PATTERN_REG,
			     RXD_INVERT_BIT,
			     RXD_INVERT_BIT,
			     mode);

	/*
	 * 14. Release SW reset
	 */
	usb3_reg_set(reg_base,
		     COMPHY_REG_GLOB_PHY_CTRL0_ADDR,
		     MODE_CORE_CLK_FREQ_SEL | MODE_PIPE_WIDTH_32 | MODE_REFDIV_BY_4,
		     REG_16_BIT_MASK,
		     mode);

	/* Wait for > 55 us to allow PCLK be enabled */
	udelay(PLL_SET_DELAY_US);

	if (comphy->index == COMPHY_LANE2) {
		writel(COMPHY_LOOPBACK_REG0 + USB3PHY_LANE2_REG_BASE_OFFSET,
		       comphy_indir_regs + COMPHY_LANE2_INDIR_ADDR_OFFSET);
		ret = polling_with_timeout(comphy_indir_regs + COMPHY_LANE2_INDIR_DATA_OFFSET,
					   TXDCLK_PCLK_EN,
					   TXDCLK_PCLK_EN,
					   A3700_COMPHY_PLL_LOCK_TIMEOUT,
					   REG_32BIT);
	} else {
		ret = polling_with_timeout(LANE_STATUS1_ADDR(USB3) + usb3_gbe1_phy_regs,
					   TXDCLK_PCLK_EN,
					   TXDCLK_PCLK_EN,
					   A3700_COMPHY_PLL_LOCK_TIMEOUT,
					   REG_16BIT);
	}
	if (ret)
		dev_err(priv->dev, "Failed to lock USB3 PLL\n");

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	/* Unmap resource */
	if (comphy->index == COMPHY_LANE2) {
		devm_iounmap(&pdev->dev, comphy_indir_regs);
		devm_release_mem_region(&pdev->dev, res->start, resource_size(res_indirect));
	} else {
		devm_iounmap(&pdev->dev, usb3_gbe1_phy_regs);
		devm_release_mem_region(&pdev->dev, res->start, resource_size(res));
	}

	return ret;
}

static int mvebu_a3700_comphy_pcie_power_on(struct mvebu_comphy_priv *priv,
					    struct mvebu_comphy *comphy)
{
	int ret;
	int invert = COMPHY_GET_POLARITY_INVERT(priv->lanes[comphy->index].mode);

	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	/*
	 * 1. Enable max PLL.
	 */
	reg_set16(LANE_CFG1_ADDR(PCIE) + priv->comphy_pipe_regs, USE_MAX_PLL_RATE_EN, 0x0);

	/*
	 * 2. Select 20 bit SERDES interface.
	 */
	reg_set16(GLOB_CLK_SRC_LO_ADDR(PCIE) + priv->comphy_pipe_regs, CFG_SEL_20B, 0);

	/*
	 * 3. Force to use reg setting for PCIe mode
	 */
	reg_set16(MISC_REG1_ADDR(PCIE) + priv->comphy_pipe_regs, SEL_BITS_PCIE_FORCE, 0);

	/*
	 * 4. Change RX wait
	 */
	reg_set16(PWR_MGM_TIM1_ADDR(PCIE) + priv->comphy_pipe_regs,
		  CFG_PM_RXDEN_WAIT_1_UNIT | CFG_PM_RXDLOZ_WAIT_12_UNIT,
		  CFG_PM_OSCCLK_WAIT_MASK | CFG_PM_RXDEN_WAIT_MASK | CFG_PM_RXDLOZ_WAIT_MASK);

	/*
	 * 5. Enable idle sync
	 */
	reg_set16(UNIT_CTRL_ADDR(PCIE) + priv->comphy_pipe_regs,
		  UNIT_CTRL_DEFAULT_VALUE | IDLE_SYNC_EN, REG_16_BIT_MASK);

	/*
	 * 6. Enable the output of 100M/125M/500M clock
	 */
	reg_set16(MISC_REG0_ADDR(PCIE) + priv->comphy_pipe_regs,
		  MISC_REG0_DEFAULT_VALUE | CLK500M_EN | CLK100M_125M_EN,
		  REG_16_BIT_MASK);

	/*
	 * 7. Enable TX, PCIE global register, 0xd0074814, it is done in  PCI-E driver
	 */

	/*
	 * 8. Check crystal jumper setting and program the Power and PLL Control accordingly
	 */
	reg_set16(PWR_PLL_CTRL_ADDR(PCIE) + priv->comphy_pipe_regs,
		  (PU_IVREF_BIT | PU_PLL_BIT | PU_RX_BIT | PU_TX_BIT |
		   PU_TX_INTP_BIT | PU_DFE_BIT | PCIE_REF_CLOCK_SPEED_25M | PHY_MODE_PCIE),
		   REG_16_BIT_MASK);

	/*
	 * 9. Override Speed_PLL value and use MAC PLL
	 */
	reg_set16(KVCO_CAL_CTRL_ADDR(PCIE) + priv->comphy_pipe_regs,
		  SPEED_PLL_VALUE_16 | USE_MAX_PLL_RATE_BIT, REG_16_BIT_MASK);

	/*
	 * 10. Check the Polarity invert bit
	 */
	if (invert & COMPHY_POLARITY_TXD_INVERT)
		reg_set16(SYNC_PATTERN_REG_ADDR(PCIE) + priv->comphy_pipe_regs,
			  TXD_INVERT_BIT, 0x0);

	if (invert & COMPHY_POLARITY_RXD_INVERT)
		reg_set16(SYNC_PATTERN_REG_ADDR(PCIE) + priv->comphy_pipe_regs,
			  RXD_INVERT_BIT, 0x0);

	/*
	 * 11. Release SW reset
	 */
	reg_set16(GLOB_PHY_CTRL0_ADDR(PCIE) + priv->comphy_pipe_regs,
		  MODE_CORE_CLK_FREQ_SEL | MODE_PIPE_WIDTH_32,
		  SOFT_RESET | MODE_REFDIV);

	/* Wait for > 55 us to allow PCLK be enabled */
	udelay(PLL_SET_DELAY_US);

	ret = polling_with_timeout(LANE_STATUS1_ADDR(PCIE) + priv->comphy_pipe_regs,
				   TXDCLK_PCLK_EN,
				   TXDCLK_PCLK_EN,
				   A3700_COMPHY_PLL_LOCK_TIMEOUT,
				   REG_16BIT);
	if (ret)
		dev_err(priv->dev, "Failed to lock PCIE PLL\n");

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	return ret;
}

static int mvebu_a3700_comphy_power_on(struct phy *phy)
{
	struct mvebu_comphy *comphy = phy_get_drvdata(phy);
	struct mvebu_comphy_priv *priv = to_mvebu_comphy_priv(comphy);
	int mode = COMPHY_GET_MODE(priv->lanes[comphy->index].mode);
	int err = 0;

	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	spin_lock(&priv->lock);

	switch (mode) {
	case(COMPHY_SATA_MODE):
		err = mvebu_a3700_comphy_sata_power_on(priv, comphy);
		break;

	case(COMPHY_SGMII_MODE):
	case(COMPHY_HS_SGMII_MODE):
		err = mvebu_a3700_comphy_sgmii_power_on(priv, comphy);
		break;

	case (COMPHY_USB3_MODE):
		err = mvebu_a3700_comphy_usb3_power_on(priv, comphy);
		break;

	case (COMPHY_PCIE_MODE):
		err = mvebu_a3700_comphy_pcie_power_on(priv, comphy);
		break;

	default:
		dev_err(priv->dev, "comphy%d: unsupported comphy mode\n",
			comphy->index);
		err = -EINVAL;
		break;
	}

	spin_unlock(&priv->lock);

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	return err;
}

static int mvebu_a3700_comphy_power_off(struct phy *phy)
{
	struct mvebu_comphy *comphy = phy_get_drvdata(phy);
	struct mvebu_comphy_priv *priv = to_mvebu_comphy_priv(comphy);

	dev_dbg(priv->dev, "%s: Enter\n", __func__);

	spin_lock(&priv->lock);

	dev_dbg(priv->dev, "power off is not implemented\n");

	spin_unlock(&priv->lock);

	dev_dbg(priv->dev, "%s: Exit\n", __func__);

	return 0;
}

static struct phy_ops a3700_comphy_ops = {
	.power_on	= mvebu_a3700_comphy_power_on,
	.power_off	= mvebu_a3700_comphy_power_off,
	.set_mode	= mvebu_comphy_set_mode,
	.get_mode	= mvebu_comphy_get_mode,
	.owner		= THIS_MODULE,
};

const struct mvebu_comphy_soc_info a3700_comphy = {
	.num_of_lanes = 3,
	.functions = {
		/* Lane 0 */
		{COMPHY_UNUSED, COMPHY_PCIE0, COMPHY_SGMII0},
		/* Lane 1 */
		{COMPHY_UNUSED, COMPHY_SGMII1, COMPHY_HS_SGMII1, COMPHY_USB3},
		/* Lane 2 */
		{COMPHY_UNUSED, COMPHY_SATA0, COMPHY_USB3},
	},
	.comphy_ops = &a3700_comphy_ops,
};

