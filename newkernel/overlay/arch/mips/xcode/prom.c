// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210 "Viper"(Sony nasne): ブートローダ(4段目ブート)からの引数の受け取り
 *
 * 4段目ブートは、SoC レジスタ 0xbfff2820 に 0x55aabeef を書き、a0=argc、a1=argv、a2=envp、a3=prom で
 * カーネルへ飛ぶ。引数は「root=/dev/sda2 rw ethaddr=<MAC> rd_start=… rd_size=…」。kexec や直接ジャンプ(開発時)
 * では 0x55aabeef が無いので、引数を読まず DT の bootargs(または CONFIG_CMDLINE)を使う。
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/bootinfo.h>
#include <asm/setup.h>
#include <asm/mipsregs.h>
#include <linux/export.h>

#include "xcode.h"

const char *get_system_type(void)
{
	return "XCode Viper (Sony nasne)";
}

/* 4段目ブートが渡す MAC アドレス(ethaddr=xx:xx:xx:xx:xx:xx)。GMAC ドライバが DT の指定が無いときに使う */
static u8 xcode_boot_ethaddr[6];
static bool xcode_boot_ethaddr_valid;

static void __init xcode_parse_ethaddr(const char *s)
{
	u8 mac[6];
	int i;

	for (i = 0; i < 6; i++) {
		unsigned int v = 0, d = 0;

		while (d < 2 && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
			v = v * 16 + (*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
			s++;
			d++;
		}
		if (!d)
			return;
		mac[i] = v;
		if (i < 5 && *s++ != ':')
			return;
	}
	memcpy(xcode_boot_ethaddr, mac, 6);
	xcode_boot_ethaddr_valid = true;
}

bool xcode_get_boot_ethaddr(u8 *mac)
{
	if (!xcode_boot_ethaddr_valid)
		return false;
	memcpy(mac, xcode_boot_ethaddr, 6);
	return true;
}
EXPORT_SYMBOL_GPL(xcode_get_boot_ethaddr);

void __init prom_init(void)
{
	/*
	 * ベクタ割り込みを使わない(cpu_has_vint=0)ので、IntCtl.VS(ベクタ間隔)を0にして、割り込みを一般例外ベクタ
	 * (ebase+0x200)へ集める。コールドブートなら0だが、kexec で旧カーネル(ベクタ割り込みを使っていた)から来ると
	 * VS が残っていて、割り込みがベクタごとに別の(未設定の)アドレスへ飛んでしまう(開発時の kexec で固まる原因だった)。
	 */
	change_c0_intctl(0x3e0, 0);
	xcode_led_stage(1);	/* 段階1: prom_init に来た */

	if (xc_rd(XC_ETH_DUMMY_REG) == XC_BOOT_MAGIC && fw_arg0 && fw_arg0 < 64) {
		int argc = (int)fw_arg0, i;
		char **argv = (char **)fw_arg1;
		char *c = arcs_cmdline;

		arcs_cmdline[0] = '\0';
		for (i = 0; i < argc; i++) {
			const char *a = argv[i];
			size_t n = strlen(a);

			/*
			 * 4段目ブートは「root=/dev/sda2 rw ethaddr=<MAC> rd_start=0x81400000 rd_size=…」を渡す。
			 * rd_start/rd_size は Sony の miniroot(外部 ramdisk)を指す。使うと、内蔵 initramfs の /init が
			 * miniroot の /init で上書きされるので捨てる。root= も使わない(initramfs が決める)。ethaddr= は拾う。
			 */
			if (!strncmp(a, "ethaddr=", 8)) {
				xcode_parse_ethaddr(a + 8);
				continue;
			}
			if (!strncmp(a, "rd_start=", 9) || !strncmp(a, "rd_size=", 8) || !strncmp(a, "root=", 5))
				continue;
			if ((c - arcs_cmdline) + n + 2 >= COMMAND_LINE_SIZE)
				break;
			if (c != arcs_cmdline)
				*c++ = ' ';
			memcpy(c, a, n);
			c += n;
		}
		*c = '\0';
	}
}
