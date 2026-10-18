/* nasne-recpt1 — nasne(Debian直起動)の地デジチューナーを、recpt1 互換のコマンドラインで使うためのネイティブツール。libc不使用(静的)。
 *
 *   nasne-recpt1 [--sid <サービスID>] [-v] <UHFチャンネル(13〜62)|--freq MHz> <秒数|-> <出力ファイル|->
 *
 *   秒数 "-" は無期限(SIGTERM/SIGINT/出力側の切断で終了)、出力 "-" は標準出力。
 *   録画ソフト(Mirakurun / tvheadend / EPGStation 等)のチューナーコマンドとして使える。
 *   --b25 / --strip / --device / --lnb など recpt1 のオプションは受け付けて無視する(復号は nasne のファームがB-CASカードで行う)。
 *
 * 動作:
 *   1. ファームの稼働を確認(停止していれば /usr/local/sbin/rc.xcode4 でドライバを再読込)。
 *   2. 復調IC・チューナーを I2C(公式ドライバ経由)で選局し、ロックを待つ。
 *   3. TSパススルー(PSI)ストリームを開き、PAT/PMT から映像・音声・PCR・ECM のPIDを得る(--sid 指定がなければ最初の映像+音声サービス)。
 *   4. B-CASストリームを開いて活性化し、純正と同じ OPEN_SECUREDTS(映像パススルー=元のMPEG-2のまま、B-CASで復号)を開く。
 *   5. 復号済みTS(元画質)を出力。PSIストリームから PAT(元のもの。NITのPIDを含む)と NIT/SDT/EIT/TOT 等のSI(PID 0x10-0x14,0x23,0x24,0x26-0x28)を混ぜる(番組表用)。
 *   6. 終了時(秒数経過・シグナル・出力切断)に必ずストリームをCLOSEしてから終了する(開いたまま終わるとnasneが固まることがある)。
 * 排他: /tmp/nasne-recpt1.lock を flock(チューナーは1系統のみ)。
 * 終了コード: 0=正常、1=エラー(ロック失敗/ストリームが開けない等)、2=使い方、3=チューナー使用中。
 *
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o nasne-recpt1 nasne_recpt1.c
 */
#include "jis0208_utf16.inc"      /* gen_jis_table.py で生成。ARIB文字列(サービス名)のUTF-8化に使う */

#define SYS_exit   4001
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_ioctl  4054
#define SYS_read   4003
#define SYS_nanosleep 4166
#define SYS_creat  4008
#define SYS_lseek  4019
#define SYS_fork   4002
#define SYS_waitpid 4007
#define SYS_execve 4011
#define SYS_chdir  4012
#define SYS_gettimeofday 4078
#define SYS_flock  4143
#define SYS_rt_sigaction 4194
#define SYS_rename 4038
#define SYS_mkdir  4039

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


static unsigned long parse_hex(const char *s) { unsigned long v = 0; for (; *s; s++) { int n = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10; v = v * 16 + n; } return v; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

#define SIGINT_ 2
#define SIGPIPE_ 13
#define SIGTERM_ 15

static long fd;
static volatile int stop_flag;
static int verbose;
static unsigned long ob_pt[0x484 / 4 + 4], ob_sec[0x9ec / 4 + 4], ob_g[0x208 / 4 + 4], rb[0x230 / 4 + 4], tb[0x1d0 / 4 + 4], ib[0x110 / 4 + 4];
static unsigned char outbuf[0x20000];
static unsigned char dummy[0x1000];
static unsigned char psi[0x8000];
static unsigned long psi_len;

static void dec(unsigned long v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); ws(b + i); }
static void err(const char *m) { sys3(SYS_write, 2, (long)m, slen(m)); }
static void errhex(unsigned long v) { char b[9]; int i; for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; b[i] = n < 10 ? '0' + n : 'a' + n - 10; } b[8] = 0; err(b); }
static int logv;     /* --listen のときは常に1: 行程の節目をstderr(ログ)へ出す(固まった箇所の特定用) */
static void lgx(const char *m, unsigned long v);
static void msleep(unsigned long ms) { long ts[2]; ts[0] = ms / 1000; ts[1] = (ms % 1000) * 1000000; sys3(SYS_nanosleep, (long)ts, 0, 0); }
static unsigned long now_ms(void);
static unsigned long now_ms(void) { long tv[2]; sys3(SYS_gettimeofday, (long)tv, 0, 0); return tv[0] * 1000UL + tv[1] / 1000; }

static void lgx(const char *m, unsigned long v) {
    if (!logv) return;
    { char b[12]; int i = 11; unsigned long t = now_ms() % 100000000UL; b[i] = 0; do { b[--i] = '0' + t % 10; t /= 10; } while (t); err("["); err(b + i); err("] "); }
    err(m); err(" "); errhex(v); err("\n");
}
static void on_sig(int s) { (void)s; stop_flag = 1; }
static void set_sig(int sig) {
    /* MIPS o32 の kernel sigaction: { unsigned long sa_flags; void *sa_handler; unsigned long sa_mask[4]; } */
    unsigned long sa[6];
    sa[0] = 0; sa[1] = (unsigned long)on_sig; sa[2] = sa[3] = sa[4] = sa[5] = 0;
    sys6(SYS_rt_sigaction, sig, (long)sa, 0, 16, 0, 0);
}

/* ---- ioctl ---- */
static unsigned long ioc(unsigned long *b, unsigned long size, unsigned long cmd, unsigned long tmo) {
    b[0] = size; b[1] = cmd; b[3] = 2; b[0x94 / 4] = tmo;
    sys3(SYS_ioctl, fd, 0x17241724, (long)b);
    return b[3];
}
static void clr(unsigned long *b, unsigned long words) { unsigned long i; for (i = 0; i < words; i++) b[i] = 0; }

/* ---- I2C(ブロック1、公式ドライバ経由。i2cx.c と同じ) ---- */
static unsigned long xfer(unsigned long addr, int wr, unsigned char *d, int n, int stopbit) {
    int i; unsigned char *b = (unsigned char *)ib;
    clr(ib, sizeof(ib) / 4);
    if (wr) for (i = 0; i < n; i++) b[0xa4 + i] = d[i];
    ib[0xe4 / 4] = n; ib[0xe8 / 4] = wr; ib[0xec / 4] = addr; ib[0xf0 / 4] = 675; ib[0xf4 / 4] = 0; ib[0xf8 / 4] = 3375;
    ib[0xfc / 4] = 1; ib[0x100 / 4] = 1; ib[0x104 / 4] = stopbit;
    ioc(ib, 0x110, 0x1b, 2000);
    if (!wr) for (i = 0; i < n; i++) d[i] = b[0xa4 + i];
    return ib[3];
}
static unsigned long dem_w(unsigned char reg, unsigned char v) { unsigned char d[2]; d[0] = reg; d[1] = v; return xfer(0x20, 1, d, 2, 1); }
static unsigned long dem_r(unsigned char reg, unsigned char *out) { unsigned char d[2]; d[0] = reg; if (xfer(0x20, 1, d, 1, 0) != 1) return 0; return xfer(0x20, 0, out, 1, 1); }
static unsigned long tun_w(const unsigned char *bytes, int n) { unsigned char d[96]; int i; d[0] = 0xfe; d[1] = 0xc0; for (i = 0; i < n && i < 90; i++) d[2 + i] = bytes[i]; return xfer(0x20, 1, d, 2 + n, 1); }

static const unsigned char TUNER_INIT[] = {
    0x00,0x02, 0x02,0x21, 0x05,0x00, 0x06,0x14, 0x07,0x0c, 0x0e,0x00, 0x0f,0x00, 0x21,0xa8, 0xc8,0x00, 0xaf,0xa2, 0x1d,0x4a, 0xca,0x05,
    0x2e,0x7e, 0x2d,0x44, 0x1b,0xac, 0xac,0x00, 0x2c,0x01, 0xae,0x03, 0x54,0xe3, 0x56,0x47, 0x55,0x12, 0x09,0x01, 0xa4,0x51, 0xa0,0x0c,
    0xb0,0xc2, 0x57,0x17, 0x6f,0x79, 0x70,0x00, 0x6f,0x78, 0x70,0x00, 0x6f,0x7b, 0x70,0x00, 0x6f,0x7c, 0x70,0x00, 0x6f,0x7a, 0x70,0x00,
    0x1a,0x0d, 0x01,0x01 };

static int fe_ok;
static void fe_init(void) {
    static const unsigned char ff[] = { 0xff }, a[] = { 0x01, 0x01, 0x13, 0x01 }, s1[] = { 0x13, 0x01 }, s2[] = { 0x01, 0x00, 0x13, 0x00 };
    tun_w(ff, 1); msleep(10);
    tun_w(TUNER_INIT, sizeof(TUNER_INIT));
    tun_w(a, 4);
    dem_w(0x01, 0x40); dem_w(0x0e, 0x07); dem_w(0x0f, 0x10); dem_w(0x23, 0x38); dem_w(0x4f, 0xe5);
    tun_w(s1, 2); msleep(30); tun_w(s2, 4); dem_w(0x03, 0xf0);
}
/* UHFチャンネルまたはHz周波数を選局し、ロックを待つ。戻り値 1=ロック */
static unsigned long lock_wait_ms = 4000;
static int fe_tune(unsigned long hz) {
    static const unsigned char on[] = { 0x01, 0x01 }, fin[] = { 0x1a, 0x0d };
    unsigned char pll[16]; unsigned long n, rem, div; int i; unsigned long t0;
    n = hz / 1000000; rem = hz % 1000000; div = 1000000;
    for (i = 0; i < 6; i++) { div >>= 1; n *= 2; if (div < rem) { rem -= div; n += 1; } }
    if (rem > 0x1e84) n++;
    pll[0] = 0x13; pll[1] = 0x00; pll[2] = 0x3b; pll[3] = 0xc0; pll[4] = 0x3b; pll[5] = 0x80; pll[6] = 0x10; pll[7] = 0x95;
    pll[8] = 0x1a; pll[9] = 0x05; pll[10] = 0x11; pll[11] = n & 0xff; pll[12] = 0x12; pll[13] = (n >> 8) & 0xff; pll[14] = 0x13; pll[15] = 0x01;
    tun_w(on, 2); dem_w(0x03, 0x00); dem_w(0x77, 0x03); dem_w(0x71, 0x30); dem_w(0x20, 0x00); dem_w(0x25, 0x00); dem_w(0x23, 0x39);
    tun_w(pll, 16); msleep(31); tun_w(fin, 2);
    dem_w(0x23, 0x38); dem_w(0x47, 0x40); dem_w(0x01, 0x40);
    t0 = now_ms();
    while (now_ms() - t0 < lock_wait_ms && !stop_flag) {
        unsigned char r = 0xff;
        msleep(100);
        if (dem_r(0x80, &r) && (r & 0x88) == 0) return 1;
    }
    return 0;
}

static int fe_tune_quick(unsigned long hz);
static int fe_tune_quick(unsigned long hz) { int r; lock_wait_ms = 1500; r = fe_tune(hz); lock_wait_ms = 4000; return r; }

/* ---- ISDB-S (衛星チューナー) 制御 ---- */
#define DEMOD_S 0x22
#define TUNER_S 0xc6
#define TUNER_S_B 0xc8
#define LO_KHZ 10678000UL
#define XTAL_HZ 16000000UL

static unsigned char shadow_s[8] = { 0x03, 0x13, 0xdc, 0x85, 0x12, 0x01, 0xe6, 0x1e };

static unsigned long dem_s_w(unsigned char reg, unsigned char v) {
    unsigned char d[2]; d[0] = reg; d[1] = v;
    return xfer(DEMOD_S, 1, d, 2, 1);
}

static unsigned long dem_s_r(unsigned char reg, unsigned char *out) {
    unsigned char d[2]; d[0] = reg;
    if (xfer(DEMOD_S, 1, d, 1, 0) != 1) return 0;
    return xfer(DEMOD_S, 0, out, 1, 1);
}

static unsigned long tun_s_w_bit(unsigned char reg, unsigned char mask, unsigned char val) {
    unsigned char new_v = ((~mask & shadow_s[reg]) | (val & mask)) & 0xff;
    shadow_s[reg] = new_v;
    unsigned char d[4];
    d[0] = 0xfe; d[1] = TUNER_S; d[2] = reg; d[3] = new_v;
    return xfer(DEMOD_S, 1, d, 4, 1);
}

static unsigned long tun_s_w_raw(const unsigned char *bytes, int n) {
    unsigned char d[32]; int i;
    d[0] = 0xfe; d[1] = TUNER_S;
    for (i = 0; i < n && i < 28; i++) d[2 + i] = bytes[i];
    return xfer(DEMOD_S, 1, d, 2 + n, 1);
}

static void fe_init_s(void) {
    static const unsigned char init_tuner[9] = { 0x00, 0x03, 0x13, 0xdc, 0x85, 0x12, 0x01, 0xe6, 0x1e };
    unsigned char c8_init[4];
    c8_init[0] = 0xfe; c8_init[1] = TUNER_S_B; c8_init[2] = 0x02; c8_init[3] = 0x53;
    tun_s_w_raw(init_tuner, 9);
    tun_s_w_bit(1, 0xc0, 0x00);
    tun_s_w_bit(1, 0x0f, 0x03);

    dem_s_w(0x03, 0x01); dem_s_w(0x06, 0x40); dem_s_w(0x07, 0x01); dem_s_w(0x08, 0x00); dem_s_w(0x10, 0xb1);
    dem_s_w(0x11, 0x40); dem_s_w(0x85, 0x7a); dem_s_w(0x8e, 0x05); dem_s_w(0xa3, 0x33); dem_s_w(0xa5, 0xc0);

    xfer(DEMOD_S, 1, c8_init, 4, 1);

    tun_s_w_bit(0, 0x07, 0x00);
    dem_s_w(0x13, 0x80);
    dem_s_w(0x17, 0xff);
}

static void calc_s_pll(unsigned long if_khz, unsigned char *reg2, unsigned char *reg3) {
    unsigned long l28, l24, base, p, target;
    long s1 = 1000, s2 = 0;
    int s0;
    unsigned long n, t0;

    if (if_khz < 1022001UL) {
        l28 = 1; l24 = 0; base = 2;
    } else if (if_khz < 1324577UL) {
        l28 = 1; l24 = 1; base = 2;
    } else if (if_khz < 2046001UL) {
        l28 = 0; l24 = 0; base = 1;
    } else {
        l28 = 0; l24 = 1; base = 1;
    }
    p = 1UL << base;
    target = 10 * p;
    for (s0 = 1; s0 <= 4; s0++) {
        long v = 160 / (1 << s0);
        long diff = v > (long)target ? v - target : (long)target - v;
        long diff_s1 = s1 > (long)target ? s1 - target : (long)target - s1;
        if (diff < diff_s1) {
            s2 = s0 - 1;
            s1 = 160 / (1 << (s2 + 1));
        }
    }
    n = (unsigned long)((1UL << (s2 + 1)) * if_khz * p * 10 / (XTAL_HZ / 1000));
    t0 = n + 5;
    *reg2 = (unsigned char)((t0 / 10) & 0xff);
    *reg3 = (unsigned char)(((l24 << 5) | (l28 << 4) | (s2 << 6) | (t0 / 2560)) & 0xff);
}

static int fe_tune_s(unsigned long if_khz, unsigned long ts_id) {
    unsigned char reg2 = 0, reg3 = 0;
    unsigned long t0;
    calc_s_pll(if_khz, &reg2, &reg3);

    tun_s_w_bit(0, 0x07, 0x03);
    dem_s_w(0x13, 0x00);
    dem_s_w(0x17, 0x00);
    dem_s_w(0xa5, 0x80);
    dem_s_w(0x07, 0x01);
    dem_s_w(0xa6, 0x04);
    dem_s_w(0x03, 0x01);
    dem_s_w(0x0a, 0x00);
    dem_s_w(0x11, 0x00);
    dem_s_w(0x03, 0x01);

    tun_s_w_bit(0, 0xf8, 0x00);
    tun_s_w_bit(3, 0xff, reg3);
    tun_s_w_bit(2, 0xff, reg2);
    tun_s_w_bit(5, 0x04, 0x04);

    msleep(20);
    dem_s_w(0x0a, 0xff);
    dem_s_w(0x11, 0x40);
    dem_s_w(0x03, 0x01);

    if (ts_id) {
        dem_s_w(0x8f, (ts_id >> 8) & 0xff);
        dem_s_w(0x90, ts_id & 0xff);
    }

    t0 = now_ms();
    while (now_ms() - t0 < lock_wait_ms && !stop_flag) {
        unsigned char c3 = 0xff, ea = 0x00;
        msleep(50);
        if (dem_s_r(0xc3, &c3) && dem_s_r(0xea, &ea)) {
            if ((c3 & 0x10) == 0 && (ea & 0x80) != 0) {
                if (ts_id) {
                    dem_s_w(0x8f, (ts_id >> 8) & 0xff);
                    dem_s_w(0x90, ts_id & 0xff);
                    msleep(20);
                }
                return 1;
            }
        }
    }
    return 0;
}
/* ---- ファーム確認・ドライバ再読込 ---- */
static int fw_alive(void) {
    clr(tb, 0xb0 / 4 + 2);
    if (ioc(tb, 0xb0, 0x103, 2000) != 1) return 0;
    return tb[0xa4 / 4] == 0;
}
static void reload_driver(void) {
    long pid;
    err("nasne-recpt1: ドライバを再読込します\n");
    if (fd >= 0) sys3(SYS_close, fd, 0, 0);
    pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        char *argv[4]; char *envp[2];
        argv[0] = "sh"; argv[1] = "-c"; argv[2] = "cd /usr/local/sbin && ./rc.xcode4 >/dev/null 2>&1"; argv[3] = 0; envp[0] = "PATH=/sbin:/usr/sbin:/bin:/usr/bin"; envp[1] = 0;
        sys3(SYS_execve, (long)"/bin/sh", (long)argv, (long)envp);
        sys3(SYS_exit, 127, 0, 0);
    }
    if (pid > 0) sys3(SYS_waitpid, pid, 0, 0);
    msleep(2000);
    fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
}

/* ---- ストリーム ---- */
static unsigned long h_pt0, h_pt1, h_sec0, h_sec1, h_g0, h_g1;
static unsigned long prev_pt = 0xff, prev_sec = 0xff;

static int recv_pt(unsigned long tmo, unsigned char **p, unsigned long *sz) {
    unsigned long rv, idx;
    clr(rb, 0x230 / 4);
    rb[0xa4 / 4] = h_pt0; rb[0xa8 / 4] = h_pt1; rb[0xac / 4] = 0x20000; rb[0xbc / 4] = 0x10000; rb[0x144 / 4] = prev_pt;
    rv = ioc(rb, 0x230, 0x14, tmo);
    if (rv != 1) return 0;
    idx = rb[0x13c / 4]; *sz = rb[0xbc / 4];
    if (idx >= 8 || *sz < 188) return 0;
    *p = (unsigned char *)ob_pt[0x1b8 / 4 + idx];
    prev_pt = idx;
    return *p != 0;
}

/* PATとPMTを集める。戻り値: 0=成功
 * BS/CSでは1トランスポンダ(PAT)内に8個以上のサービス(マルチ編成・データ・ES等を含む)が存在するため上限を32とする */
#define MAX_SEC 32
static unsigned char sec_buf[MAX_SEC][1100]; static unsigned long sec_have[MAX_SEC]; static unsigned long sec_pid[MAX_SEC], sec_nsec;
struct svc { unsigned long pn, pmt, pcr, ecm, vpid, vst, apid, ast; int ok, seg1; };      /* seg1: ワンセグ(PMTのPIDが0x1FC8)。SECUREDTSを開くとファームが固まるため扱わない */
static struct svc svcs[MAX_SEC]; static int nsvc;

static void feed(const unsigned char *q) {
    unsigned long pid = ((q[1] & 0x1f) << 8) | q[2], af = (q[3] >> 4) & 3, i; const unsigned char *pl; unsigned long pln;
    if (!(af & 1)) return;
    pl = q + 4; pln = 184;
    if (af & 2) { pl = q + 5 + q[4]; pln = 183 - q[4]; }
    for (i = 0; i < sec_nsec; i++) if (sec_pid[i] == pid) break;
    if (i == sec_nsec) return;
    if (q[1] & 0x40) {
        unsigned long ptr = pl[0], k;
        if (ptr + 1 > pln) return;
        pl += 1 + ptr; pln -= 1 + ptr;
        for (k = 0; k < pln && k < 1100; k++) sec_buf[i][k] = pl[k];
        sec_have[i] = pln < 1100 ? pln : 1100;
    } else if (sec_have[i] && sec_have[i] < 1100) {
        unsigned long k;
        for (k = 0; k < pln && sec_have[i] + k < 1100; k++) sec_buf[i][sec_have[i] + k] = pl[k];
        sec_have[i] += pln;
    }
}
static int sec_complete(unsigned long i) {
    unsigned long ln;
    if (sec_have[i] < 3) return 0;
    ln = (((sec_buf[i][1] & 0xf) << 8) | sec_buf[i][2]) + 3;
    return sec_have[i] >= ln;
}
static int discover(unsigned long want_sid, unsigned long tmo_ms) {
    unsigned long t0 = now_ms(), i, j;
    sec_nsec = 1; sec_pid[0] = 0; sec_have[0] = 0; nsvc = 0;
    int pat_done = 0;
    while (now_ms() - t0 < tmo_ms && !stop_flag) {
        unsigned char *p; unsigned long sz, k;
        if (!recv_pt(500, &p, &sz)) continue;
        for (k = 0; k + 188 <= sz; k += 188) if (p[k] == 0x47) feed(p + k);
        if (!pat_done && sec_complete(0)) {
            unsigned char *s = sec_buf[0]; unsigned long ln = (((s[1] & 0xf) << 8) | s[2]) + 3;
            for (j = 8; j + 4 <= ln - 4 && sec_nsec < MAX_SEC; j += 4) {
                unsigned long pn = (s[j] << 8) | s[j + 1], pid = ((s[j + 2] & 0x1f) << 8) | s[j + 3];
                if (!pn) continue;
                sec_pid[sec_nsec] = pid; sec_have[sec_nsec] = 0; svcs[nsvc].pn = pn; svcs[nsvc].pmt = pid; svcs[nsvc].ok = 0; svcs[nsvc].seg1 = (pid == 0x1fc8); nsvc++; sec_nsec++;
            }
            pat_done = 1;
        }
        if (pat_done) {
            int all = 1;
            for (i = 0; i < (unsigned long)nsvc; i++) {
                unsigned char *s; unsigned long ln, pil, e;
                if (svcs[i].ok) continue;
                if (!sec_complete(i + 1)) { all = 0; continue; }
                s = sec_buf[i + 1]; ln = (((s[1] & 0xf) << 8) | s[2]) + 3;
                svcs[i].pcr = ((s[8] & 0x1f) << 8) | s[9]; pil = ((s[10] & 0xf) << 8) | s[11]; svcs[i].ecm = 0; svcs[i].vpid = svcs[i].apid = 0;
                for (e = 12; e + 2 <= 12 + pil; e += 2 + s[e + 1]) if (s[e] == 9 && ((s[e + 2] << 8) | s[e + 3]) == 5) svcs[i].ecm = ((s[e + 4] & 0x1f) << 8) | s[e + 5];
                for (e = 12 + pil; e + 5 <= ln - 4; e += 5 + (((s[e + 3] & 0xf) << 8) | s[e + 4])) {
                    unsigned long st = s[e], ep = ((s[e + 1] & 0x1f) << 8) | s[e + 2];
                    if ((st == 0x02 || st == 0x1b) && !svcs[i].vpid) { svcs[i].vpid = ep; svcs[i].vst = st; }
                    if ((st == 0x0f || st == 0x11) && !svcs[i].apid) { svcs[i].apid = ep; svcs[i].ast = st; }
                }
                svcs[i].ok = 1;
                if (want_sid && svcs[i].pn == want_sid && svcs[i].vpid && svcs[i].apid && !svcs[i].seg1) return (int)i;
            }
            if (all) {
                for (i = 0; i < (unsigned long)nsvc; i++) if (svcs[i].vpid && svcs[i].apid && !svcs[i].seg1 && (!want_sid || svcs[i].pn == want_sid)) return (int)i;
                return -1;
            }
        }
    }
    /* 時間切れ: 取れた範囲で選ぶ */
    for (i = 0; i < (unsigned long)nsvc; i++) if (svcs[i].ok && svcs[i].vpid && svcs[i].apid && !svcs[i].seg1 && (!want_sid || svcs[i].pn == want_sid)) return (int)i;
    return -1;
}

static void set(unsigned long off, unsigned long v) { ob_sec[off / 4] = v; }
static int open_streams(struct svc *s, int is_sat) {
    unsigned long rv, i; int tries;
    /* B-CAS ジェネリックストリーム */
    clr(ob_g, 0x208 / 4 + 2); ob_g[0xac / 4] = 0x400; ob_g[0xb0 / 4] = 0x1000;
    rv = ioc(ob_g, 0x208, 0x2e, 2000);
    if (rv != 1) { err("B-CASストリームを開けない rv="); errhex(rv); err("\n"); return -1; }
    h_g0 = ob_g[0xa4 / 4]; h_g1 = ob_g[0xa8 / 4];
    clr(tb, 0x1d0 / 4 + 2);
    tb[0xa4 / 4] = h_g0; tb[0xa8 / 4] = h_g1; tb[0xac / 4] = 0; tb[0xb0 / 4] = 0x10; tb[0xc4 / 4] = (unsigned long)dummy; tb[0xcc / 4] = 0;
    tb[0xd4 / 4] = 0x400; tb[0xd8 / 4] = 1; tb[0xdc / 4] = 1;
    rv = ioc(tb, 0x1d0, 0x13, 2000);
    if (rv != 1) { err("B-CAS ACTIVATE失敗\n"); return -1; }
    for (tries = 0, i = 0; tries < 6 && !stop_flag; tries++) {                 /* ACTIVATEDイベントを待つ */
        clr(rb, 0x230 / 4);
        rb[0xa4 / 4] = h_g0; rb[0xa8 / 4] = h_g1; rb[0xac / 4] = 0; rb[0xb0 / 4] = 0x10; rb[0xb4 / 4] = (unsigned long)dummy; rb[0xbc / 4] = sizeof(dummy);
        if (ioc(rb, 0x230, 0x14, 1000) == 1 && (rb[0x130 / 4] & 0x400)) { i = 1; break; }
        if (rb[0x130 / 4] & 0x400) { i = 1; break; }
    }
    if (!i) err("警告: B-CAS活性化イベントを確認できないまま続行\n");
    /* 復号ストリーム: 純正の既定値(映像パススルー設定)+番組ごとのPID */
    clr(ob_sec, 0x9ec / 4 + 2);
    set(0xb4, h_g0); set(0xb8, h_g1); set(0xbc, h_pt0); set(0xc0, h_pt1);
    set(0xac, 0x8204141); set(0xb0, 0x80006); set(0xcc, is_sat ? 1 : 0); set(0xd4, 1);
    set(0xd8, s->vpid); set(0xdc, s->vpid); set(0xe0, 0); set(0xe4, 0x1e8480); set(0xe8, 0x1e); set(0xec, 0x2d0); set(0xf0, 0x1e0);
    set(0xf4, 1); set(0xf8, is_sat ? 1 : 0); set(0x100, s->apid); set(0x110, s->apid); set(0x120, 0x10000); set(0x12c, 0x2dc6c0);
    set(0x134, 0x300d8); set(0x138, 0x30); set(0x13c, s->pcr); set(0x140, 0x900000); set(0x144, 2); set(0x148, 3);
    set(0x160, s->pn); set(0x178, s->ecm); set(0x184, 1); set(0x194, s->pn); set(0x19c, 0x40);
    set(0x1a0, 2); set(0x1a4, s->vpid); set(0x1a8, s->apid); set(0x204, s->ecm); set(0x208, s->ecm); set(0x264, s->vst); set(0x268, s->ast);
    set(0x608, 0x21); set(0x67c, 0x10000); set(0x698, 0x30000); set(0x894, 0xc); set(0x898, 0x5b8d80); set(0x8a8, 0x2ee00); set(0x8ac, 0xbb80);
    set(0x8b0, 2); set(0x8b4, 0x400); set(0x924, 2); set(0x940, 0xf03); set(0x950, 0x1e8480); set(0x954, 2); set(0x958, 3);
    set(0x964, 0x1e8480); set(0x968, 2); set(0x96c, 3); set(0x978, 1); set(0x980, 2);
    rv = ioc(ob_sec, 0x9ec, 0x30, 2000);
    if (rv != 1) { err("復号ストリーム(OPEN_SECUREDTS)を開けない rv="); errhex(rv); err("\n"); return -1; }
    h_sec0 = ob_sec[0xa4 / 4]; h_sec1 = ob_sec[0xa8 / 4];
    return 0;
}
static void close_streams(void) {
    lgx("close_streams: sec", h_sec0);
    clr(tb, 0xb4 / 4 + 2);
    if (h_sec0 || h_sec1) { clr(tb, 0xb4 / 4 + 2); tb[0xa4 / 4] = h_sec0; tb[0xa8 / 4] = h_sec1; ioc(tb, 0xb4, 0x11, 5000); h_sec0 = h_sec1 = 0; }
    lgx("close_streams: pt", h_pt0);
    if (h_pt0 || h_pt1) { clr(tb, 0xb4 / 4 + 2); tb[0xa4 / 4] = h_pt0; tb[0xa8 / 4] = h_pt1; ioc(tb, 0xb4, 0x11, 5000); h_pt0 = h_pt1 = 0; }
    lgx("close_streams: generic", h_g0);
    if (h_g0 || h_g1) { clr(tb, 0xb4 / 4 + 2); tb[0xa4 / 4] = h_g0; tb[0xa8 / 4] = h_g1; tb[0xb0 / 4] = 0x400; ioc(tb, 0xb4, 0x2f, 5000); h_g0 = h_g1 = 0; }
}

/* 地デジは復号ストリーム(OPEN_SECUREDTS)にPMTが含まれるため、PSIから混ぜると二重になりCCエラーが生じる。
 * 衛星(is_sat)のセッションでのみ cur_pmt_pid を設定し、PMTをPSIストリームから透過させる。 */
static unsigned long cur_pmt_pid = 0;
static int is_si_pid(unsigned long pid) {
    int i;
    if (pid == 0 || (pid >= 0x10 && pid <= 0x14) || pid == 0x23 || pid == 0x24 || (pid >= 0x26 && pid <= 0x28)) return 1;
    if (cur_pmt_pid) {
        if (pid == cur_pmt_pid) return 1;
        for (i = 0; i < nsvc; i++) {
            if (svcs[i].pmt == pid) return 1;
        }
    }
    return 0;
}
static int out_fd = 1;
static unsigned long olen;
static int flush_out(void) {
    unsigned long done = 0;
    while (done < olen) {
        long r = sys3(4004, out_fd, (long)(outbuf + done), olen - done);
        if (r <= 0) { stop_flag = 1; olen = 0; return -1; }
        done += r;
    }
    olen = 0;
    return 0;
}
static void put_pkt(const unsigned char *q) {
    unsigned long i;
    if (olen + 188 > sizeof(outbuf)) flush_out();
    for (i = 0; i < 188; i++) outbuf[olen + i] = q[i];
    olen += 188;
}

static unsigned long parse_dec(const char *s) { unsigned long v = 0; for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0'); return v; }
static int is_num(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static unsigned long uhf_hz(unsigned long ch) { return 473142857UL + 6000000UL * (ch - 13); }
static unsigned long parse_mhz(const char *v) {
    unsigned long mhz = parse_dec(v), frac = 0, scale = 1; const char *d = v;
    while (*d && *d != '.') d++;
    if (*d == '.') { d++; while (*d >= '0' && *d <= '9' && scale < 1000000) { frac = frac * 10 + (*d - '0'); scale *= 10; d++; } }
    return mhz * 1000000 + frac * (1000000 / scale);
}

static int starts(const char *s, const char *pre) { while (*pre) { if (*s++ != *pre++) return 0; } return 1; }

static unsigned long get_bs_tsid(unsigned long ch, unsigned long slot) {
    switch (ch) {
        case 1:
            if (slot == 0) return 0x4010;
            if (slot == 1) return 0x4011;
            if (slot == 2) return 0x4012;
            break;
        case 3:
            if (slot == 0) return 0x4030;
            if (slot == 1) return 0x4631; /* 2024年10月BS帯域再編: BSアニマックス(TS1) */
            if (slot == 2) return 0x4032;
            break;
        case 5:
            if (slot == 0) return 0x4450;
            if (slot == 1) return 0x4451;
            break;
        case 7:
            if (slot == 0) return 0x4670;
            break;
        case 9:
            if (slot == 0) return 0x4090;
            if (slot == 1) return 0x4091;
            if (slot == 2) return 0x4092;
            break;
        case 11:
            if (slot == 0) return 0x46b0;
            if (slot == 1) return 0x46b1;
            if (slot == 2) return 0x46b2;
            if (slot == 3) return 0x46b3;
            break;
        case 13:
            if (slot == 0) return 0x40d0;
            if (slot == 1) return 0x40d1;
            if (slot == 2) return 0x46d2; /* 2024年11月BS帯域再編: 放送大学(TS2) */
            break;
        case 15:
            if (slot == 0 || slot == 1) return 0x40f1;
            if (slot == 2) return 0x40f2;
            break;
        case 19:
            if (slot == 0) return 0x4730;
            if (slot == 1) return 0x4731;
            if (slot == 2) return 0x4732;
            if (slot == 3) return 0x4733;
            break;
        case 21:
            if (slot == 0) return 0x4750;
            if (slot == 1) return 0x4751;
            if (slot == 2) return 0x4752;
            break;
        case 23:
            if (slot == 0) return 0x4772;
            if (slot == 1) return 0x4971;
            if (slot == 2) return 0x4972;
            if (slot == 3) return 0x4973;
            break;
    }
    return 0x4000 | (ch << 4) | (slot & 0x0f);
}

/* CSトランスポンダ(ND2〜ND24)ごとのTSIDを返す。
 * 東経110度CSデジタル放送(ARIB STD-B10)では1トランスポンダ=1TS構成となっており、
 * CS1ネットワーク(ND2, ND8, ND10)には0x6000番台、CS2ネットワーク(ND4, ND6, ND12〜ND24)には0x7000番台が割り当てられている。
 * スロット多重のBSとは異なり、各トランスポンダ番号から一意にTSIDが定まる。
 */
static unsigned long get_cs_tsid(unsigned long ch) {
    switch (ch) {
        case 2:  return 0x6020; /* ND2  (CS1) */
        case 4:  return 0x7040; /* ND4  (CS2) */
        case 6:  return 0x7060; /* ND6  (CS2) */
        case 8:  return 0x6080; /* ND8  (CS1) */
        case 10: return 0x60a0; /* ND10 (CS1) */
        case 12: return 0x70c0; /* ND12 (CS2) */
        case 14: return 0x70e0; /* ND14 (CS2) */
        case 16: return 0x7100; /* ND16 (CS2) */
        case 18: return 0x7120; /* ND18 (CS2) */
        case 20: return 0x7140; /* ND20 (CS2) */
        case 22: return 0x7160; /* ND22 (CS2) */
        case 24: return 0x7180; /* ND24 (CS2) */
        default: return 0;
    }
}

/* チャンネル判別:
 *   13〜62: 地デジ (is_sat = 0, val = uhf_hz(ch))
 *   BS1〜BS23, BS01_0〜BS23_3: BS放送 (is_sat = 1, val = if_khz)
 *   CS2〜CS24, CS02〜CS24: CS放送 (is_sat = 1, val = if_khz)
 *   戻り値: 1=正常パース、0=エラー
 */
static int parse_channel_str(const char *s, int *is_sat, unsigned long *val, unsigned long *ts_id) {
    if (!s || !*s) return 0;
    *ts_id = 0;
    if ((s[0] | 32) == 'b' && (s[1] | 32) == 's') {
        const char *p = s + 2;
        unsigned long ch = 0;
        unsigned long slot = 0;
        int has_slot = 0;
        while (*p == '0') p++;
        while (*p >= '0' && *p <= '9') { ch = ch * 10 + (*p - '0'); p++; }
        if (*p == '_') {
            p++;
            while (*p >= '0' && *p <= '9') { slot = slot * 10 + (*p - '0'); p++; has_slot = 1; }
        }
        if (ch >= 1 && ch <= 23 && (ch % 2 == 1)) {
            *is_sat = 1;
            *val = (11727480UL + ((ch - 1) / 2) * 38360UL) - LO_KHZ;
            if (has_slot) {
                *ts_id = get_bs_tsid(ch, slot);
            }
            return 1;
        }
        return 0;
    }
    if ((s[0] | 32) == 'c' && (s[1] | 32) == 's') {
        const char *p = s + 2;
        unsigned long ch = 0;
        unsigned long slot = 0;
        int has_slot = 0;
        while (*p == '0') p++;
        while (*p >= '0' && *p <= '9') { ch = ch * 10 + (*p - '0'); p++; }
        if (*p == '_') {
            p++;
            while (*p >= '0' && *p <= '9') { slot = slot * 10 + (*p - '0'); p++; has_slot = 1; }
        }
        if (ch >= 2 && ch <= 24 && (ch % 2 == 0)) {
            *is_sat = 1;
            *val = (12291000UL + ((ch - 2) / 2) * 40000UL) - LO_KHZ;
            *ts_id = get_cs_tsid(ch);
            (void)slot; (void)has_slot;
            return 1;
        }
        return 0;
    }
    {   /* 地デジ: 13〜62。`27_56360` のように、あとに `_<任意>` が付いてもよい(Mirakurun のチャンネル名を、サービスごとに一意にして、`?` を使わないため。接尾辞は無視する) */
        unsigned long ch = 0; const char *p = s; int nd = 0;
        while (*p >= '0' && *p <= '9') { ch = ch * 10 + (*p - '0'); p++; nd++; }
        if (nd && (*p == 0 || *p == '_') && ch >= 13 && ch <= 62) {
            *is_sat = 0;
            *val = uhf_hz(ch);
            *ts_id = 0;
            return 1;
        }
    }
    return 0;
}

#define BUSY_WAIT_MS 10000
static long lockfd = -1;
static int acquire_tuner(int is_sat) {
    lockfd = sys3(SYS_open, (long)"/tmp/nasne-recpt1.lock", 0x100 | 2, 0644);     /* O_CREAT(MIPSは0x100)|O_RDWR */
    if (sys3(SYS_flock, lockfd, 2 | 4, 0) < 0) { sys3(SYS_close, lockfd, 0, 0); lockfd = -1; return 3; }   /* LOCK_EX|LOCK_NB: 使用中 */

    /* 前回のクローズ時刻からの経過時間を検証し、300ms 未満の連射時のみ差分だけ待機する動的クールダウン。
     * 通常視聴時は待機時間0msを保ちつつ、Mirakurun等のミリ秒単位の連続選局時にViXS SoC内部DMAや復調ICのセトリング時間を確保し、バスロック(D状態固まり)を確実に防止する。 */
    {
        unsigned long last_close = 0;
        sys3(SYS_lseek, lockfd, 0, 0);
        if (sys3(SYS_read, lockfd, (long)&last_close, sizeof(last_close)) == sizeof(last_close)) {
            unsigned long now = now_ms();
            if (now >= last_close && (now - last_close) < 300) {
                msleep(300 - (now - last_close));
            }
        }
    }
    fd = sys3(SYS_open, (long)"/dev/vixs/xcodedrv", 2, 0);
    if (fd < 0) { err("/dev/vixs/xcodedrv を開けない(ドライバ未ロード)\n"); reload_driver(); if (fd < 0) return 1; }
    if (!fw_alive()) reload_driver();
    if (fd < 0 || !fw_alive()) { err("ファームが動いていない\n"); return 1; }
    if (is_sat) fe_init_s(); else fe_init();
    return 0;
}
static void release_tuner(void) {
    if (fd >= 0) sys3(SYS_close, fd, 0, 0);
    fd = -1;
    if (lockfd >= 0) {
        unsigned long t = now_ms();
        sys3(SYS_lseek, lockfd, 0, 0);
        sys3(SYS_write, lockfd, (long)&t, sizeof(t)); /* 次のセッションのために終了時刻を記録 */
        sys3(SYS_close, lockfd, 0, 0);                 /* flock も解放される */
    }
    lockfd = -1;
}

static void http_status(int sock, const char *line, const char *body) {
    sys3(4004, sock, (long)line, slen(line));
    { const char *h2 = "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n"; sys3(4004, sock, (long)h2, slen(h2)); }
    sys3(4004, sock, (long)body, slen(body));
}
static void send_headers(int sock) {
    const char *h = "HTTP/1.0 200 OK\r\nContent-Type: video/MP2T\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    sys3(4004, sock, (long)h, slen(h));
}

/* 1回の選局+受信セッション。http>0 のとき out_fd がソケットで、準備できた時点でHTTPヘッダを送る。戻り値は終了コード */
static int session(int is_sat, unsigned long freq_val, unsigned long ts_id, unsigned long want_sid, unsigned long dur_ms, int ofd, int http) {
    int rc, sel; struct svc *s; unsigned long t_start, nto = 0;
    out_fd = ofd; olen = 0; stop_flag = 0; prev_pt = prev_sec = 0xff; h_pt0 = h_pt1 = h_sec0 = h_sec1 = h_g0 = h_g1 = 0;
    cur_pmt_pid = 0;
    lgx("session start freq", freq_val);
    if (is_sat) lgx("session ts_id", ts_id);
    rc = acquire_tuner(is_sat);
    /* HTTPでは、直前のセッションの終了処理中(ロックがまだ残っている)にすぐ次の要求が来ることがある(tvheadend・VLCのプレイリストの次送り等)。
     * すぐ503を返さず、最大 BUSY_WAIT_MS だけ待って再試行する(本当に使用中なら、そのあと503) */
    if (rc == 3 && http) { unsigned long w; for (w = 0; w < BUSY_WAIT_MS && rc == 3; w += 100) { msleep(100); rc = acquire_tuner(is_sat); } }
    if (rc == 3) { err("チューナー使用中\n"); if (http) http_status(ofd, "HTTP/1.0 503 Service Unavailable", "tuner busy\n"); return 3; }
    if (rc) { if (http) http_status(ofd, "HTTP/1.0 500 Internal Server Error", "driver/firmware error\n"); release_tuner(); return 1; }
    lgx("tuner acquired", 0);
    if (is_sat) {
        if (!fe_tune_s(freq_val, ts_id)) { err("衛星ロックしなかった(信号なし)\n"); if (http) http_status(ofd, "HTTP/1.0 404 Not Found", "no signal\n"); release_tuner(); return 1; }
    } else {
        if (!fe_tune(freq_val)) { err("地上波ロックしなかった(信号なし)\n"); if (http) http_status(ofd, "HTTP/1.0 404 Not Found", "no signal\n"); release_tuner(); return 1; }
    }
    clr(ob_pt, 0x484 / 4 + 2);
    ob_pt[0xac / 4] = 0x800000; ob_pt[0xbc / 4] = is_sat ? 1 : 0; ob_pt[0xc8 / 4] = 0xffff; ob_pt[0xd0 / 4] = 0x10000;
    if (ioc(ob_pt, 0x484, 0x2d, 2000) != 1) { err("PSIストリームを開けない\n"); if (http) http_status(ofd, "HTTP/1.0 500 Internal Server Error", "psi stream\n"); release_tuner(); return 1; }
    h_pt0 = ob_pt[0xa4 / 4]; h_pt1 = ob_pt[0xa8 / 4];
    lgx("psi stream opened", h_pt0);
    sel = discover(want_sid, 8000);
    lgx("discover sel", (unsigned long)sel);
    if (sel < 0) { err("サービスが見つからない\n"); if (http) http_status(ofd, "HTTP/1.0 404 Not Found", "service not found\n"); close_streams(); release_tuner(); return 1; }
    s = &svcs[sel];
    if (is_sat) cur_pmt_pid = s->pmt;
    if (verbose) { err("サービス 0x"); errhex(s->pn); err(" video 0x"); errhex(s->vpid); err(" audio 0x"); errhex(s->apid); err(" pcr 0x"); errhex(s->pcr); err(" ecm 0x"); errhex(s->ecm); err("\n"); }
    lgx("open_streams begin", 0);
    if (open_streams(s, is_sat) < 0) { if (http) http_status(ofd, "HTTP/1.0 500 Internal Server Error", "secured stream\n"); close_streams(); release_tuner(); return 1; }
    lgx("streaming begin", 0);
    if (http) send_headers(ofd);
    t_start = now_ms();
    while (!stop_flag) {
        unsigned long rv, idx, sz, k;
        if (dur_ms && now_ms() - t_start >= dur_ms) break;
        clr(rb, 0x230 / 4);
        rb[0xa4 / 4] = h_sec0; rb[0xa8 / 4] = h_sec1; rb[0xac / 4] = 0x2000; rb[0xbc / 4] = 0x10000; rb[0x144 / 4] = prev_sec;
        rv = ioc(rb, 0x230, 0x14, 200);
        if (rv == 1) {
            nto = 0;
            idx = rb[0x13c / 4]; sz = rb[0xbc / 4];
            if (idx < 16 && sz >= 192) {
                unsigned char *p = (unsigned char *)ob_sec[0x2d4 / 4 + idx];
                if (p) for (k = 0; k + 192 <= sz; k += 192) if (p[k + 4] == 0x47 && (p[k + 5] & 0x1f) + p[k + 6] != 0) put_pkt(p + k + 4);   /* PAT(PID 0)はファームが作り直した1サービス分ではなく、PSIストリームの元のPAT(NITのPIDも載っている)を使う */
                prev_sec = idx;
            }
        } else if (rv == 6) {
            if (++nto > 100) { err("出力が止まった\n"); break; }          /* 約20秒 */
        } else { err("RECV エラー rv="); errhex(rv); err("\n"); break; }
        {   /* SI(NIT/SDT/EIT/TOT 等)およびPMTをPSIストリームから混ぜる */
            unsigned char *p; unsigned long psz;
            if (recv_pt(1, &p, &psz)) for (k = 0; k + 188 <= psz; k += 188)
                if (p[k] == 0x47 && is_si_pid(((p[k + 1] & 0x1f) << 8) | p[k + 2])) put_pkt(p + k);
        }
        if (olen) flush_out();
    }
    if (olen) flush_out();
    lgx("loop end stop_flag", (unsigned long)stop_flag);
    cur_pmt_pid = 0;
    close_streams();
    lgx("closed", 0);
    release_tuner();
    lgx("session end", 0);
    return 0;
}

static void hexw(unsigned long v) { char b[9]; int i, started = 0, o = 0; for (i = 0; i < 8; i++) { int n = (v >> ((7 - i) * 4)) & 0xf; if (n || started || i == 7) { b[o++] = n < 10 ? '0' + n : 'a' + n - 10; started = 1; } } b[o] = 0; sys3(4004, out_fd, (long)b, o); }
static void outs(const char *x) { sys3(4004, out_fd, (long)x, slen(x)); }
static void outdec(unsigned long v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); outs(b + i); }


/* ---- SDT(サービス名)と ARIB 8単位符号 → UTF-8 ---- */
#define GS_NONE 0
#define GS_KANJI 1
#define GS_ALNUM 2
#define GS_HIRA 3
#define GS_KATA 4
#define GS_X0201K 5
#define GS_UNK1 6
#define GS_UNK2 7
static int put_u8(unsigned char *o, int n, int max, unsigned cp) {
    if (cp < 0x80) { if (n + 1 < max) o[n++] = (unsigned char)cp; }
    else if (cp < 0x800) { if (n + 2 < max) { o[n++] = 0xc0 | (cp >> 6); o[n++] = 0x80 | (cp & 0x3f); } }
    else { if (n + 3 < max) { o[n++] = 0xe0 | (cp >> 12); o[n++] = 0x80 | ((cp >> 6) & 0x3f); o[n++] = 0x80 | (cp & 0x3f); } }
    return n;
}
static int gset_of(int two, unsigned f) {
    if (two) return f == 0x42 || f == 0x39 ? GS_KANJI : GS_UNK2;
    switch (f) {
        case 0x4a: case 0x36: return GS_ALNUM;
        case 0x30: case 0x37: return GS_HIRA;
        case 0x31: case 0x38: return GS_KATA;
        case 0x49: return GS_X0201K;
        default: return GS_UNK1;
    }
}
/* hira/kata の 0x21〜0x7E → Unicode */
static unsigned kana_cp(int set, unsigned b) {
    unsigned base = set == GS_HIRA ? 0x3041 : 0x30a1;
    if (b >= 0x21 && b <= (set == GS_HIRA ? 0x73u : 0x76u)) return base + (b - 0x21);
    switch (b) {
        case 0x77: return set == GS_HIRA ? 0x309d : 0x30fd;
        case 0x78: return set == GS_HIRA ? 0x309e : 0x30fe;
        case 0x79: return 0x30fc; case 0x7a: return 0x3002; case 0x7b: return 0x300c; case 0x7c: return 0x300d;
        case 0x7d: return 0x3001; case 0x7e: return 0x30fb;
    }
    return '?';
}
/* ARIB STD-B24 の8単位符号(初期状態: G0=漢字 G1=英数 G2=ひらがな G3=カタカナ(放送の運用に合わせた。実際に1b7c+カタカナで来る)、GL=G0 GR=G2)を UTF-8 にする。戻り値は出力バイト数 */
static int arib_to_utf8(const unsigned char *s, int len, unsigned char *o, int max) {
    int g[4], gl = 0, gr = 2, ss = -1, i = 0, n = 0;
    g[0] = GS_KANJI; g[1] = GS_ALNUM; g[2] = GS_HIRA; g[3] = GS_KATA;
    while (i < len) {
        unsigned b = s[i++]; int set, hi = b >= 0xa1 && b <= 0xfe, gl_ok = b >= 0x21 && b <= 0x7e;
        if (b == 0x0f) { gl = 0; continue; } if (b == 0x0e) { gl = 1; continue; }
        if (b == 0x19) { ss = 2; continue; } if (b == 0x1d) { ss = 3; continue; }
        if (b == 0x1b && i < len) {
            unsigned c = s[i++];
            if (c == 0x6e) gl = 2; else if (c == 0x6f) gl = 3; else if (c == 0x7e) gr = 1; else if (c == 0x7d) gr = 2; else if (c == 0x7c) gr = 3;
            else if (c >= 0x28 && c <= 0x2b && i < len) { unsigned f = s[i++]; if (f == 0x20 && i < len) i++; else g[c - 0x28] = gset_of(0, f); }
            else if (c == 0x24 && i < len) {
                unsigned d = s[i++];
                if (d >= 0x29 && d <= 0x2b && i < len) { unsigned f = s[i++]; if (f == 0x20 && i < len) i++; else g[d - 0x28] = gset_of(1, f); }
                else g[0] = gset_of(1, d);
            }
            continue;
        }
        if (b == 0x20 || b == 0xa0) { n = put_u8(o, n, max, ' '); continue; }
        if (!gl_ok && !hi) continue;
        set = ss >= 0 ? g[ss] : (hi ? g[gr] : g[gl]); ss = -1;
        if (hi) b -= 0x80;
        if (set == GS_KANJI || set == GS_UNK2) {
            unsigned c2, cp;
            if (i >= len) break;
            c2 = s[i++] & 0x7f;
            cp = (set == GS_KANJI && b >= 0x21 && b <= 0x74 && c2 >= 0x21 && c2 <= 0x7e) ? jis0208[(b - 0x21) * 94 + (c2 - 0x21)] : 0;
            n = put_u8(o, n, max, cp ? cp : '?');
        } else if (set == GS_ALNUM) n = put_u8(o, n, max, b);
        else if (set == GS_HIRA || set == GS_KATA) n = put_u8(o, n, max, kana_cp(set, b));
        else if (set == GS_X0201K) n = put_u8(o, n, max, b >= 0x21 && b <= 0x5f ? 0xff61 + (b - 0x21) : '?');
        else n = put_u8(o, n, max, '?');
    }
    if (n < max) o[n] = 0;
    return n;
}

struct sdt_ent { unsigned long sid; unsigned char name[100]; };
static struct sdt_ent sdt_tab[MAX_SEC]; static int nsdt;
static void sdt_parse(const unsigned char *s) {
    unsigned long ln = (((s[1] & 0xf) << 8) | s[2]) + 3, e = 11;
    while (e + 5 <= ln - 4 && nsdt < MAX_SEC) {
        unsigned long sid = (s[e] << 8) | s[e + 1], dl = ((s[e + 3] & 0xf) << 8) | s[e + 4], d = e + 5, de = e + 5 + dl;
        int k;
        for (k = 0; k < nsdt; k++) if (sdt_tab[k].sid == sid) break;
        if (k == nsdt && de <= ln - 4) {
            for (; d + 2 <= de; d += 2 + s[d + 1]) {
                if (s[d] == 0x48 && d + 4 <= de) {          /* サービス記述子: type, provider長+名, service名長+名 */
                    unsigned long pl = s[d + 3], nl;
                    if (d + 4 + pl >= de) break;
                    nl = s[d + 4 + pl];
                    if (d + 5 + pl + nl > de) nl = de - (d + 5 + pl);
                    sdt_tab[k].sid = sid; arib_to_utf8(s + d + 5 + pl, (int)nl, sdt_tab[k].name, sizeof(sdt_tab[k].name));
                    nsdt++;
                    break;
                }
            }
        }
        e = de;
    }
}
/* PSIストリームから PID 0x11 の SDT(自TS, table_id 0x42)を集める */
static void collect_sdt(unsigned long tmo_ms) {
    unsigned long t0 = now_ms(); unsigned seen = 0;
    nsdt = 0; sec_nsec = 1; sec_pid[0] = 0x11; sec_have[0] = 0;
    while (now_ms() - t0 < tmo_ms && !stop_flag) {
        unsigned char *p; unsigned long sz, k;
        if (!recv_pt(500, &p, &sz)) continue;
        for (k = 0; k + 188 <= sz; k += 188) if (p[k] == 0x47) {
            feed(p + k);
            if (sec_complete(0)) {
                unsigned char *s = sec_buf[0];
                if (s[0] == 0x42) { unsigned sn = s[6], ls = s[7]; if (!(seen & (1u << (sn & 7)))) { seen |= 1u << (sn & 7); sdt_parse(s); } if (seen == (1u << (ls + 1)) - 1) return; }
                sec_have[0] = 0;
            }
        }
    }
}
static const char *sdt_name(unsigned long sid) { int k; for (k = 0; k < nsdt; k++) if (sdt_tab[k].sid == sid) return (const char *)sdt_tab[k].name; return 0; }

/* サービス一覧のキャッシュ: /var/lib/nasne-recpt1/services.tsv(「ch<TAB>sid(10進)<TAB>名前」)。/playlist.m3u8 が使う */
#define CACHE_DIR "/var/lib/nasne-recpt1"
#define CACHE_TMP CACHE_DIR "/services.tsv.tmp"
#define CACHE_FILE CACHE_DIR "/services.tsv"
static long cache_fd = -1;

/* チャンネルスキャン: UHF 13〜62 を順に選局し、ロックしたものについてサービス一覧を出力 */
/* BSスキャン用トランスポンダ・スロット一覧 */
static const struct { unsigned char ch; unsigned char slot; } BS_SCAN_TP[] = {
    { 1, 0 }, { 1, 1 }, { 1, 2 },
    { 3, 0 }, { 3, 1 }, { 3, 2 },
    { 5, 0 }, { 5, 1 },
    { 9, 0 }, { 9, 1 }, { 9, 2 },
    { 11, 0 }, { 11, 1 }, { 11, 2 },
    { 13, 0 }, { 13, 1 }, { 13, 2 },
    { 15, 0 }, { 15, 1 }, { 15, 2 },
    { 19, 0 }, { 19, 1 }, { 19, 2 }, { 19, 3 },
    { 21, 0 }, { 21, 1 }, { 21, 2 },
    { 23, 0 }, { 23, 1 }, { 23, 2 }, { 23, 3 },
};

/* チャンネルスキャン: mode 0=地デジのみ, 1=衛星(BS/CS)のみ, 2=全チャンネル(地デジ+BS/CS) */
static int scan(int ofd, int mode) {
    unsigned long ch; int rc;
    out_fd = ofd; stop_flag = 0;
    sys3(SYS_mkdir, (long)"/var/lib", 0755, 0); sys3(SYS_mkdir, (long)CACHE_DIR, 0755, 0);
    cache_fd = sys3(SYS_open, (long)CACHE_TMP, 0x100 | 0x200 | 0x1 /* O_CREAT|O_TRUNC|O_WRONLY(MIPS) */, 0644);

    /* 1. 地デジスキャン (mode 0 または 2) */
    if ((mode == 0 || mode == 2) && !stop_flag) {
        rc = acquire_tuner(0);
        if (rc) { err("地デジチューナーを確保できない\n"); if (cache_fd >= 0) sys3(SYS_close, cache_fd, 0, 0); return rc; }
        for (ch = 13; ch <= 62 && !stop_flag; ch++) {
            unsigned long hz = uhf_hz(ch); int i;
            if (!fe_tune_quick(hz)) continue;
            clr(ob_pt, 0x484 / 4 + 2);
            ob_pt[0xac / 4] = 0x800000; ob_pt[0xbc / 4] = 0; ob_pt[0xc8 / 4] = 0xffff; ob_pt[0xd0 / 4] = 0x10000;
            if (ioc(ob_pt, 0x484, 0x2d, 2000) != 1) continue;
            h_pt0 = ob_pt[0xa4 / 4]; h_pt1 = ob_pt[0xa8 / 4]; prev_pt = 0xff;
            discover(0, 4000);
            collect_sdt(2500);
            close_streams();
            outs("ch="); outdec(ch); outs(" freq="); outdec(hz / 1000); outs("kHz");
            for (i = 0; i < nsvc; i++) if (svcs[i].ok) {
                const char *nm = sdt_name(svcs[i].pn); int tv = svcs[i].vpid != 0 && !svcs[i].seg1;
                outs(" sid=0x"); hexw(svcs[i].pn); outs(tv ? "(tv)" : svcs[i].seg1 ? "(1seg)" : "(data)");
                if (nm && *nm) { outs(" name=\""); { const char *q; for (q = nm; *q; q++) { char c[2]; c[0] = *q == '"' ? '\'' : *q; c[1] = 0; outs(c); } } outs("\""); }
                if (tv && cache_fd >= 0) {
                    char tmp[12]; unsigned long v = ch; int ti = 11; tmp[ti] = 0; do { tmp[--ti] = '0' + v % 10; v /= 10; } while (v);
                    sys3(SYS_write, cache_fd, (long)(tmp + ti), slen(tmp + ti)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    v = svcs[i].pn; ti = 11; tmp[ti] = 0; do { tmp[--ti] = '0' + v % 10; v /= 10; } while (v);
                    sys3(SYS_write, cache_fd, (long)(tmp + ti), slen(tmp + ti)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    if (nm && *nm) sys3(SYS_write, cache_fd, (long)nm, slen(nm));
                    sys3(SYS_write, cache_fd, (long)"\n", 1);
                }
            }
            outs("\n");
        }
        release_tuner();
    }

    /* 2. 衛星 (BS/CS) スキャン (mode 1 または 2) */
    if ((mode == 1 || mode == 2) && !stop_flag) {
        int idx;
        rc = acquire_tuner(1);
        if (rc) { err("衛星チューナーを確保できない\n"); if (cache_fd >= 0) sys3(SYS_close, cache_fd, 0, 0); return rc; }

        /* BSスキャン */
        for (idx = 0; idx < (int)(sizeof(BS_SCAN_TP) / sizeof(BS_SCAN_TP[0])) && !stop_flag; idx++) {
            unsigned long bsch = BS_SCAN_TP[idx].ch;
            unsigned long slot = BS_SCAN_TP[idx].slot;
            unsigned long ts_id = get_bs_tsid(bsch, slot);
            unsigned long if_khz = (11727480UL + ((bsch - 1) / 2) * 38360UL) - LO_KHZ;
            int i;
            char ch_name[16]; int cni = 0;

            ch_name[cni++] = 'B'; ch_name[cni++] = 'S';
            ch_name[cni++] = '0' + (bsch / 10); ch_name[cni++] = '0' + (bsch % 10);
            ch_name[cni++] = '_'; ch_name[cni++] = '0' + slot; ch_name[cni] = 0;

            if (!fe_tune_s(if_khz, ts_id)) continue;
            clr(ob_pt, 0x484 / 4 + 2);
            ob_pt[0xac / 4] = 0x800000; ob_pt[0xbc / 4] = 1; ob_pt[0xc8 / 4] = 0xffff; ob_pt[0xd0 / 4] = 0x10000;
            if (ioc(ob_pt, 0x484, 0x2d, 2000) != 1) continue;
            h_pt0 = ob_pt[0xa4 / 4]; h_pt1 = ob_pt[0xa8 / 4]; prev_pt = 0xff;
            discover(0, 3500);
            collect_sdt(2000);
            close_streams();

            outs("channel="); outs(ch_name); outs(" tsid=0x"); hexw(ts_id);
            for (i = 0; i < nsvc; i++) if (svcs[i].ok) {
                const char *nm = sdt_name(svcs[i].pn); int tv = svcs[i].vpid != 0 && !svcs[i].seg1;
                outs(" sid=0x"); hexw(svcs[i].pn); outs(tv ? "(tv)" : svcs[i].seg1 ? "(1seg)" : "(data)");
                if (nm && *nm) { outs(" name=\""); { const char *q; for (q = nm; *q; q++) { char c[2]; c[0] = *q == '"' ? '\'' : *q; c[1] = 0; outs(c); } } outs("\""); }
                if (tv && cache_fd >= 0) {
                    char tmp[12]; unsigned long v = svcs[i].pn; int ti = 11;
                    sys3(SYS_write, cache_fd, (long)ch_name, slen(ch_name)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    tmp[ti] = 0; do { tmp[--ti] = '0' + v % 10; v /= 10; } while (v);
                    sys3(SYS_write, cache_fd, (long)(tmp + ti), slen(tmp + ti)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    if (nm && *nm) sys3(SYS_write, cache_fd, (long)nm, slen(nm));
                    sys3(SYS_write, cache_fd, (long)"\n", 1);
                }
            }
            outs("\n");
        }

        /* CSスキャン (ND2〜ND24 偶数) */
        for (ch = 2; ch <= 24 && !stop_flag; ch += 2) {
            unsigned long ts_id = get_cs_tsid(ch);
            unsigned long if_khz = (12291000UL + ((ch - 2) / 2) * 40000UL) - LO_KHZ;
            int i;
            if (!fe_tune_s(if_khz, ts_id)) continue;
            clr(ob_pt, 0x484 / 4 + 2);
            ob_pt[0xac / 4] = 0x800000; ob_pt[0xbc / 4] = 1; ob_pt[0xc8 / 4] = 0xffff; ob_pt[0xd0 / 4] = 0x10000;
            if (ioc(ob_pt, 0x484, 0x2d, 2000) != 1) continue;
            h_pt0 = ob_pt[0xa4 / 4]; h_pt1 = ob_pt[0xa8 / 4]; prev_pt = 0xff;
            discover(0, 3500);
            collect_sdt(2000);
            close_streams();

            outs("channel=CS"); outdec(ch); outs(" tsid=0x"); hexw(ts_id);
            for (i = 0; i < nsvc; i++) if (svcs[i].ok) {
                const char *nm = sdt_name(svcs[i].pn); int tv = svcs[i].vpid != 0 && !svcs[i].seg1;
                outs(" sid=0x"); hexw(svcs[i].pn); outs(tv ? "(tv)" : svcs[i].seg1 ? "(1seg)" : "(data)");
                if (nm && *nm) { outs(" name=\""); { const char *q; for (q = nm; *q; q++) { char c[2]; c[0] = *q == '"' ? '\'' : *q; c[1] = 0; outs(c); } } outs("\""); }
                if (tv && cache_fd >= 0) {
                    char ch_name[16]; int cni = 0; char tmp[12]; unsigned long v; int ti;
                    ch_name[cni++] = 'C'; ch_name[cni++] = 'S';
                    if (ch >= 10) { ch_name[cni++] = '0' + (ch / 10); ch_name[cni++] = '0' + (ch % 10); }
                    else { ch_name[cni++] = '0' + ch; }
                    ch_name[cni++] = '_';
                    v = svcs[i].pn; ti = 11; tmp[ti] = 0; do { tmp[--ti] = '0' + v % 10; v /= 10; } while (v);
                    while (tmp[ti]) ch_name[cni++] = tmp[ti++];
                    ch_name[cni] = 0;

                    sys3(SYS_write, cache_fd, (long)ch_name, slen(ch_name)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    v = svcs[i].pn; ti = 11; tmp[ti] = 0; do { tmp[--ti] = '0' + v % 10; v /= 10; } while (v);
                    sys3(SYS_write, cache_fd, (long)(tmp + ti), slen(tmp + ti)); sys3(SYS_write, cache_fd, (long)"\t", 1);
                    if (nm && *nm) sys3(SYS_write, cache_fd, (long)nm, slen(nm));
                    sys3(SYS_write, cache_fd, (long)"\n", 1);
                }
            }
            outs("\n");
        }
        release_tuner();
    }

    if (cache_fd >= 0) {
        sys3(SYS_close, cache_fd, 0, 0); cache_fd = -1;
        if (!stop_flag) sys3(SYS_rename, (long)CACHE_TMP, (long)CACHE_FILE, 0);      /* 最後まで走ったときだけ置き換える */
    }
    return 0;
}

/* リクエストの Host: ヘッダ(無ければ空)を取り出す */
static int get_host(const char *req, char *out, int max) {
    const char *q; int n = 0;
    for (q = req; *q; q++) {
        if ((q == req || q[-1] == '\n') && (q[0] | 32) == 'h' && (q[1] | 32) == 'o' && (q[2] | 32) == 's' && (q[3] | 32) == 't' && q[4] == ':') {
            q += 5; while (*q == ' ') q++;
            while (*q && *q != '\r' && *q != '\n' && n < max - 1) out[n++] = *q++;
            break;
        }
    }
    out[n] = 0; return n;
}
static void send_playlist(int sock, const char *req) {
    static char buf[16384]; char host[128]; long f, r, total = 0; int hl = get_host(req, host, sizeof(host)), i, start;
    static char out[24576]; int on = 0;
    f = sys3(SYS_open, (long)CACHE_FILE, 0, 0);
    if (f < 0) { http_status(sock, "HTTP/1.0 404 Not Found", "サービス一覧のキャッシュがありません。先に /scan を実行してください\n"); return; }
    while ((r = sys3(SYS_read, f, (long)(buf + total), sizeof(buf) - 1 - total)) > 0) total += r;
    sys3(SYS_close, f, 0, 0); buf[total] = 0;
    if (!hl) { host[0] = 0; }
#define OPUT(str) do { const char *z_ = (str); while (*z_ && on < (int)sizeof(out) - 1) out[on++] = *z_++; } while (0)
    OPUT("#EXTM3U\n");
    for (i = 0, start = 0; i <= total; i++) {
        if (i == total || buf[i] == '\n') {
            if (i > start) {
                char *ln = buf + start; char *t1, *t2; char chs[16], sidd[12], sidh[8]; unsigned long sid = 0; int k;
                buf[i] = 0;
                t1 = ln; while (*t1 && *t1 != '\t') t1++;
                if (*t1) { *t1++ = 0; t2 = t1; while (*t2 && *t2 != '\t') t2++; if (*t2) *t2++ = 0; else t2 = t1 + slen(t1); }
                else t2 = t1;
                for (k = 0; k < 15 && ln[k]; k++) chs[k] = ln[k]; chs[k] = 0;
                for (k = 0; k < 11 && t1[k]; k++) sidd[k] = t1[k]; sidd[k] = 0;
                sid = parse_dec(sidd);
                for (k = 0; k < 4; k++) { int nb = (sid >> ((3 - k) * 4)) & 0xf; sidh[k] = nb < 10 ? '0' + nb : 'a' + nb - 10; } sidh[4] = 0;
                if ((chs[0] | 32) == 'b' && (chs[1] | 32) == 's') {
                    OPUT("#EXTINF:-1 group-title=\"BS\",");
                } else if ((chs[0] | 32) == 'c' && (chs[1] | 32) == 's') {
                    OPUT("#EXTINF:-1 group-title=\"CS\",");
                } else {
                    OPUT("#EXTINF:-1 group-title=\"UHF\",");
                }
                if (*t2) OPUT(t2); else { OPUT(chs); OPUT(" 0x"); OPUT(sidh); }
                OPUT("\nhttp://"); OPUT(host); OPUT("/tuner/"); OPUT(chs); OPUT("?sid=0x"); OPUT(sidh); OPUT("\n");
            }
            start = i + 1;
        }
    }
#undef OPUT
    { const char *h = "HTTP/1.0 200 OK\r\nContent-Type: audio/x-mpegurl; charset=utf-8\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"; sys3(4004, sock, (long)h, slen(h)); }
    sys3(4004, sock, (long)out, on);
}

static int listen_port(unsigned long port) {
    long ls, one = 1; unsigned char sa[16]; unsigned long sg[6];
    int i;
    /* SIGCHLD(MIPSは18)を無視して子プロセスを回収 */
    sg[0] = 0; sg[1] = 1; sg[2] = sg[3] = sg[4] = sg[5] = 0; sys6(SYS_rt_sigaction, 18, (long)sg, 0, 16, 0, 0);
    ls = sys3(4183, 2, 2, 0);                                                    /* socket(AF_INET, SOCK_STREAM(MIPS=2), 0) */
    if (ls < 0) { err("socket失敗\n"); return 1; }
    sys6(4181, ls, 0xffff, 4, (long)&one, 4, 0);                                  /* SOL_SOCKET(MIPS=0xffff), SO_REUSEADDR(=4) */
    for (i = 0; i < 16; i++) sa[i] = 0;
    sa[0] = 2; sa[2] = (port >> 8) & 0xff; sa[3] = port & 0xff;                  /* AF_INET(リトルエンディアンの16ビットなので先頭バイト。2.6.29 は family を見なかったが、新しいカーネルは見る)、ポート(ネットワークバイトオーダー)、0.0.0.0 */
    if (sys3(4169, ls, (long)sa, 16) < 0) { err("bind失敗\n"); return 1; }
    sys3(4174, ls, 8, 0);
    err("nasne-recpt1: listening on port "); errhex(port); err(" (hex)\n");
    for (;;) {
        long c = sys3(4168, ls, 0, 0), pid;
        if (c < 0) { msleep(100); continue; }
        pid = sys3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            char req[1024]; long n; unsigned long ch = 0, sid = 0, sec = 0; char *p;
            sys3(SYS_close, ls, 0, 0);
            n = sys3(SYS_read, c, (long)req, sizeof(req) - 1);
            if (n <= 0) sys3(SYS_exit, 0, 0, 0);
            req[n] = 0;
            if (!(req[0] == 'G' && req[1] == 'E' && req[2] == 'T' && req[3] == ' ')) { http_status(c, "HTTP/1.0 400 Bad Request", "GET only\n"); sys3(SYS_exit, 0, 0, 0); }
            p = req + 4;
            if (*p == '/') p++;
            if (starts(p, "status")) {                                         /* /status: tuner idle|busy */
                long l2 = sys3(SYS_open, (long)"/tmp/nasne-recpt1.lock", 0x100 | 2, 0644);
                int busy = sys3(SYS_flock, l2, 2 | 4, 0) < 0;
                http_status(c, "HTTP/1.0 200 OK", busy ? "busy\n" : "idle\n");
                sys3(SYS_exit, 0, 0, 0);
            }
            if (starts(p, "playlist")) {                                       /* /playlist.m3u8: /scan のキャッシュからVLC等向けのM3Uを作る */
                send_playlist(c, req);
                sys3(SYS_exit, 0, 0, 0);
            }
            if (starts(p, "scan")) {                                           /* /scan: サービス一覧 (既定: 地デジ, ?sat: 衛星のみ, ?all: 全局) */
                int sat_mode = 0;
                const char *sp = p + 4;
                if (*sp == '?' || *sp == '&') {
                    if (starts(sp + 1, "type=sat") || starts(sp + 1, "sat")) sat_mode = 1;
                    else if (starts(sp + 1, "type=all") || starts(sp + 1, "all")) sat_mode = 2;
                }
                http_status(c, "HTTP/1.0 200 OK", "");
                set_sig(SIGINT_); set_sig(SIGTERM_); set_sig(SIGPIPE_);
                scan(c, sat_mode);
                sys3(SYS_exit, 0, 0, 0);
            }
            if (starts(p, "tuner/")) p += 6;                                   /* /tuner/<ch>?sid=0xNNNN&sec=N  または /<ch>?... */
            {
                char ch_buf[32]; int ci = 0; int is_sat = 0; unsigned long freq_val = 0, ts_id = 0, parsed_ts_id = 0;
                while (*p && *p != '?' && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n' && ci < 31) {
                    ch_buf[ci++] = *p++;
                }
                ch_buf[ci] = 0;
                while (*p == '?' || *p == '&') {
                    p++;
                    if (starts(p, "sid=")) {
                        p += 4;
                        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; while ((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f')) { sid = sid * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); p++; } }
                        else while (*p >= '0' && *p <= '9') { sid = sid * 10 + (*p - '0'); p++; }
                    } else if (starts(p, "tsid=")) {
                        p += 5;
                        ts_id = 0;
                        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; while ((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'f')) { ts_id = ts_id * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); p++; } }
                        else while (*p >= '0' && *p <= '9') { ts_id = ts_id * 10 + (*p - '0'); p++; }
                    } else if (starts(p, "sec=")) { p += 4; while (*p >= '0' && *p <= '9') { sec = sec * 10 + (*p - '0'); p++; } }
                    else break;
                }
                if (!parse_channel_str(ch_buf, &is_sat, &freq_val, &parsed_ts_id)) {
                    http_status(c, "HTTP/1.0 400 Bad Request", "channel must be UHF 13-62 (e.g. 31), BS<odd 1-23>[_<slot>] (e.g. BS15_0 or BS1), or CS<even 2-24> (e.g. CS2). Mirakurun BS channel names 101-256 are not supported: scan with useSubCh=1 (BS01_0 style). Add ?sid=0x<serviceId>.\n");
                    sys3(SYS_exit, 0, 0, 0);
                }
                if (!ts_id) ts_id = parsed_ts_id;
                set_sig(SIGINT_); set_sig(SIGTERM_); set_sig(SIGPIPE_);
                session(is_sat, freq_val, ts_id, sid, sec * 1000, (int)c, 1);
                sys3(SYS_close, c, 0, 0);
                sys3(SYS_exit, 0, 0, 0);
            }
        }
        sys3(SYS_close, c, 0, 0);
    }
    return 0;
}

int main_c(long *sp) {
    int argc = (int)sp[0], ai, npos = 0, do_scan = 0;
    char **argv = (char **)(sp + 1);
    const char *pos[3] = { 0, 0, 0 };
    unsigned long want_sid = 0, freq_hz = 0, dur_ms = 0, port = 0, cli_tsid = 0; int ofd = 1;
    for (ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        if (streq(a, "--sid") && ai + 1 < argc) { const char *v = argv[++ai]; want_sid = (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) ? parse_hex(v + 2) : parse_dec(v); }
        else if (streq(a, "--tsid") && ai + 1 < argc) { const char *v = argv[++ai]; cli_tsid = (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) ? parse_hex(v + 2) : parse_dec(v); }
        else if (streq(a, "--freq") && ai + 1 < argc) freq_hz = parse_mhz(argv[++ai]);
        else if (streq(a, "--listen") && ai + 1 < argc) port = parse_dec(argv[++ai]);
        else if (streq(a, "--scan")) do_scan = 1;
        else if (streq(a, "--scan-sat")) do_scan = 2;
        else if (streq(a, "--scan-all")) do_scan = 3;
        else if (streq(a, "-v")) verbose = 1;
        else if (streq(a, "--device") || streq(a, "--lnb") || streq(a, "--udp") || streq(a, "--port") || streq(a, "--http")) ai++;
        else if (a[0] == '-' && a[1] == '-') { /* --b25 --strip 等の recpt1 オプションは無視 */ }
        else if (npos < 3) pos[npos++] = a;
    }
    if (port) { logv = 1; return listen_port(port); }        /* 待ち受け(親)は既定のシグナル動作のまま。各子プロセスがハンドラを設定する */
    set_sig(SIGINT_); set_sig(SIGTERM_); set_sig(SIGPIPE_);
    if (do_scan) return scan(1, do_scan == 2 ? 1 : do_scan == 3 ? 2 : 0);
    if (npos < 3 - (freq_hz ? 1 : 0)) { err("usage: nasne-recpt1 [--sid N] [--tsid N] [-v] <ch 13-62 | BS1-BS23(奇数。BS15_0 のようにスロット付きも可) | CS2-CS24(偶数) | --freq MHz> <秒数|-> <出力|->\n       nasne-recpt1 --scan [--scan-sat | --scan-all] | --listen <port>\n"); return 2; }
    {
        const char *chs, *secs, *dst;
        int is_sat = 0; unsigned long freq_val = 0, parsed_tsid = 0;
        if (freq_hz) { secs = pos[0]; dst = pos[1]; chs = 0; freq_val = freq_hz; is_sat = 0; }
        else { chs = pos[0]; secs = pos[1]; dst = pos[2]; }
        if (chs) {
            if (!parse_channel_str(chs, &is_sat, &freq_val, &parsed_tsid)) {
                err("チャンネルは UHF 13〜62、BS1〜BS23、または CS2〜CS24 で指定してください\n");
                return 2;
            }
        }
        if (!cli_tsid) cli_tsid = parsed_tsid;
        dur_ms = streq(secs, "-") ? 0 : parse_dec(secs) * 1000;
        if (!streq(dst, "-")) { ofd = sys3(SYS_creat, (long)dst, 0644, 0); if (ofd < 0) { err("出力ファイルを開けない\n"); return 1; } }
        return session(is_sat, freq_val, cli_tsid, want_sid, dur_ms, ofd, 0);
    }
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
