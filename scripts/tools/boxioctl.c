/* xcode4drv(/dev/vixs/xcodedrv)の Boxster_Ioctl(0x17241724)を汎用に試す小ツール。libc不使用(静的)。
 *   boxioctl <cmd_hex> <size_hex> [<off_hex>=<value_hex>]...  [-d <dump_words>]
 * バッファ(size バイト、ゼロ初期化)に、ヘッダ(size_of_buf=size, command=cmd, return_value=2, time_out_value=2000@+0x94)を作り、
 * 指定した32bit値をオフセットに書いて ioctl を発行し、return_value(+0xc)と、バッファの先頭から指定語数(既定 0x40)を表示する。
 * ヘッダは0xa4バイト(IOCTLBUFFERHEADER)。本体は +0xa4 から。コマンド番号は docs/boxster_cmd_table.md。
 * 例: boxioctl 103 b0     (CMD_GET_FW_STATE: fw_state@+0xa4, cpu_id@+0xa8, epc@+0xac)
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o boxioctl boxioctl.c
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
static void hex8(unsigned long v) { char b[9]; int i; for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; b[i] = n < 10 ? '0' + n : 'a' + n - 10; } b[8] = 0; ws(b); }
static unsigned long parse_hex(const char *s) {
    unsigned long v = 0;
    for (; *s; s++) { int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * 16 + n; }
    return v;
}

static unsigned long buf[0x1000 / 4];

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 3) { ws("usage: boxioctl <cmd_hex> <size_hex> [<off_hex>=<val_hex>]... [-d <dump_words_hex>]\n"); return 1; }
    unsigned long cmd = parse_hex(argv[1]), size = parse_hex(argv[2]), dump = 0x40;
    int i;
    if (size > sizeof(buf)) size = sizeof(buf);
    buf[0] = size; buf[1] = cmd; buf[3] = 2; buf[0x94 / 4] = 2000;
    for (i = 3; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'd' && i + 1 < argc) { dump = parse_hex(argv[++i]); continue; }
        char *p = argv[i]; unsigned long off = 0;
        while (*p && *p != '=') { off = off * 16 + ((*p >= '0' && *p <= '9') ? *p - '0' : (*p | 32) - 'a' + 10); p++; }
        if (*p == '=' && off + 4 <= size) buf[off / 4] = parse_hex(p + 1);
    }
    long fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open /dev/vixs/xcodedrv failed\n"); return 2; }
    long r = sys3(SYS_ioctl, fd, 0x17241724, (long)buf);
    ws("ioctl="); hex8(r); ws(" return_value="); hex8(buf[3]); ws("\n");
    if (dump > size / 4) dump = size / 4;
    for (i = 0; i < (int)dump; i++) {
        if (i % 8 == 0) { hex8(i * 4); ws(":"); }
        ws(" "); hex8(buf[i]);
        if (i % 8 == 7 || i == (int)dump - 1) ws("\n");
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
