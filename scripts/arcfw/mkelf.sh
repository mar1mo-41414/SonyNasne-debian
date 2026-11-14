#!/bin/sh
# ファーム(生のバイナリ)を、ARC の objdump で読める ELF に包む。binutils-arc-linux-gnu が要る(sudo apt install binutils-arc-linux-gnu)
# 使い方: sh mkelf.sh firmware.bin fw.o   (fw.o の中のアドレス = ファイル内のオフセット。実アドレスは 0x100000 を足す。docs/26)
set -e
printf '\t.section .text,"ax"\n\t.incbin "%s"\n' "$1" > "$2.s"
arc-linux-gnu-as -mcpu=arc700 "$2.s" -o "$2"
rm -f "$2.s"
