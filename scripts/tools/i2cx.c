/* 任意のI2Cブロックのデバイスを公式ドライバ経由(ioctl command 0x1b)で読み書きする。
 *   i2cx <block> <addr8hex> r <n>                 n バイト読む
 *   i2cx <block> <addr8hex> w <byte>...           書く
 *   i2cx <block> <addr8hex> g <reg>... <n>        regを書いてからn バイト読む(レジスタ読み出し。stopを挟む)
 *   i2cx <block> <demod8> p <tuner8> <n> [pre...] 復調IC経由(パススルー)でRFチューナーICを読む。dtvtunerと同じ手順:
 *        [pre...]があれば [0xfe,tuner8,pre...] を書き(stop有)、続けて [0xfe,tuner8|1] を書き(stopなし)、repeated startでn バイト読む
 *   i2cx <block> <addr8hex> d <reg0> <count>      reg0からcountバイトをレジスタ順に1バイトずつ読んでダンプ(復調器等のレジスタ空間用)
 * バス速度は10kHz固定。ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o i2cx i2cx.c
 */
#define SYS_exit   4001
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_ioctl  4054
#define SYS_read   4003
#define SYS_nanosleep 4166

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long v0 __asm__("$2") = n;
    register long a0 __asm__("$4") = a;
    register long a1 __asm__("$5") = b;
    register long a2 __asm__("$6") = c;
    register long a3 __asm__("$7") = d;
    __asm__ volatile(
        ".set push\n.set noat\n"
        "subu $29,$29,32\n"
        "sw %5,16($29)\n"
        "sw %6,20($29)\n"
        "syscall\n"
        "addu $29,$29,32\n"
        ".set pop\n"
        : "+r"(v0), "+r"(a3)
        : "r"(a0), "r"(a1), "r"(a2), "r"(e), "r"(f)
        : "memory", "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi", "lo");
    return a3 ? -v0 : v0;
}
#define sys3(n, a, b, c) sys6(n, a, b, c, 0, 0, 0)

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void ws(const char *s) { sys3(SYS_write, 1, (long)s, slen(s)); }
static void hex2(unsigned long v) { char b[3]; int n = (v >> 4) & 0xf; b[0] = n < 10 ? '0' + n : 'a' + n - 10; n = v & 0xf; b[1] = n < 10 ? '0' + n : 'a' + n - 10; b[2] = 0; ws(b); }
static unsigned long parse_hex(const char *s) {
    unsigned long v = 0;
    for (; *s; s++) { int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * 16 + n; }
    return v;
}

static unsigned long parse_hex_dec(const char *s) { unsigned long v = 0; for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0'); return v; }
static unsigned long buf[0x110 / 4 + 4];
static long fd;
static unsigned long blk, addr;

static unsigned long sbit = 1, pbit = 1;
static unsigned long xfer(int wr, unsigned char *d, int n) {
    int i;
    unsigned char *b = (unsigned char *)buf;
    for (i = 0; i < 0x110 / 4 + 4; i++) buf[i] = 0;
    if (wr) for (i = 0; i < n; i++) b[0xa4 + i] = d[i];
    buf[0x00 / 4] = 0x110; buf[0x04 / 4] = 0x1b; buf[0x0c / 4] = 2; buf[0x94 / 4] = 2000;
    buf[0xe4 / 4] = n; buf[0xe8 / 4] = wr; buf[0xec / 4] = addr;
    buf[0xf0 / 4] = 675; buf[0xf4 / 4] = 0; buf[0xf8 / 4] = 3375;
    buf[0xfc / 4] = blk; buf[0x100 / 4] = sbit; buf[0x104 / 4] = pbit;
    sys3(SYS_ioctl, fd, 0x17241724, (long)buf);
    if (!wr) for (i = 0; i < n; i++) d[i] = b[0xa4 + i];
    return buf[0x0c / 4];
}

static int run(int argc, char **argv) {
    unsigned char d[64];
    int i, n;
    if (argc < 5) { ws("usage: i2cx <block> <addr8hex> r <n> | w <b>... | g <reg>... <n> | h <reg> <n> | d <reg0> <count> | p <tuner8> <n> [pre...]   /  i2cx -b (標準入力から1行1コマンド)\n"); return 1; }
    blk = parse_hex(argv[1]); addr = parse_hex(argv[2]);
    char m = argv[3][0];
    if (m == 'r') {
        n = (int)parse_hex(argv[4]); if (n > 60) n = 60;
        unsigned long rv = xfer(0, d, n);
        ws("rv="); hex2(rv); ws(" data="); if (rv == 1) for (i = 0; i < n; i++) { hex2(d[i]); ws(" "); } ws("\n");
    } else if (m == 'w') {
        n = argc - 4; if (n > 60) n = 60;
        for (i = 0; i < n; i++) d[i] = (unsigned char)parse_hex(argv[4 + i]);
        unsigned long rv = xfer(1, d, n);
        ws("rv="); hex2(rv); ws("\n");
    } else if (m == 'g') {
        int nw = argc - 5; if (nw > 8) nw = 8;
        for (i = 0; i < nw; i++) d[i] = (unsigned char)parse_hex(argv[4 + i]);
        n = (int)parse_hex(argv[argc - 1]); if (n > 60) n = 60;
        unsigned long rv = xfer(1, d, nw);
        ws("write rv="); hex2(rv);
        if (rv == 1) { rv = xfer(0, d, n); ws(" read rv="); hex2(rv); ws(" data="); if (rv == 1) for (i = 0; i < n; i++) { hex2(d[i]); ws(" "); } }
        ws("\n");
    } else if (m == 'h') {   /* h <reg> <n>: [reg]を書き(stopなし)、repeated startでn バイト読む(dtvtunerの復調IC読み出しと同じ) */
        d[0] = (unsigned char)parse_hex(argv[4]);
        n = (int)parse_hex(argv[5]); if (n > 60) n = 60;
        pbit = 0; unsigned long rv = xfer(1, d, 1); pbit = 1;
        ws("write rv="); hex2(rv);
        if (rv == 1) { rv = xfer(0, d, n); ws(" read rv="); hex2(rv); ws(" data="); if (rv == 1) for (i = 0; i < n; i++) { hex2(d[i]); ws(" "); } }
        ws("\n");
    } else if (m == 'p') {
        unsigned long t8 = parse_hex(argv[4]);
        n = (int)parse_hex(argv[5]); if (n > 60) n = 60;
        int np = argc - 6; if (np > 8) np = 8;
        unsigned long rv = 1;
        if (np > 0) {
            d[0] = 0xfe; d[1] = (unsigned char)t8;
            for (i = 0; i < np; i++) d[2 + i] = (unsigned char)parse_hex(argv[6 + i]);
            rv = xfer(1, d, 2 + np);
            ws("pre-write rv="); hex2(rv); ws("  ");
        }
        if (rv == 1) {
            d[0] = 0xfe; d[1] = (unsigned char)(t8 | 1);
            pbit = 0; rv = xfer(1, d, 2); pbit = 1;
            ws("sel-read rv="); hex2(rv); ws("  ");
            if (rv == 1) { rv = xfer(0, d, n); ws("read rv="); hex2(rv); ws(" data="); if (rv == 1) for (i = 0; i < n; i++) { hex2(d[i]); ws(" "); } }
        }
        ws("\n");
    } else if (m == 'd') {
        unsigned long r0 = parse_hex(argv[4]), cnt = parse_hex(argv[5]), r;
        for (r = 0; r < cnt; r++) {
            unsigned char x = (unsigned char)(r0 + r), y = 0;
            if (r % 16 == 0) { ws("\n"); hex2(r0 + r); ws(": "); }
            if (xfer(1, &x, 1) == 1 && xfer(0, &y, 1) == 1) hex2(y); else ws("--");
            ws(" ");
        }
        ws("\n");
    }
    return 0;
}

static char line[512];
static char *tok[40];

/* -b: 標準入力の各行を1コマンドとして実行する(行頭が # は無視、"sleep <ms>" は待機)。結果は "> 行" の次に出力 */
static int batch(void) {
    int len = 0;
    char c;
    for (;;) {
        long r = sys3(SYS_read, 0, (long)&c, 1);
        if (r <= 0 && len == 0) break;
        if (r <= 0 || c == '\n') {
            line[len] = 0;
            if (len > 0 && line[0] != '#') {
                int nt = 1; char *p = line;
                ws("> "); ws(line); ws("\n");
                tok[0] = "i2cx";
                while (*p && nt < 39) {
                    while (*p == ' ') *p++ = 0;
                    if (!*p) break;
                    tok[nt++] = p;
                    while (*p && *p != ' ') p++;
                }
                if (nt >= 3 && tok[1][0] == 's' && tok[1][1] == 'l') {
                    unsigned long ms = parse_hex_dec(tok[2]);
                    long ts[2]; ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000;
                    sys3(SYS_nanosleep, (long)ts, 0, 0);
                } else run(nt, tok);
            }
            len = 0;
            if (r <= 0) break;
        } else if (len < 500) line[len++] = c;
    }
    return 0;
}

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int rc;
    fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open failed\n"); return 2; }
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == 'b') rc = batch();
    else rc = run(argc, argv);
    sys3(SYS_close, fd, 0, 0);
    return rc;
}

__asm__(".text\n.globl __start\n.ent __start\n__start:\n"
        "move $4,$29\n"
        "subu $29,$29,32\n"
        "jal main_c\n"
        "nop\n"
        "move $4,$2\n"
        "li $2,4001\n"
        "syscall\n"
        ".end __start\n");
