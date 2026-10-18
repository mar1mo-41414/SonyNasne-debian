/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ViXS XCode 4210 の Ethernet(Synopsys DWC GMAC)の、SoC 固有のラッパ(CSR 窓)と、GMAC 本体のレジスタ定義。
 *
 * GMAC 本体(MAC は +0x0000、DMA は +0x1000)のレジスタは、メモリ空間に直接は見えない。SoC のラッパが持つ
 * 「CSR 窓」を通す(書き込み: WDAT にデータ→CSR_CTRL にアドレス|0x80000000。読み出し: CSR_CTRL にアドレス→RDAT)。
 * そのため、メインラインの stmmac はそのままでは使えない。
 * eth0 のラッパは SoC レジスタ窓の 0x2800〜、eth1 は 0x2c00〜。
 */
#ifndef __ASM_MACH_XCODE_GMAC_H
#define __ASM_MACH_XCODE_GMAC_H

/* ラッパ(ブロック先頭からのオフセット) */
#define XG_WRAP_CSR_CTRL	0x00
#define  XG_CSR_WRITE		0x80000000
#define XG_WRAP_STATUS		0x04
#define  XG_STATUS_BUSY		0x00000001
#define XG_WRAP_WDAT		0x08
#define XG_WRAP_RDAT		0x0c
#define XG_WRAP_CTRL		0x10
#define XG_WRAP_INT_STATUS	0x14	/* bit0=PMT bit1=SBD(DMA 割り込み) */
#define XG_WRAP_HOST_INT_MASK	0x18
#define XG_WRAP_MIPS_INT_MASK	0x1c	/* 同上。MIPS 向けの割り込みを有効にする */
#define  XG_INT_PMT		0x00000001
#define  XG_INT_SBD		0x00000002

#define XG_ETH0_BASE		0x2800
#define XG_ETH1_BASE		0x2c00

/* MAC(CSR 窓のアドレス) */
#define XG_MAC_CONFIG		0x0000
#define  XG_CFG_RE		0x00000004
#define  XG_CFG_TE		0x00000008
#define  XG_CFG_DM		0x00000800	/* 全二重 */
#define  XG_CFG_IPC		0x00000400	/* 受信チェックサムオフロード */
#define  XG_CFG_DO		0x00002000	/* 自分の送信を受信しない */
#define  XG_CFG_FES		0x00004000	/* 100Mbps(PS と併用) */
#define  XG_CFG_PS		0x00008000	/* MII(10/100)。0 なら GMII(1000) */
#define  XG_CFG_IFG_88		0x00020000
#define XG_MAC_FRAME_FILTER	0x0004
#define  XG_FF_PR		0x00000001	/* 全部受ける */
#define  XG_FF_PM		0x00000010	/* 全マルチキャスト */
#define XG_MAC_HASH_HI		0x0008
#define XG_MAC_HASH_LO		0x000c
#define XG_MAC_GMII_ADDR	0x0010
#define  XG_GMII_BUSY		0x00000001
#define  XG_GMII_WRITE		0x00000002
#define  XG_GMII_CLK_100_150	0x00000004
#define XG_MAC_GMII_DATA	0x0014
#define XG_MAC_FLOW		0x0018
#define XG_MAC_VERSION		0x0020
#define XG_MAC_INT_STATUS	0x0038
#define XG_MAC_INT_MASK		0x003c
/* MMC(統計カウンタ)。カウンタが半分(0x80000000。送信バイト数なら約2GB)に達すると割り込みを上げ続けて、共有 IRQ 6 が止まらなくなり、本体が固まる。使わないので全部マスク */
#define XG_MMC_RX_INTR_MASK	0x010c
#define XG_MMC_TX_INTR_MASK	0x0110
#define XG_MMC_RX_IPC_INTR_MASK	0x0200
#define XG_MAC_ADDR0_HI		0x0040
#define XG_MAC_ADDR0_LO		0x0044

/* DMA(CSR 窓のアドレス) */
#define XG_DMA_BUS_MODE		0x1000
#define  XG_BUS_SWR		0x00000001
#define  XG_BUS_DSL2		0x00000008
#define  XG_BUS_PBL8		0x00000800
#define  XG_BUS_FB		0x00010000
#define XG_DMA_TX_POLL		0x1004
#define XG_DMA_RX_POLL		0x1008
#define XG_DMA_RX_BASE		0x100c
#define XG_DMA_TX_BASE		0x1010
#define XG_DMA_STATUS		0x1014
#define  XG_DS_TI		0x00000001
#define  XG_DS_TU		0x00000004
#define  XG_DS_RI		0x00000040
#define  XG_DS_RU		0x00000080
#define  XG_DS_FBI		0x00002000
#define  XG_DS_AIS		0x00008000
#define  XG_DS_NIS		0x00010000
#define XG_DMA_OP_MODE		0x1018
#define  XG_OP_SR		0x00000002
#define  XG_OP_OSF		0x00000004
#define  XG_OP_ST		0x00002000
#define  XG_OP_SF		0x00200000
#define XG_DMA_INT_EN		0x101c
#define  XG_IE_TI		0x00000001
#define  XG_IE_RI		0x00000040
#define  XG_IE_RU		0x00000080
#define  XG_IE_FBE		0x00002000
#define  XG_IE_AIE		0x00008000
#define  XG_IE_NIE		0x00010000
#define XG_DMA_MISSED		0x1020

/* ディスクリプタ(16バイト、チェーンまたはリング)。「enhanced」配置: 送信の制御ビットは TDES0 */
#define XG_TDES0_OWN		0x80000000
#define XG_TDES0_IC		0x40000000
#define XG_TDES0_LS		0x20000000
#define XG_TDES0_FS		0x10000000
#define XG_TDES0_DC		0x08000000
#define XG_TDES0_DP		0x04000000
#define XG_TDES0_TER		0x00200000
#define XG_TDES0_TCH		0x00100000
#define XG_TDES0_ES		0x00008000
#define XG_RDES0_OWN		0x80000000
#define XG_RDES0_FL_MASK	0x3fff0000
#define XG_RDES0_FL_SHIFT	16
#define XG_RDES0_ES		0x00008000
#define XG_RDES0_LE		0x00001000
#define XG_RDES0_FS		0x00000200
#define XG_RDES0_LS		0x00000100
#define XG_RDES1_DIC		0x80000000
#define XG_RDES1_RER		0x00008000
#define XG_RDES1_RCH		0x00004000

struct xg_desc {
	u32 status;
	u32 ctrl;
	u32 buf1;
	u32 buf2;
};

/* 外部 PHY(Marvell 88E1xxx、MDIO アドレス 0) */
#define XG_PHY_ADDR		0
#define XG_PHY_BMSR		0x01
#define XG_PHY_SPEC_STATUS	0x11	/* [15:14] 速度(0=10 1=100 2=1000)、[13] 全二重、[11] 速度・二重の確定、[10] リンク */

#endif
