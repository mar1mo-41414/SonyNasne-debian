# nasne をチューナーにした EPGStation(録画サーバー)

nasne(Debian直起動)を「地デジチューナー」として使い、別のPC(Docker)で Mirakurun + EPGStation を動かして、番組表・予約録画をします。
nasne上では録画ソフトを動かしません。録画ファイルは Mirakurun/EPGStation を動かすPC側に保存されます(元画質のMPEG-2 HDのTS、約15Mbps)。詳しい仕組み: [docs/11](../../docs/11_tv_streaming.md)。

実機で確認済み: Mirakurun 4.1 + EPGStation 3.0(beta)で、サービスの検出 → 番組表の取得 → EPGStationからの録画予約(放送中の番組、これから始まる番組)→ 録画ファイルの再生確認(フレーム欠落なし)。

## 手順

1. nasne側: `nasne-recpt1` のサーバーが動いていること(Debian直起動のイメージでは既定で動く)。`curl http://<nasne>:8301/status` が `idle` を返す。
2. このディレクトリを置いたPCで(Docker と docker compose が要る):

   ```bash
   mkdir -p mirakurun-conf mirakurun-data epgstation/{config,data,recorded,thumbnail,logs}
   # Mirakurun のチューナー・チャンネル設定を nasne から作る(--scan: 約80秒チューナーを使って局を調べる)
   python3 ../../scripts/nasne_mirakurun_conf.py <nasneのIP> --scan -o mirakurun-conf
   # EPGStation の設定: イメージ内の雛形を取り出して、Mirakurun の場所だけ変える
   docker create --name tmp-epg l3tnun/epgstation:master-debian && docker cp tmp-epg:/app/config/. epgstation/config/ && docker rm tmp-epg
   cd epgstation/config && cp config.yml.template config.yml && cp enc.js.template enc.js \
       && cp operatorLogConfig.sample.yml operatorLogConfig.yml && cp serviceLogConfig.sample.yml serviceLogConfig.yml \
       && cp epgUpdaterLogConfig.sample.yml epgUpdaterLogConfig.yml \
       && sed -i 's#^mirakurunPath:.*#mirakurunPath: http://mirakurun:40772/#' config.yml && cd ../..
   docker compose up -d
   ```
3. 数分待つ(サービスの検出に約3分、全局の番組表の取得に約10分)。そのあと **EPGStation を再起動**する(`docker compose restart epgstation`)。
   EPGStation は起動時に Mirakurun からサービスと番組を読むので、Mirakurun の準備が終わる前に起動していると空のまま待つことになります。
4. ブラウザで `http://<このPC>:8888/` を開く。番組表から予約する。

## 注意

- 公式の Mirakurun イメージには **curl が入っていません**。コマンド型チューナー(`curl -s http://<nasne>:8301/tuner/<ch>`)が動かないので、`mirakurun-curl/Dockerfile` で curl を足したイメージを使います(compose がビルドします)。
- チューナーは **1系統**です。同時に録画できるのは1番組だけで、裏番組は録画できません(EPGStation が競合として扱う)。録画中は番組表の取得(EPG収集)が止まります(録画のほうが優先される)。
- 録画の開始は、予約時刻の約15秒前にチューナーを開きます(EPGStation の既定)。nasne が選局して映像が出るまで数秒かかるため、ちょうど良い余裕です。
- 録画ファイルの先頭に、デコードの警告が数行出ることがあります(番組の途中から受け始めたときの、最初のGOPが揃うまで)。再生は問題ありません。
- ワンセグ・データ放送は設定に含めません(`nasne_mirakurun_conf.py` が除外します)。
- チャンネル設定はサービスごとに1エントリです(nasne は1回に1サービスしか出力しないため)。局が増えた・変わったら `--scan` で作り直してください。
- BS/CS は、作者の環境では信号が来ておらず**未検証**です(他の人の環境で動作確認)。使うときは下の「BS/CS を使う」を見てください。

## BS/CS を使う(任意。作者の環境では信号が無く未検証。他の人の環境で動作確認)

nasne は**1回に1サービスしか出さない**ので、Mirakurun のチャンネルは「サービスごと」に、サービスの指定を `commandVars` で渡す形にします(チャンネル名に `?sid=` を含めると、Mirakurun の API が 404 になる)。詳しくは [docs/11](../../docs/11_tv_streaming.md)。

```bash
# nasne に地デジ+BS+CS をスキャンさせて(数分。録画中は避ける)、tuners.yml / channels.yml を作る。そして Mirakurun を再起動
python3 ../../scripts/nasne_mirakurun_conf.py <nasne> --sat --scan -o mirakurun-conf
```

Mirakurun のチャンネルスキャンで作った BS/CS の項目は、`--fix-sat mirakurun-conf/channels.yml --enable` で nasne 用に直せます(BS は `useSubCh=1` でスキャンする)。

## 予約に合わせて nasne を起こす(任意)

nasne を電源オフにしておき、予約の少し前に自動で起こせます([docs/06](../../docs/06_mcu_watchdog.md))。nasne で `/etc/default/nasne-wake` に EPGStation の場所を書き、`/etc/init.d/nasne-wake restart`:

```
NASNE_WAKE_URL=http://<EPGStationのホスト>:8888
```

あとは、nasne を `poweroff`(または電源ボタンの長押し)で止めるだけです。次の予約の5分前に起きて、予約どおりに録画されます。
注意: 電源を切っている間に追加した予約は、次に起きるまで反映されません。たまに起こしたいときは `/etc/default/nasne-poweroff` に `WAKE_SECONDS=<秒>`(予約の起床と短いほうが使われる)。

自動で電源も切りたい(予約録画が終わって、使われなくなったら切る)ときは、`/etc/default/nasne-wake` に `NASNE_WAKE_AUTOOFF=1` を足します。**タイマで起きたときだけ**、録画・視聴・ssh が `NASNE_WAKE_IDLE` 秒(既定180)使われず、次の予約まで10分以上あるときに電源を切ります(手動で入れた電源は切りません)。Mirakurun の番組表の取得(約10分)は、最長 `NASNE_WAKE_EPG_MAX` 秒(既定600)まで待ちます(0 なら待たずに切る。そのぶん番組表の取得は途中で終わる)。
電源が切れている間に追加した予約は、次に起きるまで反映されません。`/etc/default/nasne-poweroff` に `WAKE_SECONDS=21600`(6時間)のように書くと、予約の起床と短いほうで定期的に起きて、予約を確認します。

## Tip: 番組表の巡回でチューナーが塞がるのを防ぐ
nasne を地デジ用・衛星用の2チューナーで使う(`--dual-tuner`、[docs/11](../../docs/11_tv_streaming.md))場合、Mirakurun の `server.yml`(`mirakurun-conf/server.yml`)に `jobMaxRunning: 1` を設定すると、バックグラウンドの番組表の巡回が地デジと衛星で同時に走って、両方のチューナーを塞ぐのを防げます(他の人の実機で、極めて安定した)。
