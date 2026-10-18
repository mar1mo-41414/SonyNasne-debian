/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ViXS XCode 4210 の SATA(AHCI)。AHCI のレジスタは、メモリ空間に直接は見えず、SoC の「間接アクセス窓」
 * (制御/状態/書き込みデータ/読み出しデータ。物理 0x1fffd000)を通してだけ触れる。窓はメディアエンジン側の
 * ファームも使うので、SoC のハードウェア・ミューテックス(MIPS_INTERLOCK7)で排他する。
 *
 * libahci は readl()/writel() でレジスタを触るので、libahci.c だけを、この窓経由の readl/writel に差し替える
 * (drivers/ata/libahci.c の先頭のパッチ)。ポインタは本物のアドレスではなく、「ホスト番号(bit31) | レジスタオフセット」。
 */
#ifndef __ASM_MACH_XCODE_AHCI_H
#define __ASM_MACH_XCODE_AHCI_H

#include <linux/types.h>

#define XCODE_AHCI_HOST1	0x80000000	/* ポインタの bit31: 2番目のホスト(SATA ポート1) */

struct ata_port;

/* libata の ata_wait_register()(レジスタが期待値の間待つ)の、間接アクセス窓版 */
u32 xcode_ata_wait_register(struct ata_port *ap, void __iomem *reg, u32 mask, u32 val, unsigned int interval,
			    unsigned int timeout);

/* SoC 側の SATA 割り込み(ベンダーの ISR と同じ手順: 先に状態を見て、処理後に SATA_INT_STATUS へ書いて落とす) */
bool xcode_ahci_irq_pending(void);
void xcode_ahci_irq_ack(void);

u32 xcode_ahci_readl(const volatile void __iomem *addr);
void xcode_ahci_writel(u32 val, volatile void __iomem *addr);

#endif
