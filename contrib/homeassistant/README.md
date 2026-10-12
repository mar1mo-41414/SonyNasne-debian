# Home Assistant 連携(オプション)

nasne の状態を Home Assistant に出す設定です。**やりたい人向けのオプション**で、nasne 側に追加のインストールは要りません
(nasne の「状態ページ」[docs/18](../../docs/18_status_page.md)が既定で動いているので、それを HA が読みます)。

出てくるもの: HDD温度・SMART・通電時間・保留セクタ・メモリ空き・データ領域の使用率・稼働時間・負荷・チューナー使用中・HDD待機中・注意が必要・警告・online。

## 設定

1. [`nasne.yaml`](nasne.yaml) を `<HAの設定ディレクトリ>/packages/nasne.yaml` にコピーし、中の `NASNE_HOST` を nasne の IP アドレス(またはホスト名)に置換する。
2. `configuration.yaml` に、まだ無ければ次を足す:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
3. HA を再起動する。`sensor.nasne_hdd_temperature` などが現れる。

nasne の電源が切れている間、センサーは「利用不可」になり、`binary_sensor.nasne_online` が off になります。
`binary_sensor.nasne_problem`(注意が必要)が on のとき、`sensor.nasne_warnings` の属性 `warnings` に理由の一覧(HDDの温度・SMART・保留セクタ・ディスク使用率など)が入ります。

## 電源オフ・起床タイマ(さらにオプション)

nasne は LAN からの電源 ON はできません(SoC が止まっているため)。電源オフは ssh で `poweroff` を呼べます。起こしたい時刻があるなら、
`/etc/default/nasne-poweroff` の `WAKE_SECONDS`(電源を切ってから何秒後に自動起動するか。[docs/06](../../docs/06_mcu_watchdog.md))で、電源オフ時にタイマを打てます。

HA の `shell_command` の例(HA の設定ディレクトリに nasne 用の **RSA** 秘密鍵を置き、公開鍵を nasne の `/root/.ssh/authorized_keys` に入れておく。
nasne の sshd は古いので、RSA 鍵と下の2つの `-o` が必要):

```yaml
shell_command:
  nasne_poweroff: >-
    ssh -i /config/ssh/nasne_rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa
    -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/config/ssh/known_hosts root@NASNE_HOST poweroff
```

(HA のコンテナ内からの ssh は確認済み。`poweroff` 自体は [docs/06](../../docs/06_mcu_watchdog.md) のとおり実機で確認済みです。)
**状態ページ(8302番)には認証がありません。** 読み取り専用ですが、LAN の外へ出さないでください。`NASNE_STATUS_BIND=127.0.0.1`(`/etc/default/nasne-status`)で閉じることもできます。

確認した環境: Home Assistant 2026.x のコンテナで、上のパッケージを読み込んで 13 個のエンティティが値を持つこと。
