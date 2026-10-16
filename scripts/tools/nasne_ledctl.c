/* nasne-ledctl: 前面の REC(赤)・LAN(橙)LED を、SoC の GPIO ポート10 で点灯/消灯する小さなツール。libc 不使用(静的、素のsyscall)。
 *   nasne-ledctl rec on|off     REC LED(GPIO ポート10 の bit 0x100)
 *   nasne-ledctl lan on|off     LAN LED(同 bit 0x200)
 *   nasne-ledctl status         OUT / EN の値と、REC・LAN の状態を表示
 * 触るのは bit 0x100 と 0x200 だけ(他のビットは読んだ値のまま書き戻す。他のビットはリセットや電源などにつながっている可能性がある)。
 * 出力許可(EN、0x...fc68)にその bit が立っていなければ立てる。読み→書きの間は flock で排他する(複数の呼び出しが重ならないように)。
 * PWR LED は MCU(mcui2c)、HDD LED はディスクのアクセスに連動する(このツールの対象外)。docs/06。
 * ビルド: mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -e __start
 */
#define SYS_exit   4001
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_munmap 4091
#define SYS_flock  4143
#define SYS_mmap2  4210

#define GPIO_PHYS_PAGE  0x1ffff000UL      /* SoC レジスタ窓(0x1fff0000)の中の GPIO ポート10 を含むページ */
#define OFF_OUT         0xc64UL           /* 0x1ffffc64: GPIO ポート10 の出力(High で点灯) */
#define OFF_EN          0xc68UL           /* 0x1ffffc68: 出力許可 */
#define LED_REC         0x100UL
#define LED_LAN         0x200UL

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
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void hex(unsigned long v) {
    char b[11]; int i;
    b[0] = '0'; b[1] = 'x';
    for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; b[2 + i] = n < 10 ? '0' + n : 'a' + n - 10; }
    b[10] = 0; ws(b);
}

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    unsigned long bit = 0; int on = 0, status = 0;

    if (argc == 2 && streq(argv[1], "status")) status = 1;
    else if (argc == 3 && (streq(argv[1], "rec") || streq(argv[1], "lan")) && (streq(argv[2], "on") || streq(argv[2], "off"))) {
        bit = streq(argv[1], "rec") ? LED_REC : LED_LAN;
        on = streq(argv[2], "on");
    } else { ws("usage: nasne-ledctl rec on|off | lan on|off | status\n"); return 1; }

    long fd = sys3(SYS_open, (long)"/dev/mem", (status ? 0 : 2) | 04010000 /*O_SYNC*/, 0);
    if (fd < 0) { ws("open /dev/mem failed\n"); return 2; }
    /* PROT_READ=1 | PROT_WRITE=2、MAP_SHARED=1。ページオフセットは(ページ番号)。O_SYNC 付きで開いた /dev/mem は uncached になる */
    long m = sys6(SYS_mmap2, 0, 0x1000, status ? 1 : 3, 1, fd, GPIO_PHYS_PAGE >> 12);
    if (m < 0 && m > -4096) { ws("mmap failed\n"); return 3; }
    volatile unsigned long *out = (volatile unsigned long *)(m + OFF_OUT);
    volatile unsigned long *en  = (volatile unsigned long *)(m + OFF_EN);

    if (status) {
        ws("OUT="); hex(*out); ws(" EN="); hex(*en);
        ws(" REC="); ws((*out & LED_REC) ? "on" : "off");
        ws(" LAN="); ws((*out & LED_LAN) ? "on" : "off"); ws("\n");
        return 0;
    }

    /* 読み→書きの間を排他する(別の nasne-ledctl と重ならないように)。ロックファイルが作れなくても、続行する(単独で動くだけ) */
    long lk = sys3(SYS_open, (long)"/var/run/nasne-ledctl.lock", 0100 | 02 /*O_CREAT|O_RDWR*/, 0644);
    if (lk >= 0) sys3(SYS_flock, lk, 2 /*LOCK_EX*/, 0);

    unsigned long e = *en;
    if (!(e & bit)) *en = e | bit;                 /* 出力許可(Debian の起動後は、すでに立っている) */
    unsigned long v = *out;
    *out = on ? (v | bit) : (v & ~bit);            /* ほかのビットは、読んだ値のまま */

    if (lk >= 0) sys3(SYS_close, lk, 0, 0);        /* close でロックが外れる */
    sys3(SYS_munmap, m, 0x1000, 0);
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
