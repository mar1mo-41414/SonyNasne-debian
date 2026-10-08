/* xcode4drv(/dev/vixs/xcodedrv)の ioctl を、同一プロセス・同一fdで連続して実行するスクリプト実行ツール。libc不使用(静的)。
 * ストリームのハンドルはfdに紐づくため、B-CAS(ジェネリック)ストリームとTSストリームを同時に開く実験に使う。
 *   drvsh < script       (標準入力から1行1コマンド。 # で始まる行は無視)
 *
 * コマンド:
 *   io <name> <cmd_hex> <size_hex> [off=val ...]   ioctlを1回発行。ヘッダ(size/command/return_value=2/timeout=2000)は自動。
 *       val は 16進数、@<name2>:<off>(保存済み応答 name2 の +off の32bit語)、または &<name2>(スロットのアドレス。バッファ引数用)。応答全体を name で保存。
 *   dump <name> <from_hex> <to_hex>                保存済み応答の非ゼロ語を表示
 *   rx <name> <handle_name:off> <flags_lo> <flags_hi> <iters>
 *       RECV(0x14, サイズ0x230)を iters 回。成功した応答の +0xbc〜 の非ゼロ語を表示(B-CASのイベント受信などに使う)
 *   ts <openname> <handle_off> <tbl_off> <flags_lo> <iters> <outfile> [stride_hex] [skip_hex]
 *       TS受信ループ。openname の応答にある出力バッファ表(+tbl_off、ユーザアドレスの並び)からデータを読み、
 *       前回のindexを+0x144で返して解放し、統計(同期バイト、PID別、スクランブル数)を表示。outfile に保存。
 *       stride = パケット間隔(188=通常TS、192=タイムスタンプ付きTS)、skip = パケット先頭からTSヘッダまでのバイト数(192なら4)。既定 188/0。
 *   sleep <ms>
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o drvsh drvsh.c
 */
#define SYS_exit   4001
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_ioctl  4054
#define SYS_read   4003
#define SYS_nanosleep 4166
#define SYS_creat  4008

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


#define NSLOT 12
#define SLOTW 0x500                     /* 1スロット = 0x500語(最大のOPEN構造体 0x9ec を含む) */
static unsigned long slot[NSLOT][SLOTW];
static char slotname[NSLOT][16];
static int nslots;
static long fd;
static unsigned long pidcnt[8192], scrcnt[8192];
static char line[1024];
static char *tok[48];

static unsigned long parse_hex(const char *s) { unsigned long v = 0; for (; *s; s++) { int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * 16 + n; } return v; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned long *getslot(const char *name, int create) {
    int i;
    for (i = 0; i < nslots; i++) if (streq(slotname[i], name)) return slot[i];
    if (!create || nslots >= NSLOT) return 0;
    for (i = 0; name[i] && i < 15; i++) slotname[nslots][i] = name[i];
    slotname[nslots][i] = 0;
    return slot[nslots++];
}
static unsigned long val(const char *s) {
    if (s[0] == '&') { unsigned long *b = getslot(s + 1, 1); return (unsigned long)b; }       /* &name: スロットのアドレス(バッファ用) */
    if (s[0] == '@') {
        char nm[16]; int i = 0; const char *p = s + 1; unsigned long *b;
        while (*p && *p != ':' && i < 15) nm[i++] = *p++;
        nm[i] = 0;
        b = getslot(nm, 0);
        if (!b || *p != ':') return 0;
        return b[parse_hex(p + 1) / 4];
    }
    return parse_hex(s);
}
static void dump_nz(unsigned long *b, unsigned long from, unsigned long to) {
    unsigned long i;
    for (i = from / 4; i < to / 4; i++) if (b[i]) { ws("  +"); hex8(i * 4); ws(": "); hex8(b[i]); ws("\n"); }
}
static unsigned long ioc(unsigned long *b, const char *tag) {
    long r = sys3(SYS_ioctl, fd, 0x17241724, (long)b);
    ws(tag); ws(": ioctl="); hex8(r); ws(" return_value="); hex8(b[3]); ws("\n");
    return b[3];
}
static int split_name(const char *s, char *nm, unsigned long *off, unsigned long defoff) {
    int k = 0; const char *p = s;
    while (*p && *p != ':' && k < 15) nm[k++] = *p++;
    nm[k] = 0;
    *off = *p == ':' ? parse_hex(p + 1) : defoff;
    return 0;
}

static void cmd_io(int n, char **t) {
    unsigned long *b; int i;
    if (n < 4) return;
    b = getslot(t[1], 1); if (!b) { ws("slot full\n"); return; }
    for (i = 0; i < SLOTW; i++) b[i] = 0;
    b[0] = parse_hex(t[3]); b[1] = parse_hex(t[2]); b[3] = 2; b[0x94 / 4] = 2000;
    for (i = 4; i < n; i++) {
        char *eq = t[i]; unsigned long off;
        while (*eq && *eq != '=') eq++;
        if (*eq != '=') continue;
        *eq = 0; off = parse_hex(t[i]);
        b[off / 4] = val(eq + 1);
    }
    ioc(b, t[1]);
}

static void cmd_rx(int n, char **t) {
    unsigned long *b, *hb, hoff, i, iters, j; char nm[16];
    if (n < 6) return;
    split_name(t[2], nm, &hoff, 0xa4);
    hb = getslot(nm, 0); if (!hb) { ws("no handle slot\n"); return; }
    b = getslot(t[1], 1); if (!b) return;
    iters = parse_hex(t[5]);
    for (i = 0; i < iters; i++) {
        for (j = 0; j < SLOTW; j++) b[j] = 0;
        b[0] = 0x230; b[1] = 0x14; b[3] = 2; b[0x94 / 4] = 2000;
        b[0xa4 / 4] = hb[hoff / 4]; b[0xa8 / 4] = hb[hoff / 4 + 1];
        b[0xac / 4] = parse_hex(t[3]); b[0xb0 / 4] = parse_hex(t[4]);
        b[0xbc / 4] = 0x1000;
        if (ioc(b, "RECV") == 1) dump_nz(b, 0xbc, 0x230);
    }
}

static void cmd_ts(int n, char **t) {
    unsigned long *ob, *rb, hoff, tbl, iters, i, j, total = 0, good = 0, bad = 0, scr = 0, prev = 0xff, stride = 188, skip = 0, tmo = 0;
    char nm[16]; long ofd;
    if (n < 7) return;
    ob = getslot(t[1], 0); if (!ob) { ws("no open slot\n"); return; }
    hoff = parse_hex(t[2]); tbl = parse_hex(t[3]); iters = parse_hex(t[5]);
    if (n > 7) stride = parse_hex(t[7]);
    if (n > 8) skip = parse_hex(t[8]);
    ofd = sys3(SYS_creat, (long)t[6], 0644, 0);
    rb = getslot("_ts_rx", 1); if (!rb) return;
    (void)nm;
    for (i = 0; i < iters; i++) {
        unsigned long rv, idx, sz;
        for (j = 0; j < SLOTW; j++) rb[j] = 0;
        rb[0] = 0x230; rb[1] = 0x14; rb[3] = 2; rb[0x94 / 4] = 2000;
        rb[0xa4 / 4] = ob[hoff / 4]; rb[0xa8 / 4] = ob[hoff / 4 + 1];
        rb[0xac / 4] = parse_hex(t[4]); rb[0xb0 / 4] = 0;
        rb[0xbc / 4] = 0x10000;
        rb[0x144 / 4] = prev;
        rv = ioc(rb, "RECV");
        if (rv != 1) { if (rv == 6 && ++tmo >= 5) break; continue; }   /* タイムアウトが5回連続したら終了 */
        tmo = 0;
        idx = rb[0x13c / 4]; sz = rb[0xbc / 4];
        if (idx < 16 && sz >= stride) {
            unsigned char *p = (unsigned char *)ob[tbl / 4 + idx];
            unsigned long k;
            if (!p) continue;
            for (k = 0; k + stride <= sz; k += stride) {
                unsigned long pid; unsigned char *q = p + k + skip;
                total++;
                if (q[0] != 0x47) { bad++; continue; }
                good++;
                pid = ((q[1] & 0x1f) << 8) | q[2];
                pidcnt[pid]++;
                if (q[3] & 0xc0) { scr++; scrcnt[pid]++; }
            }
            if (ofd >= 0) sys3(4004, ofd, (long)p, sz);
            prev = idx;
        }
    }
    ws("TS: packets="); hex8(total); ws(" sync_ok="); hex8(good); ws(" sync_ng="); hex8(bad); ws(" scrambled="); hex8(scr); ws("\n");
    for (i = 0; i < 8192; i++) if (pidcnt[i]) { ws("  pid=0x"); hex8(i); ws(" n=0x"); hex8(pidcnt[i]); ws(" scr=0x"); hex8(scrcnt[i]); ws("\n"); }
    if (ofd >= 0) sys3(SYS_close, ofd, 0, 0);
}

static void run_line(int n, char **t) {
    if (streq(t[0], "io")) cmd_io(n, t);
    else if (streq(t[0], "dump")) { unsigned long *b = n >= 4 ? getslot(t[1], 0) : 0; if (b) dump_nz(b, parse_hex(t[2]), parse_hex(t[3])); }
    else if (streq(t[0], "rx")) cmd_rx(n, t);
    else if (streq(t[0], "ts")) cmd_ts(n, t);
    else if (streq(t[0], "sleep") && n >= 2) {
        unsigned long ms = 0; const char *p = t[1]; long ts[2];
        for (; *p >= '0' && *p <= '9'; p++) ms = ms * 10 + (*p - '0');
        ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000;
        sys3(SYS_nanosleep, (long)ts, 0, 0);
    }
}

int main_c(long *sp) {
    int len = 0; char c; long r;
    (void)sp;
    fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { ws("open failed\n"); return 2; }
    for (;;) {
        r = sys3(SYS_read, 0, (long)&c, 1);
        if (r <= 0 && len == 0) break;
        if (r <= 0 || c == '\n') {
            line[len] = 0;
            if (len > 0 && line[0] != '#') {
                int nt = 0; char *p = line;
                ws("> "); ws(line); ws("\n");
                while (*p && nt < 47) {
                    while (*p == ' ') *p++ = 0;
                    if (!*p) break;
                    tok[nt++] = p;
                    while (*p && *p != ' ') p++;
                }
                if (nt) run_line(nt, tok);
            }
            len = 0;
            if (r <= 0) break;
        } else if (len < 1000) line[len++] = c;
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
