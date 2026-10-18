// SPDX-License-Identifier: GPL-2.0
/*
 * Sony nasne の MCU(I2C バス0、7ビットアドレス 0x10 = 8ビットで 0x20)。
 * 書き込みは [レジスタ, 値] の2バイト。procmng の逆アセンブルで分かったコマンド(docs/19):
 *   [0x20,0x01] ウォッチドッグ停止(これをしないと約333秒でリセットされる)
 *   [0x20,0x00] 心拍(約5秒ごと。ウォッチドッグを止めずに生存を知らせる場合)
 *   [0x10,code] 表示コード(0x32=PWR LED 点灯、0x00=消灯、0x26=速い点滅、0x51=シャットダウン中)
 *   [0x02,0x01] 電源断(電源を切る。その後は AC を入れ直すまで起動しない)
 * 読み出しは 6 バイトのステータス(末尾2バイトは最後のコマンドからの経過、2バイト目の 0x80 は WD 停止中)。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/sysfs.h>

struct mcu {
	struct i2c_client *client;
	struct mutex lock;
};
static struct mcu *g_mcu;

static int mcu_write_buf(struct mcu *m, const u8 *b, int n)
{
	int r;

	mutex_lock(&m->lock);
	r = i2c_master_send(m->client, b, n);
	mutex_unlock(&m->lock);
	return r == n ? 0 : (r < 0 ? r : -EIO);
}

static int mcu_write(struct mcu *m, u8 reg, u8 val)
{
	u8 b[2] = { reg, val };

	return mcu_write_buf(m, b, 2);
}

static int mcu_status(struct mcu *m, u8 *buf)
{
	int r;

	mutex_lock(&m->lock);
	r = i2c_master_recv(m->client, buf, 6);
	mutex_unlock(&m->lock);
	return r == 6 ? 0 : (r < 0 ? r : -EIO);
}

static ssize_t status_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct mcu *m = dev_get_drvdata(dev);
	u8 s[6];
	int r = mcu_status(m, s);

	if (r)
		return r;
	return sysfs_emit(buf, "%02x %02x %02x %02x %02x %02x\n", s[0], s[1], s[2], s[3], s[4], s[5]);
}
static DEVICE_ATTR_RO(status);

static ssize_t display_code_store(struct device *dev, struct device_attribute *a, const char *buf, size_t n)
{
	struct mcu *m = dev_get_drvdata(dev);
	u8 v;
	int r = kstrtou8(buf, 16, &v);

	if (!r)
		r = mcu_write(m, 0x10, v);
	return r ? r : n;
}
static DEVICE_ATTR_WO(display_code);

/* 任意のバイト列(16進、空白区切り、最大 8 バイト)を MCU へ送る: echo "10 32" > raw、起床タイマは echo "02 02 00 00 27 10" > raw
 * (最初の1バイトがレジスタ。MCU のコマンドは [レジスタ, 値...]。docs/19) */
static ssize_t raw_store(struct device *dev, struct device_attribute *a, const char *buf, size_t n)
{
	struct mcu *m = dev_get_drvdata(dev);
	u8 b[8];
	unsigned int v;
	int cnt = 0, used, r;
	const char *p = buf;

	while (cnt < 8 && sscanf(p, " %x%n", &v, &used) == 1) {
		if (v > 0xff)
			return -EINVAL;
		b[cnt++] = v;
		p += used;
	}
	if (cnt < 2)
		return -EINVAL;
	r = mcu_write_buf(m, b, cnt);
	return r ? r : n;
}
static DEVICE_ATTR_WO(raw);

static struct attribute *mcu_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_display_code.attr,
	&dev_attr_raw.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mcu);

static int mcu_power_off(struct sys_off_data *data)
{
	struct mcu *m = data->cb_data;

	mcu_write(m, 0x10, 0x51);
	mcu_write(m, 0x02, 0x01);
	mdelay(1000);
	return NOTIFY_DONE;
}

/* 再起動: MCU に再起動を頼む([0x04,0x02]。ベンダーの reset.c と同じ)。MCU が電源を入れ直すので、cold boot で起動し直す */
static int mcu_restart(struct sys_off_data *data)
{
	struct mcu *m = data->cb_data;

	mcu_write(m, 0x10, 0x51);
	mcu_write(m, 0x04, 0x02);
	mdelay(3000);
	return NOTIFY_DONE;
}

static int mcu_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct mcu *m;
	u8 s[6];
	u32 led = 0x32;
	int r;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->client = client;
	mutex_init(&m->lock);
	i2c_set_clientdata(client, m);
	dev_set_drvdata(dev, m);
	of_property_read_u32(dev->of_node, "vixs,led-code", &led);

	r = mcu_status(m, s);
	if (r)
		return dev_err_probe(dev, r, "MCU のステータスが読めません\n");
	dev_info(dev, "MCU status %02x %02x %02x %02x %02x %02x\n", s[0], s[1], s[2], s[3], s[4], s[5]);

	/* ウォッチドッグ停止(procmng の MCU Watchdog Disable)と、PWR LED 点灯 */
	r = mcu_write(m, 0x20, 0x01);
	if (r)
		return dev_err_probe(dev, r, "ウォッチドッグ停止に失敗\n");
	mcu_write(m, 0x10, led);
	g_mcu = m;
	devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT, mcu_power_off, m);
	devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART, SYS_OFF_PRIO_DEFAULT, mcu_restart, m);
	dev_info(dev, "MCU のウォッチドッグを止めました\n");
	return 0;
}

static const struct of_device_id mcu_of_match[] = {
	{ .compatible = "sony,nasne-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, mcu_of_match);

static struct i2c_driver mcu_driver = {
	.driver = {
		.name = "nasne-mcu",
		.of_match_table = mcu_of_match,
		.dev_groups = mcu_groups,
	},
	.probe = mcu_probe,
};
module_i2c_driver(mcu_driver);

MODULE_DESCRIPTION("Sony nasne MCU (watchdog, LED, power)");
MODULE_LICENSE("GPL");
