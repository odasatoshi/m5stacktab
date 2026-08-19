# 実機での検証手順

実機の目視やネットワークが必要で、ホストテストでは代替できない項目をまとめる。
上から順に実行すれば、依存関係の順序で確認できる。

## 0. 準備

```sh
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem101 flash
```

WiFi を設定する（SSID が変わったら再設定が必要）:

```sh
printf 'wifi <SSID> <パスワード>\r\n' > /dev/cu.usbmodem101
python tools/serial_log.py --no-reset --send wifi-status --seconds 5
# → connected ssid=... ip=192.168.x.x
```

## 1. 画面キーボードと日本語入力（#7）

`kbd` で画面下部にキーボードが出ている状態で、実機を触って確認する。

- [ ] 3 列 × 4 行のかなキーと、右端の機能キー（⌫ / 変換 / 確定 / abc）が見える
- [ ] キーを押すと青くなり、四方にフリック先のかなが薄く出る
- [ ] 「あ」キーを上フリックで「う」、右フリックで「え」が入る
- [ ] 「か」を押して `゛小` キーで「が」になる（もう一度で「か」に戻る）
- [ ] 「にほんご」と入れて `変換` を押すと「日本語」が候補に出る
- [ ] `確定` で端末に文字が入る（SSH 未接続ならエコーされる）

## 2. SSH（#5）

```sh
# 接続先で公開鍵を登録しておく
cat ~/.ssh/id_rsa_tab5.pub >> ~/.ssh/authorized_keys

printf 'ssh <user> <host>\r\n' > /dev/cu.usbmodem101
```

- [ ] ログに `authenticated with private key` → `connected to ...` が出る
- [ ] 画面にリモートのシェルプロンプトが出る
- [ ] `key ls -la\n` を送ると結果が画面に出る
- [ ] `key echo 日本語テスト\n` の日本語が正しく表示される
- [ ] ホスト鍵を変えた別ホストに繋ぐと `HOST KEY CHANGED` で拒否される
- [ ] 接続を切って再接続してもクラッシュしない

## 3. ターミナル UI（#6）

- [ ] `key vim /tmp/t.txt\n` で vim が開く（代替画面に切り替わる）
- [ ] 文字を入力して保存・終了でき、元の画面に戻る
- [ ] `key yes | head -2000\n` のような連続出力で取りこぼしやクラッシュがない
- [ ] `scroll 5` で過去に戻り、`scroll -5` で最新に戻る

## 4. WireGuard トンネル（#9）

相手側（Linux / macOS）で WireGuard を用意する。Tab5 の公開鍵は `wg <ip>` の実行時に表示される。

```sh
printf 'wg 100.64.0.9 <peer-pubkey-hex> <peer-ip>:51820\r\n' > /dev/cu.usbmodem101
python tools/serial_log.py --no-reset --send "wg stat" --seconds 5
```

- [ ] `handshake complete (peer index ...)` がログに出る
- [ ] `wg stat` で `handshake=1`
- [ ] トンネル越しに ping が通る（相手から 100.64.0.9 へ）
- [ ] 3 分以上放置しても通信が続く（rekey と keepalive が効いている）
- [ ] `wg stat` の `rekeys` が増える

## 5. Tailscale（#10, #11）

ローカル Headscale を使う（SaaS より原因が分かる）:

```sh
docker run -d --name headscale-tab5 -p 8080:8080 \
  -v <config>:/etc/headscale -v <data>:/var/lib/headscale \
  headscale/headscale:latest serve
docker exec headscale-tab5 headscale users create tab5
docker exec headscale-tab5 headscale preauthkeys create --user 1 --reusable --expiration 720h
```

Rancher Desktop などでポートフォワードが localhost 限定の場合は、`0.0.0.0` で待って
`127.0.0.1` に中継する TCP プロキシを挟む。

```sh
printf 'ts <headscale-ip> <authkey> 8080 131\r\n' > /dev/cu.usbmodem101
```

- [ ] `>>> assigned address: 100.64.x.x/32` が出る
- [ ] `headscale nodes list` に Tab5 が現れる
- [ ] `wg disco` で `peers` が 1 以上になる（netmap から登録される）
- [ ] 他のピアから `tailscale ping <tab5>` を打つと `wg disco` の `pings` と `pongs` が増える
- [ ] ピアから 100.64.x.x へ ping が通る

## 6. 耐久

- [ ] 30 分放置して `alive` ログが続き、ヒープが減り続けないこと
- [ ] WiFi の AP を落として戻すと再接続する（`reconnect in ... ms` のあと `got ip`）
