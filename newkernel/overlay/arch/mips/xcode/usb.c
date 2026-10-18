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
