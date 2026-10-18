/* usbfs の割り込み IN を読む試験(USB の OHCI/EHCI の転送確認用): usb_intr_test /dev/bus/usb/BBB/DDD。インターフェース 0・1 の ep 0x81/0x82 を 25 秒間読む。ビルド: mipsel-linux-gnu-gcc -static -O2 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDWR), n = 0, i, polls = 0;
	unsigned eps[2] = { 0x81, 0x82 }, ifs[2] = { 0, 1 };
	static unsigned char buf[2][64];
	struct usbdevfs_urb urb[2], *r;
	time_t end = time(NULL) + 25;

	if (fd < 0) { perror("open"); return 1; }
	for (i = 0; i < 2; i++) {
		unsigned ifn = ifs[i];
		if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifn)) perror("claim");
		memset(&urb[i], 0, sizeof urb[i]);
		urb[i].type = USBDEVFS_URB_TYPE_INTERRUPT; urb[i].endpoint = eps[i];
		urb[i].buffer = buf[i]; urb[i].buffer_length = 8; urb[i].usercontext = (void *)(long)i;
		if (ioctl(fd, USBDEVFS_SUBMITURB, &urb[i])) perror("submit");
	}
	while (time(NULL) < end) {
		if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &r)) { if (errno != EAGAIN) perror("reap"); polls++; usleep(5000); continue; }
		i = (long)r->usercontext; n++;
		if (n <= 25) { printf("完了 ep%02x status=%d actual=%d:", r->endpoint, r->status, r->actual_length);
			for (int k = 0; k < r->actual_length; k++) printf(" %02x", buf[i][k]); printf("\n"); }
		if (r->status == 0) ioctl(fd, USBDEVFS_SUBMITURB, r); else break;
	}
	printf("完了数 %d ポーリング %d\n", n, polls);
	return 0;
}
