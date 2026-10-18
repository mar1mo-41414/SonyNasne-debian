// SPDX-License-Identifier: GPL-2.0
/*
 * デバッグ: ユーザー空間が SIGILL/SIGSEGV で落ちるとき、epc 周りの命令を「ユーザー空間の仮想アドレス経由(Dキャッシュ越し)」と
 * 「物理メモリ(キャッシュなしの KSEG1 経由)」の両方で読んで比べる。違えば D キャッシュの書き戻し漏れ、
 * 同じなのに CPU が違う命令を実行したなら、I キャッシュ/PFB の古い内容。
 */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/pagemap.h>
#include <linux/crc32.h>
#include <linux/backing-dev.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/mmap_lock.h>
#include <linux/fs.h>
#include <asm/addrspace.h>
#include <asm/cacheflush.h>



/* 壊れたページの出どころ: どのファイルの何ページ目か、先読みの窓、前後のページの CRC32(ホストで正しい値と比べる) */
static void xcode_dbg_file_ctx(unsigned long epc, struct page *page)
{
	struct vm_area_struct *vma;
	struct file *f = NULL;
	char buf[160], *name = "?";
	unsigned long idx = 0, pgoff = 0;
	struct folio *folio = page_folio(page);

	mmap_read_lock(current->mm);
	vma = find_vma(current->mm, epc);
	if (vma && vma->vm_start <= epc && vma->vm_file) {
		f = get_file(vma->vm_file);
		pgoff = vma->vm_pgoff + ((epc - vma->vm_start) >> PAGE_SHIFT);
	}
	mmap_read_unlock(current->mm);
	if (!f) {
		pr_info("xcode-ctx: 匿名ページ(ファイル無し)\n");
		return;
	}
	name = d_path(&f->f_path, buf, sizeof(buf));
	if (IS_ERR(name))
		name = "?";
	idx = folio->index;
	pr_info("xcode-ctx: file=%s pgoff=%lu folio_index=%lu nr=%lu uptodate=%d ra(start=%lu size=%u async=%u ra_pages=%lu prev_pos=%lld) mapping_ra=%lu\n",
		name, pgoff, idx, folio_nr_pages(folio), folio_test_uptodate(folio),
		f->f_ra.start, f->f_ra.size, f->f_ra.async_size, f->f_ra.ra_pages, (long long)f->f_ra.prev_pos,
		(unsigned long)inode_to_bdi(f->f_mapping->host)->ra_pages);
	{
		long i;

		for (i = -8; i <= 8; i++) {
			struct folio *nf;

			if ((long)pgoff + i < 0)
				continue;
			nf = filemap_get_folio(f->f_mapping, pgoff + i);
			if (IS_ERR(nf)) {
				pr_info("xcode-ctx: idx=%ld: なし\n", (long)pgoff + i);
				continue;
			}
			{
				u32 *m = (u32 *)CKSEG1ADDR(page_to_phys(folio_page(nf, 0)));
				u32 c = crc32_le(~0U, (const u8 *)m, PAGE_SIZE) ^ ~0U;

				pr_info("xcode-ctx: idx=%ld phys=%08llx uptodate=%d crc=%08x%s\n", (long)pgoff + i,
					(unsigned long long)page_to_phys(folio_page(nf, 0)), folio_test_uptodate(nf), c,
					i == 0 ? "  <== epc" : "");
			}
			folio_put(nf);
		}
		/* 元データ側: ext4 のブロック(= loop デバイス上の位置 = root.img のページ番号)に対応する、root.img の XFS ページキャッシュの CRC32 */
		{
			struct file *img = filp_open("/data/.nasne-debian12/root.img", O_RDONLY | O_LARGEFILE, 0);

			if (IS_ERR(img)) {
				pr_info("xcode-ctx: root.img を開けない(%ld)\n", PTR_ERR(img));
			} else {
				for (i = -8; i <= 8; i++) {
					sector_t blk = pgoff + i;
					struct folio *xf;

					if ((long)pgoff + i < 0 || bmap(f->f_inode, &blk) || !blk) {
						pr_info("xcode-src: idx=%ld ブロック不明\n", (long)pgoff + i);
						continue;
					}
					xf = filemap_get_folio(img->f_mapping, blk);
					if (IS_ERR(xf)) {
						pr_info("xcode-src: idx=%ld blk=%llu: root.img のキャッシュなし\n", (long)pgoff + i, (unsigned long long)blk);
						continue;
					}
					{
						u32 *m = (u32 *)CKSEG1ADDR(page_to_phys(folio_page(xf, 0)));
						u32 c = crc32_le(~0U, (const u8 *)m, PAGE_SIZE) ^ ~0U;

						pr_info("xcode-src: idx=%ld blk=%llu phys=%08llx nr=%lu uptodate=%d crc=%08x\n", (long)pgoff + i,
							(unsigned long long)blk, (unsigned long long)page_to_phys(folio_page(xf, 0)), folio_nr_pages(xf),
							folio_test_uptodate(xf), c);
					}
					folio_put(xf);
				}
				fput(img);
			}
		}
	}
	fput(f);
}

void xcode_dbg_ifetch(struct pt_regs *regs)
{
	unsigned long epc = regs->cp0_epc & ~3UL;
	struct page *page;
	int i;

	if (!user_mode(regs))
		return;
	if (get_user_pages_fast(epc & PAGE_MASK, 1, 0, &page) != 1) {
		pr_info("xcode-dbg: epc=%08lx: ページを取得できません\n", epc);
		return;
	}
	pr_info("xcode-dbg: epc=%08lx phys=%08llx flags=%lx dcache_dirty=%d\n", epc,
		(unsigned long long)page_to_phys(page) + (epc & ~PAGE_MASK), page->flags,
		folio_test_dcache_dirty(page_folio(page)));
	xcode_dbg_file_ctx(epc, page);
	for (i = -4; i <= 4; i++) {
		unsigned long a = epc + i * 4;
		u32 wu = 0, wm = 0;
		u32 *mp;

		if ((a & PAGE_MASK) != (epc & PAGE_MASK))
			continue;
		if (get_user(wu, (u32 __user *)a))
			wu = 0xdeadbeef;
		mp = (u32 *)CKSEG1ADDR(page_to_phys(page) + (a & ~PAGE_MASK));
		wm = *mp;
		pr_info("xcode-dbg: %08lx user=%08x mem=%08x%s%s\n", a, wu, wm, wu != wm ? "  <== 違う" : "", i == 0 ? "  <== epc" : "");
	}
	/* 壊れたページの中身(ホスト側で出所を探すため): epc 周辺 64 ワードと、ページ先頭・末尾 8 ワード */
	{
		u32 *pg = (u32 *)CKSEG1ADDR(page_to_phys(page));
		unsigned int base = ((epc & ~PAGE_MASK) / 4) & ~15U;
		unsigned int j;

		(void)base;
		for (j = 0; j < PAGE_SIZE / 4; j += 8)
			pr_info("xcode-page: %04x: %08x %08x %08x %08x %08x %08x %08x %08x\n", j * 4,
				pg[j], pg[j + 1], pg[j + 2], pg[j + 3], pg[j + 4], pg[j + 5], pg[j + 6], pg[j + 7]);
		pr_info("xcode-dbg: head: %08x %08x %08x %08x %08x %08x %08x %08x\n", pg[0], pg[1], pg[2], pg[3], pg[4], pg[5], pg[6], pg[7]);
		pr_info("xcode-dbg: tail: %08x %08x %08x %08x %08x %08x %08x %08x\n", pg[PAGE_SIZE / 4 - 8], pg[PAGE_SIZE / 4 - 7],
			pg[PAGE_SIZE / 4 - 6], pg[PAGE_SIZE / 4 - 5], pg[PAGE_SIZE / 4 - 4], pg[PAGE_SIZE / 4 - 3], pg[PAGE_SIZE / 4 - 2],
			pg[PAGE_SIZE / 4 - 1]);
	}
	{
		/* Dキャッシュ越し(KSEG0・カーネル色)と実メモリ(KSEG1)でページ全体を比べる: 違えばエイリアスの書き戻し漏れ */
		u32 *p0 = (u32 *)page_address(page);
		u32 *p1 = (u32 *)CKSEG1ADDR(page_to_phys(page));
		unsigned int nd = 0, first = 0, w;

		for (w = 0; w < PAGE_SIZE / 4; w++)
			if (p0[w] != p1[w]) {
				if (!nd)
					first = w;
				nd++;
			}
		pr_info("xcode-dbg: KSEG0とKSEG1の差: %u ワード (最初 +%04x: k0=%08x k1=%08x)\n", nd, first * 4,
			nd ? p0[first] : 0, nd ? p1[first] : 0);
	}
	{
		u32 *pg = (u32 *)CKSEG1ADDR(page_to_phys(page));
		unsigned int k = (epc & ~PAGE_MASK) / 4;
		u32 before = pg[k];

		mdelay(300);
		pr_info("xcode-dbg: 300ms後 epc語: %08x -> %08x%s\n", before, pg[k], before != pg[k] ? "  <== 変化!(DMA遅れ?)" : "");
	}
	put_page(page);
}
