/* SPDX-License-Identifier: GPL-2.0 */
/*
 * カーネル image の先頭(リンクアドレス)に置く入口。
 * 4段目ブートは、カーネルを物理 0x10000000 へ展開し、a0=argc、a1=argv、a2=envp、a3=prom のまま 0xB0040000 へ飛ぶ。
 * ベンダーのカーネルは、この入口で 0xa0000000(物理0)へ 0xbbbbbbbc を書いてから kernel_entry へ進む(U-Boot のリセットと
 * 同じ印。Sony のカーネルと同じ挙動にそろえる)。a0〜a3 は壊さない。
 */
#include <asm/asm.h>
#include <asm/regdef.h>

	.section .text.xcode_head, "ax"
	.set	noreorder
	.globl	xcode_entry
	.ent	xcode_entry
xcode_entry:
	lui	t0, 0xa000
	lui	t1, 0xbbbb
	ori	t1, t1, 0xbbbc
	j	kernel_entry
	 sw	t1, 0(t0)
	.end	xcode_entry
