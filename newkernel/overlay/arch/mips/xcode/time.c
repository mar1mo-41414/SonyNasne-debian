// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210 "Viper": タイマ
 *
 * CPU の CP0 count/compare を使う(count は CPU クロックの 1/2。実機で CPU 約512MHz → count 約256MHz)。
 * 周波数は、SoC の 27MHz/16 のカウンタ(CG_SYSTEM_CNT)で 1ms 測って求める(ベンダーと同じ)。
 */
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <asm/mipsregs.h>
#include <asm/time.h>

#include "xcode.h"

#define SYSTEM_CNT_HZ	(27000000 / 16)

static unsigned int __init xcode_count_hz(void)
{
	unsigned long flags;
	unsigned int sys0, start, count;

	local_irq_save(flags);
	sys0 = xc_rd(XC_CG_SYSTEM_CNT);
	start = read_c0_count();
	while (xc_rd(XC_CG_SYSTEM_CNT) - sys0 < SYSTEM_CNT_HZ / 1000)
		;
	count = (read_c0_count() - start) * 1000;
	local_irq_restore(flags);
	return count;
}

void __init plat_time_init(void)
{
	xcode_led_stage(4);	/* 段階4: plat_time_init に来た */

	mips_hpt_frequency = xcode_count_hz();
#ifdef XCODE_DIAG_TIMER
	{
		/* 診断(タイマ割り込みが来ない原因の切り分け)。表示: 15(開始印)、
		 * 入口の Cause[31:28](BD,TI,CE)・Cause.IP3..IP0、compare=count で確認応答した後の同じ2つ */
		unsigned int t0, c1, c2;

		c1 = read_c0_cause();
		write_c0_compare(read_c0_count());
		t0 = read_c0_count();
		while (read_c0_count() - t0 < 2000)
			;
		c2 = read_c0_cause();
		xcode_led_stage(15);
		xcode_led_stage((c1 >> 28) & 0xf);
		xcode_led_stage((c1 >> 8) & 0xf);
		xcode_led_stage((c2 >> 28) & 0xf);
		xcode_led_stage((c2 >> 8) & 0xf);
	}
#endif
	pr_info("XCode: count %u.%03u MHz (CPU %u MHz)\n", mips_hpt_frequency / 1000000,
		(mips_hpt_frequency % 1000000) / 1000, mips_hpt_frequency * 2 / 1000000);
}
