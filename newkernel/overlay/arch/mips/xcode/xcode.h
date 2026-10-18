/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ViXS XCode 4210 "Viper" のレジスタ(必要なものだけ)。
 * 値は SIE 公開の GPL ソース(arch/mips/include/asm/mach-viper/viper.h)から。
 */
#ifndef __XCODE_H
#define __XCODE_H

#include <linux/io.h>

#define XCODE_MMREG_PHYS	0x1fff0000	/* SoC レジスタ窓(64KB)。KSEG1 では 0xbfff0000 */
#define XCODE_MMREG_SIZE	0x10000
#define XCODE_MMREG		((void __iomem *)0xbfff0000)	/* KSEG1(キャッシュなし)の直接マップ */

#define XC_CG_RESET_REG		0x0000
#define  XC_DMA_RESET_MASK	0x00000002
#define  XC_SATA_RESET_MASK	0x00000080
#define  XC_PROC2_RESET_MASK	0x00100000
#define  XC_USBH_RESET_MASK	0x00800000
#define  XC_ETH0_RESET_MASK	0x01000000
#define  XC_ETH1_RESET_MASK	0x02000000
#define XC_CG_SYSTEM_CNT	0x00f8		/* 27MHz/16 のカウンタ */
#define XC_CG_DUMMY_REG		0x00fc		/* 読み出すとボードID(0x1202 など) */
#define XC_MIPS5_INTERRUPT	0x04f4
#define XC_MIPS5_INTERRUPT_MASK	0x04f8
#define XC_MIPS6_INTERRUPT	0x04fc
#define XC_MIPS6_INTERRUPT_MASK	0x0500
#define XC_MIPS7_INTERRUPT	0x0504
#define XC_MIPS7_INTERRUPT_MASK	0x0508
#define XC_MIPS5_INTERRUPT1_MASK 0x0540
#define XC_MIPS6_INTERRUPT1_MASK 0x0548
#define XC_MIPS7_INTERRUPT1_MASK 0x0550
/* Ethernet の cold 初期化(ベンダーの synopGMAC_plat.c の turnon_eth_clk / plat_eth_init と同じ) */
#define XC_CG_CLK_STOP0		0x012c
#define  XC_ETHR125CLK_STOP_M	0x00800000
#define  XC_ETHR0CLK_STOP_M	0x00200000
#define  XC_ETHR1CLK_STOP_M	0x00400000
#define XC_CG_BLK_CLK_STOP0	0x0130
#define  XC_ETH0_MCLK_STOP_M	0x00100000
#define  XC_ETH0_SCLK_STOP_M	0x00200000
#define  XC_ETH1_MCLK_STOP_M	0x00400000
#define  XC_ETH1_SCLK_STOP_M	0x00800000
#define XC_LOWPOWER_HW_MUTEX	0x0454		/* MIPS_INTERLOCK5。ID 2 = Ethernet ドライバ */
#define XC_LOWPOWER_ETH_ID	2
#define XC_ETH_CTRL_REG		0x2810
#define XC_ETH_CTRL_REG1	0x2c10
#define  XC_ETH_CTRL_RESETN	0x00000001
#define  XC_ETH_TX_NEGEDGE	0x00000002
#define  XC_ETH_GTXCLK_SOURCE	0x00002000
#define XC_GPIO_DEDICATED_OUT	0x1100
#define XC_GPIO_DEDICATED_OUTEN	0x1104
#define  XC_GPIO_DEDICATED3	0x00000008	/* PHY のリセット線 */
#define XC_GPIO_A_CTRL		0xfc0c
#define XC_GPIO_B_CTRL		0xfc1c
#define  XC_GPIO_MODE_SEL	0x80000000	/* 落とすと、共有ピンが GPIO でなく Ethernet になる */
#define XC_ETH_DUMMY_REG	0x2820		/* 4段目ブートが、起動の引数を渡したときに 0x55aabeef を書く */
#define  XC_BOOT_MAGIC		0x55aabeef

/* GPIO ポート10(前面LED): bit0x100=LED_A、bit0x200=LED_B。High で点灯 */
#define XC_GPIO_PORT10_OUT	0xfc64
#define XC_GPIO_PORT10_EN	0xfc68
#define  XC_LED_A		0x100
#define  XC_LED_B		0x200

static inline u32 xc_rd(unsigned int off)
{
	return __raw_readl(XCODE_MMREG + off);
}

static inline void xc_wr(u32 v, unsigned int off)
{
	__raw_writel(v, XCODE_MMREG + off);
}

void xcode_led_set(u32 mask);		/* LED_A/LED_B の点灯状態(ビットマスク)を設定 */
void xcode_led_stage(int n);		/* 起動段階 n(0〜15)を LED_B=ビット、LED_A=クロックの4ビットで出す(XCODE_LED_DEBUG のときだけ) */
void xcode_led_heartbeat_tick(void);	/* 割り込みの届いている印。plat_irq_dispatch から呼ぶ */
void xcode_led_panic(void);
void xcode_earlycon_init(void);	/* Ethernet 経由の早期コンソール(earlycon.c) */		/* panic: LED_B を高速点滅(戻らない) */

#endif
