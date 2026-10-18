/* 診断用(libc なし): ファイルを mmap(PROT_READ|PROT_EXEC か PROT_READ)して、全ワードの和を出す。
 *   mapsum <path> <x|r>   x = PROT_EXEC 付き。r = 読み取りのみ。read() の和は cat | cksum と比べる。
 * ビルド: mipsel-linux-gnu-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -Os -fno-builtin -e __start -o mapsum mapsum.c */
#define SYS_exit 4001
#define SYS_write 4004
#define SYS_open 4005
#define SYS_mmap2 4210
#define SYS_fstat64 4215

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long v0 __asm__("$2") = n;
	register long a0 __asm__("$4") = a;
	register long a1 __asm__("$5") = b;
	register long a2 __asm__("$6") = c;
	register long a3 __asm__("$7") = d;
	__asm__ volatile(".set push\n.set noat\nsubu $29,$29,32\nsw %5,16($29)\nsw %6,20($29)\nsyscall\naddu $29,$29,32\n.set pop\n"
		: "+r"(v0), "+r"(a3) : "r"(a0), "r"(a1), "r"(a2), "r"(e), "r"(f)
		: "memory", "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi", "lo");
	return a3 ? -v0 : v0;
}

static void hex(unsigned long v)
{
	char b[9];
	int i;

	for (i = 0; i < 8; i++) {
		int n = (v >> ((7 - i) * 4)) & 0xf;
		b[i] = n < 10 ? '0' + n : 'a' + n - 10;
	}
	b[8] = '\n';
	sys6(SYS_write, 1, (long)b, 9, 0, 0, 0);
}

void __start_c(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	long fd, m;
	long st[24];
	unsigned long size, i, sum = 0, x = 0;
	int prot = 1;

	if (argc < 3)
		sys6(SYS_exit, 2, 0, 0, 0, 0, 0);
	if (argv[2][0] == 'x')
		prot = 5;
	fd = sys6(SYS_open, (long)argv[1], 0, 0, 0, 0, 0);
	if (fd < 0 || sys6(SYS_fstat64, fd, (long)st, 0, 0, 0, 0) < 0)
		sys6(SYS_exit, 3, 0, 0, 0, 0, 0);
	size = st[14];	/* struct stat64(mips o32)の st_size 下位ワード(オフセット 56) */
	m = sys6(SYS_mmap2, 0, size, prot, 2, fd, 0);
	if (m < 0 && m > -4096)
		sys6(SYS_exit, 4, 0, 0, 0, 0, 0);
	for (i = 0; i + 4 <= size; i += 4) {
		unsigned long w = *(volatile unsigned long *)(m + i);

		sum += w;
		x ^= w;
	}
	hex(size);
	hex(sum);
	hex(x);
	sys6(SYS_exit, 0, 0, 0, 0, 0, 0);
}
__asm__(".globl __start\n__start:\n\tmove $4, $29\n\tli $2, -16\n\tand $29, $29, $2\n\tsubu $29, $29, 32\n\tjal __start_c\n\tnop\n");
