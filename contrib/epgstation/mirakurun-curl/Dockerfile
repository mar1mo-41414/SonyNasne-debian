# 公式の Mirakurun イメージには curl が入っていない(コマンド型チューナー `curl -s http://<nasne>:8301/tuner/<ch>` が動かない)ので、足す。
FROM chinachu/mirakurun:latest
RUN apt-get update && apt-get install -y --no-install-recommends curl && rm -rf /var/lib/apt/lists/*
