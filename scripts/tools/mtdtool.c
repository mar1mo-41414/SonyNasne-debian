/* 実機の /dev/mtd0 (SPIフラッシュ先頭13MB) を消去/書き込み/検証する最小ツール。libc不使用の静的バイナリ
 * (公式rootfs・Debianのどちらの環境でも同じバイナリがそのまま動く)。誤爆防止のため許可範囲を固定:
 *   scratch 0x0b0000-0x0cffff (未使用の消去済み領域) / KNL 0x100000-0x37ffff / BKNL 0x380000-0x5fffff
 * BOOT(0x0-0x4ffff)・BFWF・FMAP・INFO/INF2・ECC・RFS 等は触れない。
 *   mtdtool erase  <dev> <off_hex> <len_hex>        64KB単位で消去(範囲は64KB境界)
 *   mtdtool write  <dev> <off_hex> <file>           消去済みの領域へファイルを書き込む
 *   mtdtool verify <dev> <off_hex> <file>           読み戻してファイルと比較(一致なら終了コード0、不一致は8)
 *   mtdtool flash  <dev> <off_hex> <file>           消去(ファイルサイズを64KB切り上げ)→書き込み→読み戻し検証を一括で行う(失敗は非0)
 *   mtdtool dump   <dev> <off_hex> <len_hex> <out>  読み出してファイルに保存(許可範囲の制限なし。読み出しだけ)
 * 標準スタートアップは使わず、引数は/proc/self/cmdlineから取る。
 * ビルド: mipsel-linux-gcc -nostdlib -static -fno-pic -mno-abicalls -mips32r2 -O2 -e __start -o mtdtool mtdtool.c
 */
#define S_exit 4001
#define S_read 4003
#define S_write 4004
#define S_open 4005
#define S_close 4006
#define S_lseek 4019
#define S_ioctl 4054
static long syscall(long n, long a, long b, long c) {
    register long v0 __asm__("$2") = n;
    register long a0 __asm__("$4") = a;
    register long a1 __asm__("$5") = b;
    register long a2 __asm__("$6") = c;
    register long a3 __asm__("$7") = 0;
    __asm__ volatile("syscall" : "+r"(v0), "+r"(a3) : "r"(a0), "r"(a1), "r"(a2) : "memory", "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi", "lo");
    return a3 ? -v0 : v0;
}
/* MIPSの_IOW('M',2,struct erase_info_user{u32 start;u32 length;}) */
#define MEMERASE ((4u<<29)|(8u<<16)|('M'<<8)|2u)
#define BLK 0x10000

static unsigned long slen(const char *s){unsigned long n=0;while(s[n])n++;return n;}
static void ws(const char *s){syscall(S_write,1,(long)s,slen(s));}
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

static unsigned char buf[BLK], buf2[BLK];

static long file_size(const char *path){
    long ff=syscall(S_open,(long)path,0,0), total=0, n;
    if(ff<0) return -1;
    while((n=syscall(S_read,ff,(long)buf,sizeof(buf)))>0) total+=n;
    syscall(S_close,ff,0,0);
    return n<0?-1:total;
}
static int do_erase(const char *dev,unsigned long off,unsigned long len){
    if((off&(BLK-1))||(len&(BLK-1))||!allowed(off,len)){ws("REFUSED: range not allowed/aligned\n");return 2;}
    long fd=syscall(S_open,(long)dev,2,0); if(fd<0){ws("open failed\n");return 3;}
    unsigned long p;
    for(p=off;p<off+len;p+=BLK){
        unsigned long ei[2]; ei[0]=p; ei[1]=BLK;
        long r=syscall(S_ioctl,fd,MEMERASE,(long)ei);
        if(r<0){ws("erase FAILED at ");hx(p);ws("\n");return 4;}
    }
    ws("erased "); hx(off); ws("+"); hx(len); ws("\n");
    syscall(S_close,fd,0,0); return 0;
}
static int do_write(const char *dev,unsigned long off,const char *file){
    long total=file_size(file); if(total<0){ws("open file failed\n");return 3;}
    if(!allowed(off,total)){ws("REFUSED: range not allowed\n");return 2;}
    long ff=syscall(S_open,(long)file,0,0);
    long fd=syscall(S_open,(long)dev,2,0); if(fd<0||ff<0){ws("open failed\n");return 3;}
    if(syscall(S_lseek,fd,off,0)<0){ws("lseek failed\n");return 5;}
    long done=0;
    while(done<total){
        long got=syscall(S_read,ff,(long)buf,sizeof(buf)); if(got<=0)break;
        long w=0;
        while(w<got){ long r=syscall(S_write,fd,(long)(buf+w),got-w); if(r<=0){ws("write FAILED at ");hx(off+done+w);ws("\n");return 6;} w+=r; }
        done+=got;
    }
    ws("wrote "); hx(done); ws(" bytes at "); hx(off); ws("\n");
    syscall(S_close,fd,0,0); syscall(S_close,ff,0,0);
    return done==total?0:7;
}
/* ファイルとデバイスを比較。戻り値 0=一致 */
static int do_verify(const char *dev,unsigned long off,const char *file){
    long ff=syscall(S_open,(long)file,0,0), fd=syscall(S_open,(long)dev,0,0);
    if(ff<0||fd<0){ws("open failed\n");return 3;}
    if(syscall(S_lseek,fd,off,0)<0){ws("lseek failed\n");return 5;}
    unsigned long pos=0;
    for(;;){
        long a=syscall(S_read,ff,(long)buf,sizeof(buf)); if(a<=0) break;
        long got=0;
        while(got<a){ long r=syscall(S_read,fd,(long)(buf2+got),a-got); if(r<=0){ws("device read FAILED\n");return 6;} got+=r; }
        long i; for(i=0;i<a;i++) if(buf[i]!=buf2[i]){ws("MISMATCH at ");hx(off+pos+i);ws("\n");return 8;}
        pos+=a;
    }
    ws("verify OK "); hx(pos); ws(" bytes at "); hx(off); ws("\n");
    syscall(S_close,fd,0,0); syscall(S_close,ff,0,0);
    return 0;
}
static int do_dump(const char *dev,unsigned long off,unsigned long len,const char *out){
    long fd=syscall(S_open,(long)dev,0,0), fo=syscall(S_open,(long)out,0x100|0x200|1,0644);   /* MIPS: O_CREAT=0x100,O_TRUNC=0x200,O_WRONLY=1 */
    if(fd<0||fo<0){ws("open failed\n");return 3;}
    if(syscall(S_lseek,fd,off,0)<0){ws("lseek failed\n");return 5;}
    unsigned long done=0;
    while(done<len){
        long want=len-done>BLK?BLK:len-done, got=syscall(S_read,fd,(long)buf,want);
        if(got<=0){ws("read FAILED at ");hx(off+done);ws("\n");return 6;}
        long w=0; while(w<got){ long r=syscall(S_write,fo,(long)(buf+w),got-w); if(r<=0){ws("write FAILED\n");return 6;} w+=r; }
        done+=got;
    }
    ws("dumped "); hx(done); ws(" bytes from "); hx(off); ws("\n");
    syscall(S_close,fd,0,0); syscall(S_close,fo,0,0);
    return 0;
}

static int run(int ac,char **av){
    if(ac<5){ws("usage: mtdtool erase|write|verify|flash <dev> <off_hex> <len_hex|file>\n       mtdtool dump <dev> <off_hex> <len_hex> <out>\n");return 1;}
    unsigned long off=ph(av[3]);
    char c=av[1][0];
    if(c=='e') return do_erase(av[2],off,ph(av[4]));
    if(c=='w') return do_write(av[2],off,av[4]);
    if(c=='v') return do_verify(av[2],off,av[4]);
    if(c=='d'){ if(ac<6){ws("usage: mtdtool dump <dev> <off_hex> <len_hex> <out>\n");return 1;} return do_dump(av[2],off,ph(av[4]),av[5]); }
    if(c=='f'){
        long total=file_size(av[4]); if(total<=0){ws("file missing/empty\n");return 3;}
        unsigned long len=((unsigned long)total+BLK-1)&~(unsigned long)(BLK-1);
        int r=do_erase(av[2],off,len); if(r) return r;
        r=do_write(av[2],off,av[4]); if(r) return r;
        return do_verify(av[2],off,av[4]);
    }
    ws("unknown command\n"); return 1;
}

int main_c(long *sp){
    static char cl[1024]; static char *av[8]; int ac=0;
    (void)sp;
    long fd=syscall(S_open,(long)"/proc/self/cmdline",0,0);
    long n=fd>=0?syscall(S_read,fd,(long)cl,sizeof(cl)-1):0; if(n<0)n=0; cl[n]=0;
    long i=0; while(i<n&&ac<8){av[ac++]=&cl[i];while(i<n&&cl[i])i++;i++;}
    return run(ac,av);
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
