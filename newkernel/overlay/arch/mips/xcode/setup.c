// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210 "Viper"(Sony nasne): メモリ・DT・キャッシュまわり
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/of_fdt.h>
#include <linux/memblock.h>
#include <asm/mipsregs.h>
#include <asm/bcache.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/reboot.h>
#include <asm/bootinfo.h>

#include "xcode.h"

/*
 * SoC 側に、CPU の L1 の外のキャッシュ(PFB: 先読みバッファ)がある。DMA の前後でこれを無効化する必要がある
 * (ベンダーの arch/mips/xcode/viper/sc-viper.c と同じ)。CPU の L1 の処理(cache 命令)は標準のコードがやる。
 */
#define XC_PROC5_PFB_INVALID_ALL	0xb5b0
#define XC_PROC5_PFB_CFG		0xb5a0
static u32 xcode_pfb_cfg_before;

static int __init xcode_pfb_report(void)
{
	pr_info("xcode: PFB_CFG 起動時=%08x 現在=%08x (先読み %s)\n", xcode_pfb_cfg_before, xc_rd(XC_PROC5_PFB_CFG),
#ifdef XCODE_DISABLE_PFB
		"停止"
#else
		"そのまま"
#endif
	);
	return 0;
}
late_initcall(xcode_pfb_report);

static void xcode_pfb_invalidate(unsigned long addr, unsigned long size)
{
	unsigned long flags;

	local_irq_save(flags);
	(void)__raw_readl((void __iomem *)0xb0000000);	/* 保留中の書き込みを押し出す(ベンダーも _text を読んでいた) */
	__asm__ __volatile__("sync" ::: "memory");
	xc_wr(0x10052006, XC_PROC5_PFB_INVALID_ALL);
	(void)xc_rd(XC_PROC5_PFB_INVALID_ALL);
	local_irq_restore(flags);
}

static void xcode_sc_noop(void)
{
}

static struct bcache_ops xcode_sc_ops = {
	.bc_enable	= xcode_sc_noop,
	.bc_disable	= xcode_sc_noop,
	.bc_wback_inv	= xcode_pfb_invalidate,
	.bc_inv		= xcode_pfb_invalidate,
};

static void xcode_pulse_reset(u32 mask)
{
	u32 v = xc_rd(XC_CG_RESET_REG);

	xc_wr(v | mask, XC_CG_RESET_REG);
	xc_wr(xc_rd(XC_CG_RESET_REG) & ~mask, XC_CG_RESET_REG);
}

/* CP0 count(約256MHz)で待つ。plat_mem_setup の時点では、mdelay の較正(loops_per_jiffy)がまだ無い */
static void xcode_early_delay_ms(unsigned int ms)
{
	unsigned int t0 = read_c0_count();

	while ((unsigned int)(read_c0_count() - t0) < ms * 256000U)
		;
}

/*
 * Ethernet の cold 初期化。kexec では旧カーネルが済ませているので不要だったが、SPI から cold で起動すると、PHY(RTL8211EG)がリセットのまま、
 * クロックが止まったまま、共有ピンが GPIO のままで、ネットワークが沈黙する。ベンダーの synopGMAC_plat.c の
 * turnon_eth_clk() と plat_eth_init() と同じ手順(ボードID 0x1201 以外 = PHY のリセットは専用 GPIO3、GETH 構成)。
 */
static void __init xcode_eth_cold_init(void)
{
	u32 boardid = xc_rd(XC_CG_DUMMY_REG) & 0xffff;
	unsigned int tries = 0;
	u32 v;

	/* ---- turnon_eth_clk: 低電力レジスタのミューテックスを取って、Ethernet のクロックを有効にする ---- */
	xc_wr(XC_LOWPOWER_ETH_ID, XC_LOWPOWER_HW_MUTEX);
	while (((xc_rd(XC_LOWPOWER_HW_MUTEX) & 0x0f) != XC_LOWPOWER_ETH_ID) && tries++ < 1000) {
		xcode_early_delay_ms(1);
		xc_wr(XC_LOWPOWER_ETH_ID, XC_LOWPOWER_HW_MUTEX);
	}
	xc_wr(xc_rd(XC_CG_CLK_STOP0) & ~XC_ETHR125CLK_STOP_M, XC_CG_CLK_STOP0);			/* ギガビット */
	xc_wr(xc_rd(XC_CG_CLK_STOP0) | XC_ETHR0CLK_STOP_M, XC_CG_CLK_STOP0);			/* ETHERNET0 */
	xc_wr(xc_rd(XC_CG_BLK_CLK_STOP0) & ~XC_ETH0_MCLK_STOP_M, XC_CG_BLK_CLK_STOP0);
	xc_wr(xc_rd(XC_CG_BLK_CLK_STOP0) & ~XC_ETH0_SCLK_STOP_M, XC_CG_BLK_CLK_STOP0);
	xc_wr(xc_rd(XC_CG_CLK_STOP0) | XC_ETHR1CLK_STOP_M, XC_CG_CLK_STOP0);			/* ETHERNET1 */
	xc_wr(xc_rd(XC_CG_BLK_CLK_STOP0) & ~XC_ETH1_MCLK_STOP_M, XC_CG_BLK_CLK_STOP0);
	xc_wr(xc_rd(XC_CG_BLK_CLK_STOP0) & ~XC_ETH1_SCLK_STOP_M, XC_CG_BLK_CLK_STOP0);
	if (((xc_rd(XC_LOWPOWER_HW_MUTEX)) & 0x0f) == XC_LOWPOWER_ETH_ID)
		xc_wr(XC_LOWPOWER_ETH_ID, XC_LOWPOWER_HW_MUTEX);					/* ミューテックスを放す */

	/* ---- plat_eth_init ---- */
	if (!(xc_rd(XC_ETH_CTRL_REG) & XC_ETH_CTRL_RESETN)) {
		/* 4段目ブートが Ethernet をリセットしていない(cold)ので、PHY を専用 GPIO3 でリセットする(ボードID 0x1201 は別の GPIO。nasne は 0x12xx の別) */
		xc_wr(xc_rd(XC_GPIO_DEDICATED_OUTEN) | XC_GPIO_DEDICATED3, XC_GPIO_DEDICATED_OUTEN);
		xc_wr(xc_rd(XC_GPIO_DEDICATED_OUT) & ~XC_GPIO_DEDICATED3, XC_GPIO_DEDICATED_OUT);
		xcode_early_delay_ms(100);
		xc_wr(xc_rd(XC_GPIO_DEDICATED_OUT) | XC_GPIO_DEDICATED3, XC_GPIO_DEDICATED_OUT);
	}
	/* 共有ピンを GPIO でなく Ethernet に */
	xc_wr(xc_rd(XC_GPIO_B_CTRL) & ~XC_GPIO_MODE_SEL, XC_GPIO_B_CTRL);
	xc_wr(xc_rd(XC_GPIO_A_CTRL) & ~XC_GPIO_MODE_SEL, XC_GPIO_A_CTRL);
	/* Ethernet ブロックのリセット */
	xcode_pulse_reset(XC_ETH0_RESET_MASK);
	xcode_pulse_reset(XC_ETH1_RESET_MASK);
	/* ソフトウェアリセット(GETH: ボードID 0x1103〜0x1106 以外は、GTXCLK のソースも指定) */
	v = XC_ETH_TX_NEGEDGE | XC_ETH_CTRL_RESETN;
	if (boardid < 0x1103 || boardid > 0x1106)
		v |= XC_ETH_GTXCLK_SOURCE;
	xc_wr(v, XC_ETH_CTRL_REG);
	xc_wr(v, XC_ETH_CTRL_REG1);
	pr_info("xcode: Ethernet cold 初期化 boardid=%04x ETH_CTRL=%08x\n", boardid, xc_rd(XC_ETH_CTRL_REG));
}

void __init plat_mem_setup(void)
{
	xcode_led_stage(2);	/* 段階2: plat_mem_setup に来た */

	/* ブロックのリセット(ベンダーの viper_board_setup と同じ): USB ホスト、SATA を一度リセットし、DMA エンジンのリセットを外す */
	xcode_pulse_reset(XC_USBH_RESET_MASK);
	xcode_pulse_reset(XC_SATA_RESET_MASK);
	xc_wr(xc_rd(XC_CG_RESET_REG) & ~XC_DMA_RESET_MASK, XC_CG_RESET_REG);

	bcops = &xcode_sc_ops;

	/* 実験: PFB(SoC 側の先読みバッファ)の設定を読んで記録し、先読み(MC_PFB_EN=bit4、SEQ_PFB_EN=bit5)を止める */
	xcode_pfb_cfg_before = xc_rd(XC_PROC5_PFB_CFG);
#ifdef XCODE_DISABLE_PFB
	xc_wr(xcode_pfb_cfg_before & ~0x30U, XC_PROC5_PFB_CFG);
#endif

	/* メモリ・cmdline(chosen/bootargs)・割り込み等は、内蔵 DTB から */
	__dt_setup_arch(__dtb_start);

	/*
	 * 物理 0x10000000〜0x1003ffff(256KB)は、ベンダーのカーネルでは「イメージ先頭のゼロ埋め」(4段目ブートは 0x10000000 へ展開して 0x10040000 へ飛ぶ)で、
	 * Linux が一般のメモリとして使っていなかった。この領域へ SATA の DMA で書くと、読んだデータが化ける(実測: 転送先が 0x10034000〜0x1003bfff のページだけが
	 * 必ず壊れ、ほかの場所は壊れない。壊れたデータは読んだ直後から違う)。使わないよう予約する。
	 */
	memblock_reserve(0x10000000, 0x40000);

	xcode_eth_cold_init();
	xcode_earlycon_init();
}
