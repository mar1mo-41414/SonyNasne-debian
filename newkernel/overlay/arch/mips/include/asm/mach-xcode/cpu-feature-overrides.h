/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_MACH_XCODE_CPU_FEATURE_OVERRIDES_H
#define __ASM_MACH_XCODE_CPU_FEATURE_OVERRIDES_H

/*
 * 74Kc は VI(ベクタ割り込み)を持つが、このSoCの割り込みは CPU の IP4〜IP7 に共有線で来るだけなので、
 * 通常の一般例外ベクタ + plat_irq_dispatch() で足りる(ベクタ設定を省ける)。
 */
#define cpu_has_vint		0
#define cpu_has_veic		0

#endif
