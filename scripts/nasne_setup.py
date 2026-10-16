#!/usr/bin/env python3
"""nasne を Debian 直起動にする `00550066.dlm` を、手元の公式ファームから1コマンドで作る。標準ライブラリのみ。

docs/13 の手順 1〜3(カーネルのビルド → KNLセグメント → Debianツリー → `.dlm`)を順に実行します。
Sony のファイルは配布しません。公式ファームパッケージ(KRST3101_xxxx_SECURE.dlm)は、あなたの環境へダウンロードして使います。
終わったら表示されるコマンドで、HDDのsys1に入れます(`install_dlm.sh`。この段階はHDDをPCにつなぐので自動化していません)。

使い方:
  nasne_setup.py check                              必要なコマンド・パッケージがそろっているかを調べる(足りないものと apt のコマンドを表示)
  nasne_setup.py build --hdd-dlm <HDDのsys1の00550066.dlm> [--download-official | --official-pkg <KRST3101_0260_SECURE.dlm>]
                       [--ssh-pubkey ~/.ssh/id_rsa.pub] [--work build_nasne] [--image-size 8G] [--telnet off|fail|early]

  各ステップは作業ディレクトリ(--work)に結果を残し、2回目以降は済んだステップを飛ばします(やり直すにはその成果物を消す)。
  最初は時間がかかります(カーネル用ツールチェーンのDockerビルドが30分〜1時間、debootstrapが数分)。
  `--hdd-dlm` を付けないと、最後の `.dlm` 作成の前で止まります(HDDにまだ触らずにビルドだけ確かめられます)。
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFFICIAL_URL = "http://ps-peripheral.dl.playstation.net/ps-peripheral/nasne/0260/KRST3101_0260_SECURE.dlm"
OFFICIAL_NAME = "KRST3101_0260_SECURE.dlm"

# (コマンド, apt のパッケージ, 必須か, 用途)
NEED = [
    ("python3", "python3", True, "このツール、.dlm の作成"),
    ("curl", "curl", True, "GPLソース・公式ファームの取得"),
    ("bzip2", "bzip2", True, "カーネルソースの展開"),
    ("tar", "tar", True, ""),
    ("file", "file", True, "ツリー作成時のバイナリ検査"),
    ("docker", "docker.io", True, "カーネルのビルド(古いgcc 4.3.2のコンテナ)"),
    ("debootstrap", "debootstrap", True, "Debian wheezy (mipsel) の作成"),
    ("qemu-mipsel-static", "qemu-user-static", True, "mipselのDebianをPC上で実行(debootstrapの第2段階)"),
    ("mipsel-linux-gnu-gcc", "gcc-mipsel-linux-gnu", False, "MIPS用ツールのビルド(無ければDockerのgccで代用)"),
]


def have(cmd):
    return shutil.which(cmd) is not None


def check(verbose=True):
    missing = []
    for cmd, pkg, required, why in NEED:
        ok = have(cmd)
        if verbose:
            print(f"  [{'OK' if ok else ('NG' if required else '--')}] {cmd:24s} {'' if ok else '← apt: ' + pkg}  {why}")
        if not ok and required:
            missing.append(pkg)
    # binfmt(qemu-user-static を入れると自動登録される)
    if os.path.isdir("/proc/sys/fs/binfmt_misc") and not os.path.exists("/proc/sys/fs/binfmt_misc/qemu-mipsel"):
        if verbose:
            print("  [NG] binfmt の qemu-mipsel が未登録 ← apt: binfmt-support(登録されない場合は update-binfmts --enable qemu-mipsel)")
        missing.append("binfmt-support")
    if have("docker"):
        r = subprocess.run(["docker", "info"], capture_output=True, text=True)
        if r.returncode != 0:
            if verbose:
                print("  [NG] docker を実行できない(権限またはデーモン): " + (r.stderr.strip().splitlines() or ["?"])[-1])
            missing.append("(dockerの権限: sudo usermod -aG docker $USER など)")
    if os.geteuid() != 0 and not have("sudo"):
        if verbose:
            print("  [NG] root でも sudo でもない")
        missing.append("sudo")
    if verbose:
        if missing:
            print("\n足りないもの(Debian/Ubuntu): sudo apt install " + " ".join(sorted(set(m for m in missing if not m.startswith("(")))))
            for m in missing:
                if m.startswith("("):
                    print("  " + m)
        else:
            print("\n必要なものはそろっています。")
    return not missing


def run(cmd, **kw):
    print("  $ " + " ".join(cmd if isinstance(cmd, list) else [cmd]))
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit(f"エラー: コマンドが失敗しました (rc={r.returncode})")


def step(n, title):
    print(f"\n== ステップ {n}: {title}")


def build(a):
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    sudo = [] if os.geteuid() == 0 else ["sudo"]
    if not check(verbose=False):
        print("先に `nasne_setup.py check` で足りないものを入れてください。\n")
        check()
        sys.exit(1)
    pub = os.path.expanduser(a.ssh_pubkey)
    if not os.path.isfile(pub):
        sys.exit(f"エラー: SSH公開鍵 {pub} が無い。nasne の sshd(OpenSSH 6.0)は RSA 鍵だけなので、`ssh-keygen -t rsa -b 3072` で作ってください")
    if not open(pub).read().startswith("ssh-rsa"):
        sys.exit(f"エラー: {pub} は RSA 鍵ではない(wheezy の sshd は ed25519 に非対応)。--ssh-pubkey で RSA の公開鍵を指定してください")
    hdd_dlm = os.path.abspath(a.hdd_dlm) if a.hdd_dlm else None

    # 1. 公式ファームパッケージ
    step(1, "公式ファームパッケージ")
    pkg = os.path.abspath(a.official_pkg) if a.official_pkg else os.path.join(work, OFFICIAL_NAME)
    if not os.path.isfile(pkg):
        if not a.download_official:
            sys.exit(f"エラー: 公式ファームパッケージが無い。--official-pkg で指定するか、--download-official で Sony のサーバから取得してください\n  ({OFFICIAL_URL})")
        run(["curl", "-fL", "--retry", "3", "-o", pkg + ".part", OFFICIAL_URL])
        os.replace(pkg + ".part", pkg)
    print(f"  {pkg} ({os.path.getsize(pkg)} バイト)")

    # 2. GPLソース
    step(2, "カーネルのGPLソース(Sony公開)")
    src_tar = os.path.join(ROOT, "gpl_src", "mips-linux-2.6.29.tar.bz2")
    if not os.path.isfile(src_tar):
        run(["sh", os.path.join(ROOT, "scripts", "fetch_gpl_sources.sh")])
    kdir = os.path.join(ROOT, "gpl_src", "build", "mips-linux-2.6.29")
    if not os.path.isdir(kdir):
        os.makedirs(os.path.join(ROOT, "gpl_src", "build"), exist_ok=True)
        run(["tar", "xjf", src_tar, "-C", os.path.join(ROOT, "gpl_src", "build")])
    else:
        print("  展開済み")

    # 3. カーネル用ツールチェーン(Dockerイメージ)
    step(3, "カーネル用ツールチェーン(Dockerイメージ nasne-kcc:gcc432。gcc 4.3.2 + binutils 2.19.1 をビルド。30分〜1時間)")
    if subprocess.run(["docker", "image", "inspect", "nasne-kcc:gcc432"], capture_output=True).returncode != 0:
        run(["sh", os.path.join(ROOT, "scripts", "kernel_build", "fetch_toolchain_sources.sh")])
        run(["docker", "build", "-t", "nasne-kcc:gcc432", os.path.join(ROOT, "scripts", "kernel_build")])
    else:
        print("  イメージあり")

    # 4. カーネル
    step(4, "自作カーネルのビルド(直起動用の内蔵initramfs入り)")
    vmlinux = os.path.join(ROOT, "gpl_src", "build", "out", "vmlinux_new.bin")
    # ビルド済みのカーネルが、入力(内蔵initramfs・ビルドスクリプト)より古ければ作り直す。
    # (古いまま使うと、nasne_init の更新(imageモード対応など)が入らず、起動時にSonyの経路へフォールバックし続ける)
    kb = os.path.join(ROOT, "scripts", "kernel_build")
    inputs = [os.path.join(kb, "build.sh"), os.path.join(kb, "initramfs", "nasne_init"), os.path.join(kb, "initramfs", "initramfs.list")]
    newest_input = max(os.path.getmtime(f) for f in inputs if os.path.isfile(f))
    if os.path.isfile(vmlinux) and os.path.getmtime(vmlinux) < newest_input:
        print(f"  {vmlinux} は、入力(nasne_init など)より古いので作り直します")
        os.remove(vmlinux)
    if not os.path.isfile(vmlinux):
        run(["sh", os.path.join(ROOT, "scripts", "kernel_build", "build.sh")], env=dict(os.environ, NASNE_CUSTOM="1"))
    else:
        print(f"  {vmlinux} あり")

    # 5. KNLセグメント
    step(5, "KNLセグメント(公式のKNLを雛形に、自作カーネルを包む)")
    knl = os.path.join(work, "knl_new.bin")
    ofw = os.path.join(work, "ofw_out")
    if os.path.isfile(knl) and os.path.getmtime(knl) < os.path.getmtime(vmlinux):
        print("  knl_new.bin は、カーネルより古いので作り直します")
        os.remove(knl)
    if not os.path.isfile(knl):
        run([sys.executable, os.path.join(ROOT, "scripts", "ofw_tool.py"), "split", pkg, ofw])
        run([sys.executable, os.path.join(ROOT, "scripts", "build_knl.py"), "build", os.path.join(ofw, "KNL.bin"), knl,
             "--slot", "KNL", "--raw", "--kernel", vmlinux])
    run([sys.executable, os.path.join(ROOT, "scripts", "build_knl.py"), "verify", knl])

    # 6. Debianツリー
    step(6, "Debian wheezy (mipsel) のツリー(debootstrap。数分)")
    tree = os.path.join(work, "tree")
    tar = os.path.join(work, "debian_tree.tar")
    if os.path.isfile(tar) and os.path.getmtime(tar) < os.path.getmtime(knl):
        print("  debian_tree.tar は、KNLより古い(KNLを作り直した)ので作り直します")
        run(sudo + ["rm", "-rf", tree, tar])
    if not os.path.isfile(tar):
        run(sudo + [os.path.join(ROOT, "scripts", "stage1", "prepare_tree.sh"), "--out", tree, "--knl", knl,
                    "--official", pkg, "--ssh-pubkey", pub])
        run(sudo + ["tar", "--numeric-owner", "-cpf", tar, "-C", tree, "."])
        if sudo:
            run(sudo + ["chown", f"{os.getuid()}:{os.getgid()}", tar])
    else:
        print(f"  {tar} あり")

    # 7. .dlm
    step(7, "00550066.dlm の作成")
    if hdd_dlm and not os.path.isfile(hdd_dlm):
        sys.exit(f"エラー: {hdd_dlm} が無い")
    if not hdd_dlm:
        print("  --hdd-dlm が無いので、ここで止めます。ビルドは完了しています。HDDの00550066.dlmを用意して、もう一度 build を実行してください(以前のステップは飛ばされます)。")
        return
    out = os.path.join(work, "custom_00550066.dlm")
    cmd = [sys.executable, os.path.join(ROOT, "scripts", "stage1", "make_stage1_dlm.py"), "build", "--official", hdd_dlm,
           "--out", out, "--mode", a.mode, "--image-size", a.image_size, "--telnet", a.telnet]
    if a.mode == "embed":
        cmd += ["--debian-tree-tar", tar]
    run(cmd)
    print(f"""
完了: {out}

次はHDDに入れます(HDDをPCにつなぐ。sdX は lsblk で必ず確認):
  sudo mkdir -p /mnt/nasne_sys1 && sudo mount /dev/sdX1 /mnt/nasne_sys1
  mkdir -p backup && cp /mnt/nasne_sys1/00550066.dlm backup/00550066.dlm.orig        # 元に戻す手段
  sudo bash {os.path.join(ROOT, 'scripts', 'stage1', 'install_dlm.sh')} /mnt/nasne_sys1 {out} --backup-dir backup
  sudo umount /mnt/nasne_sys1
その後、HDDを nasne に挿して電源を入れ、約5分待ちます(docs/13)。事前に SPI のバックアップ(docs/16)を取っておくことを強くお勧めします。""")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    b = sub.add_parser("build")
    b.add_argument("--hdd-dlm", help="対象HDDの sys1 にある 00550066.dlm(ヘッダの日付がHDDのマネージャと一致している必要がある)")
    b.add_argument("--official-pkg", help="公式ファームパッケージ KRST3101_xxxx_SECURE.dlm")
    b.add_argument("--download-official", action="store_true", help="Sony のサーバから v2.60 パッケージを取得する")
    b.add_argument("--ssh-pubkey", default="~/.ssh/id_rsa.pub", help="nasne の root 用公開鍵(RSA 必須)")
    b.add_argument("--work", default="build_nasne")
    b.add_argument("--mode", choices=["embed"], default="embed", help="Debianを .dlm に同梱する(p3 に手で置く方式は docs/13)")
    b.add_argument("--image-size", default="8G")
    b.add_argument("--telnet", choices=["off", "fail", "early"], default="off", help="デバッグ用の認証なしtelnetを開く .dlm にする(既定 off)。early=最初から、fail=インストール失敗時だけ。開くとLAN内の誰でもrootシェルを使える")
    a = ap.parse_args()
    if a.cmd == "check":
        sys.exit(0 if check() else 1)
    build(a)


if __name__ == "__main__":
    main()
