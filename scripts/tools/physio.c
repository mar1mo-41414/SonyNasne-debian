/* /dev/mem(uncached mmap)経由のSoCレジスタ/物理メモリ アクセスツール。libc不使用(静的、素のsyscall)。
 *   physio r <phys_hex> <nwords>        32bit語を読んで "addr: value" を表示
 *   physio w <phys_hex> <value_hex>     32bit語を1つ書く(書いた後に読み戻して表示)
 * 書き込みは危険(レジスタ直書き)。実験用。ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start
 */
#define SYS_exit   4001
#define SYS_read   4003
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_munmap 4091
#define SYS_mmap2  4210

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
static void hex8(unsigned long v) {
    char b[9]; int i;
    for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; b[i] = n < 10 ? '0' + n : 'a' + n - 10; }
    b[8] = 0; ws(b);
}
static unsigned long parse_hex(const char *s) {
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) { int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * 16 + n; }
    return v;
}

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 4) { ws("usage: physio r <phys_hex> <nwords> | physio w <phys_hex> <value_hex>\n"); return 1; }
    int wr = argv[1][0] == 'w';
    unsigned long phys = parse_hex(argv[2]);
    unsigned long arg = parse_hex(argv[3]);
    long fd = sys3(SYS_open, (long)"/dev/mem", (wr ? 2 : 0) | 04010000 /*O_SYNC*/, 0);
    if (fd < 0) { ws("open /dev/mem failed\n"); return 2; }
    unsigned long words = wr ? 1 : arg, i;
    for (i = 0; i < words; i++) {
        unsigned long cur = phys + i * 4, page = cur & ~0xfffUL, off = cur & 0xfffUL;
        /* PROT_READ|PROT_WRITE=3 or READ=1, MAP_SHARED=1 */
        long m = sys6(SYS_mmap2, 0, 0x1000, wr ? 3 : 1, 1, fd, page >> 12);
        if (m < 0 && m > -4096) { ws("mmap failed\n"); return 3; }
        volatile unsigned long *p = (volatile unsigned long *)(m + off);
        if (wr) { hex8(cur); ws(": old "); hex8(*p); *p = arg; ws(" new "); hex8(*p); ws("\n"); }
        else { hex8(cur); ws(": "); hex8(*p); ws("\n"); }
        sys3(SYS_munmap, m, 0x1000, 0);
    }
    sys3(SYS_close, fd, 0, 0);
    return 0;
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
