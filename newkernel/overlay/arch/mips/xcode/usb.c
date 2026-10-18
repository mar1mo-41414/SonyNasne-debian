// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)の USB ホスト(SDUH)の、EHCI/OHCI レジスタへの「間接アクセス」。
 *
 * この SoC の USB ホストは、EHCI/OHCI のレジスタがメモリ空間に見えず、SoC レジスタ窓の 0x2000 以降の少数のレジスタ経由で読み書きする:
 *   0x2000 IAS(bit0=ビジー、bit1=エラー)  0x2004 EOC(EHCI のアドレス)  0x200c OOC(OHCI のアドレス)
 *   0x2010 WDATA  0x2014 RDATA
 *   読む: EOC(または OOC)へレジスタのオフセットを書く → ビジーが落ちるのを待つ → RDATA を読む
 *   書く: WDATA へ値 → EOC(または OOC)へ「オフセット | 0x80000000」→ ビジーが落ちるのを待つ
 * ベンダーのドライバ(drivers/usb/core/sduh-hcd.c)の SDUH_readl/SDUH_writel と同じ。標準の ehci-hcd の ehci_readl/ehci_writel(ehci.h の
 * CONFIG_USB_EHCI_XCODE の分岐)が、これを呼ぶ。割り込みハンドラからも呼ばれるので、raw のスピンロック。
 */
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <asm/mach-xcode/xcode-soc.h>
#include "xcode.h"

#define SDUH_IAS	0x2000
#define SDUH_EOC	0x2004
#define SDUH_OOC	0x200c
#define SDUH_WDATA	0x2010
#define SDUH_RDATA	0x2014
#define SDUH_BUSY	0x1
#define SDUH_ERR	0x2
#define SDUH_WR		0x80000000

static DEFINE_RAW_SPINLOCK(sduh_lock);

static int sduh_wait(u32 *st)
{
	int n = 100000;		/* ビジーは数百ナノ秒で落ちる。ハングを避ける上限 */

	while ((*st = xc_rd(SDUH_IAS)) & SDUH_BUSY)
		if (!--n)
			return -1;
	return 0;
}

u32 xcode_sduh_readl(unsigned long off, int ohci)
{
	unsigned long flags;
	u32 st, v = 0;

	raw_spin_lock_irqsave(&sduh_lock, flags);
	if (!sduh_wait(&st)) {
		xc_wr(off, ohci ? SDUH_OOC : SDUH_EOC);
		if (!sduh_wait(&st) && !(st & SDUH_ERR))
			v = xc_rd(SDUH_RDATA);
	}
	raw_spin_unlock_irqrestore(&sduh_lock, flags);
	return v;
}
EXPORT_SYMBOL_GPL(xcode_sduh_readl);

void xcode_sduh_writel(u32 val, unsigned long off, int ohci)
{
	unsigned long flags;
	u32 st;

	raw_spin_lock_irqsave(&sduh_lock, flags);
	if (!sduh_wait(&st)) {
		xc_wr(val, SDUH_WDATA);
		xc_wr(off | SDUH_WR, ohci ? SDUH_OOC : SDUH_EOC);
		sduh_wait(&st);
	}
	raw_spin_unlock_irqrestore(&sduh_lock, flags);
}
EXPORT_SYMBOL_GPL(xcode_sduh_writel);

/* USB ホストの初期化に使う SoC レジスタ(オフセット。窓 0x1fff0000 から) */
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
#define USBD_PHY_CLK_CTRL	0x242c
#define  PHY_REFCLKSEL_SHIFT	2
#define  PHY_REFCLKDIV_SHIFT	0
#define USBD_PHY_CTRL		0x2430
#define  PHY_PORT0_H_D_MASK	0x00000001
#define  PHY_PORT1_H_D_MASK	0x00000002

static void __sduh_hw_init(void)
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
}


/*
 * EHCI と OHCI が同じ SDUH を共有するので、最初に呼んだほうが初期化し、あとは何もしない
 * (ベンダーの SDUH_HCD_REF_CNT == 0 の分岐に相当)。割り込みマスクの自分のビットは、各ドライバが立てる。
 */
static DEFINE_MUTEX(sduh_init_lock);
static bool sduh_inited;

int xcode_sduh_hw_init(void)
{
	mutex_lock(&sduh_init_lock);
	if (!sduh_inited) {
		__sduh_hw_init();
		sduh_inited = true;
	}
	mutex_unlock(&sduh_init_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(xcode_sduh_hw_init);
