# m5stacktab

M5Stack Tab5 (ESP32-P4) 用ファームウェア。SSH コンソール・VPN・日本語入力を持つ携帯端末を作る。

## 必須機能と現状

| 機能 | 状態 |
|---|---|
| **SSH コンソール** | 秘密鍵認証で実機接続まで動作。VT100 エミュレータは自作（ホストテスト 172 チェック） |
| **VPN** | Tailscale の制御プロトコル (ts2021) と WireGuard データプレーンを実装。ローカル Headscale 相手に登録と netmap 取得を実証済み |
| **日本語入力** | SKK 辞書 17.5 万語をフラッシュから mmap して検索（実機 160〜272μs、RAM 消費ゼロ）。12 キーフリック入力 |

詳細は [Issues](https://github.com/odasatoshi/m5stacktab/issues) を参照。

## 実機で確認できていること

```
panel        : 1280x720 (MIPI-DSI, PSRAM 200MHz で underrun 0)
terminal     : 106x16 (cell 12x24) + 画面キーボード 640x304
1 文字の再描画: 224us / 849px
全画面書き換え : 67ms
WiFi         : ESP32-C6 経由 (esp-hosted / SDIO)、切断時は指数バックオフで再接続
               接続先を 5 件まで NVS に保存し、画面から追加・選択・削除できる
SSH          : RSA (PEM) 秘密鍵で認証、PTY 106x16
かな漢字変換  : にほんご→日本語 160us / かんじ→12 候補 272us
WireGuard    : netif 100.64.0.0/10 mtu 1280、keepalive と rekey 実装
DISCO        : NaCl crypto_box（公式テストベクタで全層一致）
```

## ビルドと書き込み

```sh
source ~/esp/esp-idf/export.sh          # ESP-IDF v5.5.1
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
python tools/serial_log.py --seconds 20 # ログ採取（idf.py monitor は TTY 必須で使えない）
```

辞書と SSH 秘密鍵はパーティションに置く（ファームには含めない）:

```sh
python tools/build_dict.py --out build/dict.bin
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name dict --input build/dict.bin

ssh-keygen -t rsa -b 2048 -m PEM -N '' -f ~/.ssh/id_rsa_tab5   # ed25519 は使えない
# ECDSA なら named curve 形式にする（ssh-keygen が作る形式は mbedTLS が読めない）
# openssl ecparam -name prime256v1 -genkey -noout -out ~/.ssh/id_ecdsa_tab5.pem
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name sshkey --input ~/.ssh/id_rsa_tab5
```

## コンソールコマンド

USB Type-C のシリアルコンソール（`screen /dev/cu.usbmodem101 115200`）で操作する。
非対話で送るなら `python tools/serial_log.py --send "<command>" --send-delay 13 --seconds 30`。

| コマンド | 用途 |
|---|---|
| `wifi <ssid> [password]` | WiFi 設定（NVS に保存して接続。最大 5 件） |
| `wifi-list` | 保存済みの WiFi（`*` = 接続中） |
| `wifi-del <index>` | 保存済みの WiFi を消す |
| `wifi-scan` | 周りの AP を探す |
| `wifi-status` | 接続状態 |
| `ssh <user> <host>` | 秘密鍵で SSH 接続（`sshkey` パーティションの鍵を使う） |
| `ssh <user> <host> <password> [port]` | パスワードで SSH 接続 |
| `key <text>` | SSH にキー入力を送る（`\e` = ESC。キーボードが無いときの入力手段） |
| `sshclose` | SSH 切断 |
| `profiles [reload\|import\|clear]` | 接続先 (NVS) の一覧／SD からの取り込み／消去 |
| `connect <name\|index>` | 接続先に繋ぐ（メニューから選ぶのと同じ経路） |
| `nvsstat` | NVS の使用量と中身を見る |
| `conv <romaji>` | ローマ字→かな→漢字を試す |
| `term <text>` / `termtest` / `termscroll` | 端末描画の確認（`\e` でエスケープを送れる） |
| `bench` | 描画コストの実測（全画面 vs 1 文字） |
| `scroll [lines]` | スクロールバックを動かす |
| `kbd [off]` | 画面キーボードの表示切り替え（端末領域のダブルタップでも同じ） |
| `kbdhw` | 純正キーボード (Ext.Port1 I2C) の状態を見る／読み取りを立て直す |
| `kbdlog [off]` | 打鍵をシリアルに出す |
| `kbdinject <key-name> [mod]` | 打鍵を合成する（mod: 1=Ctrl 4=Alt）。遠隔で送出経路を確かめる |
| `flip [0\|1\|on\|off]` | 画面を 180 度反転（純正キーボードを付けた向き） |
| `menu [show\|hide\|up\|down\|enter\|esc]` | メニューの操作（指なしで検証するため） |
| `wgtest` | WireGuard の暗号とハンドシェイクを実機で検証 |
| `wg <tunnel-ip> [pubkey] [host:port]` | WireGuard トンネルの netif |
| `wg stat` / `wg disco` / `wg down` | 統計・DISCO 状態・停止 |
| `ts <host> <authkey> [port] [capver]` | Tailscale / Headscale の制御プレーンに接続 |

## WiFi の接続先

`Settings → WiFi` に保存済みが並ぶ（**最大 5 件**、`*` が今つながっているもの）。
選ぶと `接続` / `削除`、`Create new wifi setting` でスキャンして足す。

- **SSID はスキャンから選ぶ。** 打つのはパスワードだけ（画面キーボードはフリック主体なので、
  SSID を 1 文字ずつ打つ UI は実用に耐えない）。オープンな AP はパスワードも聞かない
- **隠し SSID は `SSID を手入力`** から。スキャン結果には出ない
- **今つながっている設定を消すと実際に切断する。** 消したのに繋がったままだと一覧が嘘になる
- 5 件埋まっているときは**パスワードを聞く前に断る**（打った後で捨てない）
- **パスワードは純正キーボード (A164) かコンソールから打つ。** 画面キーボードは
  かな（フリック）専用で ASCII 配列を持っていない（`abc` キーはかな漢字変換を
  止めるだけで、キーの面は変わらない）
- 認証方式は入力させない。`threshold.authmode` はパスワードの有無だけで決めていて、
  実際の方式は esp_wifi が AP に合わせる
- 保存先は **NVS**。SD の `profiles.json` には混ぜない — SD を読む前に繋ぎたいし、
  SD は抜けば誰でも読めるのでパスワードの置き場として弱い
- 起動時は**前回つながったもの**に繋ぐ。繋がらない場所へ移ったら一覧から選ぶ
  （総当たりはしない）

## 接続先（SSH / VPN）の設定

接続先と鍵は **NVS に保存する**。SD カードは PC で書いた設定を**取り込むための入力**で、
取り込んだ後は挿していなくてよい。メニューの `SSH` / `VPN` に一覧が出て、選んでから繋ぐ。

```sh
# SD に置いて（PC 側で）
#   /sdcard/tab5/profiles.json  （NVS に置くので 8KB まで / SSH 5 件・VPN 5 件）
#   /sdcard/tab5/keys/          鍵ファイル（SSH の PEM / WireGuard の秘密鍵 / authkey）
# 端末で取り込む
tab5> profiles import
取り込んだ: 7 件、鍵 3 本（SD は抜いてよい）

tab5> profiles          # NVS の中身を見る
tab5> profiles clear    # 取り込んだものを全部消す
```

`docs/profiles.example.json` をコピーして書き換える。

- **`name` が一覧に出る名前で、`via` の参照先でもある。** 重複したら取り込みごと失敗する
  （どちらに繋がったのか分からないのが一番困るため）
- **鍵と authkey は JSON に埋めない。** `keys/` 配下の**ファイル名だけ**を書く
  （ディレクトリを含む名前は拒否する）。**NVS のキー名の都合で 13 文字まで**
- パスワードは書けるが既定にしない。`"auth": "password"` で `password` を書かなければ、
  繋ぐときに画面から入力させる（入力中はエコーしない）
- 鍵の形式の制約は `sshkey` パーティションと同じ（**PEM のみ。OpenSSH 形式と ed25519 は不可**。
  ECDSA は named curve）
- 未知のキーは黙って無視、未知の `type`・必須項目の欠け・**上限超え**・**鍵の取り込み失敗**は
  **その項目だけ**飛ばす（理由が `profiles import` と画面に出る）
- **上限は SSH 5 件 / VPN 5 件**（別枠で数える。`via` は SSH 1 件が VPN 1 件を要求するので、
  合計で数えるとすぐ埋まる）。6 件目を書いても 5 件目までは使える
- **`profiles import` は毎回「消してから書く」。** 鍵の名前を変えたり接続先を消したりしても、
  古い秘密鍵が NVS に residue として残らない

> **NVS はフラッシュを吸えば読める。** SD より強いのは「抜き差しできない」ぶんだけで、
> 秘密鍵を置いている以上、端末を失ったら失効させること。
> 持ち歩く端末には用途を限った専用鍵を置く。

WireGuard は netif のアドレス 1 本ぶんしか経路を持てないので、`allowed_ips` は
**先頭の 1 本だけ**をネットマスクとして使う。`peer.endpoint` は名前を引けないので
リテラルの IPv4 で書く。

## タッチの割り当て

| 操作 | 動作 |
|---|---|
| ステータスバーをタップ | メニューの開閉 |
| 端末領域を縦にスワイプ | スクロールバック（下に引くと過去へ） |
| **端末領域をダブルタップ** | 画面キーボードの表示・非表示（消すと端末が 15 行 → 29 行） |

ダブルタップはスクロールバックのスワイプと同じ領域にあるので、**次のどれかに当たると
数えない**: 24px 以上動く / 400ms 以上触れる / スクロールバックが動いた /
キーボードの帯（キーの無い余白と候補表示を含む）で始まるか終わる。
2 回目は **1 回目から 400ms 以内・24px 以内**に来る必要がある。
判定は `main/tap_gesture.hpp` にあり、ホストテスト付き。
選んだ表示状態はメニューを開閉しても保たれる。

## 純正キーボードのキー割り当て

| キー | 動作 |
|---|---|
| **Ctrl+Alt+M** | メニューを開く／閉じる（端末からもメニューからも往復できる） |
| Esc / ← | メニューの中で 1 段戻る。最上位なら端末へ戻る |
| ↑↓ / Enter | メニューの項目の移動と決定 |

**端末に送りたいキーは潰していない。** Esc も Ctrl+C も Ctrl+B（tmux の prefix）も
そのままリモートへ届く。潰しているのは Ctrl+Alt+M だけで、vim も tmux も既定では使わない。
キーボードを外しているときはステータスバーのタップでメニューを開く。

## テスト

実機が要らないものはホストでテストする。CI（GitHub Actions）で ASan + UBSan 付きで回している。

```sh
brew install mbedtls@3 cjson     # mbedtls は 3.x（4.x は API が合わない）

M3=/opt/homebrew/opt/mbedtls@3
c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined \
    -o /tmp/t components/vt100/test_vt100.cpp components/vt100/vt100.cpp && /tmp/t
```

| 対象 | チェック数 |
|---|---|
| VT100 エミュレータ | 172 |
| ローマ字・SKK 辞書 | 55 |
| IME 状態機械・フリック入力 | 98 |
| WireGuard 暗号（BLAKE2s / Noise IK / トランスポート） | 181 |
| NaCl crypto_box（XSalsa20-Poly1305） | 39 |
| DISCO メッセージ・レスポンダ | 108 |
| ts2021 Noise | 252 |
| HTTP/2 クライアント | 57 |
| ts2021 制御プレーン | 70 |
| netmap パーサ | 45 |

## 開発フロー

issue → branch → `/code-review` → PR → **実機での Reality Check** → squash merge。
詳細と、実機で踏んだ罠（PSRAM 200MHz が必須、lwIP のスレッド制約、mbedTLS の鍵形式など）は
[CLAUDE.md](CLAUDE.md) にまとめてある。

## 開発環境

- ESP-IDF v5.5.1 (esp32p4)
- 描画は M5GFX 単体（LVGL は使わない）
- Tailscale の検証はローカル Headscale（Docker）を相手にする
