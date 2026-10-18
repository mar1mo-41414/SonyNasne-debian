// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)の SATA(AHCI)ホスト。
 *
 * AHCI のレジスタは SoC の間接アクセス窓の向こう側にあるため、libahci を「readl/writel を窓経由に差し替えた」
 * 形でビルドしてある(libahci.c の先頭のパッチ。xcode_ahci_readl/xcode_ahci_writel を使う)。
 * このファイルは、SoC 固有の初期化(クロック・PHY・OOB)と、窓へのアクセス、ホストの組み立てを担う。
 * SoC 側の手順は、SIE が GPL で公開した 2.6.29 の ahci_vixs.c と、実機のレジスタ値に基づく。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/libata.h>
#include <linux/ahci_platform.h>
#include <linux/delay.h>
#define XC_AHCI_DMA_SETTLE_US 0
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <asm/mach-xcode/xcode-ahci.h>
#include <asm/mach-xcode/xcode-soc.h>
#include "ahci.h"
#include "libata.h"

#undef DRV_NAME
#define DRV_NAME "xcode-ahci"


/* SoC レジスタ窓(物理 0x1fff0000 から)のオフセット */
#define SOC_CG1_CLK_STOP0	0x0928
#define  SATACLK_STOP		0x20000000
#define SOC_CG1_BLK_CLK_STOP0	0x0930
#define  SATA_MCLK_STOP		0x00000400
#define SOC_CG_RESET_REG	0x0000
#define  SATA_RESET		0x00000080
#define SOC_INTERLOCK7		0x045c
#define  INTERLOCK_ID_SHIFT	4
#define  INTERLOCK_DRV_ID	0x1
#define SOC_SATA_SFT_RESETS	0xd010
#define SOC_SATA_CONTROL	0xd014
#define  SATA0_LOCAL_CLK_GATE	0x00000004
#define  SATA1_LOCAL_CLK_GATE	0x00040000
#define SOC_SATA0_PHY_TXRX	0xd03c
#define SOC_SATA1_PHY_TXRX	0xd040
#define SOC_SATA_PHY_MPLL	0xd044
#define  MPLL_SS_EN		0x00000008
#define SOC_SATA_PHY_ANALOG	0xd048
#define SOC_SATA_INT_STATUS	0xd018
#define  SATA0_INT		0x00000008
#define SOC_SATA_MIPS_INT_MASK	0xd01c

/* 間接アクセス窓(AHCI 側。物理 0x1fffd000 から) */
#define WIN_CTRL		0x00
#define WIN_STATUS		0x04
#define  WIN_BUSY		0x01
#define  WIN_ERR		0x02
#define WIN_WDAT		0x08
#define WIN_RDAT		0x0c
#define  WIN_WRITE		0x80000000
#define  WIN_SIZE_LONG		0x00200000
#define  WIN_HOST0		0x00010000
#define  WIN_HOST1		0x00020000

#define HOST_OOB		0xbc		/* ベンダー固有: OOB タイミング(クロック依存) */

static void __iomem *xc_win;		/* 間接アクセス窓 */
static void __iomem *xc_soc;		/* SoC レジスタ窓(ミューテックス用に直接触る) */
static DEFINE_RAW_SPINLOCK(xc_win_lock);

static void xc_mutex_lock(void)
{
	u32 id = (readl(xc_soc + SOC_INTERLOCK7) >> INTERLOCK_ID_SHIFT) & 0xf;

	while (id != INTERLOCK_DRV_ID) {
		writel(INTERLOCK_DRV_ID << INTERLOCK_ID_SHIFT, xc_soc + SOC_INTERLOCK7);
		id = (readl(xc_soc + SOC_INTERLOCK7) >> INTERLOCK_ID_SHIFT) & 0xf;
	}
}

static void xc_mutex_unlock(void)
{
	u32 id = (readl(xc_soc + SOC_INTERLOCK7) >> INTERLOCK_ID_SHIFT) & 0xf;

	if (id == INTERLOCK_DRV_ID) {
		writel(INTERLOCK_DRV_ID << INTERLOCK_ID_SHIFT, xc_soc + SOC_INTERLOCK7);
		(void)readl(xc_soc + SOC_INTERLOCK7);
	}
}

static void xc_win_wait(void)
{
	while (readl(xc_win + WIN_STATUS) & WIN_BUSY)
		cpu_relax();
}

bool xcode_ahci_irq_pending(void)
{
	bool p = xcode_soc_irq_status() & XC_INT0_SATA;

	if (p)
		udelay(XC_AHCI_DMA_SETTLE_US);	/* 実験: DMA の最後の書き込みが DRAM に届くのを待つ */
	return p;
}
EXPORT_SYMBOL_GPL(xcode_ahci_irq_pending);

void xcode_ahci_irq_ack(void)
{
	writel(SATA0_INT, xc_soc + SOC_SATA_INT_STATUS);
}
EXPORT_SYMBOL_GPL(xcode_ahci_irq_ack);

u32 xcode_ahci_readl(const volatile void __iomem *addr)
{
	unsigned long a = (unsigned long)addr, flags;
	u32 ctrl = (a & 0xffff) | WIN_SIZE_LONG | ((a & XCODE_AHCI_HOST1) ? WIN_HOST1 : WIN_HOST0);
	u32 v = 0;

	raw_spin_lock_irqsave(&xc_win_lock, flags);
	xc_mutex_lock();
	xc_win_wait();
	writel(ctrl, xc_win + WIN_CTRL);
	xc_win_wait();
	if (!(readl(xc_win + WIN_STATUS) & WIN_ERR))
		v = readl(xc_win + WIN_RDAT);
	xc_mutex_unlock();
	raw_spin_unlock_irqrestore(&xc_win_lock, flags);
	return v;
}
EXPORT_SYMBOL_GPL(xcode_ahci_readl);

void xcode_ahci_writel(u32 val, volatile void __iomem *addr)
{
	unsigned long a = (unsigned long)addr, flags;
	u32 ctrl = (a & 0xffff) | WIN_WRITE | WIN_SIZE_LONG | ((a & XCODE_AHCI_HOST1) ? WIN_HOST1 : WIN_HOST0);

	raw_spin_lock_irqsave(&xc_win_lock, flags);
	xc_mutex_lock();
	xc_win_wait();
	writel(val, xc_win + WIN_WDAT);
	writel(ctrl, xc_win + WIN_CTRL);
	xc_win_wait();
	xc_mutex_unlock();
	raw_spin_unlock_irqrestore(&xc_win_lock, flags);
}
EXPORT_SYMBOL_GPL(xcode_ahci_writel);

u32 xcode_ata_wait_register(struct ata_port *ap, void __iomem *reg, u32 mask, u32 val, unsigned int interval,
			    unsigned int timeout)
{
	unsigned long deadline;
	u32 tmp = xcode_ahci_readl(reg);

	deadline = ata_deadline(jiffies, timeout);
	while ((tmp & mask) == val && time_before(jiffies, deadline)) {
		ata_msleep(ap, interval);
		tmp = xcode_ahci_readl(reg);
	}
	return tmp;
}
EXPORT_SYMBOL_GPL(xcode_ata_wait_register);

/* SATA ブロックのクロック・リセット・PHY(ベンダーの xcode_ahci_host_init) */
static void xc_sata_hw_init(unsigned int clk_mhz)
{
	u32 t;

	t = readl(xc_soc + SOC_CG1_CLK_STOP0) & ~SATACLK_STOP;
	writel(t, xc_soc + SOC_CG1_CLK_STOP0);
	t = readl(xc_soc + SOC_CG1_BLK_CLK_STOP0) & ~SATA_MCLK_STOP;
	writel(t, xc_soc + SOC_CG1_BLK_CLK_STOP0);

	xcode_soc_rmw(SOC_CG_RESET_REG, SATA_RESET, 0);		/* リセットを外す */

	t = readl(xc_soc + SOC_SATA_CONTROL) & ~(SATA0_LOCAL_CLK_GATE | SATA1_LOCAL_CLK_GATE);
	writel(t, xc_soc + SOC_SATA_CONTROL);
	writel(0, xc_soc + SOC_SATA_SFT_RESETS);

	t = readl(xc_soc + SOC_SATA_PHY_MPLL) & ~0x7f6;
	t |= clk_mhz == 125 ? 0x54 : 0x460;			/* 50MHz(この機体)か 125MHz */
	t &= ~MPLL_SS_EN;					/* スペクトラム拡散なし */
	writel(t, xc_soc + SOC_SATA_PHY_MPLL);
	t = readl(xc_soc + SOC_SATA_PHY_MPLL) & ~0x2000;	/* 外部クロック周波数 */
	writel(t, xc_soc + SOC_SATA_PHY_MPLL);
	t = readl(xc_soc + SOC_SATA_PHY_MPLL) | 0x1000;		/* mp_ck オフ */
	writel(t, xc_soc + SOC_SATA_PHY_MPLL);
	t = readl(xc_soc + SOC_SATA_PHY_MPLL) & ~0x1000;	/* mp_ck オン */
	writel(t, xc_soc + SOC_SATA_PHY_MPLL);

	t = readl(xc_soc + SOC_SATA_PHY_ANALOG);
	t &= ~0x1fffff;
	t |= 0x911;
	writel(t, xc_soc + SOC_SATA_PHY_ANALOG);
	writel(0x23354030, xc_soc + SOC_SATA0_PHY_TXRX);
	writel(0x23354030, xc_soc + SOC_SATA1_PHY_TXRX);
	writel(0xff, xc_soc + SOC_SATA_SFT_RESETS);
}

static const struct ata_port_info xc_ahci_port_info = {
	.flags		= AHCI_FLAG_COMMON,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_ops,
};

static const struct scsi_host_template xc_ahci_sht = {
	AHCI_SHT(DRV_NAME),
};

static int xc_ahci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ata_port_info pi = xc_ahci_port_info;
	const struct ata_port_info *ppi[] = { &pi, NULL };
	struct ahci_host_priv *hpriv;
	struct ata_host *host;
	struct resource *res;
	u32 hostno = 0;
	int i, irq, n_ports, rc;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	irq = platform_get_irq(pdev, 0);
	if (!res || irq <= 0)
		return -ENODEV;
	of_property_read_u32(dev->of_node, "vixs,host", &hostno);

	xc_win = devm_ioremap(dev, res->start, resource_size(res));
	xc_soc = devm_ioremap(dev, 0x1fff0000, 0x10000);
	if (!xc_win || !xc_soc)
		return -ENOMEM;

	/* 初期化中の予期しない SATA 割り込みを避ける */
	xcode_soc_irq_disable(XC_INT0_SATA);
	xc_sata_hw_init(50);

	hpriv = devm_kzalloc(dev, sizeof(*hpriv), GFP_KERNEL);
	if (!hpriv)
		return -ENOMEM;
	/* mmio はポインタではなく「ホスト番号 | オフセット」の印(libahci は readl/writel を窓経由に差し替えてある) */
	hpriv->mmio = (void __iomem *)(unsigned long)(hostno ? XCODE_AHCI_HOST1 : 0);
	hpriv->irq = irq;
	hpriv->saved_port_map = 0x1;		/* ベンダーも PORTS_IMPL を 1 に固定している(1ホスト1ポート)。この値が強制される */
	hpriv->flags |= AHCI_HFLAG_NO_PMP | AHCI_HFLAG_SECT255;	/* PMP は使わない。1コマンド最大 255 セクタ(ベンダーのドライバも dev->max_sectors=255 に制限している) */

	/* SoC の OOB タイミング(50MHz クロック用の値) */
	xcode_ahci_writel(0x82060b13, hpriv->mmio + HOST_OOB);
	xcode_ahci_writel(0x82060b13, hpriv->mmio + HOST_OOB);

	pi.private_data = (void *)(unsigned long)hpriv->flags;
	ahci_save_initial_config(dev, hpriv);
	if (hpriv->cap & HOST_CAP_NCQ)
		pi.flags |= ATA_FLAG_NCQ;
	if (hpriv->cap & HOST_CAP_PMP)
		pi.flags |= ATA_FLAG_PMP;
	n_ports = max(ahci_nr_ports(hpriv->cap), fls(hpriv->port_map));

	host = ata_host_alloc_pinfo(dev, ppi, n_ports);
	if (!host)
		return -ENOMEM;
	host->private_data = hpriv;
	host->flags |= ATA_HOST_PARALLEL_SCAN;
	for (i = 0; i < host->n_ports; i++) {
		struct ata_port *ap = host->ports[i];

		ata_port_desc(ap, "host %u port 0x%x", hostno, 0x100 + ap->port_no * 0x80);
		if (!(hpriv->port_map & (1 << i)))
			ap->ops = &ata_dummy_port_ops;
	}

	rc = ahci_reset_controller(host);
	if (rc)
		return rc;
	ahci_init_controller(host);
	ahci_print_info(host, "xcode");

	/* SATA ブロックの割り込みを MIPS へ出す(ベンダーの vixs_ata_host_activate と同じ) */
	writel(readl(xc_soc + SOC_SATA_INT_STATUS), xc_soc + SOC_SATA_INT_STATUS);
	writel(readl(xc_soc + SOC_SATA_MIPS_INT_MASK) | (SATA0_INT << (hostno * 16)), xc_soc + SOC_SATA_MIPS_INT_MASK);
	xcode_soc_irq_enable(XC_INT0_SATA);
	return ahci_host_activate(host, &xc_ahci_sht);
}

static const struct of_device_id xc_ahci_of_match[] = {
	{ .compatible = "vixs,xcode-ahci" },
	{ }
};
MODULE_DEVICE_TABLE(of, xc_ahci_of_match);

static struct platform_driver xc_ahci_driver = {
	.probe	= xc_ahci_probe,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= xc_ahci_of_match,
	},
};
module_platform_driver(xc_ahci_driver);

MODULE_DESCRIPTION("ViXS XCode 4210 AHCI (indirect register window)");
MODULE_LICENSE("GPL");
