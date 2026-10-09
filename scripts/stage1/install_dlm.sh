#!/usr/bin/env bash
# install_dlm.sh — 作った 00550066.dlm を、HDDの sys1(p1)に入れる。元のファイルは --backup-dir に退避する。
#   使い方: install_dlm.sh <sys1をmountしたディレクトリ> <新しい00550066.dlm> [--backup-dir DIR]
# 検査: sys1らしいこと(00110022.dlm と 00550066.dlm がある)、新旧のヘッダ(hwtype・日付)が一致すること(マネージャの検査に通る条件)。
# 変更するのは /00550066.dlm の1ファイルだけ(マネージャ・バンクdirは触らない)。
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
SYS1=${1:?sys1のmount先}; NEW=${2:?新しい.dlm}; BK=./backup_sys1
[ "${3:-}" = "--backup-dir" ] && BK=${4:?}
[ -f "$SYS1/00110022.dlm" ] && [ -f "$SYS1/00550066.dlm" ] || { echo "$SYS1 は nasne の sys1 に見えない(00110022.dlm / 00550066.dlm が無い)" >&2; exit 1; }
[ -s "$NEW" ] || { echo "$NEW が無い" >&2; exit 1; }
python3 -I - "$SYS1/00550066.dlm" "$NEW" "$ROOT" <<'PY'
import sys
sys.path.insert(0, sys.argv[3] + "/scripts")
from dlm_crypto import NasneBlowfish
import build_dlm
def hdr(p): return NasneBlowfish().chain_decrypt(open(p, "rb").read(64))
old, new = hdr(sys.argv[1]), hdr(sys.argv[2])
assert new[:4] == b"DLM\0", "新しいファイルが .dlm ではない"
assert old[0x10:0x14] == new[0x10:0x14], "hwtype が違う"
assert old[0x14:0x34] == new[0x14:0x34], f"日付が違う: {bytes(old[0x14:0x34])!r} != {bytes(new[0x14:0x34])!r} (このHDD自身の00550066.dlmを入力に作ること)"
ok, d = build_dlm.validate_dlm_bytes(open(sys.argv[2], "rb").read())
assert ok, f"新しい.dlmの自己検証NG: {d}"
print("検査OK: hwtype・日付が一致、CRC OK")
PY
mkdir -p "$BK"
if [ ! -e "$BK/00550066.dlm.orig" ]; then cp -p "$SYS1/00550066.dlm" "$BK/00550066.dlm.orig"; echo "退避: $BK/00550066.dlm.orig"; else echo "(すでに $BK/00550066.dlm.orig がある。上書きしない)"; fi
# 全体の sync ではなく対象ファイル/ファイルシステムだけを同期する(他に重い書き込みがあると全体syncは何分も待たされる)
cp "$NEW" "$SYS1/00550066.dlm.tmp" && { sync "$SYS1/00550066.dlm.tmp" 2>/dev/null || sync; } && mv "$SYS1/00550066.dlm.tmp" "$SYS1/00550066.dlm" && { sync -f "$SYS1" 2>/dev/null || sync; }
cmp -s "$NEW" "$SYS1/00550066.dlm" && echo "書き込み完了: $SYS1/00550066.dlm" || { echo "書き込み後の比較が一致しない" >&2; exit 1; }
df -h "$SYS1" | tail -1
