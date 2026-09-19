# 実機での検証手順

実機の目視やネットワークが必要で、ホストテストでは代替できない項目をまとめる。
上から順に実行すれば依存関係の順序で確認できる。

コマンドは USB Type-C のシリアルコンソールに送る。対話で使うなら
`screen /dev/cu.usbmodem101 115200`、非対話なら次の形で送れる。

```sh
printf '<command>\r\n' > /dev/cu.usbmodem101
python tools/serial_log.py --no-reset --send "<command>" --seconds 10
```

## 0. 準備

```sh
source ~/esp/esp-idf/export.sh
idf.py -p /dev/cu.usbmodem101 flash
```

WiFi を設定する（**SSID が変わったら再設定が必要**）:

```sh
printf 'wifi <SSID> <パスワード>\r\n' > /dev/cu.usbmodem101
python tools/serial_log.py --no-reset --send wifi-status --seconds 5
# → connected ssid=... ip=192.168.x.x
```

## 1. 画面キーボード（#7 / #65 / #70）

**コンソールに 256 文字以上の行を送らないこと (#72)。** ESP-IDF の linenoise が
確保領域の外に書き、**何十秒も後に無関係な `free` が abort する**。
`tools/serial_log.py` は送る前に弾くが、`printf > /dev/cu...` で直に送ると素通りする。

### 1.1 段の巡回

ステータスバーの `MENU` の隣（x 96..184）をタップすると段が変わる。
`kbd [off|ascii|kana|pad|next]` と `tap 140 12` は同じ経路を通る。

- [ ] `なし → ABC → かな → PAD → なし` の順に回る
- [ ] 端末の行数が `なし` 29 / `ABC` 13 / `かな` 13 / **`PAD` 29**（PAD は削らない）
- [ ] メニュー表示中はバー全体が `CLOSE`（段は変わらない）
- [ ] ステータスバーの面が `なし` / `ABC` / `かな` / `PAD` に変わる

### 1.2 ASCII 面

- [ ] 12 列 × 5 行が出る（1 キー 106x70px）
- [ ] `Shift` を押すと面が大文字・記号に変わり、**次の 1 打だけ**効いて戻る
- [ ] `Ctrl` + `c` で `03` が出る（`kbdlog` で見る）
- [ ] `Tab` = `09`、`↑` = `1B 5B 41`、`Enter` = `0D`、`BS` = `7F`
- [ ] WiFi のパスワード入力を開くと**自動で ASCII 面になり**、終わると元の段へ戻る

### 1.3 PAD 段（端末に重ねる）

- [ ] 右下に `Esc ↑ Tab ^C` / `← ↓ → Enter` が薄く浮く
- [ ] **端末が 29 行のまま**で、PAD の下の行を書き換えても PAD が欠けない
- [ ] `^C` が `03` を送る（`Ctrl` 単体のキーは無い）
- [ ] PAD を抜けると重ねた画素が消える
- [ ] **見た目は等倍で確かめる。** `screencap` の縮小は点サンプリングなので市松は
      実物より薄く写り、**step が偶数だと透過画素だけを拾って丸ごと消えて見える**。
      `screencap 1 880 540 400 180` で吸うこと

### 1.4 かな面と日本語入力（#7）

- [ ] 3 列 × 4 行のかなキーと、右端の機能キー（BS / 変換 / 確定 / abc）が見える
- [ ] キーを押すと青くなり、四方にフリック先のかなが薄く出る
- [ ] 「あ」キーを上フリックで「う」、右フリックで「え」が入る
- [ ] 「か」を押して `゛小` キーで「が」になる（もう一度押すと「か」に戻る）
- [ ] 「にほんご」と入れて `変換` を押すと「日本語」が候補に出る
- [ ] `確定` で端末に文字が入る（SSH 未接続なら画面にエコーされる）

## 2. SSH（#5）

接続先で公開鍵を登録しておく（Tab5 の鍵は `sshkey` パーティションに書いたもの）:

```sh
cat ~/.ssh/id_rsa_tab5.pub >> ~/.ssh/authorized_keys
printf 'ssh <user> <host>\r\n' > /dev/cu.usbmodem101
```

- [ ] ログに `authenticated with private key` → `connected to ...` が出る
- [ ] 画面にリモートのシェルプロンプトが出る
- [ ] `key ls -la\n` を送ると結果が画面に出る
- [ ] `key echo 日本語テスト\n` の日本語が正しく表示される
- [ ] ホスト鍵が変わった相手には `HOST KEY CHANGED` で接続を拒否する
- [ ] 切断して再接続してもクラッシュしない

## 3. ターミナル UI（#6）

- [ ] `key vim /tmp/t.txt\n` で vim が開く（代替画面に切り替わる）
- [ ] 文字を入力して保存・終了でき、元の画面に戻る
- [ ] `key yes | head -2000\n` のような連続出力で取りこぼしやクラッシュがない
- [ ] `scroll 5` で過去に戻り、`scroll -5` で最新に戻る。
      `termdump` のヘッダが `scrollback=5/N` を出し、上 5 行が過去の行になっている
      （`termdump` は「今画面に見えているもの」を出すので、スクロール中の中身が見える）
- [ ] `swipe 600 100 300` で指のスワイプと同じ経路でスクロールする。
      出力の `[terminal]` / `[menu]` / `(alt screen: ...)` で、どこに食われたか分かる
- [ ] `bench` の値が記録できる（1 文字あたり数百 us、全画面 70ms 程度）

## 3.5 相手機なしでできる疎通確認

WiFi や相手機が無くても、UDP ループバックで暗号とプロトコルの往復を確かめられる。
実機に書き込んだ直後の健全性確認に使う。

```sh
printf 'wgloop\r\n' > /dev/cu.usbmodem101      # WireGuard のハンドシェイクとデータ往復
printf 'discoloop\r\n' > /dev/cu.usbmodem101   # DISCO の Ping/Pong 往復
printf 'wgtest\r\n' > /dev/cu.usbmodem101      # 暗号プリミティブの自己検証
```

期待する出力:

```
handshake over udp loopback: ok (698075 us)
data over udp loopback: ok (314 us round trip)
reverse direction: ok
  old-key packet after rekey: ok
  unconfirmed sends on old key: ok
  after confirmation both on new key: ok
  two unconfirmed rekeys keep the live key: ok
rekey crossover (no drop): ok

pong says our address is 127.0.0.1:41651
disco ping/pong over udp loopback: ok (1420 us round trip)
  pings=1 pongs=1 unknown=0
```

これが通らない状態で相手機と繋いでも原因の切り分けができないので、先にここを確認する。

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

ローカル Headscale を相手にする（SaaS より原因が分かる。プロトコルは同一）。

```sh
docker run -d --name headscale-tab5 -p 8080:8080 \
  -v <config>:/etc/headscale -v <data>:/var/lib/headscale \
  headscale/headscale:latest serve
docker exec headscale-tab5 headscale users create tab5
docker exec headscale-tab5 headscale preauthkeys create --user 1 --reusable --expiration 720h
```

Rancher Desktop などでポートフォワードが localhost 限定の場合は、`0.0.0.0` で待って
`127.0.0.1` へ中継する TCP プロキシを挟む必要がある。

```sh
printf 'ts <headscale-ip> <authkey> 8080 131\r\n' > /dev/cu.usbmodem101
```

- [ ] `>>> assigned address: 100.64.x.x/32` が出る
- [ ] `headscale nodes list` に Tab5 が現れる
- [ ] `wg disco` で `peers` が 1 以上になる（netmap から登録される）
- [ ] 他のピアから `tailscale ping <tab5>` を打つと `wg disco` の `pings` と `pongs` が増える
- [ ] ピアから 100.64.x.x へ ping が通る

## 6. 耐久

- [ ] 30 分放置して `alive` ログが続き、ヒープが減り続けないこと。
      **画面キーボードが絡む変更では PAD 段のまま**、コンソールを叩き続けて回す
      （放置だけだと描画経路をほとんど通らない）
- [ ] WiFi の AP を落として戻すと再接続する（`reconnect in ... ms` のあと `got ip`）

## 記録の残し方

PR に貼るログは次で採取する。`idf.py monitor` は標準入力が TTY でないと動かないので使えない。

```sh
python tools/serial_log.py --seconds 30 > /tmp/log.txt          # リセットしてから採取
python tools/serial_log.py --no-reset --seconds 30 > /tmp/l.txt # 動作中の様子を採取
```

## 画面の向き

`rottest` はピクセルを読み戻して自動判定するので、目視は要らない。

```sh
printf 'rottest\r\n' > /dev/cu.usbmodem101
```

```
  top-left      landscape(  40, 40) -> native(679,  40) want f800 got f800 ok
  top-right     landscape(1240, 40) -> native(679,1240) want 07e0 got 07e0 ok
  bottom-left   landscape(  40,680) -> native( 39,  40) want 001f got 001f ok
  bottom-right  landscape(1240,680) -> native( 39,1240) want ffe0 got ffe0 ok
rotation matches setRotation(1): ok
```

MISMATCH が出たら、逆向き（反時計回り 90 度）で何個一致するかも出るので原因がすぐ分かる。

`rottest` が見ているのは**座標の写像だけ**で、PPA の回転角は見ていない。
角度の側は `ppatest` が判定する（左半分が赤・右半分が青の非対称なブロックを
転送して、読み戻した画素で確かめる）。

```sh
printf 'ppatest\r\n' > /dev/cu.usbmodem101
```

```
ppa rotate 1280x24 -> native(696,0) 24x1280: 941 us
ppa angle check: left f800 (want f800) right 001f (want 001f) ok
```

角度と写像が食い違っていると `SWAPPED` と出る（意図的に `ANGLE_90` に戻して確認済み）。

```
ppa angle check: left 001f (want f800) right f800 (want 001f) SWAPPED - rotation_angle が rot と食い違っている
```

**本番の描画経路**（vt100 → スプライト → PPA → フレームバッファ）は `termcheck` が判定する。
`rottest` は純関数の写像、`ppatest` は自前の PPA 設定しか見ないので、
**push_row_ppa の角度がずれてもあの 2 つは緑のまま通る**（実機で確認済み）。
ユーザーが報告した症状を捕まえられるのは `termcheck` だけ。

エスケープは実機側で組むので、シリアル越しのクォートで壊れることがない。

```sh
printf 'termcheck\r\n' > /dev/cu.usbmodem101
```

```
  (   6, 36) want c800 got c800  row0 cell0 = red                   ok
  (  18, 36) want 0660 got 0660  row0 cell1 = green (red の右)      ok
  (1254, 36) want 0000 got 0000  row0 右端付近 = 黒                 ok
  (   6, 60) want 001d got 001d  row1 cell0 = blue (row0 の下)      ok
  (   6, 84) want 0000 got 0000  row2 = 黒                          ok
render path (vt100 -> sprite -> PPA -> framebuffer): ok
```

y が 36 から始まるのは、画面上端の 24px をステータスバーに譲っているため
（`renderer->origin_y()`）。ここを足し忘れるとステータスバーの背景色を読む。

角度だけを `ANGLE_90` に戻すと、右端に緑が来て（横方向の反転）`FAILED` になる。

任意の座標の色を見たいときは `pix <lx> <ly> ...`。

## 画面キャプチャ

写真が撮れない状況（遠隔、CI、エージェント実行）でも、フレームバッファを吸い出せば
画面が絡む変更の証跡を残せる。視差も照明も入らないので写真より正確。

```sh
# 全画面（間引き 4 → 320x180、約 20 秒）
python tools/serial_log.py --no-reset --seconds 40 --send screencap > cap.log
python tools/screencap.py cap.log screen.png

# 文字を読みたいときは範囲を絞って間引きなし
python tools/serial_log.py --no-reset --seconds 40 --send "screencap 1 0 0 640 48" > cap.log
python tools/screencap.py cap.log top.png
```

`screencap [step] [x] [y] [w] [h]`。rotation 1（キーボードが描くのと同じ向き）で読むので、
出てくる絵は目で見えているものと同じになる。

`docs/screenshots/` に、端末とキーボードの天地が揃っていることを確かめたときの
キャプチャを置いてある。

## 初期メニュー

電源投入でメニューが出る。純正キーボード (#15) が来るまでのキー入力手段は
シリアルの `menu` コマンド。

```sh
printf 'menu\r\n'        > /dev/cu.usbmodem101   # 表示（引数なしは show）
printf 'menu down\r\n'   > /dev/cu.usbmodem101
printf 'menu enter\r\n'  > /dev/cu.usbmodem101
printf 'menu esc\r\n'    > /dev/cu.usbmodem101   # 戻る
printf 'menu hide\r\n'   > /dev/cu.usbmodem101
```

タップは 3 通り。

- 項目をタップすると、移動と決定を兼ねる（指で 2 度押しは使いにくい）
- **ステータスバーをタップするとメニューを開閉する。** 判定は上から 56px
  （描画は 24px だが、指で当てるには 3.5mm は狭い）
- VPN / Settings 画面の `< Back` をタップで戻れる

メニュー表示中はキーボードを隠す。隠さないとメニューがキーボードの領域を覆わないので
指でキーを押せてしまい、その出力が端末経由でメニューの矩形に描かれて崩れる。

画面の配分は `apply_layout()` 1 箇所で決める。

| 状態 | 端末 |
|---|---|
| キーボード表示 | 15 行 (360px) |
| キーボード非表示 | 29 行 (696px) |

## タッチの検証

**タッチは 2 段に分けて検証する。** 座標を受け取った後は機械で全部叩けるが、
その上流（タッチ IC が物理的な角でどの生座標を報告するか）は指で押すしかない。

### 座標より下流（機械で叩ける）

`tap` / `swipe` は実タッチと**同じ関数**（`touch_down_at` / `touch_move_at` /
`touch_up_at`）を通るので、ルーティングと動作の検証になる。

```sh
printf 'tap 40 12\r\n'            > /dev/cu.usbmodem101   # ステータスバー = メニュー開閉
printf 'tap 100 152\r\n'          > /dev/cu.usbmodem101   # メニューの 2 番目の項目
printf 'swipe 600 100 300\r\n'    > /dev/cu.usbmodem101   # 端末の縦スワイプ
printf 'swipe 400 490 490 340\r\n' > /dev/cu.usbmodem101  # IME の横フリック
```

`tap` は `menu=` / `keyboard=` / `terminal=` / `scrollback=` を出すので、
状態の遷移がそのまま読める。

### 描画側とタッチ側の回転の一致（機械で叩ける）

**描画は自前の `components/rotate`、タッチは M5GFX と実装が別物**なので、
この 2 系がずれると「押した場所と違うところが反応する」。`touchmap` が
`convertRawXY` に合成座標を通して照合する。

```sh
printf 'touchmap\r\n' > /dev/cu.usbmodem101
```

```
左上 native (  0,   0) -> m5gfx(   0,719) rotate(   0,719) 差 (0,0)
右上 native (719,   0) -> m5gfx(   0,  0) rotate(   0,  0) 差 (0,0)
左下 native (  0,1279) -> m5gfx(1279,719) rotate(1279,719) 差 (0,0)
右下 native (719,1279) -> m5gfx(1279,  0) rotate(1279,  0) 差 (0,0)
grid 546 点 (最端を含む): 最大ずれ dx=1 (native 0,53) dy=0 (native 0,0)
touch/render rotation agreement: ok (許容 1 画素, 超え 0 点)
```

**ビット一致は要求しない。** タッチ側は float のアフィン変換を int32 に切り捨て、
描画側は最初から整数なので 1 画素の差は構造的に出る。誤差はスケール由来で
**座標が大きいところで最大**になるので、格子は最端 (719 / 1279) を必ず踏む。

### 生座標の範囲（**指でしか確かめられない**）

`convertRawXY` に合成座標を通せるのは「生座標を受け取った後」まで。
**タッチ IC が物理的な角で実際にどの生座標を報告するか**は押さないと分からない。

```sh
printf 'touchlog\r\n' > /dev/cu.usbmodem101
# 画面の四隅と、キーボードのキーを 1 つ指で押す
python tools/serial_log.py --no-reset --seconds 30
```

期待値は `touchlog` が出す（左上 `(0,0)` 付近 / 右上 `(1279,0)` / 左下 `(0,719)` /
右下 `(1279,719)`）。ここがずれていたら、パネルと IC の個体差か、M5GFX の
ボード定義が合っていない。

あわせて**触った感触**（判定の広さ、スワイプの追従感）も見る。数値では測れない。
