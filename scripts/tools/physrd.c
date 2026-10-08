/* 物理アドレスのレジスタ/RAMを/dev/memのmmap経由で読むだけの小ツール(書き込みなし)。
 *   physrd w <phys_hex> <nwords>    32bit語をuncachedで読んで "addr: value" を表示
 *   physrd r <phys_hex> <nbytes>    生バイトをstdoutへ(RAMのダンプ用、uncached)
 * glibcのバージョン非互換を避けるため標準スタートアップを使わず自前_startで組む。
 * ビルド: mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o physrd physrd.c
 */
extern long syscall(long number, ...);
#define SYS_open  4005
#define SYS_close 4006
#define SYS_write 4004
#define SYS_exit  4001
#define SYS_mmap2 4210
#define SYS_munmap 4091

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void wr(const void *p, unsigned long n) { syscall(SYS_write, 1, p, n); }
static void ws(const char *s) { wr(s, slen(s)); }
static void hex8(unsigned long v) {
    char b[9]; int i;
    for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; b[i] = n < 10 ? '0' + n : 'a' + n - 10; }
    b[8] = 0; ws(b);
}
static unsigned long parse_hex(const char *s) {
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10;
        v = v * 16 + n;
    }
    return v;
}

int main_c(int argc, char **argv) {
    if (argc < 4) { ws("usage: physrd w|r <phys_hex> <n>\n"); return 1; }
    int words = argv[1][0] == 'w';
    unsigned long phys = parse_hex(argv[2]);
    unsigned long n = parse_hex(argv[3]);
    long fd = syscall(SYS_open, "/dev/mem", 0 /*O_RDONLY*/ | 04010000 /*O_SYNC*/, 0);
    if (fd < 0) { ws("open /dev/mem failed\n"); return 2; }
    unsigned long total = words ? n * 4 : n;
    unsigned long done = 0;
    while (done < total) {
        unsigned long cur = phys + done;
        unsigned long page = cur & ~0xfffUL, off = cur & 0xfffUL;
        unsigned long chunk = 0x1000 - off; if (chunk > total - done) chunk = total - done;
        /* PROT_READ=1, MAP_SHARED=1; mmap2のオフセットはページ単位 */
        long m = syscall(SYS_mmap2, 0, 0x1000, 1, 1, fd, page >> 12);
        if (m < 0 && m > -4096) { ws("mmap failed at "); hex8(cur); ws("\n"); return 3; }
        volatile unsigned char *bp = (volatile unsigned char *)m + off;
        if (words) {
            unsigned long i;
            for (i = 0; i + 3 < chunk; i += 4) {
                unsigned long v = *(volatile unsigned long *)(bp + i);
                hex8(cur + i); ws(": "); hex8(v); ws("\n");
            }
        } else {
            static unsigned char buf[0x1000]; unsigned long i;
            for (i = 0; i < chunk; i++) buf[i] = bp[i];
            wr(buf, chunk);
        }
        syscall(SYS_munmap, m, 0x1000);
        done += chunk;
    }
    syscall(SYS_close, fd);
    return 0;
}

/* 引数は /proc/self/cmdline(NUL区切り)から取得する。standard startupを使わないため
 * スタック上のargvを自前で辿るより、PICの_startのままで安全に動く。 */
void _start(void) {
    static char cl[512];
    static char *av[8];
    int ac = 0;
    long fd = syscall(SYS_open, "/proc/self/cmdline", 0, 0);
    long n = fd >= 0 ? syscall(4003 /*read*/, fd, cl, sizeof(cl) - 1) : 0;
    if (n < 0) n = 0;
    cl[n] = 0;
    long i = 0;
    while (i < n && ac < 8) { av[ac++] = &cl[i]; while (i < n && cl[i]) i++; i++; }
    syscall(SYS_exit, main_c(ac, av));
    for (;;);
}
