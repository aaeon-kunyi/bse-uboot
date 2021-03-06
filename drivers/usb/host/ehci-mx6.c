// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2009 Daniel Mack <daniel@caiaq.de>
 * Copyright (C) 2010 Freescale Semiconductor, Inc.
 * Copyright 2017 NXP
 *
 */

#include <common.h>
#include <usb.h>
#include <errno.h>
#include <wait_bit.h>
#include <linux/compiler.h>
#include <usb/ehci-ci.h>
#include <asm/io.h>
#include <asm/arch/imx-regs.h>
#include <asm/arch/clock.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm/mach-imx/regs-usbphy.h>
#include <asm/mach-imx/sys_proto.h>
#include <dm.h>
#include <asm/mach-types.h>
#include <power/regulator.h>
#include <asm/arch/sys_proto.h>

#include "ehci.h"
#if CONFIG_IS_ENABLED(POWER_DOMAIN)
#include <power-domain.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define USB_OTGREGS_OFFSET	0x000
#define USB_H1REGS_OFFSET	0x200
#define USB_H2REGS_OFFSET	0x400
#define USB_H3REGS_OFFSET	0x600
#define USB_OTHERREGS_OFFSET	0x800

#define USB_H1_CTRL_OFFSET	0x04

#define ANADIG_USB2_CHRG_DETECT_EN_B		0x00100000
#define ANADIG_USB2_CHRG_DETECT_CHK_CHRG_B	0x00080000

#define ANADIG_USB2_PLL_480_CTRL_BYPASS		0x00010000
#define ANADIG_USB2_PLL_480_CTRL_ENABLE		0x00002000
#define ANADIG_USB2_PLL_480_CTRL_POWER		0x00001000
#define ANADIG_USB2_PLL_480_CTRL_EN_USB_CLKS	0x00000040

#define USBNC_OFFSET		0x200
#define USBNC_PHYCFG2_ACAENB	(1 << 4) /* otg_id detection enable */
#define UCTRL_PWR_POL		(1 << 9) /* OTG Polarity of Power Pin */
#define UCTRL_OVER_CUR_POL	(1 << 8) /* OTG Polarity of Overcurrent */
#define UCTRL_OVER_CUR_DIS	(1 << 7) /* Disable OTG Overcurrent Detection */

#define PLL_USB_EN_USB_CLKS_MASK	(0x01 << 6)
#define PLL_USB_PWR_MASK		(0x01 << 12)
#define PLL_USB_ENABLE_MASK		(0x01 << 13)
#define PLL_USB_BYPASS_MASK		(0x01 << 16)
#define PLL_USB_REG_ENABLE_MASK		(0x01 << 21)
#define PLL_USB_DIV_SEL_MASK		(0x07 << 22)
#define PLL_USB_LOCK_MASK		(0x01 << 31)

/* USBCMD */
#define UCMD_RUN_STOP           (1 << 0) /* controller run/stop */
#define UCMD_RESET		(1 << 1) /* controller reset */

#if defined(CONFIG_MX6) || defined(CONFIG_MX7ULP) || defined(CONFIG_IMX8)
static const ulong phy_bases[] = {
	USB_PHY0_BASE_ADDR,
#if defined(USB_PHY1_BASE_ADDR)
	USB_PHY1_BASE_ADDR,
#endif
};

static void usb_internal_phy_clock_gate(int index, int on)
{
	void __iomem *phy_reg;

	if (index >= ARRAY_SIZE(phy_bases))
		return;

	phy_reg = (void __iomem *)phy_bases[index];
	phy_reg += on ? USBPHY_CTRL_CLR : USBPHY_CTRL_SET;
	writel(USBPHY_CTRL_CLKGATE, phy_reg);
}

static void usb_power_config(int index)
{
#if defined(CONFIG_MX7ULP)
	struct usbphy_regs __iomem *usbphy =
		(struct usbphy_regs __iomem *)USB_PHY0_BASE_ADDR;

	if (index > 0)
		return;

	writel(ANADIG_USB2_CHRG_DETECT_EN_B |
		   ANADIG_USB2_CHRG_DETECT_CHK_CHRG_B,
		   &usbphy->usb1_chrg_detect);

	scg_enable_usb_pll(true);

#elif defined(CONFIG_IMX8)
	struct usbphy_regs __iomem *usbphy =
		(struct usbphy_regs __iomem *)USB_PHY0_BASE_ADDR;
	int timeout = 1000000;

	if (index > 0)
		return;

	writel(ANADIG_USB2_CHRG_DETECT_EN_B |
		   ANADIG_USB2_CHRG_DETECT_CHK_CHRG_B,
		   &usbphy->usb1_chrg_detect);

	if (!(readl(&usbphy->usb1_pll_480_ctrl) & PLL_USB_LOCK_MASK)) {

		/* Enable the regulator first */
		writel(PLL_USB_REG_ENABLE_MASK,
		       &usbphy->usb1_pll_480_ctrl_set);

		/* Wait at least 25us */
		udelay(25);

		/* Enable the power */
		writel(PLL_USB_PWR_MASK, &usbphy->usb1_pll_480_ctrl_set);

		/* Wait lock */
		while (timeout--) {
			if (readl(&usbphy->usb1_pll_480_ctrl) &
			    PLL_USB_LOCK_MASK)
				break;
			udelay(10);
		}

		if (timeout <= 0) {
			/* If timeout, we power down the pll */
			writel(PLL_USB_PWR_MASK,
			       &usbphy->usb1_pll_480_ctrl_clr);
			return;
		}
	}

	/* Clear the bypass */
	writel(PLL_USB_BYPASS_MASK, &usbphy->usb1_pll_480_ctrl_clr);

	/* Enable the PLL clock out to USB */
	writel((PLL_USB_EN_USB_CLKS_MASK | PLL_USB_ENABLE_MASK),
	       &usbphy->usb1_pll_480_ctrl_set);

#else
	struct anatop_regs __iomem *anatop =
		(struct anatop_regs __iomem *)ANATOP_BASE_ADDR;
	void __iomem *chrg_detect;
	void __iomem *pll_480_ctrl_clr;
	void __iomem *pll_480_ctrl_set;

	switch (index) {
	case 0:
		chrg_detect = &anatop->usb1_chrg_detect;
		pll_480_ctrl_clr = &anatop->usb1_pll_480_ctrl_clr;
		pll_480_ctrl_set = &anatop->usb1_pll_480_ctrl_set;
		break;
	case 1:
		chrg_detect = &anatop->usb2_chrg_detect;
		pll_480_ctrl_clr = &anatop->usb2_pll_480_ctrl_clr;
		pll_480_ctrl_set = &anatop->usb2_pll_480_ctrl_set;
		break;
	default:
		return;
	}
	/*
	 * Some phy and power's special controls
	 * 1. The external charger detector needs to be disabled
	 * or the signal at DP will be poor
	 * 2. The PLL's power and output to usb
	 * is totally controlled by IC, so the Software only needs
	 * to enable them at initializtion.
	 */
	writel(ANADIG_USB2_CHRG_DETECT_EN_B |
		     ANADIG_USB2_CHRG_DETECT_CHK_CHRG_B,
		     chrg_detect);

	writel(ANADIG_USB2_PLL_480_CTRL_BYPASS,
		     pll_480_ctrl_clr);

	writel(ANADIG_USB2_PLL_480_CTRL_ENABLE |
		     ANADIG_USB2_PLL_480_CTRL_POWER |
		     ANADIG_USB2_PLL_480_CTRL_EN_USB_CLKS,
		     pll_480_ctrl_set);

#endif
}

/* Return 0 : host node, <>0 : device mode */
static int usb_phy_enable(int index, struct usb_ehci *ehci)
{
	void __iomem *phy_reg;
	void __iomem *phy_ctrl;
	void __iomem *usb_cmd;
	int ret;

	if (index >= ARRAY_SIZE(phy_bases))
		return 0;

	phy_reg = (void __iomem *)phy_bases[index];
	phy_ctrl = (void __iomem *)(phy_reg + USBPHY_CTRL);
	usb_cmd = (void __iomem *)&ehci->usbcmd;

	/* Stop then Reset */
	clrbits_le32(usb_cmd, UCMD_RUN_STOP);
	ret = wait_for_bit_le32(usb_cmd, UCMD_RUN_STOP, false, 10000, false);
	if (ret)
		return ret;

	setbits_le32(usb_cmd, UCMD_RESET);
	ret = wait_for_bit_le32(usb_cmd, UCMD_RESET, false, 10000, false);
	if (ret)
		return ret;

	/* Reset USBPHY module */
	setbits_le32(phy_ctrl, USBPHY_CTRL_SFTRST);
	udelay(10);

	/* Remove CLKGATE and SFTRST */
	clrbits_le32(phy_ctrl, USBPHY_CTRL_CLKGATE | USBPHY_CTRL_SFTRST);
	udelay(10);

	/* Power up the PHY */
	writel(0, phy_reg + USBPHY_PWD);
	/* enable FS/LS device */
	setbits_le32(phy_ctrl, USBPHY_CTRL_ENUTMILEVEL2 |
			USBPHY_CTRL_ENUTMILEVEL3);

	return 0;
}

int usb_phy_mode(int port)
{
	void __iomem *phy_reg;
	void __iomem *phy_ctrl;
	u32 val;

	phy_reg = (void __iomem *)phy_bases[port];
	phy_ctrl = (void __iomem *)(phy_reg + USBPHY_CTRL);

	val = readl(phy_ctrl);

	if (val & USBPHY_CTRL_OTG_ID)
		return USB_INIT_DEVICE;
	else
		return USB_INIT_HOST;
}

#if defined(CONFIG_MX7ULP)
struct usbnc_regs {
	u32 ctrl1;
	u32 ctrl2;
	u32 reserve0[2];
	u32 hsic_ctrl;
};
#elif defined(CONFIG_IMX8)
struct usbnc_regs {
	u32 ctrl1;
	u32 ctrl2;
	u32 reserve1[10];
	u32 phy_cfg1;
	u32 phy_cfg2;
	u32 reserve2;
	u32 phy_status;
	u32 reserve3[4];
	u32 adp_cfg1;
	u32 adp_cfg2;
	u32 adp_status;
};
#else
/* Base address for this IP block is 0x02184800 */
struct usbnc_regs {
	u32	ctrl[4];	/* otg/host1-3 */
	u32	uh2_hsic_ctrl;
	u32	uh3_hsic_ctrl;
	u32	otg_phy_ctrl_0;
	u32	uh1_phy_ctrl_0;
};
#endif

#elif defined(CONFIG_USB_EHCI_MX7)
struct usbnc_regs {
	u32 ctrl1;
	u32 ctrl2;
	u32 reserve1[10];
	u32 phy_cfg1;
	u32 phy_cfg2;
	u32 reserve2;
	u32 phy_status;
	u32 reserve3[4];
	u32 adp_cfg1;
	u32 adp_cfg2;
	u32 adp_status;
};

static void usb_power_config(int index)
{
	struct usbnc_regs *usbnc = (struct usbnc_regs *)(ulong)(USB_BASE_ADDR +
			(0x10000 * index) + USBNC_OFFSET);
	void __iomem *phy_cfg2 = (void __iomem *)(&usbnc->phy_cfg2);

	/*
	 * Clear the ACAENB to enable usb_otg_id detection,
	 * otherwise it is the ACA detection enabled.
	 */
	clrbits_le32(phy_cfg2, USBNC_PHYCFG2_ACAENB);
}

int usb_phy_mode(int port)
{
	struct usbnc_regs *usbnc = (struct usbnc_regs *)(ulong)(USB_BASE_ADDR +
			(0x10000 * port) + USBNC_OFFSET);
	void __iomem *status = (void __iomem *)(&usbnc->phy_status);
	u32 val;

	val = readl(status);

	if (val & USBNC_PHYSTATUS_ID_DIG)
		return USB_INIT_DEVICE;
	else
		return USB_INIT_HOST;
}
#endif

static void ehci_mx6_powerup_fixup(struct ehci_ctrl *ctrl, uint32_t *status_reg,
				   uint32_t *reg)
{
	uint32_t result;
	int usec = 2000;

	mdelay(50);

	do {
		result = ehci_readl(status_reg);
		udelay(5);
		if (!(result & EHCI_PS_PR))
			break;
		usec--;
	} while (usec > 0);

	*reg = ehci_readl(status_reg);
}

static void usb_oc_config(int index)
{
#if defined(CONFIG_MX6)
	struct usbnc_regs *usbnc = (struct usbnc_regs *)(USB_BASE_ADDR +
			USB_OTHERREGS_OFFSET);
	void __iomem *ctrl = (void __iomem *)(&usbnc->ctrl[index]);
#elif defined(CONFIG_USB_EHCI_MX7) || defined(CONFIG_MX7ULP) || defined(CONFIG_IMX8)
	struct usbnc_regs *usbnc = (struct usbnc_regs *)(ulong)(USB_BASE_ADDR +
			(0x10000 * index) + USBNC_OFFSET);
	void __iomem *ctrl = (void __iomem *)(&usbnc->ctrl1);
#endif

#if CONFIG_MACH_TYPE == MACH_TYPE_MX6Q_ARM2
	/* mx6qarm2 seems to required a different setting*/
	clrbits_le32(ctrl, UCTRL_OVER_CUR_POL);
#else
	setbits_le32(ctrl, UCTRL_OVER_CUR_POL);
#endif

	setbits_le32(ctrl, UCTRL_OVER_CUR_DIS);

	/* Set power polarity to high active */
#ifdef CONFIG_MXC_USB_OTG_HACTIVE
	setbits_le32(ctrl, UCTRL_PWR_POL);
#else
	clrbits_le32(ctrl, UCTRL_PWR_POL);
#endif
}

/**
 * board_usb_phy_mode - override usb phy mode
 * @port:	usb host/otg port
 *
 * Target board specific, override usb_phy_mode.
 * When usb-otg is used as usb host port, iomux pad usb_otg_id can be
 * left disconnected in this case usb_phy_mode will not be able to identify
 * the phy mode that usb port is used.
 * Machine file overrides board_usb_phy_mode.
 *
 * Return: USB_INIT_DEVICE or USB_INIT_HOST
 */
int __weak board_usb_phy_mode(int port)
{
	return usb_phy_mode(port);
}

/**
 * board_ehci_hcd_init - set usb vbus voltage
 * @port:      usb otg port
 *
 * Target board specific, setup iomux pad to setup supply vbus voltage
 * for usb otg port. Machine board file overrides board_ehci_hcd_init
 *
 * Return: 0 Success
 */
int __weak board_ehci_hcd_init(int port)
{
	return 0;
}

/**
 * board_ehci_power - enables/disables usb vbus voltage
 * @port:      usb otg port
 * @on:        on/off vbus voltage
 *
 * Enables/disables supply vbus voltage for usb otg port.
 * Machine board file overrides board_ehci_power
 *
 * Return: 0 Success
 */
int __weak board_ehci_power(int port, int on)
{
	return 0;
}

int ehci_mx6_common_init(struct usb_ehci *ehci, int index)
{
	int ret;
	u32 portsc;

	enable_usboh3_clk(1);
	mdelay(1);

	portsc = readl(&ehci->portsc);
	if (portsc & PORT_PTS_PHCD) {
		debug("suspended: portsc %x, enabled it.\n", portsc);
		clrbits_le32(&ehci->portsc, PORT_PTS_PHCD);
	}

	/* Do board specific initialization */
	ret = board_ehci_hcd_init(index);
	if (ret)
		return ret;

	usb_power_config(index);
	usb_oc_config(index);

#if defined(CONFIG_MX6) || defined(CONFIG_MX7ULP) || defined(CONFIG_IMX8)
	usb_internal_phy_clock_gate(index, 1);
	usb_phy_enable(index, ehci);
#endif

	return 0;
}

#if !CONFIG_IS_ENABLED(DM_USB)
static const struct ehci_ops mx6_ehci_ops = {
	.powerup_fixup		= ehci_mx6_powerup_fixup,
};

int ehci_hcd_init(int index, enum usb_init_type init,
		struct ehci_hccr **hccr, struct ehci_hcor **hcor)
{
	enum usb_init_type type;
#if defined(CONFIG_MX6)
	u32 controller_spacing = 0x200;
#elif defined(CONFIG_USB_EHCI_MX7) || defined(CONFIG_MX7ULP) || defined(CONFIG_IMX8)
	u32 controller_spacing = 0x10000;
#endif
	struct usb_ehci *ehci = (struct usb_ehci *)(ulong)(USB_BASE_ADDR +
		(controller_spacing * index));
	int ret;

	if (index > 3)
		return -EINVAL;

#if defined(CONFIG_MX6)
	if (mx6_usb_fused((u32)ehci)) {
		printf("USB@0x%x is fused, disable it\n", (u32)ehci);
		return -ENODEV;
	}
#endif

	ret = ehci_mx6_common_init(ehci, index);
	if (ret)
		return ret;

	ehci_set_controller_priv(index, NULL, &mx6_ehci_ops);

	type = board_usb_phy_mode(index);

	if (hccr && hcor) {
		*hccr = (struct ehci_hccr *)((ulong)&ehci->caplength);
		*hcor = (struct ehci_hcor *)((ulong)*hccr +
				HC_LENGTH(ehci_readl(&(*hccr)->cr_capbase)));
	}

	if ((type == init) || (type == USB_INIT_DEVICE))
		board_ehci_power(index, (type == USB_INIT_DEVICE) ? 0 : 1);
	if (type != init)
		return -ENODEV;
	if (type == USB_INIT_DEVICE)
		return 0;

	setbits_le32(&ehci->usbmode, CM_HOST);
	writel(CONFIG_MXC_USB_PORTSC, &ehci->portsc);
	setbits_le32(&ehci->portsc, USB_EN);

	mdelay(10);

	return 0;
}

int ehci_hcd_stop(int index)
{
	return 0;
}
#else
#define  USB_INIT_UNKNOWN (USB_INIT_DEVICE + 1)

struct ehci_mx6_priv_data {
	struct ehci_ctrl ctrl;
	struct usb_ehci *ehci;
	struct udevice *vbus_supply;
	enum usb_init_type init_type;
	void *__iomem phy_base;
	int portnr;
};

static int mx6_init_after_reset(struct ehci_ctrl *dev)
{
	struct ehci_mx6_priv_data *priv = dev->priv;
	enum usb_init_type type = priv->init_type;
	struct usb_ehci *ehci = priv->ehci;
	int ret;

	ret = board_usb_init(priv->portnr, priv->init_type);
	if (ret) {
		printf("Failed to initialize board for USB\n");
		return ret;
	}

	ret = ehci_mx6_common_init(priv->ehci, priv->portnr);
	if (ret)
		return ret;

#if CONFIG_IS_ENABLED(DM_REGULATOR)
	if (priv->vbus_supply) {
		ret = regulator_set_enable(priv->vbus_supply,
					   (type == USB_INIT_DEVICE) ?
					   false : true);
		if (ret) {
			puts("Error enabling VBUS supply\n");
			return ret;
		}
	}
#endif

	if (type == USB_INIT_DEVICE)
		return 0;

	setbits_le32(&ehci->usbmode, CM_HOST);
	writel(CONFIG_MXC_USB_PORTSC, &ehci->portsc);
	setbits_le32(&ehci->portsc, USB_EN);

	mdelay(10);

	return 0;
}

static const struct ehci_ops mx6_ehci_ops = {
	.powerup_fixup		= ehci_mx6_powerup_fixup,
	.init_after_reset 	= mx6_init_after_reset
};

/**
 * board_ehci_usb_phy_mode - override usb phy mode
 * @port:	usb host/otg port
 *
 * Target board specific, override usb_phy_mode.
 * When usb-otg is used as usb host port, iomux pad usb_otg_id can be
 * left disconnected in this case usb_phy_mode will not be able to identify
 * the phy mode that usb port is used.
 * Machine file overrides board_usb_phy_mode.
 * When the extcon property is set in DTB, machine must provide this function, otherwise
 * it will default return HOST.
 *
 * Return: USB_INIT_DEVICE or USB_INIT_HOST
 */
int __weak board_ehci_usb_phy_mode(struct udevice *dev)
{
	return USB_INIT_HOST;
}

static int ehci_usb_phy_mode(struct udevice *dev)
{
	struct ehci_mx6_priv_data *priv = dev_get_priv(dev);
	void *__iomem phy_ctrl, *__iomem phy_status;
	u32 val;

	if (is_mx6() || is_mx7ulp() || is_imx8()) {
		phy_ctrl = (void __iomem *)(priv->phy_base + USBPHY_CTRL);
		val = readl(phy_ctrl);

		if (val & USBPHY_CTRL_OTG_ID)
			priv->init_type = USB_INIT_DEVICE;
		else
			priv->init_type = USB_INIT_HOST;
	} else if (is_mx7() || is_imx8mm() || is_imx8mn()) {
		phy_status = (void __iomem *)(priv->phy_base +
					      USBNC_PHY_STATUS_OFFSET);
		val = readl(phy_status);

		if (val & USBNC_PHYSTATUS_ID_DIG)
			priv->init_type = USB_INIT_DEVICE;
		else
			priv->init_type = USB_INIT_HOST;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int ehci_get_usb_phy(struct udevice *dev)
{
	struct ehci_mx6_priv_data *priv = dev_get_priv(dev);
	void *__iomem addr = (void *__iomem)devfdt_get_addr(dev);
	const void *blob = gd->fdt_blob;
	int offset = dev_of_offset(dev), phy_off;

	/*
	 * About fsl,usbphy, Refer to
	 * Documentation/devicetree/bindings/usb/ci-hdrc-usb2.txt.
	 */
	if (is_mx6() || is_mx7ulp() || is_imx8()) {
		phy_off = fdtdec_lookup_phandle(blob,
						offset,
						"fsl,usbphy");
		if (phy_off < 0)
			return -EINVAL;

		addr = (void __iomem *)fdtdec_get_addr(blob, phy_off,
						       "reg");
		if ((fdt_addr_t)addr == FDT_ADDR_T_NONE)
			return -EINVAL;

		/* Need to power on the PHY before access it */
#if CONFIG_IS_ENABLED(POWER_DOMAIN)
		struct udevice phy_dev;
		struct power_domain pd;

		phy_dev.node = offset_to_ofnode(phy_off);
		if (!power_domain_get(&phy_dev, &pd)) {
			if (power_domain_on(&pd))
				return -EINVAL;
		}
#endif
		priv->phy_base = addr;
	} else if (is_mx7() || is_imx8mm() || is_imx8mn()) {
		priv->phy_base = addr;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int ehci_usb_ofdata_to_platdata(struct udevice *dev)
{
	struct usb_platdata *plat = dev_get_platdata(dev);
	struct ehci_mx6_priv_data *priv = dev_get_priv(dev);
	const char *mode;
	const struct fdt_property *extcon;

	mode = fdt_getprop(gd->fdt_blob, dev_of_offset(dev), "dr_mode", NULL);
	if (mode) {
		if (strcmp(mode, "peripheral") == 0)
			priv->init_type = USB_INIT_DEVICE;
		else if (strcmp(mode, "host") == 0)
			priv->init_type = USB_INIT_HOST;
		else if (strcmp(mode, "otg") == 0)
			priv->init_type = USB_INIT_UNKNOWN;
		else
			return -EINVAL;
	} else {
		extcon = fdt_get_property(gd->fdt_blob, dev_of_offset(dev),
			"extcon", NULL);
		if (extcon)
			priv->init_type = board_ehci_usb_phy_mode(dev);
		else
			priv->init_type = USB_INIT_UNKNOWN;
	}

	if (priv->init_type != USB_INIT_UNKNOWN && priv->init_type != plat->init_type) {
		debug("Request USB type is %u, board forced type is %u\n",
			plat->init_type, priv->init_type);
		return -ENODEV;
	}

	return 0;
}

static int ehci_usb_probe(struct udevice *dev)
{
	struct usb_platdata *plat = dev_get_platdata(dev);
	struct usb_ehci *ehci = (struct usb_ehci *)devfdt_get_addr(dev);
	struct ehci_mx6_priv_data *priv = dev_get_priv(dev);
	enum usb_init_type type = plat->init_type;
	struct ehci_hccr *hccr;
	struct ehci_hcor *hcor;
	int ret;

#if defined(CONFIG_MX6)
	if (mx6_usb_fused((u32)ehci)) {
		printf("USB@0x%x is fused, disable it\n", (u32)ehci);
		return -ENODEV;
	}
#endif

	priv->ehci = ehci;
	priv->portnr = dev->seq;

	/* Init usb board level according to the requested init type */
	ret = board_usb_init(priv->portnr, type);
	if (ret) {
		printf("Failed to initialize board for USB\n");
		return ret;
	}

#if CONFIG_IS_ENABLED(DM_REGULATOR)
	ret = device_get_supply_regulator(dev, "vbus-supply",
					  &priv->vbus_supply);
	if (ret)
		debug("%s: No vbus supply\n", dev->name);
#endif

	ret = ehci_get_usb_phy(dev);
	if (ret) {
		debug("%s: fail to get USB PHY base\n", dev->name);
		return ret;
	}

	ret = ehci_mx6_common_init(ehci, priv->portnr);
	if (ret)
		return ret;

	/* If the init_type is unknown due to it is not forced in DTB, we use USB ID to detect */
	if (priv->init_type == USB_INIT_UNKNOWN) {
		ret = ehci_usb_phy_mode(dev);
		if (ret)
			return ret;
		if (priv->init_type != type)
			return -ENODEV;
	}

#if CONFIG_IS_ENABLED(DM_REGULATOR)
	if (priv->vbus_supply) {
		ret = regulator_set_enable(priv->vbus_supply,
					   (priv->init_type == USB_INIT_DEVICE) ?
					   false : true);
		if (ret) {
			puts("Error enabling VBUS supply\n");
			return ret;
		}
	}
#endif

	if (priv->init_type == USB_INIT_HOST) {
		setbits_le32(&ehci->usbmode, CM_HOST);
		writel(CONFIG_MXC_USB_PORTSC, &ehci->portsc);
		setbits_le32(&ehci->portsc, USB_EN);
	}

	mdelay(10);

	hccr = (struct ehci_hccr *)((ulong)&ehci->caplength);
	hcor = (struct ehci_hcor *)((ulong)hccr +
			HC_LENGTH(ehci_readl(&(hccr)->cr_capbase)));

	return ehci_register(dev, hccr, hcor, &mx6_ehci_ops, 0, priv->init_type);
}

int ehci_usb_remove(struct udevice *dev)
{
	struct ehci_mx6_priv_data *priv = dev_get_priv(dev);
	struct usb_platdata *plat = dev_get_platdata(dev);

	ehci_deregister(dev);

	plat->init_type = 0; /* Clean the requested usb type to host mode */

	return board_usb_cleanup(dev->seq, priv->init_type);
}

static const struct udevice_id mx6_usb_ids[] = {
	{ .compatible = "fsl,imx27-usb" },
	{ }
};

U_BOOT_DRIVER(usb_mx6) = {
	.name	= "ehci_mx6",
	.id	= UCLASS_USB,
	.of_match = mx6_usb_ids,
	.ofdata_to_platdata = ehci_usb_ofdata_to_platdata,
	.probe	= ehci_usb_probe,
	.remove = ehci_usb_remove,
	.ops	= &ehci_usb_ops,
	.platdata_auto_alloc_size = sizeof(struct usb_platdata),
	.priv_auto_alloc_size = sizeof(struct ehci_mx6_priv_data),
	.flags	= DM_FLAG_ALLOC_PRIV_DMA,
};
#endif
