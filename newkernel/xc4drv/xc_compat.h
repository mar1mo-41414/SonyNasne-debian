/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xcode4drv(2.6.29 向けの GPL 版 AVDriver)を Linux 6.12 でビルドするための互換定義。ccflags の -include で全ファイルの先頭に入る。
 * このプラットフォーム(XCode 4210 "Viper")では、DMA のバスアドレスは物理アドレスと同じ。キャッシュの整合は dma_map 系と dma_alloc_coherent が担う
 * (ベンダーのカーネルの pci_* は NULL デバイスで dma_* を呼んでいた)。
 */
#ifndef XC_COMPAT_H
#define XC_COMPAT_H

#include <linux/version.h>
#include <linux/types.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/semaphore.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>

/* ドライバが DMA に使う struct device(Os_Linux_Init.c が platform_device を作って入れる) */
extern struct device *xc_dma_dev;

#define virt_to_bus(a)		((unsigned long)virt_to_phys((void *)(a)))
#define bus_to_virt(a)		phys_to_virt((unsigned long)(a))
#define ioremap_nocache(a, s)	ioremap((a), (s))
#define init_MUTEX(s)		sema_init((s), 1)
#define page_cache_release(p)	put_page(p)

#define PCI_DMA_TODEVICE	DMA_TO_DEVICE
#define PCI_DMA_FROMDEVICE	DMA_FROM_DEVICE
#define pci_map_single(d, p, s, dir)	dma_map_single(xc_dma_dev, (p), (s), (dir))
#define pci_unmap_single(d, a, s, dir)	dma_unmap_single(xc_dma_dev, (a), (s), (dir))
#define pci_alloc_consistent(d, s, h)	dma_alloc_coherent(xc_dma_dev, (s), (h), GFP_KERNEL)
#define pci_free_consistent(d, s, v, h)	dma_free_coherent(xc_dma_dev, (s), (v), (h))

/* 6.12 には無い(元のコードは pXcode->fs_old に保存するだけで、set_fs は使っていない) */
typedef int mm_segment_t;
#define get_fs()		((mm_segment_t)0)

/* Os_Wrp_Funct.c が定義する、タイマーの起動(6.12 のタイマーは data を持たない) */
void xc_timer_start(struct timer_list *t, void (*fn)(unsigned long), unsigned long data, unsigned long expires);
#endif
