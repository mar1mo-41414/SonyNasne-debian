#!/bin/sh
# nasne-stage1.sh — 公式カーネル上(Debianのchroot内)で、SPIフラッシュのKNL(スロットA)を自作カーネルに書き換える。
#
# 何度実行しても安全(冪等):
#   - 書き込み予定のカーネルと同じ内容がすでにSPIにあれば、何もしない(終了コード0)。
#   - 書く前に必ず、(a) カーネルファイルのsha256・サイズ検査、(b) SPI(mtd0)全域のバックアップ、(c) 試行回数の上限(2回)を確認する。
#   - 書き込み(消去→書き込み→読み戻し検証)に失敗したら、バックアップから元のKNLを自動で書き戻す。
#   - 成功したら reboot -f(次の起動で自作カーネルの /nasne_init が p3 のDebianへ switch_root する)。
# 終了コード: 0=成功/すでに書き換え済み(rebootする場合は戻らない)、1=事前検査に失敗(SPIには触っていない)、
#             2=試行回数の上限、3=書き込み失敗→書き戻し成功、4=書き込み失敗→書き戻しも失敗(要手動復旧)、
#             5=MCUウォッチドッグを止められなかった(約333秒でリセットされ、書き込み中に落ちる恐れがあるため、SPIには触らない)
#
# 環境変数で上書き可能(テスト用): NASNE_MTD, NASNE_MTDTOOL, NASNE_KNL, NASNE_STATE, NASNE_LOG, NASNE_NO_REBOOT, NASNE_REBOOT
PATH=/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin
MTD=${NASNE_MTD:-/dev/mtd0}
MTDTOOL=${NASNE_MTDTOOL:-/usr/local/sbin/mtdtool}
KNL=${NASNE_KNL:-/usr/local/share/nasne-stage1/knl_new.bin}
STATE=${NASNE_STATE:-/var/lib/nasne-stage1}
LOG=${NASNE_LOG:-/var/log/nasne-stage1.log}
KNL_OFF=0x100000          # KNL(スロットA)
KNL_MAX=2621440           # 0x280000: KNL領域の大きさ(これを超えるとBKNLに食い込む)
KNL_MIN=1048576           # 1MB未満は壊れているとみなす
SPI_DUMP_LEN=0xd00000     # mtd0の全域(13MB)
MAX_TRIES=2
WD_CMD=${NASNE_WD_CMD:-/etc/init.d/nasne-mcu-wd}
WD_LOG=${NASNE_WD_LOG:-/var/log/nasne-mcu-wd.log}
REBOOT=${NASNE_REBOOT:-"reboot -f"}
SYNC=${NASNE_SYNC:-sync}

mkdir -p "$STATE" "$(dirname "$LOG")" 2>/dev/null
log() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

log "=== nasne-stage1 start ==="

# --- (a) 事前検査: ここまでは SPI に一切触らない ---
[ -x "$MTDTOOL" ] || { log "ABORT: $MTDTOOL が無い"; exit 1; }
[ -s "$KNL" ] || { log "ABORT: $KNL が無い/空"; exit 1; }
SIZE=$(wc -c < "$KNL")
if [ "$SIZE" -gt "$KNL_MAX" ] || [ "$SIZE" -lt "$KNL_MIN" ]; then
    log "ABORT: $KNL のサイズ($SIZE)が範囲外($KNL_MIN〜$KNL_MAX)"; exit 1
fi
if [ ! -s "$KNL.sha256" ]; then
    log "ABORT: $KNL.sha256 が無い/空。SPIには書き込まない"; exit 1
fi
if ! ( cd "$(dirname "$KNL")" && sha256sum -c "$(basename "$KNL").sha256" >/dev/null 2>&1 ); then
    log "ABORT: $KNL のsha256が一致しない(壊れている/別のファイル)"; exit 1
fi
log "kernel sha256 OK ($SIZE bytes)"

# --- 冪等判定: すでに同じ内容が書かれていれば何もしない ---
if "$MTDTOOL" verify "$MTD" "$KNL_OFF" "$KNL" >> "$LOG" 2>&1; then
    log "SPIのKNLはすでに目的のカーネル。何もしない"
    : > "$STATE/done"
    exit 0
fi

# --- MCUウォッチドッグを止める。公式の procmng が動いていないこの環境では、約333秒(5.5分)でMCUが本体をリセットする。
#     SPIの消去・書き込み中にリセットされると壊れるので、止められなければSPIには触らない ---
log "stopping the MCU watchdog"
if [ -x "$WD_CMD" ]; then
    : > "$WD_LOG" 2>/dev/null
    "$WD_CMD" start >> "$LOG" 2>&1
    n=0; WD_OK=0
    while [ $n -lt 45 ]; do
        if grep -q "MCU watchdog disabled" "$WD_LOG" 2>/dev/null; then WD_OK=1; break; fi
        sleep 1; n=$((n + 1))
    done
else
    WD_OK=0
fi
if [ "$WD_OK" != 1 ]; then log "ABORT: ウォッチドッグを止められなかった($WD_CMD / $WD_LOG)。SPIには触らない"; exit 5; fi
log "watchdog stopped"

# --- (c) 試行回数の上限(失敗を繰り返してフラッシュを痛めない・無限ループにしない) ---
TRIES=$(cat "$STATE/tries" 2>/dev/null || echo 0)
if [ "$TRIES" -ge "$MAX_TRIES" ]; then
    log "ABORT: すでに $TRIES 回試行した(上限 $MAX_TRIES)。SPIには触らない。$STATE/tries を消せば再試行できる"
    exit 2
fi
echo $((TRIES + 1)) > "$STATE/tries"; $SYNC

# --- (b) バックアップ(初回のみ。書き込みより前に必ず取る) ---
if [ ! -f "$STATE/backup_ok" ]; then
    # 前回の処理が中断された場合、部分バックアップを上書きしない。
    if [ -e "$STATE/spi_mtd0_backup.bin" ] || [ -e "$STATE/knl_A_before.bin" ] || [ -e "$STATE/spi_mtd0_backup.sha256" ]; then
        log "ABORT: 完了印なしのバックアップが残っている。自動で上書きせず手動確認する"; exit 1
    fi
    log "backing up SPI ($MTD 0..$SPI_DUMP_LEN) to $STATE/spi_mtd0_backup.bin"
    if ! "$MTDTOOL" dump "$MTD" 0 "$SPI_DUMP_LEN" "$STATE/spi_mtd0_backup.bin" >> "$LOG" 2>&1; then
        log "ABORT: バックアップに失敗。SPIには触らない"; exit 1
    fi
    BSIZE=$(wc -c < "$STATE/spi_mtd0_backup.bin" 2>/dev/null) || exit 1
    if [ "$BSIZE" -ne $((SPI_DUMP_LEN)) ]; then
        log "ABORT: バックアップのサイズ($BSIZE)が想定外。SPIには触らない"; exit 1
    fi
    # 復旧用KNLスロット(2.5MiB)を完全に取り出せなければ書き換えない。
    if ! dd if="$STATE/spi_mtd0_backup.bin" of="$STATE/knl_A_before.bin" bs=65536 skip=16 count=40 2>/dev/null; then
        log "ABORT: 復旧用KNLの切り出しに失敗"; exit 1
    fi
    KSIZE=$(wc -c < "$STATE/knl_A_before.bin" 2>/dev/null) || exit 1
    if [ "$KSIZE" -ne "$KNL_MAX" ]; then
        log "ABORT: 復旧用KNLのサイズが不正($KSIZE)"; exit 1
    fi
    if [ "$(head -c 64 "$STATE/knl_A_before.bin" | tr -d '\377' | wc -c)" -eq 0 ]; then
        log "ABORT: 現在のKNLスロットが空(0xFF)。すでに壊れているため自動では書き込まない"; exit 1
    fi
    if ! ( cd "$STATE" && sha256sum spi_mtd0_backup.bin > spi_mtd0_backup.sha256 ); then
        log "ABORT: バックアップのSHA256作成に失敗"; exit 1
    fi
    : > "$STATE/backup_ok"; $SYNC
    log "backup created"
fi

# 初回も再試行時も、SPI消去より前に復旧に使うファイルを全検査する。
if [ ! -s "$STATE/spi_mtd0_backup.bin" ] || [ ! -s "$STATE/spi_mtd0_backup.sha256" ] || [ ! -s "$STATE/knl_A_before.bin" ]; then
    log "ABORT: 復旧用バックアップのファイルが欠けている"; exit 1
fi
BSIZE=$(wc -c < "$STATE/spi_mtd0_backup.bin" 2>/dev/null) || exit 1
KSIZE=$(wc -c < "$STATE/knl_A_before.bin" 2>/dev/null) || exit 1
if [ "$BSIZE" -ne $((SPI_DUMP_LEN)) ] || [ "$KSIZE" -ne "$KNL_MAX" ]; then
    log "ABORT: 復旧用バックアップのサイズが不正 (SPI=$BSIZE KNL=$KSIZE)"; exit 1
fi
if ! ( cd "$STATE" && sha256sum -c spi_mtd0_backup.sha256 >/dev/null 2>&1 ); then
    log "ABORT: 復旧用SPIバックアップのSHA256が不一致"; exit 1
fi
if ! dd if="$STATE/spi_mtd0_backup.bin" of="$STATE/knl_A_check.bin" bs=65536 skip=16 count=40 2>/dev/null ||
   ! cmp -s "$STATE/knl_A_check.bin" "$STATE/knl_A_before.bin"; then
    rm -f "$STATE/knl_A_check.bin"
    log "ABORT: 復旧用KNLが元SPIバックアップと一致しない"; exit 1
fi
rm -f "$STATE/knl_A_check.bin"
log "original SPI backup and restore KNL verified"

# --- 書き込み(消去→書き込み→読み戻し検証) ---
log "flashing KNL ($KNL -> $MTD @ $KNL_OFF)"
if "$MTDTOOL" flash "$MTD" "$KNL_OFF" "$KNL" >> "$LOG" 2>&1; then
    log "flash + verify OK"
    : > "$STATE/done"; $SYNC
    if [ -n "$NASNE_NO_REBOOT" ]; then log "NASNE_NO_REBOOT: rebootしない"; exit 0; fi
    log "rebooting into the new kernel"
    $REBOOT
    sleep 5
    exit 0
fi

# --- 失敗: 元のKNLを書き戻す ---
log "FLASH FAILED. restoring the original KNL from the backup"
if "$MTDTOOL" flash "$MTD" "$KNL_OFF" "$STATE/knl_A_before.bin" >> "$LOG" 2>&1; then
    log "restore OK (元のKNLに戻した。自作カーネルは入っていない)"
    exit 3
fi
log "RESTORE FAILED!! SPIのKNLが壊れた状態。$STATE/spi_mtd0_backup.bin から手動で書き戻すか、CH341A等で復旧する"
exit 4
