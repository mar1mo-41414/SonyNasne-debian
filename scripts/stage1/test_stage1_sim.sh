#!/bin/bash
# nasne-stage1.sh の論理テスト(実機なし)。ファイルを偽のSPIフラッシュにして、偽の mtdtool で
# 正常系・冪等・書き込み失敗→書き戻し・書き戻し失敗・試行上限・ハッシュ不一致・空スロットを確認する。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
FLASH=$W/flash.bin; STATE=$W/state; LOG=$W/log; BIN=$W/bin; mkdir -p "$BIN" "$W/share"
KNL=$W/share/knl_new.bin
pass=0; fail=0
ok()  { echo "  PASS: $1"; pass=$((pass+1)); }
ng()  { echo "  FAIL: $1"; fail=$((fail+1)); }

# 偽 mtdtool(実物と同じCLI。NASNE_FAIL=flash|restore で失敗を注入)
cat > "$BIN/mtdtool" <<'M'
#!/bin/bash
cmd=$1; dev=$2; off=$((${3})); arg=$4
case $cmd in
  dump)   tail -c +$((off+1)) "$dev" | head -c $((arg)) > "$5"; exit 0;;
  verify) n=$(wc -c < "$arg"); cmp -s <(tail -c +$((off+1)) "$dev" | head -c $n) "$arg" && { echo "verify OK"; exit 0; } || { echo MISMATCH; exit 8; };;
  flash)  n=$(wc -c < "$arg")
          if { [ "${NASNE_FAIL:-}" = flash ] && [ "$arg" != "$STATE/knl_A_before.bin" ]; } || [ "${NASNE_FAIL:-}" = both ]; then
              # 途中まで壊して失敗
              head -c 65536 /dev/zero | tr '\0' '\377' | dd of="$dev" bs=65536 seek=$((off/65536)) conv=notrunc 2>/dev/null; echo "write FAILED"; exit 6
          fi
          dd if="$arg" of="$dev" bs=65536 seek=$((off/65536)) conv=notrunc 2>/dev/null; echo "flash ok"; exit 0;;
esac
exit 1
M
chmod +x "$BIN/mtdtool"
printf '#!/bin/sh\necho REBOOT >> %s/reboot_called\n' "$W" > "$BIN/reboot"; chmod +x "$BIN/reboot"
# 偽のウォッチドッグ停止スクリプト(NASNE_WD=fail で失敗を注入)
cat > "$BIN/wd" <<'M'
#!/bin/sh
[ "${NASNE_WD_FAIL:-}" = 1 ] || echo "MCU watchdog disabled" >> "$NASNE_WD_LOG"
M
chmod +x "$BIN/wd"

newflash() { head -c $((0xd00000)) /dev/urandom > "$FLASH"; }
newknl()   { head -c 2125888 /dev/urandom > "$KNL"; ( cd "$W/share" && sha256sum knl_new.bin > knl_new.bin.sha256 ); }
run() { NASNE_SYNC=true NASNE_WD_CMD="$BIN/wd" NASNE_WD_LOG="$W/wd.log" NASNE_WD_FAIL="${WDFAIL:-0}" NASNE_REBOOT="$BIN/reboot" STATE="$STATE" NASNE_MTD="$FLASH" NASNE_MTDTOOL="$BIN/mtdtool" NASNE_KNL="$KNL" NASNE_STATE="$STATE" NASNE_LOG="$LOG" NASNE_FAIL="${1:-}" sh "$HERE/nasne-stage1.sh" >/dev/null 2>&1; echo $?; }
reset() { rm -rf "$STATE" "$W/reboot_called"; : > "$LOG"; newflash; newknl; }

echo "[1] 正常系: 書き換えてrebootする"
reset; orig=$(tail -c +$((0x100001)) "$FLASH" | head -c 2125888 | sha256sum)
rc=$(run); [ "$rc" = 0 ] && ok "rc=0" || ng "rc=$rc"
cmp -s <(tail -c +$((0x100001)) "$FLASH" | head -c 2125888) "$KNL" && ok "SPIにカーネルが書かれた" || ng "書かれていない"
[ -f "$W/reboot_called" ] && ok "reboot が呼ばれた" || ng "reboot なし"
[ -s "$STATE/spi_mtd0_backup.bin" ] && cmp -s "$STATE/spi_mtd0_backup.bin" <(head -c 0 /dev/null; cat "$STATE/spi_mtd0_backup.bin") && ok "バックアップあり" || ng "バックアップなし"
[ "$(sha256sum < <(head -c 2125888 "$STATE/knl_A_before.bin"))" = "$orig" ] && ok "元KNLを退避した" || ng "退避が元と違う"

echo "[2] 冪等: すでに書かれていれば何もしない(rebootもしない)"
rm -f "$W/reboot_called"; rc=$(run); [ "$rc" = 0 ] && ok "rc=0" || ng "rc=$rc"
[ ! -f "$W/reboot_called" ] && ok "reboot しない" || ng "reboot した"

echo "[3] 書き込み失敗 → 自動で元に戻す"
reset; before=$(sha256sum < "$FLASH")
rc=$(run flash); [ "$rc" = 3 ] && ok "rc=3(書き戻し成功)" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIは元の内容に戻った" || ng "SPIが元と違う"
[ ! -f "$W/reboot_called" ] && ok "reboot しない" || ng "reboot した"

echo "[4] 書き込み失敗 + 書き戻しも失敗 → 要手動復旧(rc=4)、rebootしない"
reset; rc=$(run both); [ "$rc" = 4 ] && ok "rc=4" || ng "rc=$rc"
[ ! -f "$W/reboot_called" ] && ok "reboot しない" || ng "reboot した"

echo "[5] 試行上限(2回)"
reset; echo 2 > /dev/null; mkdir -p "$STATE"; echo 2 > "$STATE/tries"
before=$(sha256sum < "$FLASH"); rc=$(run); [ "$rc" = 2 ] && ok "rc=2" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIに触っていない" || ng "SPIが変わった"

echo "[6] ハッシュ不一致(カーネルファイルが壊れている)"
reset; echo x >> "$KNL"; before=$(sha256sum < "$FLASH"); rc=$(run); [ "$rc" = 1 ] && ok "rc=1" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIに触っていない" || ng "SPIが変わった"

echo "[7] 現在のKNLスロットが空(0xFF)のとき自動では書かない"
reset; head -c 65536 /dev/zero | tr '\0' '\377' | dd of="$FLASH" bs=65536 seek=16 conv=notrunc 2>/dev/null
before=$(sha256sum < "$FLASH"); rc=$(run); [ "$rc" = 1 ] && ok "rc=1" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIに触っていない" || ng "SPIが変わった"

echo "[8] カーネルが大きすぎる(KNL領域を超える)"
reset; head -c 2700000 /dev/urandom > "$KNL"; ( cd "$W/share" && sha256sum knl_new.bin > knl_new.bin.sha256 )
before=$(sha256sum < "$FLASH"); rc=$(run); [ "$rc" = 1 ] && ok "rc=1" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIに触っていない" || ng "SPIが変わった"

echo "[9] ウォッチドッグを止められない → SPIに触らない(rc=5)"
reset; before=$(sha256sum < "$FLASH"); WDFAIL=1 rc=$(WDFAIL=1 run); [ "$rc" = 5 ] && ok "rc=5" || ng "rc=$rc"
[ "$(sha256sum < "$FLASH")" = "$before" ] && ok "SPIに触っていない" || ng "SPIが変わった"
[ ! -f "$STATE/tries" ] && ok "試行回数も増やしていない" || ng "tries が増えた"

echo; echo "結果: PASS=$pass FAIL=$fail"; [ "$fail" = 0 ]
