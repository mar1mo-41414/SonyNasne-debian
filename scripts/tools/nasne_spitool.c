/* nasne_spitool: SoC の SPI マスタを /dev/mem から直接叩いて、SPI フラッシュ(S25FL128P)の読み出し・照合・書き込みをする。
 * カーネルに SPI/MTD ドライバが無いとき(6.12 の開発初期)や、ドライバの検証用の最小ツール。レジスタ手順は
 * newkernel/overlay/drivers/spi/spi-xcode.c と同じ(ベンダーの xcode_spi.c に基づく)。
 *   nasne_spitool id
 *   nasne_spitool read   <オフセット> <長さ> <出力ファイル>
 *   nasne_spitool verify <オフセット> <ファイル>             フラッシュの内容がファイルと一致するか
 *   nasne_spitool flash  <オフセット> <ファイル>             消去→書き込み→読み戻し照合
 * 書き込みは KNL スロットA(0x100000〜0x37ffff、64KB 境界)だけに制限(ブートチェーンや B スロット、NV は触れない)。
 * ビルド: mipsel-linux-gnu-gcc -static -O2 -o nasne_spitool nasne_spitool.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

#define WIN0 0x1fff0000UL	/* SoC レジスタ窓 0x0000〜0x0fff */
#define WIN1 0x1fff1000UL	/* 0x1000〜0x1fff (PPC ブロック: SPI) */
#define R_INTERLOCK2	0x448
#define R_BOARD_ID	0x478
#define R_PADU_CTRL	0x4a8
#define MUTEX_ID	11
#define R_SPI_CFG	0x0ac
#define R_SPI_WDAT	0x0b0
#define R_SPI_RDAT	0x0b4
#define R_SPI_STATUS	0x0b8
#define SLOT_A_START	0x100000
#define SLOT_A_END	0x380000
#define SECTOR		0x10000
#define PAGE		256

static volatile uint32_t *w0, *w1;

static uint32_t rd0(unsigned o) { return w0[o / 4]; }
static void wr0(unsigned o, uint32_t v) { w0[o / 4] = v; }
static uint32_t rd1(unsigned o) { return w1[o / 4]; }
static void wr1(unsigned o, uint32_t v) { w1[o / 4] = v; }

static int get_bus(void)
{
	time_t end = time(NULL) + 2;
	uint32_t reg, want;

	for (;;) {
		wr0(R_INTERLOCK2, MUTEX_ID << 16);
		if (((rd0(R_INTERLOCK2) >> 16) & 0xf) == MUTEX_ID)
			break;
		if (time(NULL) > end) { fprintf(stderr, "ハードウェア・ミューテックスを取れません\n"); return -1; }
		usleep(1000);
	}
	reg = rd0(R_PADU_CTRL);
	want = rd0(R_BOARD_ID) & 1;
	if (want != ((reg >> 8) & 1))
		wr0(R_PADU_CTRL, want ? (reg | 0x100) : (reg & ~0x100u));
	return 0;
}

static void put_bus(void)
{
	if (((rd0(R_INTERLOCK2) >> 16) & 0xf) == MUTEX_ID)
		wr0(R_INTERLOCK2, MUTEX_ID << 16);
}

static int chunk(const uint8_t *tx, uint8_t *rx, unsigned n, int hold)
{
	uint32_t wdat = 0, rdat;
	unsigned i;
	long spin = 0;

	if (tx)
		for (i = 0; i < n; i++)
			wdat |= (uint32_t)tx[i] << ((3 - i) * 8);
	wr1(R_SPI_CFG, ((n - 1) << 1) | 0x8 | (hold ? 1 : 0));
	wr1(R_SPI_WDAT, wdat);
	while (rd1(R_SPI_STATUS) & 1)
		if (++spin > 5000000) { fprintf(stderr, "SPI がビジーのまま\n"); return -1; }
	if (rx) {
		rdat = rd1(R_SPI_RDAT);
		for (i = 0; i < n; i++)
			rx[i] = (rdat >> ((n - 1 - i) * 8)) & 0xff;
	}
	return 0;
}

/* 長さ len の転送(tx/rx は NULL 可)。最後の転送の最後で CS を上げる。hold_end=1 なら保持 */
static int xfer(const uint8_t *tx, uint8_t *rx, unsigned len, int hold_end)
{
	unsigned done = 0;

	while (done < len) {
		unsigned n = len - done > 4 ? 4 : len - done;
		int hold = (done + n < len) || hold_end;

		if (chunk(tx ? tx + done : NULL, rx ? rx + done : NULL, n, hold))
			return -1;
		done += n;
	}
	return 0;
}

static int rdsr(uint8_t *sr)
{
	uint8_t t[2] = { 0x05, 0 }, r[2];

	if (xfer(t, r, 2, 0)) return -1;
	*sr = r[1];
	return 0;
}

static int wait_ready(int sec)
{
	time_t end = time(NULL) + sec;
	uint8_t sr;

	for (;;) {
		if (rdsr(&sr)) return -1;
		if (!(sr & 1)) return 0;
		if (time(NULL) > end) { fprintf(stderr, "書き込み/消去が終わらない(SR=%02x)\n", sr); return -1; }
		usleep(2000);
	}
}

static int wren(void)
{
	uint8_t c = 0x06, sr;

	if (xfer(&c, NULL, 1, 0)) return -1;
	if (rdsr(&sr)) return -1;
	if (!(sr & 2)) { fprintf(stderr, "WEL が立たない(SR=%02x)\n", sr); return -1; }
	return 0;
}

static int read_flash(unsigned off, uint8_t *buf, unsigned len)
{
	uint8_t c[4] = { 0x03, off >> 16, off >> 8, off };

	if (xfer(c, NULL, 4, 1)) return -1;
	return xfer(NULL, buf, len, 0);
}

static int read_id(uint8_t *id)
{
	uint8_t t = 0x9f;

	if (xfer(&t, NULL, 1, 1)) return -1;
	return xfer(NULL, id, 5, 0);
}

static uint8_t *load(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;

	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	b = malloc(*len);
	if (!b || fread(b, 1, *len, f) != (size_t)*len) { fprintf(stderr, "読めません: %s\n", path); fclose(f); return NULL; }
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	int fd, rc = 1;
	uint8_t id[5];
	unsigned off;
	const char *cmd = argc > 1 ? argv[1] : "";

	if (argc < 2) { fprintf(stderr, "usage: id | read OFF LEN FILE | verify OFF FILE | flash OFF FILE\n"); return 1; }
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	w0 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, WIN0);
	w1 = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, WIN1);
	if (w0 == MAP_FAILED || w1 == MAP_FAILED) { perror("mmap"); return 1; }
	if (get_bus()) return 1;

	if (read_id(id)) goto out;
	if (!strcmp(cmd, "id")) {
		printf("JEDEC ID: %02x %02x %02x %02x %02x\n", id[0], id[1], id[2], id[3], id[4]);
		rc = 0;
	} else if (!strcmp(cmd, "read") && argc == 5) {
		unsigned len = strtoul(argv[3], NULL, 0);
		uint8_t *b = malloc(len);
		FILE *f = fopen(argv[4], "wb");

		off = strtoul(argv[2], NULL, 0);
		if (b && f && !read_flash(off, b, len) && fwrite(b, 1, len, f) == len) rc = 0;
		if (f) fclose(f);
	} else if ((!strcmp(cmd, "verify") || !strcmp(cmd, "flash")) && argc == 4) {
		long len;
		uint8_t *img = load(argv[3], &len), *back;
		unsigned p, s;

		off = strtoul(argv[2], NULL, 0);
		if (!img) goto out;
		if (!strcmp(cmd, "flash")) {
			unsigned end = (off + len + SECTOR - 1) & ~(SECTOR - 1);

			if (id[0] != 0x01 || id[1] != 0x20 || id[2] != 0x18) { fprintf(stderr, "想定外のフラッシュ ID\n"); goto out; }
			if (off % SECTOR || off < SLOT_A_START || end > SLOT_A_END) {
				fprintf(stderr, "書けるのは KNL スロットA(0x%x〜0x%x、64KB 境界)だけ\n", SLOT_A_START, SLOT_A_END);
				goto out;
			}
			for (s = off; s < end; s += SECTOR) {
				uint8_t e[4] = { 0xd8, s >> 16, s >> 8, s };

				if (wren() || xfer(e, NULL, 4, 0) || wait_ready(10)) goto out;
			}
			for (p = off; p < off + len; p += PAGE) {
				unsigned n = off + len - p < PAGE ? off + len - p : PAGE, i;
				uint8_t c[4] = { 0x02, p >> 16, p >> 8, p };

				for (i = 0; i < n && img[p - off + i] == 0xff; i++) ;
				if (i == n) continue;			/* 全部 0xFF のページは書かない */
				if (wren() || xfer(c, NULL, 4, 1) || xfer(img + (p - off), NULL, n, 0) || wait_ready(2)) goto out;
			}
		}
		back = malloc(len);
		if (!back || read_flash(off, back, len)) goto out;
		if (memcmp(back, img, len)) {
			for (p = 0; p < (unsigned)len && back[p] == img[p]; p++) ;
			printf("%s 不一致(最初の違い 0x%x)\n", cmd, off + p);
		} else {
			printf("%s OK %ld バイト @0x%x\n", cmd, len, off);
			rc = 0;
		}
	} else {
		fprintf(stderr, "usage: id | read OFF LEN FILE | verify OFF FILE | flash OFF FILE\n");
	}
out:
	put_bus();
	return rc;
}
