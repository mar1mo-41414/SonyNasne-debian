/* 公式ドライバ(xcode4drv)のI2Cコマンドストリーム(ioctl command 0x1b)で、指定I2Cブロックの全アドレスを
 * 1バイトreadで走査し、ACKするデバイスを一覧する(読み取りのみ)。ドライバ経由=公式ソフトと同じ経路。
 *   i2cscan <block 0|1> [hz]     既定 10000Hz
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o i2cscan i2cscan.c
 */
#define SYS_exit   4001
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_ioctl  4054

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

static unsigned long buf[0x110 / 4 + 4];
static void dec(unsigned long v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); ws(b + i); }

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 2) { ws("usage: i2cscan <block 0|1> [hz]\n"); return 1; }
    unsigned long blk = argv[1][0] - '0';
    unsigned long hz = 0;
    if (argc > 2) { const char *p = argv[2]; while (*p) hz = hz * 10 + (*p++ - '0'); }
    if (!hz) hz = 10000;
    long fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open failed\n"); return 2; }
    unsigned long a;
    int found = 0;
    for (a = 0x02; a < 0xfe; a += 2) {
        int i;
        for (i = 0; i < 0x110 / 4 + 4; i++) buf[i] = 0;
        buf[0x00 / 4] = 0x110; buf[0x04 / 4] = 0x1b; buf[0x0c / 4] = 2; buf[0x94 / 4] = 2000;
        buf[0xe4 / 4] = 1; buf[0xe8 / 4] = 0; buf[0xec / 4] = a;
        buf[0xf0 / 4] = 6750000 / hz; buf[0xf4 / 4] = 0; buf[0xf8 / 4] = 500 * 27 / 4;
        buf[0xfc / 4] = blk; buf[0x100 / 4] = 1; buf[0x104 / 4] = 1;
        sys3(SYS_ioctl, fd, 0x17241724, (long)buf);
        unsigned long rv = buf[0x0c / 4];
        if (rv == 1) { ws("ACK  addr8="); hex2(a); ws(" (7bit "); hex2(a >> 1); ws(") data="); hex2(((unsigned char *)buf)[0xa4]); ws("\n"); found++; }
        else { ws("."); }
    }
    ws("\nblock "); dec(blk); ws(": "); dec(found); ws(" device(s) ACKed\n");
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
