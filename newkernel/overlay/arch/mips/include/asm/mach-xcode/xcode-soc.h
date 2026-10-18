/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ViXS XCode 4210 の SoC レジスタ窓のうち、複数のドライバが共有するもの(クロック停止・割り込みマスク・リセット)を
 * 触るための関数。各ドライバが自分のレジスタ(Ethernet ラッパ等)を直接 ioremap して使うほかは、これを通す。
 */
#ifndef __ASM_MACH_XCODE_SOC_H
#define __ASM_MACH_XCODE_SOC_H

#include <linux/types.h>

#define XC_CG_CLK_STOP0		0x012c
#define  XC_ETHR0CLK_STOP	0x00200000
#define  XC_ETHR1CLK_STOP	0x00400000

/* SoC の割り込みコントローラ(CPU の IP6 に集約): グループ0 のマスク。bit が立っていれば有効 */
#define XC_INT0_USBH		0x00000008
#define XC_INT0_ETH0		0x00000020
#define XC_INT0_ETH1		0x00000040
#define XC_INT0_PPC		0x00000080	/* メディアエンジン */
#define XC_INT0_SATA		0x00080000

/* SoC レジスタ(物理 0x1fff0000 からの窓)の読み-変更-書き。全ドライバ共通のロックで保護 */
void xcode_soc_rmw(unsigned int off, u32 clear, u32 set);
u32 xcode_soc_read(unsigned int off);
void xcode_soc_write(unsigned int off, u32 val);

/* グループ0 の割り込みを SoC 側で有効/無効にする */
void xcode_soc_irq_enable(u32 mask);
void xcode_soc_irq_disable(u32 mask);
u32 xcode_soc_irq_status(void);

/* 4段目ブートが渡した MAC アドレス(ethaddr=)。無ければ false */
bool xcode_get_boot_ethaddr(u8 *mac);

/* USB ホスト(SDUH)の EHCI/OHCI レジスタへの間接アクセス(arch/mips/xcode/usb.c)。off はコントローラ内のレジスタのオフセット、ohci=0 なら EHCI、1 なら OHCI */
u32 xcode_sduh_readl(unsigned long off, int ohci);
void xcode_sduh_writel(u32 val, unsigned long off, int ohci);
int xcode_sduh_hw_init(void);	/* USB ホスト(SDUH)の電源・クロック・PHY・リセット。EHCI/OHCI で共有、2回目以降は何もしない */

/* 前面LED(デバッグ用): bit0x100=LED_A、bit0x200=LED_B */
void xcode_led_set(u32 mask);

/* Ethernet 経由の早期コンソールを止める(本物の GMAC ドライバが使い始める前に呼ぶ) */
void xcode_earlycon_shutdown(void);

#endif
