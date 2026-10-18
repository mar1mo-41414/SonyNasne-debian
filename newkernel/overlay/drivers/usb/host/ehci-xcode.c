// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)の USB ホスト(SDUH)の EHCI。
 *
 * 初期化(VBUS の電源・クロック・PHY・リセット)は、ベンダーの drivers/usb/host/ehci-sduh.c の ehci_sduh_setup に基づく。
 * EHCI のレジスタは、メモリ空間ではなく、SoC の間接アクセス(arch/mips/xcode/usb.c)経由。標準の ehci-hcd は、
 * ehci.h の CONFIG_USB_EHCI_XCODE の分岐で、その経路を使う。hcd->regs は NULL のままで、レジスタ「アドレス」は、コントローラ内のオフセットそのもの。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <asm/mach-xcode/xcode-soc.h>

#include "ehci.h"

#define DRIVER_DESC "EHCI XCode(nasne) driver"
static const char hcd_name[] = "ehci-xcode";

/* SoC レジスタ(オフセット。窓 0x1fff0000 から) */
#define CG_RESET_REG		0x0000
#define  USBD_RESET_MASK	0x04000000
#define  USBH_RESET_MASK	0x00800000
#define CG1_CLK_STOP0		0x0928
#define  UCLK_STOP_MASK		0x00008000
#define CG1_BLK_CLK_STOP0	0x0930
#define  USBD_MCLK_STOP_MASK	0x00000080
#define  USBH_MCLK_STOP_MASK	0x00000100
#define  USBH_SCLK_STOP_MASK	0x00000200
#define GPIO_J_OUT		0xfc94
#define GPIO_J_OE		0xfc98
#define GPIO_J_CTRL		0xfc9c
#define  GPIO_J_MODE_SEL_BIT	25
#define  USB_VBUS_PINS		0x3000		/* GPIO_J の 12・13: USB ポートの電源(VBUS)の出力 */
/* SDUH のレジスタ(窓の 0x2000 から) */
#define SDUH_CONTROL		0x2020
#define  SDUH_SOFT_RESET	0x00000004
#define SDUH_PHY_CONTROL	0x2030
#define  SDUH_PHY_POR		0x00000010
#define SDUH_MIPS_INT_MASK	0x2038
#define USBD_PHY_CLK_CTRL	0x242c
#define  PHY_REFCLKSEL_SHIFT	2
#define  PHY_REFCLKDIV_SHIFT	0
#define USBD_PHY_CTRL		0x2430
#define  PHY_PORT0_H_D_MASK	0x00000001
#define  PHY_PORT1_H_D_MASK	0x00000002

static void sduh_hw_init(void)
{
	/* USB ポートの電源(GPIO_J[13:12]=出力・High)。ほかの基板(ID 0x1701)は別のピンだが、nasne(0x1202 系)はこちら */
	xcode_soc_rmw(GPIO_J_CTRL, 0, 1u << GPIO_J_MODE_SEL_BIT);
	udelay(1000);
	xcode_soc_rmw(GPIO_J_OE, 0, USB_VBUS_PINS);
	udelay(1000);
	xcode_soc_rmw(GPIO_J_OUT, 0, USB_VBUS_PINS);
	udelay(1000);

	/* クロックを有効に */
	xcode_soc_rmw(CG1_CLK_STOP0, UCLK_STOP_MASK, 0);
	xcode_soc_rmw(CG1_BLK_CLK_STOP0, USBD_MCLK_STOP_MASK, 0);
	xcode_soc_rmw(CG1_BLK_CLK_STOP0, USBH_MCLK_STOP_MASK, 0);
	xcode_soc_rmw(CG1_BLK_CLK_STOP0, USBH_SCLK_STOP_MASK, 0);

	/* USB デバイス側のリセットを外す */
	xcode_soc_rmw(CG_RESET_REG, USBD_RESET_MASK, 0);
	udelay(1000);

	/*
	 * PHY のクロック: 内部 PLL(REFCLKSEL=2)、基準 48MHz(REFCLKDIV=2)。ベンダーは、内部 PLL のとき、UCLK_STOP のビットを
	 * CG1_BLK_CLK_STOP0 側に立てている(レジスタが違うが、そのまま移植)。
	 */
	xcode_soc_rmw(CG1_BLK_CLK_STOP0, 0, UCLK_STOP_MASK);
	xcode_soc_write(USBD_PHY_CLK_CTRL, (2 << PHY_REFCLKSEL_SHIFT) | (2 << PHY_REFCLKDIV_SHIFT));
	udelay(1000);

	/* 両ポートをホストに */
	xcode_soc_rmw(USBD_PHY_CTRL, PHY_PORT0_H_D_MASK | PHY_PORT1_H_D_MASK, PHY_PORT0_H_D_MASK | PHY_PORT1_H_D_MASK);
	udelay(1000);

	/* USB ホストのブロックをリセット */
	xcode_soc_rmw(CG_RESET_REG, 0, USBH_RESET_MASK);
	udelay(1000);
	xcode_soc_rmw(CG_RESET_REG, USBH_RESET_MASK, 0);
	udelay(1000);

	/* SDUH のソフトリセットと PHY の POR */
	xcode_soc_rmw(SDUH_CONTROL, 0, SDUH_SOFT_RESET);
	xcode_soc_rmw(SDUH_PHY_CONTROL, 0, SDUH_PHY_POR);
	udelay(1000);
	xcode_soc_rmw(SDUH_PHY_CONTROL, SDUH_PHY_POR, 0);
	udelay(1000);
	xcode_soc_rmw(SDUH_CONTROL, SDUH_SOFT_RESET, 0);
	udelay(1000);

	/* SoC の割り込みコントローラで USB ホストの割り込みを有効に */
	xcode_soc_irq_enable(XC_INT0_USBH);

	/* PHY のリセットのパルス */
	xcode_soc_rmw(SDUH_PHY_CONTROL, 0, 0x10);
	udelay(1000);
	xcode_soc_rmw(SDUH_PHY_CONTROL, 0x10, 0);
	udelay(1000);

	/* SDUH の MIPS 向け割り込みマスク */
	xcode_soc_rmw(SDUH_MIPS_INT_MASK, 0, 0x2);
}

static int ehci_xcode_reset(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	ehci->caps = hcd->regs;		/* NULL。レジスタのアドレス = コントローラ内のオフセット */
	return ehci_setup(hcd);
}

static const struct ehci_driver_overrides ehci_xcode_overrides = {
	.reset = ehci_xcode_reset,
};

static struct hc_driver __read_mostly ehci_xcode_hc_driver;

static int ehci_xcode_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct usb_hcd *hcd;
	int irq, ret;

	if (usb_disabled())
		return -ENODEV;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	sduh_hw_init();

	hcd = usb_create_hcd(&ehci_xcode_hc_driver, dev, dev_name(dev));
	if (!hcd)
		return -ENOMEM;
	hcd->regs = NULL;
	hcd->rsrc_start = 0x1fff2000;
	hcd->rsrc_len = 0x40;
	platform_set_drvdata(pdev, hcd);
	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret) {
		usb_put_hcd(hcd);
		return ret;
	}
	device_wakeup_enable(hcd->self.controller);
	return 0;
}

static void ehci_xcode_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
}

static const struct of_device_id ehci_xcode_of_match[] = {
	{ .compatible = "vixs,xcode-ehci" },
	{ }
};
MODULE_DEVICE_TABLE(of, ehci_xcode_of_match);

static struct platform_driver ehci_xcode_driver = {
	.probe = ehci_xcode_probe,
	.remove = ehci_xcode_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver = {
		.name = "ehci-xcode",
		.of_match_table = ehci_xcode_of_match,
	},
};

static int __init ehci_xcode_init(void)
{
	if (usb_disabled())
		return -ENODEV;
	ehci_init_driver(&ehci_xcode_hc_driver, &ehci_xcode_overrides);
	return platform_driver_register(&ehci_xcode_driver);
}
module_init(ehci_xcode_init);

static void __exit ehci_xcode_cleanup(void)
{
	platform_driver_unregister(&ehci_xcode_driver);
}
module_exit(ehci_xcode_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
