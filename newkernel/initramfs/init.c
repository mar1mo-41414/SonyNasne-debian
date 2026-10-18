/* M0 用の最小 init(libc なし): 新カーネルが userspace(PID 1)まで来たことを、/dev/kmsg 経由の printk(早期コンソール)で出す。
 * ビルド: mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -fno-builtin -e __start -o init init.c */
#define SYS_exit     4001
#define SYS_write    4004
#define SYS_open     4005
#define SYS_read     4003
#define SYS_close    4006
#define SYS_mount    4021
#define SYS_nanosleep 4166
#define SYS_mmap2    4210

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
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

static void sleep_ms(long ms)
{
	long ts[2] = { ms / 1000, (ms % 1000) * 1000000L };

	sys6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

static void put(long fd, const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sys6(SYS_write, fd, (long)s, n, 0, 0, 0);
}

/* ファイル全体を読んで、1行ずつ "<6>" を付けて kmsg へ(1回の write は1レコード) */
static void dump_file(long kmsg, const char *path)
{
	static char buf[2048], line[300];
	long fd, n, a = 0, b, k;

	fd = sys6(SYS_open, (long)path, 0, 0, 0, 0, 0);
	if (fd < 0)
		return;
	n = sys6(SYS_read, fd, (long)buf, sizeof(buf) - 1, 0, 0, 0);
	sys6(SYS_close, fd, 0, 0, 0, 0, 0);
	if (n <= 0)
		return;
	while (a < n) {
		for (b = a; b < n && buf[b] != '\n'; b++)
			;
		line[0] = '<'; line[1] = '6'; line[2] = '>';
		for (k = 0; a + k < b && k < (long)sizeof(line) - 5; k++)
			line[3 + k] = buf[a + k];
		line[3 + k] = 0;
		put(kmsg, line);
		a = b + 1;
	}
}

void __start_c(void)
{
	long kmsg;

	sys6(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0, 0);
	sys6(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0, 0);
	sys6(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0, 0);
	kmsg = sys6(SYS_open, (long)"/dev/kmsg", 1, 0, 0, 0, 0);
	if (kmsg >= 0)
		put(kmsg, "<6>init: userspace is running (PID 1)\n");
	for (;;) {
		sleep_ms(5000);
		if (kmsg < 0)
			continue;
		put(kmsg, "<6>init: alive\n");
		dump_file(kmsg, "/proc/net/dev");
	}
}

__asm__(".globl __start\n__start:\n\tli $2, -16\n\tand $29, $29, $2\n\tsubu $29, $29, 32\n\tjal __start_c\n\tnop\n");
