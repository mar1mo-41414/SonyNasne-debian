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
- BS/CS は未対応です。
