// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)の USB ホスト(SDUH)の OHCI。EHCI(ehci-xcode.c)のコンパニオンで、
 * low/full-speed の機器(キーボード・マウス・USB 1.1 のハブなど)を受け持つ。
 *
 * 初期化(電源・クロック・PHY・リセット)は EHCI と共有(arch/mips/xcode/usb.c の xcode_sduh_hw_init)で、先に動いたほうが行う。
 * OHCI のレジスタも SoC の間接アクセス経由(EOC ではなく OOC)。標準の ohci-hcd は、ohci.h の CONFIG_USB_OHCI_XCODE の分岐でその経路を使う。
 * ベンダーの drivers/usb/host/ohci-sduh.c の ohci_sduh_setup に基づく。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <asm/mach-xcode/xcode-soc.h>

#include "ohci.h"

#define DRIVER_DESC "OHCI XCode(nasne) driver"
static const char hcd_name[] = "ohci-xcode";

/* SDUH のレジスタ(窓の 0x2000 から) */
#define SDUH_MIPS_INT_MASK	0x2038
#define  SDUH_INT_OHCI		0x4

static int ohci_xcode_start(struct usb_hcd *hcd)
{
	struct ohci_hcd *ohci = hcd_to_ohci(hcd);

	ohci->num_ports = 2;	/* ベンダーと同じく固定(ルートハブのポート数 2) */
	return ohci_setup(hcd);
}

static const struct ohci_driver_overrides ohci_xcode_overrides = {
	.product_desc = "ViXS XCode OHCI",
	.reset = ohci_xcode_start,
};

static struct hc_driver __read_mostly ohci_xcode_hc_driver;

static int ohci_xcode_probe(struct platform_device *pdev)
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

	ret = xcode_sduh_hw_init();
	if (ret)
		return ret;
	xcode_soc_rmw(SDUH_MIPS_INT_MASK, 0, SDUH_INT_OHCI);	/* SDUH の MIPS 向け割り込みマスク(OHCI) */

	hcd = usb_create_hcd(&ohci_xcode_hc_driver, dev, dev_name(dev));
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

static void ohci_xcode_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_remove_hcd(hcd);
	xcode_soc_rmw(SDUH_MIPS_INT_MASK, SDUH_INT_OHCI, 0);
	usb_put_hcd(hcd);
}

static const struct of_device_id ohci_xcode_of_match[] = {
	{ .compatible = "vixs,xcode-ohci" },
	{ }
};
MODULE_DEVICE_TABLE(of, ohci_xcode_of_match);

static struct platform_driver ohci_xcode_driver = {
	.probe = ohci_xcode_probe,
	.remove = ohci_xcode_remove,
	.shutdown = usb_hcd_platform_shutdown,
	.driver = {
		.name = "ohci-xcode",
		.of_match_table = ohci_xcode_of_match,
	},
};

static int __init ohci_xcode_init(void)
{
	if (usb_disabled())
		return -ENODEV;
	ohci_init_driver(&ohci_xcode_hc_driver, &ohci_xcode_overrides);
	return platform_driver_register(&ohci_xcode_driver);
}
module_init(ohci_xcode_init);

static void __exit ohci_xcode_cleanup(void)
{
	platform_driver_unregister(&ohci_xcode_driver);
}
module_exit(ohci_xcode_cleanup);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
