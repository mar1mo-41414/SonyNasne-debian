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

/* SDUH のレジスタ(窓の 0x2000 から) */
#define SDUH_MIPS_INT_MASK	0x2038
#define  SDUH_INT_EHCI		0x2

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

	ret = xcode_sduh_hw_init();
	if (ret)
		return ret;
	xcode_soc_rmw(SDUH_MIPS_INT_MASK, 0, SDUH_INT_EHCI);	/* SDUH の MIPS 向け割り込みマスク(EHCI) */

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
