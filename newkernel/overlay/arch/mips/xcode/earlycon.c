// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne): Ethernet 経由の早期コンソール(UDP ブロードキャスト、ポーリング送信)。
 * UART が基板に出ていないため、起動の早い段階のログ(oops・panic を含む)をネットワークへ流す。
 * 受信側: socat -u UDP-RECV:6666 STDOUT  (nasne の LAN と同じセグメントの PC で)
 *
 * GMAC は、ドライバが入る前の状態(旧カーネルが使っていた、または U-Boot が初期化した状態)から、
 * DMA をリセットして TX だけを自前で組む。PHY のリンク状態(速度・全二重)は MDIO で読み、MAC に反映する。
 * 送信バッファとディスクリプタは、カーネルの .bss 内の静的領域(KSEG1 = キャッシュなしの別名で触る)。
 */
#include <linux/init.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <asm/addrspace.h>
#include <asm/mipsregs.h>
#include <asm/page.h>
#include <asm/mach-xcode/xcode-gmac.h>
#include <asm/mach-xcode/xcode-soc.h>

#include "xcode.h"

#define EC_PORT_BASE	XG_ETH0_BASE
#define EC_PORT		"eth0"
#define EC_UDP_PORT	6666
#define EC_PAYLOAD_MAX	1000

static u8 ec_mac[6] = { 0x02, 0x58, 0x43, 0x4f, 0x44, 0x45 };	/* 02:'X':'C':'O':'D':'E'(ローカル管理) */
static u8 ec_frame[1536] __aligned(32);
static struct xg_desc ec_desc __aligned(32);
static bool ec_ready;
static u32 ec_ipid;

static inline void ec_udelay(unsigned int us)
{
	unsigned int t0 = read_c0_count();

	while ((unsigned int)(read_c0_count() - t0) < us * 256)	/* count は約256MHz */
		;
}

static inline u32 ec_w(unsigned int off)
{
	return xc_rd(EC_PORT_BASE + off);
}

static bool ec_wait_idle(void)
{
	int n = 100000;

	while ((ec_w(XG_WRAP_STATUS) & XG_STATUS_BUSY) && --n)
		;
	return n > 0;
}

static u32 ec_csr_rd(u32 addr)
{
	ec_wait_idle();
	xc_wr(addr, EC_PORT_BASE + XG_WRAP_CSR_CTRL);
	ec_wait_idle();
	return ec_w(XG_WRAP_RDAT);
}

static void ec_csr_wr(u32 addr, u32 val)
{
	ec_wait_idle();
	xc_wr(val, EC_PORT_BASE + XG_WRAP_WDAT);
	xc_wr(addr | XG_CSR_WRITE, EC_PORT_BASE + XG_WRAP_CSR_CTRL);
}

static int ec_mdio_rd(int reg)
{
	int n = 10000;

	ec_csr_wr(XG_MAC_GMII_ADDR, (XG_PHY_ADDR << 11) | (reg << 6) | XG_GMII_CLK_100_150 | XG_GMII_BUSY);
	while ((ec_csr_rd(XG_MAC_GMII_ADDR) & XG_GMII_BUSY) && --n)
		ec_udelay(10);
	if (!n)
		return -1;
	return ec_csr_rd(XG_MAC_GMII_DATA) & 0xffff;
}

static u16 ip_csum(const u8 *p, int len)
{
	u32 sum = 0;
	int i;

	for (i = 0; i + 1 < len; i += 2)
		sum += (p[i] << 8) | p[i + 1];
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~sum;
}

static bool ec_init(void)
{
	int i, spec = 0;
	u32 hi, lo, cfg;

	/* 現在の MAC アドレス(旧カーネルや U-Boot が設定していれば使う) */
	hi = ec_csr_rd(XG_MAC_ADDR0_HI);
	lo = ec_csr_rd(XG_MAC_ADDR0_LO);
	if ((lo | (hi & 0xffff)) && (lo | (hi & 0xffff)) != 0xffffffff) {
		ec_mac[0] = lo;
		ec_mac[1] = lo >> 8;
		ec_mac[2] = lo >> 16;
		ec_mac[3] = lo >> 24;
		ec_mac[4] = hi;
		ec_mac[5] = hi >> 8;
	}

	xcode_soc_rmw(XC_CG_CLK_STOP0, 0, XC_ETHR0CLK_STOP | XC_ETHR1CLK_STOP);	/* ベンダーのドライバと同じ */

	/* DMA をリセット(MAC の設定も既定に戻るので、後で組み直す) */
	ec_csr_wr(XG_DMA_BUS_MODE, XG_BUS_SWR);
	for (i = 0; i < 1000 && (ec_csr_rd(XG_DMA_BUS_MODE) & XG_BUS_SWR); i++)
		ec_udelay(100);

	/* リンク待ち(最大約3秒)。速度・二重は PHY の固有ステータスから */
	for (i = 0; i < 300; i++) {
		spec = ec_mdio_rd(XG_PHY_SPEC_STATUS);
		if (spec >= 0 && (spec & 0x0400))
			break;
		ec_udelay(10000);
	}
	if (spec < 0 || !(spec & 0x0400))
		return false;

	cfg = XG_CFG_IFG_88 | XG_CFG_DO | XG_CFG_TE | XG_CFG_RE;
	if (spec & 0x2000)
		cfg |= XG_CFG_DM;
	switch ((spec >> 14) & 3) {
	case 0:
		cfg |= XG_CFG_PS;			/* 10M */
		break;
	case 1:
		cfg |= XG_CFG_PS | XG_CFG_FES;		/* 100M */
		break;
	default:
		break;					/* 1000M: GMII */
	}
	ec_csr_wr(XG_MAC_ADDR0_HI, (ec_mac[5] << 8) | ec_mac[4]);
	ec_csr_wr(XG_MAC_ADDR0_LO, (ec_mac[3] << 24) | (ec_mac[2] << 16) | (ec_mac[1] << 8) | ec_mac[0]);
	ec_csr_wr(XG_MAC_CONFIG, cfg);
	ec_csr_wr(XG_DMA_BUS_MODE, XG_BUS_FB | XG_BUS_PBL8 | XG_BUS_DSL2);

	/* 送信ディスクリプタ1個の輪(自分自身を指す) */
	ec_csr_wr(XG_DMA_INT_EN, 0);
	ec_csr_wr(XG_DMA_TX_BASE, __pa_symbol(&ec_desc));
	ec_csr_wr(XG_DMA_OP_MODE, XG_OP_SF | XG_OP_OSF | XG_OP_ST);
	return true;
}

static void ec_send(const char *s, unsigned int count)
{
	struct xg_desc *d = (struct xg_desc *)CKSEG1ADDR(__pa_symbol(&ec_desc));
	u8 *f = (u8 *)CKSEG1ADDR(__pa_symbol(ec_frame));
	unsigned int len, ip_len, udp_len;
	int n;

	if (count > EC_PAYLOAD_MAX)
		count = EC_PAYLOAD_MAX;
	udp_len = 8 + count;
	ip_len = 20 + udp_len;
	len = 14 + ip_len;

	memset(f, 0xff, 6);			/* 宛先: ブロードキャスト */
	memcpy(f + 6, ec_mac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	f[14] = 0x45;				/* IPv4、ヘッダ20バイト */
	f[15] = 0;
	f[16] = ip_len >> 8;
	f[17] = ip_len;
	f[18] = ec_ipid >> 8;
	f[19] = ec_ipid++;
	f[20] = 0;
	f[21] = 0;
	f[22] = 64;
	f[23] = 17;				/* UDP */
	f[24] = f[25] = 0;
	f[26] = 192; f[27] = 168; f[28] = 11; f[29] = 129;	/* 送信元 IP(受信側の都合。意味は無い) */
	f[30] = f[31] = f[32] = f[33] = 255;			/* 宛先 255.255.255.255 */
	{
		u16 c = ip_csum(f + 14, 20);

		f[24] = c >> 8;
		f[25] = c;
	}
	f[34] = EC_UDP_PORT >> 8;
	f[35] = EC_UDP_PORT & 0xff;
	f[36] = EC_UDP_PORT >> 8;
	f[37] = EC_UDP_PORT & 0xff;
	f[38] = udp_len >> 8;
	f[39] = udp_len;
	f[40] = f[41] = 0;			/* UDP チェックサムなし */
	memcpy(f + 42, s, count);
	if (len < 60) {
		memset(f + len, 0, 60 - len);
		len = 60;
	}

	d->buf1 = __pa_symbol(ec_frame);
	d->buf2 = __pa_symbol(&ec_desc);
	d->ctrl = len;
	d->status = XG_TDES0_OWN | XG_TDES0_LS | XG_TDES0_FS | XG_TDES0_TCH;
	__asm__ __volatile__("sync" ::: "memory");
	ec_csr_wr(XG_DMA_TX_POLL, 1);
	for (n = 0; n < 20000 && (d->status & XG_TDES0_OWN); n++)
		ec_udelay(5);
}

static void ec_write(struct console *con, const char *s, unsigned int count)
{
	if (!ec_ready)
		return;
	while (count) {
		unsigned int n = count > EC_PAYLOAD_MAX ? EC_PAYLOAD_MAX : count;

		ec_send(s, n);
		s += n;
		count -= n;
	}
}

static struct console ec_console = {
	.name	= "xcgmac",
	.write	= ec_write,
	.flags	= CON_PRINTBUFFER,
	.index	= -1,
};

void __init xcode_earlycon_init(void)
{
	if (!IS_ENABLED(CONFIG_XCODE_EARLYCON))
		return;
	ec_ready = ec_init();
	if (ec_ready) {
		xcode_led_stage(10);		/* 診断: 早期コンソールの初期化が終わった */
		register_console(&ec_console);
	}
}

/* 本物のドライバ(xcode_gmac)が GMAC を使い始める前に呼ぶ。以降の送信は netconsole など */
void xcode_earlycon_shutdown(void)
{
	if (!ec_ready)
		return;
	ec_ready = false;
	unregister_console(&ec_console);
}
EXPORT_SYMBOL_GPL(xcode_earlycon_shutdown);
