# nasne をチューナーにした tvheadend(と、Kodi・Jellyfin)

nasne(Debian直起動)の地デジを、別のPC(Docker)の **tvheadend** で扱います。tvheadend は番組表・予約録画・ライブ視聴をまとめて提供し、**Kodi**(PVR クライアント `pvr.hts`)や Jellyfin のライブTVの入力にもできます。
Mirakurun + EPGStation([contrib/epgstation](../epgstation/README.md))の代わりの選択肢です。nasne上では動かしません。

## 確認したこと(実機)

tvheadend 4.3(`lscr.io/linuxserver/tvheadend`)を Docker で動かし、nasne の `playlist.m3u8` を「IPTV 自動ネットワーク」に登録:
- **20サービスすべてを検出**(各サービスのURLを順にスキャンし、約5分)。サービスをチャンネルに割り当て、**手動のDVR予約で70秒録画**(約14.4Mbps、`Completed OK`、デコードの警告は先頭の数行だけ)。

## 手順(要点)

1. nasne 側で `curl http://<nasne>:8301/scan` を1回実行(M3Uの元になるサービス一覧を作る。約80秒)。
2. tvheadend を起動する。**`--ulimit nofile=65536:65536` を付ける**(付けないと、Docker の巨大なファイル記述子の上限のせいで、起動時の子プロセスが数分間 100% のまま止まる):

   ```bash
   docker run -d --name tvh --ulimit nofile=65536:65536 -p 9981:9981 -p 9982:9982 \
     -e TZ=Asia/Tokyo -v ./tvh-config:/config -v ./tvh-recordings:/recordings lscr.io/linuxserver/tvheadend:latest
   ```
3. Web UI(`http://<PC>:9981/`)の「設定 → DVB入力 → ネットワーク」で **IPTV 自動ネットワーク**を追加:
   - URL: `http://<nasne>:8301/playlist.m3u8`
   - **最大ストリーム数(max_streams): 1**(チューナーは1系統)
   - 「作成後にスキャン」を有効
   (REST API なら `POST /api/mpegts/network/create`、`class=iptv_auto_network`。)
4. サービスの検出が終わったら、「サービスのマップ」でチャンネルを作る。以降は、EPG・録画・視聴ができる。

## 注意(必ず読む)

- **チューナーは1系統**。tvheadend は、アイドル中も OTA EPG の取得などで定期的にストリームを開く。**録画や視聴と重なる**ので、ネットワークの設定で「アイドルスキャン」を切り、EPG の取得(`epggrab`)の対象からIPTVのmuxを外すこと。EPG は、nasne の SI(番組表の元データ)から作られる。
- nasne の HTTP は、チューナーが使用中のとき、**最大10秒だけ待ってから 503** を返す(直前のセッションの終了処理中の要求を、すぐ断らないため。tvheadend のスキャンが1つおきに失敗する現象は、これで解消した)。
- **固まる件(未解決)**: 複数のクライアントが短い間隔で開閉を繰り返すと、**まれに**(今回の試験で、数十〜数百回に1回)nasne 側の `nasne-recpt1` が D 状態(ドライバの中で戻らない)になり、**電源を入れ直すまで復旧しない**。ドライバ/メディアエンジンの問題で、tvheadend の常時スキャンと Jellyfin のプローブを同時に動かすと再現しやすい。
  固まると、[状態ページ](../../docs/18_status_page.md)に「チューナーのプロセスが固まっている」と出る(Home Assistant の `nasne 警告` にも出る)。**常用するときは、クライアントを1つに絞る**ことを勧める。
- Kodi: tvheadend の **HTSP**(9982番)を、Kodi の PVR クライアント(`pvr.hts`)で使うのが素直。Kodi の IPTV Simple Client で、nasne の M3U を直接読ませることもできるはず(**未確認**。ライブ視聴のたびにチューナーを開くので、上の注意が当てはまる)。
- Jellyfin: nasne の M3U を「ライブTV」のチューナーに追加すると、**20チャンネルが取り込める**(確認済み)。ただし、**ライブ視聴は確認できていない**。Jellyfin は再生前に `ffprobe`(`-probesize 1G`)でストリームを調べるが、ISDB の PMT に載っている字幕・データの PID からは、**パケットがほとんど来ない**ため、解析が終わらない(Jellyfin の待ち時間で失敗する)。さらに、プローブと本再生が**別々の接続**でチューナーを開くため、1系統のチューナーでは、2本目が503になる。
  Jellyfin で見たいときは、**tvheadend を経由**する(tvheadend の HDHomeRun 互換エミュレーション、または M3U を Jellyfin に登録)ほうが現実的(未確認)。
