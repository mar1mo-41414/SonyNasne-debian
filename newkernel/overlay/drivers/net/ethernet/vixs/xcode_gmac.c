// SPDX-License-Identifier: GPL-2.0
/*
 * ViXS XCode 4210(Sony nasne)のオンチップ Ethernet(Synopsys DWC GMAC)。
 *
 * GMAC 本体のレジスタは、SoC ラッパの CSR 窓(アドレス/データの間接アクセス)越しにしか触れない。
 * そのため stmmac は使えず、この専用ドライバでディスクリプタ(enhanced 配置、16バイト)・DMA・PHY(MDIO)を扱う。
 * 元の振る舞いは、SIE が GPL で公開した 2.6.29 のドライバと実機のレジスタ値(live dump)で確認した。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_irq.h>
#include <linux/phy.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <asm/mach-xcode/xcode-gmac.h>
#include <asm/mach-xcode/xcode-soc.h>

#define DRV_NAME	"xcode-gmac"
/* #define XG_LED_DEBUG 1 */  /* デバッグ: poll 中は LED_A、xmit 中は LED_B を点灯 */
#define TX_RING		128
#define RX_RING		128
#define RX_BUF_LEN	1536		/* 32 の倍数(DMA の終端をキャッシュラインに揃える) */
#define RX_OFFSET	NET_IP_ALIGN	/* IP ヘッダを4バイト境界に */
#define TX_WAKE_THRESH	(TX_RING / 4)

static bool poll_mode;
module_param(poll_mode, bool, 0444);
MODULE_PARM_DESC(poll_mode, "割り込みを使わず、タイマで10msごとに NAPI を回す(デバッグ用)");

struct xg {
	struct net_device *ndev;
	struct device *dev;
	void __iomem *wrap;
	spinlock_t csr_lock;
	struct napi_struct napi;
	struct mii_bus *mii;
	struct phy_device *phy;
	int irq;
	int soc_irq_bit;

	struct xg_desc *tx, *rx;
	dma_addr_t tx_dma, rx_dma;
	struct sk_buff *tx_skb[TX_RING], *rx_skb[RX_RING];
	dma_addr_t tx_map[TX_RING], rx_map[RX_RING];
	unsigned int tx_head, tx_tail;	/* head: 次に積む位置、tail: 完了を待つ先頭 */
	unsigned int rx_next;
	spinlock_t tx_lock;
	int link, speed, duplex;
	unsigned long irq_calls, irq_none, irq_hits;
	struct timer_list poll_timer;
};

/* ---- CSR 窓 ---- */
static inline u32 w_rd(struct xg *g, unsigned int off)
{
	return readl(g->wrap + off);
}

static void csr_wait(struct xg *g)
{
	int n = 100000;

	while ((w_rd(g, XG_WRAP_STATUS) & XG_STATUS_BUSY) && --n)
		cpu_relax();
}

static u32 csr_rd(struct xg *g, u32 addr)
{
	unsigned long flags;
	u32 v;

	spin_lock_irqsave(&g->csr_lock, flags);
	csr_wait(g);
	writel(addr, g->wrap + XG_WRAP_CSR_CTRL);
	csr_wait(g);
	v = readl(g->wrap + XG_WRAP_RDAT);
	spin_unlock_irqrestore(&g->csr_lock, flags);
	return v;
}

static void csr_wr(struct xg *g, u32 addr, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&g->csr_lock, flags);
	csr_wait(g);
	writel(val, g->wrap + XG_WRAP_WDAT);
	writel(addr | XG_CSR_WRITE, g->wrap + XG_WRAP_CSR_CTRL);
	spin_unlock_irqrestore(&g->csr_lock, flags);
}

/* ---- MDIO ---- */
static int xg_mdio_wait(struct xg *g)
{
	int n = 10000;

	while (n--) {
		if (!(csr_rd(g, XG_MAC_GMII_ADDR) & XG_GMII_BUSY))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int xg_mdio_read(struct mii_bus *bus, int phy, int reg)
{
	struct xg *g = bus->priv;

	csr_wr(g, XG_MAC_GMII_ADDR, (phy << 11) | (reg << 6) | XG_GMII_CLK_100_150 | XG_GMII_BUSY);
	if (xg_mdio_wait(g))
		return -ETIMEDOUT;
	return csr_rd(g, XG_MAC_GMII_DATA) & 0xffff;
}

static int xg_mdio_write(struct mii_bus *bus, int phy, int reg, u16 val)
{
	struct xg *g = bus->priv;

	csr_wr(g, XG_MAC_GMII_DATA, val);
	csr_wr(g, XG_MAC_GMII_ADDR, (phy << 11) | (reg << 6) | XG_GMII_CLK_100_150 | XG_GMII_WRITE | XG_GMII_BUSY);
	return xg_mdio_wait(g);
}

/* ---- MAC ---- */
static u32 xg_mac_cfg(struct xg *g)
{
	u32 cfg = XG_CFG_IFG_88 | XG_CFG_DO | XG_CFG_IPC | XG_CFG_TE | XG_CFG_RE;

	if (g->duplex == DUPLEX_FULL)
		cfg |= XG_CFG_DM;
	if (g->speed == SPEED_10)
		cfg |= XG_CFG_PS;
	else if (g->speed == SPEED_100)
		cfg |= XG_CFG_PS | XG_CFG_FES;
	return cfg;
}


static void xg_adjust_link(struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);
	struct phy_device *phy = g->phy;
	bool changed = false;

	if (phy->link) {
		if (!g->link || g->speed != phy->speed || g->duplex != phy->duplex) {
			g->speed = phy->speed;
			g->duplex = phy->duplex;
			csr_wr(g, XG_MAC_CONFIG, xg_mac_cfg(g));
			changed = true;
		}
		g->link = 1;
	} else if (g->link) {
		g->link = 0;
		changed = true;
	}
	if (changed) {
		phy_print_status(phy);
	}
}

static void xg_set_mac_addr(struct xg *g, const u8 *a)
{
	csr_wr(g, XG_MAC_ADDR0_HI, (a[5] << 8) | a[4]);
	csr_wr(g, XG_MAC_ADDR0_LO, (a[3] << 24) | (a[2] << 16) | (a[1] << 8) | a[0]);
}

static void xg_read_mac_addr(struct xg *g, u8 *a)
{
	u32 hi = csr_rd(g, XG_MAC_ADDR0_HI), lo = csr_rd(g, XG_MAC_ADDR0_LO);

	a[0] = lo; a[1] = lo >> 8; a[2] = lo >> 16; a[3] = lo >> 24;
	a[4] = hi; a[5] = hi >> 8;
}

static void xg_set_rx_mode(struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);
	u32 ff = 0;

	if (ndev->flags & IFF_PROMISC)
		ff |= XG_FF_PR;
	else if ((ndev->flags & IFF_ALLMULTI) || !netdev_mc_empty(ndev))
		ff |= XG_FF_PM;		/* マルチキャストは全部通す(ハッシュは使わない) */
	csr_wr(g, XG_MAC_FRAME_FILTER, ff);
}

/* ---- リング ---- */
static int xg_rx_alloc(struct xg *g, struct sk_buff **pskb, dma_addr_t *pmap)
{
	struct sk_buff *skb;
	dma_addr_t map;
	unsigned int pad;

	skb = netdev_alloc_skb(g->ndev, RX_BUF_LEN + 2 * L1_CACHE_BYTES);
	if (!skb)
		return -ENOMEM;
	pad = (L1_CACHE_BYTES - ((unsigned long)skb->data & (L1_CACHE_BYTES - 1))) & (L1_CACHE_BYTES - 1);
	skb_reserve(skb, pad + RX_OFFSET);	/* data は (ライン境界 + 2)。DMA の終端 = ライン境界 + RX_BUF_LEN */
	map = dma_map_single(g->dev, skb->data, RX_BUF_LEN - RX_OFFSET, DMA_FROM_DEVICE);
	if (dma_mapping_error(g->dev, map)) {
		dev_kfree_skb_any(skb);
		return -ENOMEM;
	}
	*pskb = skb;
	*pmap = map;
	return 0;
}

/* バッファ(skb, map)を受信ディスクリプタ i に載せて、DMA に渡す */
static void xg_rx_set(struct xg *g, unsigned int i, struct sk_buff *skb, dma_addr_t map)
{
	g->rx_skb[i] = skb;
	g->rx_map[i] = map;
	g->rx[i].buf1 = map;
	g->rx[i].buf2 = 0;
	g->rx[i].ctrl = (RX_BUF_LEN - RX_OFFSET) | (i == RX_RING - 1 ? XG_RDES1_RER : 0);
	wmb();
	g->rx[i].status = XG_RDES0_OWN;
}

static void xg_free_rings(struct xg *g)
{
	int i;

	for (i = 0; i < RX_RING; i++)
		if (g->rx_skb[i]) {
			dma_unmap_single(g->dev, g->rx_map[i], RX_BUF_LEN - RX_OFFSET, DMA_FROM_DEVICE);
			dev_kfree_skb(g->rx_skb[i]);
			g->rx_skb[i] = NULL;
		}
	for (i = 0; i < TX_RING; i++)
		if (g->tx_skb[i]) {
			dma_unmap_single(g->dev, g->tx_map[i], g->tx_skb[i]->len, DMA_TO_DEVICE);
			dev_kfree_skb(g->tx_skb[i]);
			g->tx_skb[i] = NULL;
		}
}

static int xg_init_rings(struct xg *g)
{
	int i;

	memset(g->tx, 0, TX_RING * sizeof(*g->tx));
	memset(g->rx, 0, RX_RING * sizeof(*g->rx));
	g->tx[TX_RING - 1].status = XG_TDES0_TER;
	g->tx_head = g->tx_tail = g->rx_next = 0;
	for (i = 0; i < RX_RING; i++) {
		struct sk_buff *skb;
		dma_addr_t map;

		if (xg_rx_alloc(g, &skb, &map)) {
			xg_free_rings(g);
			return -ENOMEM;
		}
		xg_rx_set(g, i, skb, map);
	}
	return 0;
}

static void xg_hw_start(struct xg *g)
{
	/* DMA をリセットして組み直す(MAC の設定も既定に戻る) */
	int i;

	csr_wr(g, XG_DMA_BUS_MODE, XG_BUS_SWR);
	for (i = 0; i < 1000 && (csr_rd(g, XG_DMA_BUS_MODE) & XG_BUS_SWR); i++)
		udelay(100);

	csr_wr(g, XG_DMA_BUS_MODE, XG_BUS_FB | XG_BUS_PBL8);	/* DSL=0: リングモードの間隔は16バイト(ベンダーのチェーンモード用の DSL=2 は使わない) */
	xg_set_mac_addr(g, g->ndev->dev_addr);
	csr_wr(g, XG_MAC_CONFIG, xg_mac_cfg(g));
	xg_set_rx_mode(g->ndev);
	csr_wr(g, XG_DMA_RX_BASE, g->rx_dma);
	csr_wr(g, XG_DMA_TX_BASE, g->tx_dma);
	csr_wr(g, XG_MAC_INT_MASK, 0xffffffff);	/* MAC 側の割り込み(リンク変化・PMT など)は使わない(DMA ステータスの GLI/GMI/GPI が立ち続けて割り込みが止まらなくなる) */
	/* MMC(統計カウンタ)の割り込みもマスクする。送信バイト数が 2^31 に達すると上がり続けて、IRQ 6 が止まらなくなる(本体が固まる)。ステータスの GMI は書き込みでは消えない */
	csr_wr(g, XG_MMC_RX_INTR_MASK, 0xffffffff);
	csr_wr(g, XG_MMC_TX_INTR_MASK, 0xffffffff);
	csr_wr(g, XG_MMC_RX_IPC_INTR_MASK, 0xffffffff);
	csr_wr(g, XG_DMA_STATUS, 0xffffffff);
	csr_wr(g, XG_DMA_INT_EN, XG_IE_NIE | XG_IE_AIE | XG_IE_TI | XG_IE_RI | XG_IE_RU | XG_IE_FBE);
	csr_wr(g, XG_DMA_OP_MODE, XG_OP_SF | XG_OP_OSF | XG_OP_ST | XG_OP_SR);
	csr_wr(g, XG_DMA_RX_POLL, 1);
}

static void xg_hw_stop(struct xg *g)
{
	csr_wr(g, XG_DMA_INT_EN, 0);
	csr_wr(g, XG_DMA_OP_MODE, 0);
	csr_wr(g, XG_MAC_CONFIG, csr_rd(g, XG_MAC_CONFIG) & ~(XG_CFG_TE | XG_CFG_RE));
}

/* ---- 割り込み・NAPI ---- */
static irqreturn_t xg_irq(int irq, void *dev_id)
{
	struct xg *g = dev_id;
	u32 st, ds;

	st = w_rd(g, XG_WRAP_INT_STATUS);
	g->irq_calls++;
	if (!(st & XG_INT_SBD)) {
		g->irq_none++;
		return IRQ_NONE;
	}
	ds = csr_rd(g, XG_DMA_STATUS);
	g->irq_hits++;
	csr_wr(g, XG_DMA_STATUS, ds);	/* 受け取った分を確認応答(書き込み1でクリア) */
	if (ds & (XG_DS_TI | XG_DS_RI | XG_DS_RU | XG_DS_TU)) {
		csr_wr(g, XG_DMA_INT_EN, 0);
		napi_schedule(&g->napi);
	}
	return IRQ_HANDLED;
}

static void xg_tx_reap(struct xg *g)
{
	struct net_device *ndev = g->ndev;
	unsigned int done = 0;

	spin_lock(&g->tx_lock);
	while (g->tx_tail != g->tx_head) {
		unsigned int i = g->tx_tail % TX_RING;
		u32 st = g->tx[i].status;

		if (st & XG_TDES0_OWN)
			break;
		dma_unmap_single(g->dev, g->tx_map[i], g->tx_skb[i]->len, DMA_TO_DEVICE);
		if (st & XG_TDES0_ES) {
			ndev->stats.tx_errors++;
		} else {
			ndev->stats.tx_packets++;
			ndev->stats.tx_bytes += g->tx_skb[i]->len;
		}
		dev_consume_skb_any(g->tx_skb[i]);
		g->tx_skb[i] = NULL;
		g->tx_tail++;
		done++;
	}
	if (done && netif_queue_stopped(ndev) && (TX_RING - (g->tx_head - g->tx_tail)) > TX_WAKE_THRESH)
		netif_wake_queue(ndev);
	spin_unlock(&g->tx_lock);
}

static int xg_rx(struct xg *g, int budget)
{
	struct net_device *ndev = g->ndev;
	int done = 0;

	while (done < budget) {
		unsigned int i = g->rx_next;
		u32 st = g->rx[i].status;
		struct sk_buff *skb, *nskb;
		dma_addr_t nmap;
		int len;

		if (st & XG_RDES0_OWN)
			break;
		len = (st & XG_RDES0_FL_MASK) >> XG_RDES0_FL_SHIFT;
		if ((st & (XG_RDES0_FS | XG_RDES0_LS)) != (XG_RDES0_FS | XG_RDES0_LS) || (st & XG_RDES0_ES) ||
		    len < ETH_ZLEN + ETH_FCS_LEN || len > RX_BUF_LEN - RX_OFFSET) {
			ndev->stats.rx_errors++;
			goto reuse;
		}
		if (xg_rx_alloc(g, &nskb, &nmap)) {
			ndev->stats.rx_dropped++;
			goto reuse;
		}
		skb = g->rx_skb[i];
		dma_unmap_single(g->dev, g->rx_map[i], RX_BUF_LEN - RX_OFFSET, DMA_FROM_DEVICE);
		skb_put(skb, len - ETH_FCS_LEN);
		skb->protocol = eth_type_trans(skb, ndev);
		skb->ip_summed = CHECKSUM_NONE;
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += skb->len;
		napi_gro_receive(&g->napi, skb);
		xg_rx_set(g, i, nskb, nmap);
		goto next;
reuse:
		wmb();
		g->rx[i].status = XG_RDES0_OWN;
next:
		g->rx_next = (i + 1) % RX_RING;
		done++;
	}
	return done;
}

static void xg_poll_timer(struct timer_list *t)
{
	struct xg *g = from_timer(g, t, poll_timer);

	napi_schedule(&g->napi);
	mod_timer(&g->poll_timer, jiffies + 1);
}

static int xg_poll(struct napi_struct *napi, int budget)
{
	struct xg *g = container_of(napi, struct xg, napi);
	int done;

#ifdef XG_LED_DEBUG
	xcode_led_set(0x100);
#endif
	xg_tx_reap(g);
	done = xg_rx(g, budget);
	csr_wr(g, XG_DMA_RX_POLL, 1);		/* RU(受信バッファ枯渇)で止まっていたら再開 */
	if (done < budget && napi_complete_done(napi, done) && !poll_mode)
		csr_wr(g, XG_DMA_INT_EN, XG_IE_NIE | XG_IE_AIE | XG_IE_TI | XG_IE_RI | XG_IE_RU | XG_IE_FBE);
#ifdef XG_LED_DEBUG
	xcode_led_set(0);
#endif
	return done;
}

/* ---- net_device ---- */
static netdev_tx_t xg_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);
	unsigned long flags;
	unsigned int i;
	dma_addr_t map;

#ifdef XG_LED_DEBUG
	xcode_led_set(0x200);
#endif
	if (skb_linearize(skb) || skb->len > RX_BUF_LEN) {
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_irqsave(&g->tx_lock, flags);
	if (g->tx_head - g->tx_tail >= TX_RING) {
		netif_stop_queue(ndev);
		spin_unlock_irqrestore(&g->tx_lock, flags);
		return NETDEV_TX_BUSY;
	}
	map = dma_map_single(g->dev, skb->data, skb->len, DMA_TO_DEVICE);
	if (dma_mapping_error(g->dev, map)) {
		spin_unlock_irqrestore(&g->tx_lock, flags);
		dev_kfree_skb_any(skb);
		ndev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	i = g->tx_head % TX_RING;
	g->tx_skb[i] = skb;
	g->tx_map[i] = map;
	g->tx[i].buf1 = map;
	g->tx[i].buf2 = 0;
	g->tx[i].ctrl = skb->len;
	wmb();
	g->tx[i].status = XG_TDES0_OWN | XG_TDES0_IC | XG_TDES0_LS | XG_TDES0_FS | (i == TX_RING - 1 ? XG_TDES0_TER : 0);
	g->tx_head++;
	if (g->tx_head - g->tx_tail >= TX_RING)
		netif_stop_queue(ndev);
	spin_unlock_irqrestore(&g->tx_lock, flags);
	csr_wr(g, XG_DMA_TX_POLL, 1);
#ifdef XG_LED_DEBUG
	xcode_led_set(0);
#endif
	return NETDEV_TX_OK;
}

static int xg_open(struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);
	int err;

	/* 早期コンソール(同じ GMAC をポーリングで使っている)を止める。以降のログは netconsole で */
	xcode_earlycon_shutdown();
	err = xg_init_rings(g);
	if (err)
		return err;
	xcode_soc_rmw(XC_CG_CLK_STOP0, 0, XC_ETHR0CLK_STOP | XC_ETHR1CLK_STOP);	/* ベンダーのドライバと同じ */
	xg_hw_start(g);
	if (poll_mode) {
		csr_wr(g, XG_DMA_INT_EN, 0);
		timer_setup(&g->poll_timer, xg_poll_timer, 0);
		mod_timer(&g->poll_timer, jiffies + 1);
	} else {
		err = request_irq(g->irq, xg_irq, IRQF_SHARED, ndev->name, g);
		if (err)
			goto err_irq;
		writel(XG_INT_SBD, g->wrap + XG_WRAP_MIPS_INT_MASK);
		xcode_soc_irq_enable(g->soc_irq_bit);
	}
	napi_enable(&g->napi);
	phy_start(g->phy);
	netif_start_queue(ndev);
	return 0;

err_irq:
	xg_hw_stop(g);
	xg_free_rings(g);
	return err;
}

static int xg_stop(struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);

	netif_stop_queue(ndev);
	phy_stop(g->phy);
	napi_disable(&g->napi);
	if (poll_mode) {
		timer_delete_sync(&g->poll_timer);
	} else {
		xcode_soc_irq_disable(g->soc_irq_bit);
		writel(0, g->wrap + XG_WRAP_MIPS_INT_MASK);
	}
	xg_hw_stop(g);
	if (!poll_mode)
		free_irq(g->irq, g);
	xg_free_rings(g);
	return 0;
}

static int xg_set_mac(struct net_device *ndev, void *p)
{
	struct xg *g = netdev_priv(ndev);
	int err = eth_mac_addr(ndev, p);

	if (!err && netif_running(ndev))
		xg_set_mac_addr(g, ndev->dev_addr);
	return err;
}

static void xg_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct xg *g = netdev_priv(ndev);

	netdev_warn(ndev, "TX timeout (DMA status %08x, irq calls %lu none %lu hit %lu, wrap int %08x, soc int %08x/%08x)\n",
		    csr_rd(g, XG_DMA_STATUS), g->irq_calls, g->irq_none, g->irq_hits,
		    w_rd(g, XG_WRAP_INT_STATUS), xcode_soc_irq_status(), xcode_soc_read(0x4f8));
	csr_wr(g, XG_DMA_TX_POLL, 1);
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/* netconsole など: 割り込みが使えない状況でも、送信完了・受信を処理できるように */
static void xg_poll_controller(struct net_device *ndev)
{
	struct xg *g = netdev_priv(ndev);
	unsigned long flags;

	local_irq_save(flags);
	xg_irq(g->irq, g);
	local_irq_restore(flags);
}
#endif

static const struct net_device_ops xg_netdev_ops = {
	.ndo_open		= xg_open,
	.ndo_stop		= xg_stop,
	.ndo_start_xmit		= xg_xmit,
	.ndo_set_rx_mode	= xg_set_rx_mode,
	.ndo_set_mac_address	= xg_set_mac,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_eth_ioctl		= phy_do_ioctl_running,
	.ndo_tx_timeout		= xg_tx_timeout,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller	= xg_poll_controller,
#endif
};

static void xg_get_drvinfo(struct net_device *ndev, struct ethtool_drvinfo *info)
{
	strscpy(info->driver, DRV_NAME, sizeof(info->driver));
}

static const struct ethtool_ops xg_ethtool_ops = {
	.get_drvinfo		= xg_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= phy_ethtool_get_link_ksettings,
	.set_link_ksettings	= phy_ethtool_set_link_ksettings,
	.nway_reset		= phy_ethtool_nway_reset,
};

/* ---- probe ---- */


static int xg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *mdio_np, *phy_np;
	struct net_device *ndev;
	struct xg *g;
	struct resource *res;
	u8 mac[ETH_ALEN];
	int err;

	ndev = devm_alloc_etherdev(dev, sizeof(*g));
	if (!ndev)
		return -ENOMEM;
	SET_NETDEV_DEV(ndev, dev);
	g = netdev_priv(ndev);
	g->ndev = ndev;
	g->dev = dev;
	spin_lock_init(&g->csr_lock);
	spin_lock_init(&g->tx_lock);
	platform_set_drvdata(pdev, g);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g->wrap = devm_ioremap(dev, res->start, resource_size(res));
	if (!g->wrap)
		return -ENOMEM;
	g->irq = platform_get_irq(pdev, 0);
	if (g->irq < 0)
		return g->irq;
	g->soc_irq_bit = (res->start == 0x1fff2c00) ? XC_INT0_ETH1 : XC_INT0_ETH0;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return err;
	g->tx = dmam_alloc_coherent(dev, TX_RING * sizeof(struct xg_desc), &g->tx_dma, GFP_KERNEL);
	g->rx = dmam_alloc_coherent(dev, RX_RING * sizeof(struct xg_desc), &g->rx_dma, GFP_KERNEL);
	if (!g->tx || !g->rx)
		return -ENOMEM;

	/* MAC アドレス: DT → 4段目ブートの ethaddr= → 現在のレジスタ値 → ランダム */
	if (of_get_mac_address(np, mac) && !(xcode_get_boot_ethaddr(mac) && is_valid_ether_addr(mac))) {
		xg_read_mac_addr(g, mac);
		if (!is_valid_ether_addr(mac))
			eth_random_addr(mac);
	}
	eth_hw_addr_set(ndev, mac);

	/* MDIO */
	g->mii = devm_mdiobus_alloc(dev);
	if (!g->mii)
		return -ENOMEM;
	g->mii->name = "xcode-mdio";
	snprintf(g->mii->id, MII_BUS_ID_SIZE, "%s-mdio", dev_name(dev));
	g->mii->priv = g;
	g->mii->read = xg_mdio_read;
	g->mii->write = xg_mdio_write;
	g->mii->parent = dev;
	mdio_np = of_get_child_by_name(np, "mdio");
	err = of_mdiobus_register(g->mii, mdio_np);
	of_node_put(mdio_np);
	if (err)
		return dev_err_probe(dev, err, "mdio\n");

	phy_np = of_parse_phandle(np, "phy-handle", 0);
	g->phy = phy_np ? of_phy_connect(ndev, phy_np, xg_adjust_link, 0, PHY_INTERFACE_MODE_GMII) : NULL;
	of_node_put(phy_np);
	if (!g->phy) {
		mdiobus_unregister(g->mii);
		return dev_err_probe(dev, -ENODEV, "PHY が見つかりません\n");
	}
	phy_set_max_speed(g->phy, SPEED_1000);
	phy_support_asym_pause(g->phy);

	ndev->netdev_ops = &xg_netdev_ops;
	ndev->ethtool_ops = &xg_ethtool_ops;
	ndev->watchdog_timeo = 5 * HZ;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = RX_BUF_LEN - RX_OFFSET - ETH_HLEN - ETH_FCS_LEN - VLAN_HLEN;
	netif_napi_add(ndev, &g->napi, xg_poll);

	err = register_netdev(ndev);
	if (err) {
		phy_disconnect(g->phy);
		mdiobus_unregister(g->mii);
		return err;
	}
	netdev_info(ndev, "XCode GMAC: %pM\n", ndev->dev_addr);
	return 0;
}

static void xg_remove(struct platform_device *pdev)
{
	struct xg *g = platform_get_drvdata(pdev);

	unregister_netdev(g->ndev);
	phy_disconnect(g->phy);
	mdiobus_unregister(g->mii);
}

static const struct of_device_id xg_of_match[] = {
	{ .compatible = "vixs,xcode-gmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, xg_of_match);

static struct platform_driver xg_driver = {
	.probe	= xg_probe,
	.remove	= xg_remove,
	.driver	= {
		.name		= DRV_NAME,
		.of_match_table	= xg_of_match,
	},
};
module_platform_driver(xg_driver);

MODULE_DESCRIPTION("ViXS XCode 4210 Ethernet (Synopsys GMAC with CSR window)");
MODULE_LICENSE("GPL");
