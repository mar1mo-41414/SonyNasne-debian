/* nasneのB-CASカードを、xcode4drv(/dev/vixs/xcodedrv)のジェネリックストリーム(GENERIC_MODE_BCAS)経由で読む実験ツール。libc不使用(静的)。
 *   bcas status|activate|id|deactivate ...   指定したコマンドを順に送り、受信イベントを表示
 *     status=CARDSTATUS(0=REMOVED 1=INSERTED 2=ACTIVED)、activate=ACTIVATE(ATR受信・活性化)、id=CARD_ID_ACQUIRE
 * 1回の実行内で OPEN_GENERIC → SEND → RECV → CLOSE_GENERIC を行う(ストリームは同じfdに結び付く)。
 * 前提: 完全なxcode4drv(rc.xcode4)が読み込み済みで、メディアエンジンのファームが生きている(boxioctl 103 b0 で fw_state=0)。
 * 構造体のオフセットは drv_if.h をコンパイルして確定(ヘッダ0xa4、SEND:0x1d0バイト、cardstatus@0xe0 等)。
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o bcas bcas.c
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
static void hex2(unsigned long v) { char b[3]; int n = (v >> 4) & 0xf; b[0] = n < 10 ? '0' + n : 'a' + n - 10; n = v & 0xf; b[1] = n < 10 ? '0' + n : 'a' + n - 10; b[2] = 0; ws(b); }

#define CMD_SDEV_SEND          0x13
#define CMD_SDEV_RECV          0x14
#define CMD_SDEV_OPEN_GENERIC  0x2e
#define CMD_SDEV_CLOSE_GENERIC 0x2f
#define GENERIC_MODE_BCAS      0x400
#define BCAS_CMD_CARD_ID_ACQUIRE 0x4
#define BCAS_CMD_CARDSTATUS    0x20

static unsigned long ob[0x208 / 4 + 4], sb[0x1d0 / 4 + 4], rb[0x240 / 4 + 4], cb[0xb4 / 4 + 4];
static unsigned char data[0x400];
static long fd;

static void hdr(unsigned long *b, unsigned long size, unsigned long cmd) {
    b[0] = size; b[1] = cmd; b[3] = 2; b[0x94 / 4] = 2000;
}
static unsigned long call(unsigned long *b, const char *name) {
    long r = sys3(SYS_ioctl, fd, 0x17241724, (long)b);
    ws(name); ws(": ioctl="); hex8(r); ws(" return_value="); hex8(b[3]); ws("\n");
    return b[3];
}

static unsigned long h0, h1;

/* 1コマンド送って、受信イベントを最大 n 回(各ブロック最大2秒)読む */
static unsigned long send_len;
static void run_cmd(const char *name, unsigned long cmd, int n) {
    int i, idle = 0;
    ws("\n== SEND "); ws(name); ws("\n");
    hdr(sb, 0x1d0, CMD_SDEV_SEND);
    sb[0xa4 / 4] = h0; sb[0xa8 / 4] = h1;
    sb[0xac / 4] = 0; sb[0xb0 / 4] = 0x10;   /* flags(64bit): bit36 = SDKラッパーと同じ(SDEV_IO_GENERIC_DATA相当) */
    sb[0xc4 / 4] = (unsigned long)data; sb[0xcc / 4] = send_len;
    sb[0xd4 / 4] = GENERIC_MODE_BCAS; sb[0xd8 / 4] = cmd; sb[0xdc / 4] = 1;
    unsigned long rv = call(sb, "SEND");
    ws("cardstatus="); hex8(sb[0xe0 / 4]); ws("  (0=REMOVED 1=INSERTED 2=ACTIVED)\n");
    if (rv != 1) return;
    for (i = 0; i < n; i++) {
        hdr(rb, 0x240, CMD_SDEV_RECV);
        rb[0xa4 / 4] = h0; rb[0xa8 / 4] = h1;
        rb[0xac / 4] = 0; rb[0xb0 / 4] = 0x10;
        rb[0xb4 / 4] = (unsigned long)data; rb[0xbc / 4] = sizeof(data);
        unsigned long rr = call(rb, "RECV");
        unsigned long got = rb[0xbc / 4], gr = rb[0x130 / 4];
        if (rr == 6) { if (++idle >= 4) break; continue; }   /* TIMEDOUT(各約2秒): 連続4回(約8秒)無ければ終了 */
        idle = 0;
        ws("  size_out="); hex8(got); ws(" generic_result="); hex8(gr); ws("\n");
        if (gr & 0x100) ws("  → カード挿入(INSERTED)\n");
        if (gr & 0x200) ws("  → カード抜去(REMOVED)\n");
        if (gr & 0x400) ws("  → カード活性化(ACTIVATED)\n");
        if (gr & 0x200000) ws("  → カード非活性(DEACTIVATED)\n");
        if (gr & 0x1000000) ws("  → ATR受信\n");
        if (gr & 0x80000) ws("  → B-CASメッセージ応答\n");
        if (gr & 0x40000) ws("  → ECM\n");
        if (rr == 1 && got) {
            int k; if (got > sizeof(data)) got = sizeof(data);
            ws("  data("); hex8(got); ws("):"); for (k = 0; k < (int)got && k < 96; k++) { ws(" "); hex2(data[k]); } ws("\n");
        }
        if (rr != 1) break;
    }
}

int main_c(long *sp) {
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int a;
    if (argc < 2) { ws("usage: bcas <status|activate|id|deactivate>...  (順に実行)\n"); return 1; }
    fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open /dev/vixs/xcodedrv failed\n"); return 2; }
    hdr(ob, 0x208, CMD_SDEV_OPEN_GENERIC);
    ob[0xac / 4] = GENERIC_MODE_BCAS; ob[0xb0 / 4] = 0x1000;
    if (call(ob, "OPEN_GENERIC(BCAS)") != 1) { ws("→ ストリームを開けない(ファーム停止の可能性。rc.xcode4で再読込して直後に再実行)\n"); return 3; }
    h0 = ob[0xa4 / 4]; h1 = ob[0xa8 / 4];
    for (a = 1; a < argc; a++) {
        char c = argv[a][0];
        if (c == 's') run_cmd("CARDSTATUS", BCAS_CMD_CARDSTATUS, 4);
        else if (c == 'a') run_cmd("ACTIVATE", 0x1, 8);
        else if (c == 'i') run_cmd("CARD_ID_ACQUIRE", BCAS_CMD_CARD_ID_ACQUIRE, 12);
        else if (c == 'd') run_cmd("DEACTIVATE", 0x10, 4);
        else if (c == 'm') {   /* m<hex>: BCASMSG(cmd 8)で生バイト列(APDU)を送る */
            const char *p = argv[a] + 1; int k = 0;
            while (p[0] && p[1] && k < 256) {
                int v = 0, j;
                for (j = 0; j < 2; j++) { char ch = p[j]; v = v * 16 + (ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10); }
                data[k++] = (unsigned char)v; p += 2;
            }
            send_len = k;
            run_cmd("BCASMSG", 0x8, 12);
            send_len = 0;
        }
    }
    hdr(cb, 0xb4, CMD_SDEV_CLOSE_GENERIC);
    cb[0xa4 / 4] = h0; cb[0xa8 / 4] = h1; cb[0xb0 / 4] = GENERIC_MODE_BCAS;
    call(cb, "CLOSE_GENERIC");
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
