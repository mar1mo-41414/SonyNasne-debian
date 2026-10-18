/* nasne 新カーネルの initramfs の /init(libc なし、約10KB)。busybox(2.4MB)の代わり。KNL スロットの 2.5MB(gzip 後)に収めるため。
 *   1. /proc /sys /dev をマウント
 *   2. /dev/sda3(p3、XFS)が現れるまで待つ(最大 120 秒)→ マウント
 *   3. p3 の /.nasne-debian12/root.img を loop にして ext4 としてマウント → /newroot
 *   4. p3 を /newroot/data へ、/proc /sys /dev を /newroot/ へ移動し、switch_root して /sbin/init を exec
 * 失敗したら、/dev/kmsg にログを出して待つ(netconsole で見える)。
 * ビルド: mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -Os -fno-builtin -e __start -o init init_boot.c
 */
#define SYS_exit       4001
#define SYS_execve     4011
#define SYS_chdir      4012
#define SYS_mknod      4014
#define SYS_mount      4021
#define SYS_umount2    4052
#define SYS_ioctl      4054
#define SYS_chroot     4061
#define SYS_open       4005
#define SYS_close      4006
#define SYS_write      4004
#define SYS_nanosleep  4166
#define SYS_mkdir      4039
#define SYS_stat64     4213

#define MS_RDONLY 1
#define MS_MOVE   8192
#define O_RDWR    2
#define O_LARGEFILE 0x2000	/* MIPS。これが無いと、32bit のプロセスが開いたファイルの 2GiB 以降への書き込みが EFBIG になる(loop の裏ファイルが 2GiB を超えると壊れる) */
#define LOOP_SET_FD 0x4C00
#define LOOP_CTL_GET_FREE 0x4C82

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

static long kmsg = -1;

static void put(const char *s)
{
	long n = 0;

	if (kmsg < 0)
		return;
	while (s[n])
		n++;
	sys6(SYS_write, kmsg, (long)s, n, 0, 0, 0);
}

static void sleep_ms(long ms)
{
	long ts[2] = { ms / 1000, (ms % 1000) * 1000000L };

	sys6(SYS_nanosleep, (long)ts, 0, 0, 0, 0, 0);
}

static void fail(const char *msg)
{
	put(msg);
	for (;;)
		sleep_ms(60000);
}

static int mount(const char *src, const char *dst, const char *type, unsigned long flags)
{
	return sys6(SYS_mount, (long)src, (long)dst, (long)type, flags, 0, 0);
}

static int exists(const char *path)
{
	long st[32];

	return sys6(SYS_stat64, (long)path, (long)st, 0, 0, 0, 0) == 0;
}

void __start_c(long *sp)
{
	static char *const argv[] = { "/sbin/init", 0 };
	static char *const envp[] = { "HOME=/", "TERM=linux", "PATH=/sbin:/usr/sbin:/bin:/usr/bin", 0 };
	static char loopdev[16] = "/dev/loop0";
	long i, fd, lfd, devn;

	(void)sp;
	sys6(SYS_mkdir, (long)"/proc", 0755, 0, 0, 0, 0);
	sys6(SYS_mkdir, (long)"/sys", 0755, 0, 0, 0, 0);
	sys6(SYS_mkdir, (long)"/dev", 0755, 0, 0, 0, 0);
	mount("proc", "/proc", "proc", 0);
	mount("sysfs", "/sys", "sysfs", 0);
	mount("devtmpfs", "/dev", "devtmpfs", 0);
	kmsg = sys6(SYS_open, (long)"/dev/kmsg", 1, 0, 0, 0, 0);
	put("<6>[init] start\n");

	for (i = 0; i < 120 && !exists("/dev/sda3"); i++)
		sleep_ms(1000);
	if (!exists("/dev/sda3"))
		fail("<3>[init] /dev/sda3 が見つかりません\n");
	sys6(SYS_mkdir, (long)"/p3", 0755, 0, 0, 0, 0);
	sys6(SYS_mkdir, (long)"/newroot", 0755, 0, 0, 0, 0);
	if (mount("/dev/sda3", "/p3", "xfs", 0))
		fail("<3>[init] p3(XFS) のマウントに失敗\n");
	if (!exists("/p3/.nasne-debian12/root.img"))
		fail("<3>[init] /p3/.nasne-debian12/root.img がありません\n");

	/* 空きの loop デバイスを探して root.img を結びつける */
	fd = sys6(SYS_open, (long)"/dev/loop-control", O_RDWR, 0, 0, 0, 0);
	devn = fd >= 0 ? sys6(SYS_ioctl, fd, LOOP_CTL_GET_FREE, 0, 0, 0, 0) : 0;
	if (devn < 0)
		devn = 0;
	loopdev[9] = '0' + (devn % 10);
	if (devn >= 10) {
		loopdev[9] = '0' + (devn / 10);
		loopdev[10] = '0' + (devn % 10);
		loopdev[11] = 0;
	}
	lfd = sys6(SYS_open, (long)loopdev, O_RDWR, 0, 0, 0, 0);
	fd = sys6(SYS_open, (long)"/p3/.nasne-debian12/root.img", O_RDWR | O_LARGEFILE, 0, 0, 0, 0);
	if (lfd < 0 || fd < 0 || sys6(SYS_ioctl, lfd, LOOP_SET_FD, fd, 0, 0, 0) < 0)
		fail("<3>[init] loop の設定に失敗\n");
	if (mount(loopdev, "/newroot", "ext4", 0))
		fail("<3>[init] root.img(ext4) のマウントに失敗\n");

	sys6(SYS_mkdir, (long)"/newroot/data", 0755, 0, 0, 0, 0);
	mount("/p3", "/newroot/data", 0, MS_MOVE);
	mount("/dev", "/newroot/dev", 0, MS_MOVE);
	mount("/proc", "/newroot/proc", 0, MS_MOVE);
	mount("/sys", "/newroot/sys", 0, MS_MOVE);
	put("<6>[init] switch_root to Debian 12\n");

	sys6(SYS_chdir, (long)"/newroot", 0, 0, 0, 0, 0);
	mount("/newroot", "/", 0, MS_MOVE);
	sys6(SYS_chroot, (long)".", 0, 0, 0, 0, 0);
	sys6(SYS_chdir, (long)"/", 0, 0, 0, 0, 0);
	sys6(SYS_execve, (long)"/sbin/init", (long)argv, (long)envp, 0, 0, 0);
	fail("<3>[init] /sbin/init の exec に失敗\n");
}

__asm__(".globl __start\n__start:\n\tmove $4, $29\n\tli $2, -16\n\tand $29, $29, $2\n\tsubu $29, $29, 32\n\tjal __start_c\n\tnop\n");
