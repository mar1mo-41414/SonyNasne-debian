#!/usr/bin/env python3
"""
nasne `.dlm` ファームウェアパッケージ(`/disk0/00550066.dlm`)を自作するツール。

`dlm_crypto.py` で解読したヘッダ暗号(独自Blowfish変種)を使い、実機の `init`
バイナリ(`FUN_004005c4`)が行う全チェックをPython側でも再現・自己検証したうえで、
"正規の実機が受理できるはずの"カスタム`.dlm`を組み立てる。

## ヘッダ64バイトの全フィールド(FUN_004005c4のフル逆コンパイルで確定)

| offset | 長さ | 内容                         | チェックの有無                                      |
|-------:|-----:|------------------------------|------------------------------------------------------|
| 0x00   | 4    | マジック `"DLM\x00"`         | 必須一致(`DAT_0050661c`とmemcmp)                     |
| 0x04   | 4    | サイズ/情報フィールド        | **未チェック**(実測値=ファイル全体サイズだったが検証なし) |
| 0x08   | 4    | バージョン風フィールド       | **未チェック**(実測値="0002 003c"=v2.60と推定)       |
| 0x0C   | 2    | 不明(実測値=0000)            | **未チェック**                                        |
| 0x0E   | 1    | **ボディ暗号化フラグ**       | 1ならボディもBlowfish復号、それ以外は無変換コピー     |
| 0x0F   | 1    | 不明(実測値=00)              | **未チェック**                                        |
| 0x10   | 4    | **hwtype**(ビッグエンディアン)| `DAT_00552020`に関係なく毎回必須。`/dev/mem`オフセット0x10cのハードウェアレジスタ値と一致必須 |
| 0x14   | 32   | **日付管理文字列**            | `DAT_00552020`(グローバルフラグ)==1の時のみ、`param_1*0x80+0x55da90`のテーブルとexact memcmp必須 |
| 0x34   | 8    | ゼロ padding                  | **未チェック**                                        |
| 0x3C   | 4    | **CRC-32**(ビッグエンディアン)| 必須。スコープはheader(このフィールドを0クリアした64バイト) + ボディ全体 |

**CRCの正確なアルゴリズム(`FUN_0040545c`、実データで完全検証済み)**:
標準CRC-32(zlib互換、seed=0)を、(1) 複号済みヘッダ64バイト(offset 0x3Cを
`00 00 00 00`にクリアしたもの)→(2) ファイル中のヘッダに続く残り全バイト(ボディ、
暗号化されていてもその生バイトのまま)、の順に連続して通して計算する。結果をビッグ
エンディアンでheader[0x3C:0x40]に格納する。

**hwtype・日付管理フィールドは実機依存の値なので、このスクリプトでは常に「本物の
`.dlm`から復号したテンプレートヘッダ」からそのままコピーする**(変更するのはフラグ
バイトとCRCのみ)。これにより、未知のチェックに引っかかるリスクを最小化する
(詳細はdocs/03_firmware_format.md参照)。

## 使い方

    # 1. ソフトウェアのみの自己検証(実機不要、既存の本物.dlmで動作確認)
    python3 build_dlm.py selftest /path/to/real_00550066.dlm

    # 2. カスタム.dlmを構築(テンプレヘッダ+自作ボディ、フラグ=平文コピー)
    python3 build_dlm.py build --template /path/to/real_00550066.dlm \\
        --body /path/to/payload.tar.gz --out custom_00550066.dlm

    # 3. 構築済み.dlmが実機の全チェックを通るか、Python側で再現検証
    python3 build_dlm.py verify /path/to/custom_00550066.dlm --template /path/to/real_00550066.dlm
"""
import argparse
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from dlm_crypto import NasneBlowfish, DLM_MAGIC  # noqa: E402

FLAG_OFFSET = 0x0E
HWTYPE_OFFSET = 0x10
DATE_OFFSET = 0x14
DATE_LEN = 32
CRC_OFFSET = 0x3C
HEADER_LEN = 64


def decrypt_header_bytes(enc_header: bytes) -> bytes:
    assert len(enc_header) == HEADER_LEN
    return NasneBlowfish().chain_decrypt(enc_header)


def compute_crc(header_with_zeroed_crc: bytes, body: bytes) -> int:
    crc = zlib.crc32(header_with_zeroed_crc)
    crc = zlib.crc32(body, crc)
    return crc & 0xFFFFFFFF


def build_custom_dlm(template_dlm_path: str, body: bytes, encrypt_body: bool = False) -> bytes:
    """テンプレート.dlmの実ヘッダを土台に、フラグとCRCだけ書き換えたカスタム.dlmを作る。

    hwtype・日付管理フィールドはテンプレートの値をそのまま維持するため、
    "このテンプレートが実際に起動実績のある実機"である場合にのみ安全に使える。
    """
    if encrypt_body:
        raise NotImplementedError(
            "ボディ暗号化(flag=1)での構築は未実装。平文コピー(flag!=1)のみサポート"
        )

    with open(template_dlm_path, "rb") as f:
        enc_header = f.read(HEADER_LEN)
    header = bytearray(decrypt_header_bytes(enc_header))

    if header[0:4] != DLM_MAGIC:
        raise ValueError("テンプレートのマジックが不正(復号に失敗している可能性)")

    header[FLAG_OFFSET] = 0x00  # 平文コピー指定
    header[CRC_OFFSET : CRC_OFFSET + 4] = b"\x00\x00\x00\x00"

    crc = compute_crc(bytes(header), body)
    header[CRC_OFFSET : CRC_OFFSET + 4] = struct.pack(">I", crc)

    enc_new_header = NasneBlowfish().chain_encrypt(bytes(header))
    return enc_new_header + body


def validate_dlm_bytes(data: bytes, hwtype_be: int | None = None, date_field: bytes | None = None):
    """FUN_004005c4 相当のチェックをPython側で再現し、結果を人間可読な形で返す。

    hwtype_be / date_field を渡すと、実機側の期待値との比較も行う(省略時はスキップ)。
    戻り値: (ok: bool, details: dict)
    """
    details = {}
    if len(data) < HEADER_LEN:
        return False, {"error": "ファイルが64バイト未満"}

    enc_header = data[0:HEADER_LEN]
    header = bytearray(decrypt_header_bytes(enc_header))
    body = data[HEADER_LEN:]

    magic_ok = header[0:4] == DLM_MAGIC
    details["magic_ok"] = magic_ok
    if not magic_ok:
        return False, details

    flag = header[FLAG_OFFSET]
    details["flag"] = flag
    details["body_encrypted"] = flag == 1

    stored_crc = struct.unpack(">I", bytes(header[CRC_OFFSET : CRC_OFFSET + 4]))[0]
    check_header = bytearray(header)
    check_header[CRC_OFFSET : CRC_OFFSET + 4] = b"\x00\x00\x00\x00"
    calc_crc = compute_crc(bytes(check_header), body)
    crc_ok = calc_crc == stored_crc
    details["crc_stored"] = hex(stored_crc)
    details["crc_calculated"] = hex(calc_crc)
    details["crc_ok"] = crc_ok

    hwtype_field = struct.unpack(">I", bytes(header[HWTYPE_OFFSET : HWTYPE_OFFSET + 4]))[0]
    details["hwtype_field"] = hex(hwtype_field)
    if hwtype_be is not None:
        details["hwtype_ok"] = hwtype_field == hwtype_be
    else:
        details["hwtype_ok"] = "未検証(実機のhwtypeレジスタ値が未提供)"

    date_bytes = bytes(header[DATE_OFFSET : DATE_OFFSET + DATE_LEN])
    details["date_field"] = date_bytes.rstrip(b"\x00")
    if date_field is not None:
        details["date_ok"] = date_bytes == date_field
    else:
        details["date_ok"] = "未検証(DAT_00552020フラグ次第では不要)"

    ok = magic_ok and crc_ok and details["hwtype_ok"] is not False and details["date_ok"] is not False
    return ok, details


def cmd_selftest(args):
    with open(args.template, "rb") as f:
        raw = f.read()
    enc_header = raw[0:HEADER_LEN]
    dec = decrypt_header_bytes(enc_header)

    reenc = NasneBlowfish().chain_encrypt(dec)
    print("[1] chain_encrypt往復テスト(元の暗号文と完全一致するか):",
          "OK" if reenc == enc_header else "NG")

    ok, details = validate_dlm_bytes(raw)
    print("[2] 実ファイル全体のPython側バリデーション再現:", "OK" if ok else "NG")
    for k, v in details.items():
        print(f"      {k}: {v}")

    rebuilt = build_custom_dlm(args.template, raw[HEADER_LEN:], encrypt_body=False)
    # テンプレのflagが1(暗号化)の場合、rebuiltはflag=0で強制するため元ファイルとは
    # 一致しない。flag/CRC以外のヘッダフィールドが保持されているかだけ確認する。
    rebuilt_header = decrypt_header_bytes(rebuilt[0:HEADER_LEN])
    orig_header = dec
    same_except_flag_crc = (
        rebuilt_header[0:FLAG_OFFSET] == orig_header[0:FLAG_OFFSET]
        and rebuilt_header[FLAG_OFFSET + 1 : CRC_OFFSET] == orig_header[FLAG_OFFSET + 1 : CRC_OFFSET]
    )
    print("[3] build_custom_dlm()でflag/CRC以外のフィールドが保持されるか:",
          "OK" if same_except_flag_crc else "NG")

    ok2, details2 = validate_dlm_bytes(rebuilt)
    print("[4] build_custom_dlm()で組み立てた「テンプレボディそのまま・flag=0」版の自己検証:",
          "OK" if ok2 else "NG")
    for k, v in details2.items():
        print(f"      {k}: {v}")


def cmd_build(args):
    with open(args.body, "rb") as f:
        body = f.read()
    out = build_custom_dlm(args.template, body, encrypt_body=args.encrypt_body)
    with open(args.out, "wb") as f:
        f.write(out)
    print(f"構築完了: {args.out} ({len(out)} bytes)")

    ok, details = validate_dlm_bytes(out)
    print("自己検証(Python再現、hwtype/dateはテンプレ由来なので実機と一致するはず):",
          "OK" if ok else "NG")
    for k, v in details.items():
        print(f"  {k}: {v}")
    if not ok:
        print("警告: 自己検証NG。実機に書き込む前に原因を確認してください。", file=sys.stderr)


def cmd_verify(args):
    with open(args.dlm, "rb") as f:
        data = f.read()
    ok, details = validate_dlm_bytes(data)
    print("検証結果:", "OK" if ok else "NG")
    for k, v in details.items():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("selftest", help="実物の.dlmを使ったソフトウェアのみの自己検証")
    sp.add_argument("template", help="実物の00550066.dlm(sys1から吸い出したもの)")
    sp.set_defaults(func=cmd_selftest)

    sp = sub.add_parser("build", help="カスタム.dlmを構築する")
    sp.add_argument("--template", required=True, help="ヘッダの土台にする実物の.dlm")
    sp.add_argument("--body", required=True, help="ボディに使う平文ファイル(tar.gz等)")
    sp.add_argument("--out", required=True, help="出力先パス")
    sp.add_argument("--encrypt-body", action="store_true", help="未実装(常にエラーになる)")
    sp.set_defaults(func=cmd_build)

    sp = sub.add_parser("verify", help="既存の.dlmファイルをPython側で検証する")
    sp.add_argument("dlm")
    sp.set_defaults(func=cmd_verify)

    args = p.parse_args()
    args.func(args)
