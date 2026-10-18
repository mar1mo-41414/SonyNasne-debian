// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)の SPI マスタ(SPI フラッシュ S25FL128P が CS0 に付いている)。
 *
 * ベンダーのドライバ(drivers/spi/xcode_spi.c、2.6.29。PPC 側の SPI_DONE 割り込みで 1 回 4 バイトずつ送る)に基づくが、
 * ここでは割り込みを使わず、ベンダーの platform.c(eeprom_read_id)と同じ SPI_STATUS のビジーをポーリングする。
 *   - SPI_CFG に (サイズ-1)<<1 | キャプチャ辺<<3 | CS 保持 を書いてから SPI_WDAT に書くと 1 回(最大 4 バイト)の転送が始まる。
 *     送信データは上位バイトから詰める(最初のバイトが bit31..24)。受信は SPI_RDAT の下位サイズ分、最初のバイトが上位。
 *   - フラッシュのピンは NAND と共有で、PPC(メディアエンジン)の側もフラッシュを触るので、ハードウェア・ミューテックス
 *     (MIPS_INTERLOCK2 の ID4 フィールド、ID は 11)を取ってから、パッドのマルチプレクサ(RBM_PADU_CTRL の bit8)を SPI 側に向ける。
 *   - CS は転送ごとに自動で上がる(CS 保持=0 の転送の最後で上がる)。Linux の意味(メッセージの最後で上げる、cs_change)に合わせて保持を決める。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/jiffies.h>
#include <asm/mach-xcode/xcode-soc.h>

#define SOC_CG_RESET_REG	0x0000
#define  PPC_RESET		0x00000200
#define SOC_INTERLOCK2		0x0448
#define  ID4_SHIFT		16
#define  SPI_MUTEX_ID		11		/* HOST_MUTEX_START(8) + 3 */
#define SOC_BOARD_ID		0x0478
#define SOC_PADU_CTRL		0x04a8
#define  PADU_FLASH_MASK	0x00000100
#define  PADU_FLASH_SHIFT	8
#define SOC_PPC_CLOCK_CTRL	0x10a0
#define SOC_PPC_SFT_RSTN	0x10a4
#define  CCSPI_SFT_RSTN		0x00000002
#define SOC_SPI_CFG		0x10ac
#define  CFG_HOLD		0x00000001
#define  CFG_SIZE_SHIFT		1
#define  CFG_CAP_EDGE		0x00000008
#define  CFG_RATE_MASK		0x00000030
#define SOC_SPI_WDAT		0x10b0
#define SOC_SPI_RDAT		0x10b4
#define SOC_SPI_STATUS		0x10b8
#define  SPI_BUSY		0x00000001

struct xspi {
	struct spi_controller *ctlr;
};

static int xspi_get_bus(void)
{
	unsigned long end = jiffies + HZ;
	u32 reg, want;

	while (1) {
		xcode_soc_write(SOC_INTERLOCK2, SPI_MUTEX_ID << ID4_SHIFT);
		if (((xcode_soc_read(SOC_INTERLOCK2) >> ID4_SHIFT) & 0xf) == SPI_MUTEX_ID)
			break;
		if (time_after(jiffies, end))
			return -EBUSY;
		usleep_range(500, 1000);
	}
	/* NAND と共有のピンを SPI に向ける(すでに SPI なら書かない) */
	reg = xcode_soc_read(SOC_PADU_CTRL);
	want = xcode_soc_read(SOC_BOARD_ID) & 1;
	if (want != ((reg & PADU_FLASH_MASK) >> PADU_FLASH_SHIFT))
		xcode_soc_write(SOC_PADU_CTRL, want ? (reg | PADU_FLASH_MASK) : (reg & ~PADU_FLASH_MASK));
	return 0;
}

static void xspi_put_bus(void)
{
	if (((xcode_soc_read(SOC_INTERLOCK2) >> ID4_SHIFT) & 0xf) == SPI_MUTEX_ID)
		xcode_soc_write(SOC_INTERLOCK2, SPI_MUTEX_ID << ID4_SHIFT);	/* 同じ値を書くと解放 */
	(void)xcode_soc_read(SOC_INTERLOCK2);
}

/* 1回(1〜4バイト)の転送。tx が NULL なら 0 を送る。rx が NULL なら捨てる */
static int xspi_chunk(const u8 *tx, u8 *rx, unsigned int n, bool hold)
{
	u32 wdat = 0, rdat;
	unsigned int i;
	unsigned long end;

	if (tx)
		for (i = 0; i < n; i++)
			wdat |= (u32)tx[i] << ((3 - i) * 8);
	xcode_soc_write(SOC_SPI_CFG, ((n - 1) << CFG_SIZE_SHIFT) | CFG_CAP_EDGE | (hold ? CFG_HOLD : 0));
	xcode_soc_write(SOC_SPI_WDAT, wdat);
	end = jiffies + HZ;
	while (xcode_soc_read(SOC_SPI_STATUS) & SPI_BUSY) {
		if (time_after(jiffies, end))
			return -ETIMEDOUT;
		cpu_relax();
	}
	if (rx) {
		rdat = xcode_soc_read(SOC_SPI_RDAT);
		for (i = 0; i < n; i++)
			rx[i] = (rdat >> ((n - 1 - i) * 8)) & 0xff;
	}
	return 0;
}

static int xspi_transfer_one_message(struct spi_controller *ctlr, struct spi_message *msg)
{
	struct spi_transfer *t;
	int ret = 0;

	ret = xspi_get_bus();
	if (ret)
		goto out;
	list_for_each_entry(t, &msg->transfers, transfer_list) {
		bool last = list_is_last(&t->transfer_list, &msg->transfers);
		/* 転送の最後で CS を上げるか: 最後の転送は上げる(cs_change なら保持)、途中の転送は保持する(cs_change なら上げる) */
		bool hold_end = last ? t->cs_change : !t->cs_change;
		unsigned int done = 0;

		while (done < t->len) {
			unsigned int n = min_t(unsigned int, 4, t->len - done);
			bool hold = (done + n < t->len) || hold_end;

			ret = xspi_chunk(t->tx_buf ? (const u8 *)t->tx_buf + done : NULL,
					 t->rx_buf ? (u8 *)t->rx_buf + done : NULL, n, hold);
			if (ret)
				goto put;
			done += n;
		}
		msg->actual_length += t->len;
		if (t->delay.value)
			spi_transfer_delay_exec(t);
	}
put:
	xspi_put_bus();
out:
	msg->status = ret;
	spi_finalize_current_message(ctlr);
	return 0;
}

static int xspi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct xspi *x;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*x));
	if (!ctlr)
		return -ENOMEM;
	x = spi_controller_get_devdata(ctlr);
	x->ctlr = ctlr;

	/* kexec で PPC がリセットされたままでも使えるように(i2c-xcode と同じ) */
	if (of_property_read_bool(dev->of_node, "vixs,init-ppc")) {
		xcode_soc_rmw(SOC_CG_RESET_REG, PPC_RESET, 0);
		xcode_soc_write(SOC_PPC_CLOCK_CTRL, 0x3c);
	}
	/* SPI ブロックのリセットと、転送速度の既定(27MHz/4 = 6.75MHz) */
	xcode_soc_rmw(SOC_PPC_SFT_RSTN, CCSPI_SFT_RSTN, 0);
	xcode_soc_rmw(SOC_PPC_SFT_RSTN, 0, CCSPI_SFT_RSTN);
	xcode_soc_rmw(SOC_SPI_CFG, CFG_RATE_MASK, 0);

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = 27000000 / 4;
	ctlr->transfer_one_message = xspi_transfer_one_message;
	platform_set_drvdata(pdev, ctlr);

	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return dev_err_probe(dev, ret, "SPI コントローラを登録できません\n");
	dev_info(dev, "XCode SPI マスタ(ポーリング)\n");
	return 0;
}

static const struct of_device_id xspi_of_match[] = {
	{ .compatible = "vixs,xcode-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, xspi_of_match);

static struct platform_driver xspi_driver = {
	.probe = xspi_probe,
	.driver = {
		.name = "xcode-spi",
		.of_match_table = xspi_of_match,
	},
};
module_platform_driver(xspi_driver);

MODULE_DESCRIPTION("ViXS XCode SPI master (Sony nasne)");
MODULE_LICENSE("GPL");
