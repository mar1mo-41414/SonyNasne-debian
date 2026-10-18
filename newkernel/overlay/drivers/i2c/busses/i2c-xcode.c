// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)のハードウェア I2C マスタ(ブロック0/1)。
 *
 * MCU(電源・LED・ウォッチドッグ)はブロック0の 8ビットアドレス 0x20(7ビット 0x10)にいる。
 * レジスタ手順は、SIE 公開のメディアエンジンドライバ(drv/i2c.c の i2c_submit_command)と、ベンダーの reset.c
 * (MCU へ直接再起動コマンドを書いている)に基づく。
 *   - ARC ファームと共有するハードウェア・ミューテックス MIPS_INTERLOCK1(ID4)を取ってから触る
 *   - GPIO_I_CTRL のビット(29-ブロック)を落として I2C マスタモードにする
 *   - 速度: CONFIG のクロック分周 = 27MHz/(4*分周)。MCU は低速(10kHz = 分周 675)が前提。
 *     以前 100kHz(分周 67)で直叩きしたら MCU 側の I2C が固まった(電源サイクルが必要になった)ので、既定は 10kHz。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/jiffies.h>
#include <asm/mach-xcode/xcode-soc.h>

#define SOC_CG_RESET_REG	0x0000
#define  PPC_RESET		0x00000200
#define SOC_INTERLOCK1		0x0444
#define  ID4_SHIFT		16
#define  DRV_ID			9
#define SOC_PPC_CLOCK_CTRL	0x10a0
#define SOC_PPC_INT_STATUS	0x1094
#define SOC_PPC_MIPS_INT_MASK	0x109c
#define SOC_GPIO_I_CTRL		0xfc8c

#define CMD_ERROR		0x80000000
#define CMD_START		0x00000100
#define CMD_STOP		0x00000200
#define CMD_WRITE		0x00001000

struct xi2c {
	struct i2c_adapter adap;
	int block;
	unsigned int off_config, off_load, off_read, off_cmd;
	u32 m_done, m_arb, m_busy;
	u32 period, timeout;
};

static int xi2c_lock_hw(void)
{
	int tries = 0;

	while (((xcode_soc_read(SOC_INTERLOCK1) >> ID4_SHIFT) & 0xf) != DRV_ID) {
		if (++tries > 100000)
			return -EBUSY;
		xcode_soc_write(SOC_INTERLOCK1, (DRV_ID & 0xf) << ID4_SHIFT);
	}
	return 0;
}

static void xi2c_unlock_hw(void)
{
	if (((xcode_soc_read(SOC_INTERLOCK1) >> ID4_SHIFT) & 0xf) == DRV_ID) {
		xcode_soc_write(SOC_INTERLOCK1, (DRV_ID & 0xf) << ID4_SHIFT);
		(void)xcode_soc_read(SOC_INTERLOCK1);
	}
}

static int xi2c_msg(struct xi2c *x, struct i2c_msg *m)
{
	bool wr = !(m->flags & I2C_M_RD);
	u32 st = 0, cmd;
	unsigned long deadline = jiffies + msecs_to_jiffies(200);
	int i, k, err;

	if (m->len < 1 || m->len > 60)
		return -EINVAL;

	/* I2C マスタモードにする(GPIO_I_CTRL のビット(29-ブロック)を落とす) */
	if (xcode_soc_read(SOC_GPIO_I_CTRL) & (1u << (29 - x->block)))
		xcode_soc_rmw(SOC_GPIO_I_CTRL, 1u << (29 - x->block), 0);

	if (wr) {
		for (i = 0; i < m->len; i += 4) {
			u32 w = 0;

			for (k = 0; k < 4 && i + k < m->len; k++)
				w |= (u32)m->buf[i + k] << (8 * k);
			xcode_soc_write(x->off_load, w);
		}
	}
	xcode_soc_write(x->off_config, (x->timeout << 16) | x->period);
	cmd = ((u32)m->addr << 16) | (wr ? CMD_WRITE : 0) | CMD_START | CMD_STOP | m->len;
	xcode_soc_write(x->off_cmd, cmd);

	err = -ETIMEDOUT;
	while (time_before(jiffies, deadline)) {
		st = xcode_soc_read(SOC_PPC_INT_STATUS);
		if (st & (x->m_arb | x->m_busy)) {
			err = -EIO;
			break;
		}
		if (st & x->m_done) {
			err = (xcode_soc_read(x->off_cmd) & CMD_ERROR) ? -ENXIO : 0;
			break;
		}
		usleep_range(100, 300);
	}
	xcode_soc_write(SOC_PPC_INT_STATUS, st);	/* 書き込み1でクリア */

	if (!wr && !err) {
		for (i = 0; i < m->len; i += 4) {
			u32 w = xcode_soc_read(x->off_read);

			for (k = 0; k < 4 && i + k < m->len; k++)
				m->buf[i + k] = w >> (8 * k);
		}
	}
	return err;
}

static int xi2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct xi2c *x = i2c_get_adapdata(adap);
	u32 mask;
	int i, err = 0;

	err = xi2c_lock_hw();
	if (err)
		return err;
	mask = xcode_soc_read(SOC_PPC_MIPS_INT_MASK);
	xcode_soc_write(SOC_PPC_MIPS_INT_MASK, 0);	/* 割り込みは使わずポーリング */
	for (i = 0; i < num && !err; i++)
		err = xi2c_msg(x, &msgs[i]);
	xcode_soc_write(SOC_PPC_MIPS_INT_MASK, mask);
	xi2c_unlock_hw();
	return err ? err : num;
}

static u32 xi2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm xi2c_algo = {
	.master_xfer	= xi2c_xfer,
	.functionality	= xi2c_func,
};

static int xi2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct xi2c *x;
	u32 block = 0, freq = 10000;

	x = devm_kzalloc(dev, sizeof(*x), GFP_KERNEL);
	if (!x)
		return -ENOMEM;
	of_property_read_u32(dev->of_node, "vixs,block", &block);
	of_property_read_u32(dev->of_node, "clock-frequency", &freq);
	x->block = block;
	if (block == 0) {
		x->off_config = 0x1000; x->off_load = 0x1004; x->off_read = 0x1008; x->off_cmd = 0x100c;
		x->m_done = 0x00000040; x->m_arb = 0x00100000; x->m_busy = 0x00200000;
	} else {
		x->off_config = 0x1010; x->off_load = 0x1014; x->off_read = 0x1018; x->off_cmd = 0x101c;
		x->m_done = 0x00000080; x->m_arb = 0x02000000; x->m_busy = 0x04000000;
	}
	x->period = 27000000 / (4 * freq);
	x->timeout = 0x0d2f;

	/*
	 * ドライバの i2c_init は PPC のリセット解除とクロック設定(PPC_CLOCK_CTRL=0x3c)をするが、ここでは触らない。
	 * この機体では I2C ブロックはすでに動いており、PPC 側を勝手に動かすと(新カーネルでは)ユーザー空間が
	 * でたらめに落ちる現象と時期が重なったため。必要になったら dt の属性で有効にする。
	 */
	if (of_property_read_bool(dev->of_node, "vixs,init-ppc")) {
		xcode_soc_rmw(SOC_CG_RESET_REG, PPC_RESET, 0);
		xcode_soc_write(SOC_PPC_CLOCK_CTRL, 0x3c);
	}

	x->adap.owner = THIS_MODULE;
	x->adap.algo = &xi2c_algo;
	x->adap.dev.parent = dev;
	x->adap.dev.of_node = dev->of_node;
	x->adap.nr = block;
	snprintf(x->adap.name, sizeof(x->adap.name), "xcode-i2c%d", block);
	i2c_set_adapdata(&x->adap, x);
	return i2c_add_numbered_adapter(&x->adap) ?: 0;
}

static const struct of_device_id xi2c_of_match[] = {
	{ .compatible = "vixs,xcode-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, xi2c_of_match);

static struct platform_driver xi2c_driver = {
	.probe	= xi2c_probe,
	.driver	= { .name = "xcode-i2c", .of_match_table = xi2c_of_match },
};
module_platform_driver(xi2c_driver);

MODULE_DESCRIPTION("ViXS XCode 4210 hardware I2C master");
MODULE_LICENSE("GPL");
