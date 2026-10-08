/* 実機の /dev/mtd0 (SPIフラッシュ先頭13MB) を消去/書き込みする最小ツール。誤爆防止のため許可範囲を固定:
 *   scratch 0x0b0000-0x0cffff (未使用の消去済み領域) / KNL 0x100000-0x37ffff / BKNL 0x380000-0x5fffff
 * BOOT(0x0-0x4ffff)・BFWF・FMAP・INFO/INF2・ECC・RFS 等は触れない。
 *   mtdtool erase <dev> <off_hex> <len_hex>     64KB単位で消去(範囲は64KB境界)
 *   mtdtool write <dev> <off_hex> <file>        消去済みの領域へファイルを書き込む
 * 読み出し・検証は dd/cmp で行う(読み出しは/dev/mtd0roを使う)。
 * 標準スタートアップは使わず、引数は/proc/self/cmdlineから取る(physrd.cと同様)。
 * ビルド: mipsel-linux-gnu-gcc -O2 -nostartfiles -Wl,-e,_start -o mtdtool mtdtool.c
 */
extern long syscall(long number, ...);
#define S_open 4005
#define S_close 4006
#define S_read 4003
#define S_write 4004
#define S_lseek 4019
#define S_ioctl 4054
#define S_exit 4001
/* MIPSの_IOW('M',2,struct erase_info_user{u32 start;u32 length;}) */
#define MEMERASE ((4u<<29)|(8u<<16)|('M'<<8)|2u)

static unsigned long slen(const char *s){unsigned long n=0;while(s[n])n++;return n;}
static void ws(const char *s){syscall(S_write,1,s,slen(s));}
static void hx(unsigned long v){char b[9];int i;for(i=0;i<8;i++){int n=(v>>((7-i)*4))&0xf;b[i]=n<10?'0'+n:'a'+n-10;}b[8]=0;ws(b);}
static unsigned long ph(const char *s){unsigned long v=0;if(s[0]=='0'&&(s[1]|32)=='x')s+=2;for(;*s;s++){int n=*s>='0'&&*s<='9'?*s-'0':(*s|32)-'a'+10;v=v*16+n;}return v;}

static int allowed(unsigned long off,unsigned long len){
    unsigned long end=off+len;
    if(len==0) return 0;
    if(off>=0x0b0000&&end<=0x0d0000) return 1;
    if(off>=0x100000&&end<=0x380000) return 1;
    if(off>=0x380000&&end<=0x600000) return 1;
    return 0;
}

static int run(int ac,char **av){
    if(ac<5){ws("usage: mtdtool erase|write <dev> <off_hex> <len_hex|file>\n");return 1;}
    unsigned long off=ph(av[3]);
    if(av[1][0]=='e'){
        unsigned long len=ph(av[4]);
        if((off&0xffff)||(len&0xffff)||!allowed(off,len)){ws("REFUSED: range not allowed/aligned\n");return 2;}
        long fd=syscall(S_open,av[2],2,0); if(fd<0){ws("open failed\n");return 3;}
        unsigned long p;
        for(p=off;p<off+len;p+=0x10000){
            unsigned long ei[2]; ei[0]=p; ei[1]=0x10000;
            long r=syscall(S_ioctl,fd,MEMERASE,ei);
            ws("erase "); hx(p); ws(r<0?" FAILED\n":" ok\n"); if(r<0) return 4;
        }
        syscall(S_close,fd); return 0;
    }
    /* write */
    long ff=syscall(S_open,av[4],0,0); if(ff<0){ws("open file failed\n");return 3;}
    static unsigned char buf[0x10000];
    /* 先にサイズを数える */
    unsigned long total=0; long n;
    while((n=syscall(S_read,ff,buf,sizeof(buf)))>0) total+=n;
    syscall(S_close,ff);
    if(!allowed(off,total)){ws("REFUSED: range not allowed\n");return 2;}
    ff=syscall(S_open,av[4],0,0);
    long fd=syscall(S_open,av[2],2,0); if(fd<0){ws("open dev failed\n");return 3;}
    if(syscall(S_lseek,fd,off,0)<0){ws("lseek failed\n");return 5;}
    unsigned long done=0;
    while(done<total){
        long got=syscall(S_read,ff,buf,sizeof(buf)); if(got<=0)break;
        long w=0;
        while(w<got){ long r=syscall(S_write,fd,buf+w,got-w); if(r<=0){ws("write FAILED at ");hx(off+done+w);ws("\n");return 6;} w+=r; }
        done+=got;
    }
    ws("wrote "); hx(done); ws(" bytes at "); hx(off); ws("\n");
    syscall(S_close,fd); syscall(S_close,ff);
    return done==total?0:7;
}

void _start(void){
    static char cl[1024]; static char *av[8]; int ac=0;
    long fd=syscall(S_open,"/proc/self/cmdline",0,0);
    long n=fd>=0?syscall(S_read,fd,cl,sizeof(cl)-1):0; if(n<0)n=0; cl[n]=0;
    long i=0; while(i<n&&ac<8){av[ac++]=&cl[i];while(i<n&&cl[i])i++;i++;}
    syscall(S_exit,run(ac,av));
    for(;;);
}
