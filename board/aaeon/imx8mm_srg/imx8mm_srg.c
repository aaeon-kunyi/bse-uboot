// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2018-2019 NXP
 * Copyright 2020 AAEON
 */

#include <common.h>
#include <malloc.h>
#include <errno.h>
#include <asm/io.h>
#include <miiphy.h>
#include <netdev.h>
#include <asm/mach-imx/iomux-v3.h>
#include <asm-generic/gpio.h>
#include <fsl_esdhc.h>
#include <mmc.h>
#include <asm/arch/imx8mm_pins.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/gpio.h>
#include <asm/mach-imx/mxc_i2c.h>
#include <asm/arch/clock.h>
#include <spl.h>
#include <asm/mach-imx/dma.h>
#include <power/pmic.h>
#include <power/bd71837.h>
#include <usb.h>

DECLARE_GLOBAL_DATA_PTR;

#define UART_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL1)
#define WDOG_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_ODE | PAD_CTL_PUE | PAD_CTL_PE)

static iomux_v3_cfg_t const uart_pads[] = {
	IMX8MM_PAD_UART2_RXD_UART2_RX | MUX_PAD_CTRL(UART_PAD_CTRL),
	IMX8MM_PAD_UART2_TXD_UART2_TX | MUX_PAD_CTRL(UART_PAD_CTRL),
};

static iomux_v3_cfg_t const wdog_pads[] = {
	IMX8MM_PAD_GPIO1_IO02_WDOG1_WDOG_B  | MUX_PAD_CTRL(WDOG_PAD_CTRL),
};

#ifdef CONFIG_FSL_FSPI
int board_qspi_init(void)
{
	set_clk_qspi();

	return 0;
}
#endif

#ifdef CONFIG_NAND_MXS
#ifdef CONFIG_SPL_BUILD
#define NAND_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL2 | PAD_CTL_HYS)
#define NAND_PAD_READY0_CTRL (PAD_CTL_DSE6 | PAD_CTL_FSEL2 | PAD_CTL_PUE)
static iomux_v3_cfg_t const gpmi_pads[] = {
	IMX8MM_PAD_NAND_ALE_RAWNAND_ALE | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_CE0_B_RAWNAND_CE0_B | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_CE1_B_RAWNAND_CE1_B | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_CLE_RAWNAND_CLE | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA00_RAWNAND_DATA00 | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA01_RAWNAND_DATA01 | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA02_RAWNAND_DATA02 | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA03_RAWNAND_DATA03 | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA04_RAWNAND_DATA04 | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA05_RAWNAND_DATA05	| MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA06_RAWNAND_DATA06	| MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_DATA07_RAWNAND_DATA07	| MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_RE_B_RAWNAND_RE_B | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_READY_B_RAWNAND_READY_B | MUX_PAD_CTRL(NAND_PAD_READY0_CTRL),
	IMX8MM_PAD_NAND_WE_B_RAWNAND_WE_B | MUX_PAD_CTRL(NAND_PAD_CTRL),
	IMX8MM_PAD_NAND_WP_B_RAWNAND_WP_B | MUX_PAD_CTRL(NAND_PAD_CTRL),
};
#endif

static void setup_gpmi_nand(void)
{
#ifdef CONFIG_SPL_BUILD
	imx_iomux_v3_setup_multiple_pads(gpmi_pads, ARRAY_SIZE(gpmi_pads));
#endif

	init_nand_clk();
}
#endif


#define ECSPI_PAD_CTRL	(PAD_CTL_DSE6 | PAD_CTL_FSEL2 | PAD_CTL_HYS)
static iomux_v3_cfg_t const tpm_spi_pads[] = {
	IMX8MM_PAD_ECSPI2_SCLK_ECSPI2_SCLK | MUX_PAD_CTRL(ECSPI_PAD_CTRL),
	IMX8MM_PAD_ECSPI2_MOSI_ECSPI2_MOSI | MUX_PAD_CTRL(ECSPI_PAD_CTRL),
	IMX8MM_PAD_ECSPI2_MISO_ECSPI2_MISO | MUX_PAD_CTRL(ECSPI_PAD_CTRL),
	IMX8MM_PAD_ECSPI2_SS0_ECSPI2_SS0 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

#define TPM_RST_PAD IMX_GPIO_NR(3, 24)
static iomux_v3_cfg_t const tpm_rst_pads[] = {
	IMX8MM_PAD_SAI5_RXD3_GPIO3_IO24 | MUX_PAD_CTRL(NO_PAD_CTRL),
};


#define USB1_VBUS_PAD	 IMX_GPIO_NR(1, 3)
#define USB2_VBUS_PAD	 IMX_GPIO_NR(1, 4)
#define USB3_VBUS_PAD	 IMX_GPIO_NR(1, 5)
#define USB4_VBUS_PAD	 IMX_GPIO_NR(1, 6)
#define USB5_VBUS_PAD	 IMX_GPIO_NR(1, 7)
#define USB6_VBUS_PAD	 IMX_GPIO_NR(1, 8)
static iomux_v3_cfg_t const vbus_pads[] = {
	IMX8MM_PAD_GPIO1_IO03_GPIO1_IO3 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MM_PAD_GPIO1_IO04_GPIO1_IO4 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MM_PAD_GPIO1_IO05_GPIO1_IO5 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MM_PAD_GPIO1_IO06_GPIO1_IO6 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MM_PAD_GPIO1_IO07_GPIO1_IO7 | MUX_PAD_CTRL(NO_PAD_CTRL),
	IMX8MM_PAD_GPIO1_IO08_GPIO1_IO8 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

int board_early_init_f(void)
{
	struct wdog_regs *wdog = (struct wdog_regs *)WDOG1_BASE_ADDR;

	imx_iomux_v3_setup_multiple_pads(wdog_pads, ARRAY_SIZE(wdog_pads));

	set_wdog_reset(wdog);

	imx_iomux_v3_setup_multiple_pads(uart_pads, ARRAY_SIZE(uart_pads));

	init_uart_clk(1);

#ifdef CONFIG_NAND_MXS
	setup_gpmi_nand(); /* SPL will call the board_early_init_f */
#endif
	return 0;
}

int dram_init(void)
{
	/* rom_pointer[1] contains the size of TEE occupies */
	if (rom_pointer[1])
		gd->ram_size = PHYS_SDRAM_SIZE - rom_pointer[1];
	else
		gd->ram_size = PHYS_SDRAM_SIZE;

#if CONFIG_NR_DRAM_BANKS == 2
	gd->ram_size += PHYS_SDRAM_2_SIZE;
#endif

	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = PHYS_SDRAM;
	if (rom_pointer[1])
		gd->bd->bi_dram[0].size = PHYS_SDRAM_SIZE -rom_pointer[1];
	else
		gd->bd->bi_dram[0].size = PHYS_SDRAM_SIZE;

#if CONFIG_NR_DRAM_BANKS == 2
	gd->bd->bi_dram[1].start = PHYS_SDRAM_2;
	gd->bd->bi_dram[1].size = PHYS_SDRAM_2_SIZE;
#endif

	return 0;
}

#define FEC_RST_PAD IMX_GPIO_NR(4, 1)
static iomux_v3_cfg_t const fec1_rst_pads[] = {
	IMX8MM_PAD_SAI1_RXC_GPIO4_IO1 | MUX_PAD_CTRL(NO_PAD_CTRL),
};

static void srg_board_reset(void)
{
	imx_iomux_v3_setup_multiple_pads(fec1_rst_pads,
					 ARRAY_SIZE(fec1_rst_pads));
	imx_iomux_v3_setup_multiple_pads(tpm_spi_pads,
					ARRAY_SIZE(tpm_spi_pads));
	imx_iomux_v3_setup_multiple_pads(tpm_rst_pads,
					 ARRAY_SIZE(tpm_rst_pads));
	imx_iomux_v3_setup_multiple_pads(vbus_pads,
					 ARRAY_SIZE(vbus_pads));

	gpio_request(FEC_RST_PAD, "fec1_rst");
	gpio_direction_output(FEC_RST_PAD, 0);
	gpio_request(TPM_RST_PAD, "tpm_rst");
	gpio_direction_output(TPM_RST_PAD, 0);
	udelay(500);
	gpio_direction_output(FEC_RST_PAD, 1);
	gpio_direction_output(TPM_RST_PAD, 1);
	gpio_free(TPM_RST_PAD);
}

#ifdef CONFIG_FEC_MXC
static int setup_fec(void)
{
	struct iomuxc_gpr_base_regs *gpr =
		(struct iomuxc_gpr_base_regs *)IOMUXC_GPR_BASE_ADDR;

	/* Use 125M anatop REF_CLK1 for ENET1, not from external */
	clrsetbits_le32(&gpr->gpr[1],
			IOMUXC_GPR_GPR1_GPR_ENET1_TX_CLK_SEL_MASK, 0);
	return set_clk_enet(ENET_125MHZ);
}

int board_phy_config(struct phy_device *phydev)
{
	/* enable rgmii rxc skew and phy mode select to RGMII copper */
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x1f);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x8);

	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x00);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x82ee);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1d, 0x05);
	phy_write(phydev, MDIO_DEVAD_NONE, 0x1e, 0x100);

	if (phydev->drv->config)
		phydev->drv->config(phydev);
	return 0;
}
#endif


int board_usb_init(int index, enum usb_init_type init)
{
	int ret = 0;
	debug("board_usb_init %d, type %d\n", index, init);

#ifdef CONFIG_SPL_BUILD
	imx8m_usb_power(index, true);
#endif

	return ret;
}

int board_usb_cleanup(int index, enum usb_init_type init)
{
	int ret = 0;

	debug("board_usb_cleanup %d, type %d\n", index, init);

	imx8m_usb_power(index, false);

	return ret;
}

int board_ehci_usb_phy_mode(struct udevice *dev)
{
	return USB_INIT_DEVICE;
}

int board_init(void)
{
	srg_board_reset();
#ifdef CONFIG_FEC_MXC
	setup_fec();
#endif

#ifdef CONFIG_FSL_FSPI
	board_qspi_init();
#endif
	return 0;
}

int board_late_init(void)
{
#ifdef CONFIG_ENV_IS_IN_MMC
#ifndef CONFIG_SPL_BUILD
	board_late_mmc_env_init();
#endif
#endif

	return 0;
}

#ifdef CONFIG_FSL_FASTBOOT
#ifdef CONFIG_ANDROID_RECOVERY
int is_recovery_key_pressing(void)
{
	return 0; /*TODO*/
}
#endif /*CONFIG_ANDROID_RECOVERY*/
#endif /*CONFIG_FSL_FASTBOOT*/

phys_size_t get_effective_memsize(void)
{
	if (rom_pointer[1])
		return (PHYS_SDRAM_SIZE - rom_pointer[1]);
	else
		return PHYS_SDRAM_SIZE;
}
