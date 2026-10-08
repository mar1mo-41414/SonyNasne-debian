# nasneカーネル(Linux 2.6.29, MIPS32r2 LE)をビルドするための古いクロスツールチェーン。
# Sonyのビルドは Timesys製 mipsisa32r2el-timesys-linux-gnu-gcc 4.3.2。素の GCC 4.3.2 + binutils 2.19.1 で近い環境を作る。
# ホストコンパイラは jessie の gcc 4.9(デフォルトgnu90で古いGCCをビルドしやすい)。カーネルにlibcは不要。
FROM debian/eol:jessie
RUN apt-get -o Acquire::Check-Valid-Until=false update && \
    apt-get install -y --no-install-recommends build-essential texinfo bison flex libgmp-dev libmpfr-dev \
        bzip2 xz-utils patch file bc libncurses5-dev ca-certificates python rsync cpio u-boot-tools && \
    rm -rf /var/lib/apt/lists/*
COPY dl/binutils-2.19.1.tar.bz2 dl/gcc-core-4.3.2.tar.bz2 /src/
ENV TARGET=mipsel-linux PREFIX=/opt/cross PATH=/opt/cross/bin:$PATH
WORKDIR /build
RUN set -e; cd /src && tar xjf binutils-2.19.1.tar.bz2 && tar xjf gcc-core-4.3.2.tar.bz2 && \
    mkdir /build/binutils && cd /build/binutils && \
    /src/binutils-2.19.1/configure --target=$TARGET --prefix=$PREFIX --disable-nls --disable-werror MAKEINFO=true && \
    make -j4 MAKEINFO=true && make install MAKEINFO=true
RUN set -e; mkdir /build/gcc && cd /build/gcc && \
    /src/gcc-4.3.2/configure --target=$TARGET --prefix=$PREFIX --disable-nls --enable-languages=c --without-headers \
        --disable-shared --disable-threads --disable-libssp --disable-libmudflap --disable-libgomp --disable-multilib \
        --with-gnu-as --with-gnu-ld --disable-bootstrap MAKEINFO=true && \
    make -j4 all-gcc MAKEINFO=true CFLAGS="-O2 -fgnu89-inline" && make install-gcc MAKEINFO=true
RUN mipsel-linux-gcc --version | head -1 && mipsel-linux-ld --version | head -1
WORKDIR /work
