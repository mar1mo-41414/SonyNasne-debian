/* 公式ドライバ(xcode4drv、/dev/vixs/xcodedrv)のI2Cコマンドストリーム(ioctl 0x17241724、command 0x1b)で、
 * nasneのMCU(I2Cバス0、8bitアドレス0x20)へ読み書きする最小ツール。libc不使用(静的)。
 * procmngの FUN_0040f980 と同じ要求を作る(バス0は10kHz: atomic_period=675, timeout=3375)。
 *   mcui2c w <addr8hex> <byte>...     書く。 例: mcui2c w 20 20 01   (= procmngの"MCU Watchdog Disable")
 *   mcui2c r <addr8hex> <nbytes>      読む
 * 前提: 完全な公式ドライバ(xcode4drv.ko、rc.xcode4で読込)がロード済みで /dev/vixs/xcodedrv があること。
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o mcui2c mcui2c.c
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

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 4) { ws("usage: mcui2c w <addr8hex> <byte>... | r <addr8hex> <nbytes>\n"); return 1; }
    int wr = argv[1][0] == 'w';
    unsigned long addr = parse_hex(argv[2]);
    unsigned char *b = (unsigned char *)buf;
    int n, i;
    if (wr) { n = argc - 3; if (n > 60) n = 60; for (i = 0; i < n; i++) b[0xa4 + i] = (unsigned char)parse_hex(argv[3 + i]); }
    else { n = (int)parse_hex(argv[3]); if (n > 60) n = 60; }
    /* IOCTLBUFFERHEADER: size / command / async / return_value ... */
    buf[0x00 / 4] = 0x110;          /* size_of_buf */
    buf[0x04 / 4] = 0x1b;           /* command = I2C_COMMAND_STREAM */
    buf[0x0c / 4] = 2;              /* return_value(初期値。成功すると1=COMPLETE) */
    buf[0x94 / 4] = 2000;           /* time_out_value */
    /* IOCTLI2CCOMMAND 本体(data_array[64] は +0xa4) */
    buf[0xe4 / 4] = n;              /* num_bytes */
    buf[0xe8 / 4] = wr ? 1 : 0;     /* rop: 1=write, 0=read */
    buf[0xec / 4] = addr;           /* slave_address(8bit形式。ドライバが>>1する) */
    buf[0xf0 / 4] = 675;            /* atomic_period (6750000/10000) */
    buf[0xf4 / 4] = 0;              /* address_mode(7bit) */
    buf[0xf8 / 4] = 3375;           /* timeout (500*27/4) */
    buf[0xfc / 4] = 0;              /* i2c_block_number = バス0 */
    buf[0x100 / 4] = 1;             /* startbit */
    buf[0x104 / 4] = 1;             /* stopbit */
    long fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open /dev/vixs/xcodedrv failed (完全なxcode4drvを読み込んでください)\n"); return 2; }
    long r = sys3(SYS_ioctl, fd, 0x17241724, (long)buf);
    unsigned long rv = buf[0x0c / 4];
    ws(wr ? "mcui2c write addr8=" : "mcui2c read  addr8="); hex2(addr); ws(" n="); hex2(n);
    ws(" ioctl="); hex2(r & 0xff); ws(" return_value="); hex2(rv);
    ws(rv == 1 ? " (COMPLETE)" : " (FAILED?)");
    if (!wr && rv == 1) { ws(" data="); for (i = 0; i < n; i++) { hex2(b[0xa4 + i]); ws(" "); } }
    ws("\n");
    sys3(SYS_close, fd, 0, 0);
    return rv == 1 ? 0 : 3;
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
