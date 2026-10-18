// SPDX-License-Identifier: GPL-2.0
/*
 * 前面LED(SoCのGPIOポート10)による、UARTの無い本体向けの起動デバッグ。
 *   LED_A(bit0x100)・LED_B(bit0x200)。Webカメラ(scripts/camera/ledtrace.py)で読む。
 *   段階 n(0〜15)= 4ビット(上位から): LED_B=ビット、LED_A を0.3秒点灯/0.6秒消灯(カメラの残光で点灯が延びて見えるため)。最後に1.2秒空ける。
 *   panic = LED_B を高速点滅(0.1秒周期)。
 * CP0 count(約256MHz)で待つので、タイマの初期化前から使える。
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/printk.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>
#include <linux/irqflags.h>
#include <asm/mipsregs.h>

#include "xcode.h"


void xcode_led_set(u32 mask)
{
	u32 v = xc_rd(XC_GPIO_PORT10_OUT);

	xc_wr(xc_rd(XC_GPIO_PORT10_EN) | XC_LED_A | XC_LED_B, XC_GPIO_PORT10_EN);
	xc_wr((v & ~(XC_LED_A | XC_LED_B)) | (mask & (XC_LED_A | XC_LED_B)), XC_GPIO_PORT10_OUT);
	__asm__ __volatile__("sync" ::: "memory");
}

EXPORT_SYMBOL_GPL(xcode_led_set);

/* LED_A(REC)だけを点灯/消灯する(LED_B のハートビートには触らない)。リンク状態の表示用(段階表示のあとは LED_A が空いている) */
void xcode_led_a(bool on)
{
	u32 v = xc_rd(XC_GPIO_PORT10_OUT);

	xc_wr(xc_rd(XC_GPIO_PORT10_EN) | XC_LED_A | XC_LED_B, XC_GPIO_PORT10_EN);
	xc_wr(on ? (v | XC_LED_A) : (v & ~XC_LED_A), XC_GPIO_PORT10_OUT);
	__asm__ __volatile__("sync" ::: "memory");
}
EXPORT_SYMBOL_GPL(xcode_led_a);

static void led_wait(unsigned int ticks)
{
	unsigned int t0 = read_c0_count();

	while ((unsigned int)(read_c0_count() - t0) < ticks)
		;
}

/* 段階番号 n(0〜15)を4ビット(上位から)で出す: 各ビットで、LED_B = そのビット、LED_A を0.3秒点灯→0.3秒消灯。最後に1.2秒空ける */
void xcode_led_stage(int n)
{
	int bit;

	if (!IS_ENABLED(CONFIG_XCODE_LED_DEBUG))
		return;
	for (bit = 3; bit >= 0; bit--) {
		u32 b = (n >> bit) & 1 ? XC_LED_B : 0;

		xcode_led_set(b | XC_LED_A);
		led_wait(76800000);
		xcode_led_set(b);
		led_wait(153600000);
	}
	xcode_led_set(0);
	led_wait(307200000);
}

/* 割り込みが届いているか: 段階表示が終わった後、タイマ割り込み50回(約0.5秒)ごとに LED_B を反転する */
static int hb_enabled, hb_count, hb_state;

void xcode_led_heartbeat_tick(void)
{
	if (!hb_enabled)
		return;
	if (++hb_count >= 50) {
		hb_count = 0;
		hb_state ^= 1;
		xcode_led_set(hb_state ? XC_LED_B : 0);
	}
}

void xcode_led_panic(void)
{
	for (;;) {
		xcode_led_set(XC_LED_B);
		led_wait(12800000);
		xcode_led_set(0);
		led_wait(12800000);
	}
}

static int xcode_panic_event(struct notifier_block *nb, unsigned long ev, void *p)
{
	if (IS_ENABLED(CONFIG_XCODE_LED_PANIC) || IS_ENABLED(CONFIG_XCODE_LED_DEBUG))
		xcode_led_panic();
	return NOTIFY_DONE;
}

static struct notifier_block xcode_panic_nb = {
	.notifier_call = xcode_panic_event,
	.priority = INT_MIN,		/* 他の panic 通知(ログ出力)の後 */
};

/* initcall の各レベルで段階番号を出す(どこまで進んだかが分かる) */
#define XC_STAGE_INITCALL(level, n)					\
	static int __init xcode_stage_##level(void)			\
	{								\
		xcode_led_stage(n);					\
		return 0;						\
	}								\
	level##_initcall(xcode_stage_##level)

static int __init xcode_led_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &xcode_panic_nb);
	xcode_led_stage(5);
	return 0;
}
early_initcall(xcode_led_init);			/* 段階5: start_kernel を抜けて initcall に来た */
static int __init xcode_stage_arch(void)
{
	xcode_led_stage(6);			/* 段階6: arch_initcall */
#ifdef XCODE_DIAG_ALIVE
	for (;;) {		/* 診断: ここで CPU が生きているか(両 LED を0.5秒周期で点滅し続ける) */
		xcode_led_set(XC_LED_A | XC_LED_B);
		led_wait(64000000);
		xcode_led_set(0);
		led_wait(64000000);
	}
#endif
#ifdef XCODE_DIAG_IRQ
	{
		unsigned int st = read_c0_status(), ca = read_c0_cause();
		unsigned long j0 = jiffies;
		void *p;

		/* 診断1: タイマ割り込みが生きていれば、約0.2秒の busy-wait の間に jiffies が進む(13)。進まなければ 12 */
		local_irq_enable();
		led_wait(51200000);
		xcode_led_stage(jiffies != j0 ? 13 : 12);
		/* 診断2: vmalloc(TLB リフィル例外が通るか)。通れば 14 */
		p = vmalloc(8192);
		if (p) {
			*(volatile u32 *)p = 0x1234;
			vfree(p);
			xcode_led_stage(14);
		}

		/* 診断: 15(開始印) → Status[15:12] → Status[11:8](IM3..IM0 ほか) → Status[3:0](UM,ERL,EXL,IE) → Cause[15:12] → Cause[11:8] */
		xcode_led_stage(15);
		xcode_led_stage((st >> 12) & 0xf);
		xcode_led_stage((st >> 8) & 0xf);
		xcode_led_stage(st & 0xf);
		xcode_led_stage((ca >> 12) & 0xf);
		xcode_led_stage((ca >> 8) & 0xf);
	}
#endif
	return 0;
}
arch_initcall(xcode_stage_arch);
XC_STAGE_INITCALL(subsys, 7);			/* 段階7: subsys_initcall */
XC_STAGE_INITCALL(device, 8);			/* 段階8: device_initcall */
static int __init xcode_stage_late(void)
{
	xcode_led_stage(9);			/* 段階9: late_initcall(ほぼ起動完了。次は init) */
	hb_enabled = IS_ENABLED(CONFIG_XCODE_LED_DEBUG);	/* 以降、タイマ割り込みで LED_B が0.5秒ごとに反転(割り込みが生きている印) */
	return 0;
}
late_initcall(xcode_stage_late);

static int __init xcode_hb_start(void)
{
	if (IS_ENABLED(CONFIG_XCODE_LED_HEARTBEAT))
		hb_enabled = 1;
	return 0;
}
late_initcall(xcode_hb_start);
