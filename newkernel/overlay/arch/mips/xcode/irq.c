// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210 "Viper": 割り込み
 *
 * SoC の割り込みコントローラは、各ブロックの割り込みを集めて CPU の IP4〜IP7 の4本に出す(MIPS5〜7_INTERRUPT_MASK で
 * 有効/無効を選ぶ)。ベンダーのカーネルも、この4本をそのまま共有割り込みとして各ドライバへ渡している
 * (実機の /proc/interrupts: IRQ5 = メディアエンジン、IRQ6 = SATA・USB・Ethernet・SPI・メディアエンジン)。
 * ここでは CPU 側の割り込みコントローラ(DT の mti,cpu-interrupt-controller)をそのまま使い、
 * SoC 側の有効化は、各ドライバの親になる platform 側(後で追加)で行う。
 */
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/bits.h>
#include <linux/irqchip.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <asm/mach-xcode/xcode-soc.h>
#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>

#include "xcode.h"

/* デバッグ: 各 CPU 割り込み線(IP2〜IP7)の最初の割り込みで、段階表示(10+線番号)を一度だけ出す(割り込みが届いているか・どの線かが分かる) */
static unsigned int xcode_irq_seen;

asmlinkage void plat_irq_dispatch(void)
{
	unsigned int pending = read_c0_cause() & read_c0_status() & ST0_IM;

	if (!pending) {
		spurious_interrupt();
		return;
	}
	xcode_led_heartbeat_tick();
	{
		int line = __fls(pending) - CAUSEB_IP;

		if (false && IS_ENABLED(CONFIG_XCODE_LED_DEBUG) && !(xcode_irq_seen & BIT(line))) {
			xcode_irq_seen |= BIT(line);
			xcode_led_stage(8 + line);	/* IP2=10 … IP7=15 */
		}
		do_IRQ(MIPS_CPU_IRQ_BASE + line);
	}
}

void __init arch_init_irq(void)
{
	xcode_led_stage(3);	/* 段階3: arch_init_irq に来た */

	/* リセット後に残った保留・有効状態を消す(ベンダーも全て0にしている) */
	xc_wr(0, XC_MIPS5_INTERRUPT_MASK);
	xc_wr(0, XC_MIPS6_INTERRUPT_MASK);
	xc_wr(0, XC_MIPS7_INTERRUPT_MASK);
	xc_wr(0, XC_MIPS5_INTERRUPT1_MASK);
	xc_wr(0, XC_MIPS6_INTERRUPT1_MASK);
	xc_wr(0, XC_MIPS7_INTERRUPT1_MASK);

	irqchip_init();
}

/* SoC レジスタ窓(KSEG1 直接マップ)を、共有レジスタ用の1つのロックで保護して読み書きする */
static DEFINE_RAW_SPINLOCK(xcode_soc_lock);

void xcode_soc_rmw(unsigned int off, u32 clear, u32 set)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&xcode_soc_lock, flags);
	xc_wr((xc_rd(off) & ~clear) | set, off);
	raw_spin_unlock_irqrestore(&xcode_soc_lock, flags);
}
EXPORT_SYMBOL_GPL(xcode_soc_rmw);

void xcode_soc_write(unsigned int off, u32 val)
{
	xc_wr(val, off);
}
EXPORT_SYMBOL_GPL(xcode_soc_write);

u32 xcode_soc_read(unsigned int off)
{
	return xc_rd(off);
}
EXPORT_SYMBOL_GPL(xcode_soc_read);

void xcode_soc_irq_enable(u32 mask)
{
	xcode_soc_rmw(XC_MIPS5_INTERRUPT_MASK, 0, mask);
}
EXPORT_SYMBOL_GPL(xcode_soc_irq_enable);

void xcode_soc_irq_disable(u32 mask)
{
	xcode_soc_rmw(XC_MIPS5_INTERRUPT_MASK, mask, 0);
}
EXPORT_SYMBOL_GPL(xcode_soc_irq_disable);

u32 xcode_soc_irq_status(void)
{
	return xc_rd(XC_MIPS5_INTERRUPT);
}
EXPORT_SYMBOL_GPL(xcode_soc_irq_status);
