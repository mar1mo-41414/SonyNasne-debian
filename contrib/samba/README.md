# nasne を Samba のファイルサーバーにする(オプション)

nasne(Debian直起動)の `/data`(p3。録画領域のXFS、容量が大きい)を、Windows・macOS・Linux から SMB で使えるようにします。nasne は空き容量が大きく(HDDの容量ぶん)、省電力なので、ホームサーバー向きです。
**やりたい人向けのオプション**で、既定では入っていません(メモリ約240MBの機体なので、使わない人には載せない)。

## 手順

nasne に ssh で入って(root)、スクリプトを実行します:

```bash
scp contrib/samba/setup-samba.sh root@<nasne>:/tmp/     # (nasne の ssh は RSA 鍵と、-o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa が要る)
ssh root@<nasne> sh /tmp/setup-samba.sh                  # 共有名 nasne、/data/share、ユーザー nasne。Sambaのパスワードを聞かれる
```

- 引数: `sh setup-samba.sh [共有名 [ディレクトリ [ユーザー名]]]`。パスワードを環境変数で渡すなら `SMB_PASSWORD=… sh setup-samba.sh`(履歴に残るので注意)。
- やること: `samba` と `samba-common-bin` を入れる(推奨パッケージなし)、`/etc/samba/smb.conf` を作る(元は `smb.conf.bak` へ)、共有用のユーザーとディレクトリを作る、`smbd` を起動。
- 繋ぎかた: Windows はエクスプローラーに `\\<nasneのIP>\nasne`、macOS は Finder の「サーバへ接続」に `smb://<nasneのIP>/nasne`、Linux は `mount -t cifs //<nasneのIP>/nasne /mnt/x -o username=nasne`。

## 確認したこと(実機)

- Samba 3.6.6(wheezy)。`smbd` のメモリは約4MB。
- `smbclient`(Linux)から、SMB2 でログイン・一覧・50MBの書き込み(約14MB/s)・読み出し(約31MB/s)・md5一致。**間違ったパスワード・ゲストは拒否**される。
- 公式が作る `share/VIDEO・MUSIC・PHOTO` がそのまま見える(`/data/share`)。

## 注意

- Windows 10/11・macOS からは**未確認**(SMB2 で繋がるはず)。
- Samba 3.6 には `min protocol` が無く、**SMB1(NT1)でも繋がる**。SMB1 を許したくない場合は、ファイアウォール等で LAN の信頼できる機器だけにするか、新しい Samba を別途用意する。
- NetBIOS(`nmbd`)は止めている。ネットワークの一覧には出ないので、IPアドレスかホスト名で繋ぐ。
- 認証はユーザー名とパスワード(ゲスト不可)。**LAN内で使い、インターネットには出さない**こと。古いカーネル・Sambaなので更新は受けられない([README のセキュリティ](../../README.md#セキュリティネットワークに出るもの))。
- 公式ファーム(Sonyのアプリ)が `/data/share` を使う(DLNAのメディア共有など)。公式アプリを併用しないDebian構成なので、ここを共有フォルダに使っている。
