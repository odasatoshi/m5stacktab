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
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name sshkey --input ~/.ssh/id_rsa_tab5
```

## コンソールコマンド

USB Type-C のシリアルコンソール（`screen /dev/cu.usbmodem101 115200`）で操作する。
非対話で送るなら `python tools/serial_log.py --send "<command>" --send-delay 13 --seconds 30`。

| コマンド | 用途 |
|---|---|
| `wifi <ssid> [password]` | WiFi 設定（NVS に保存して以後自動接続） |
| `wifi-status` | 接続状態 |
| `ssh <user> <host>` | 秘密鍵で SSH 接続（`sshkey` パーティションの鍵を使う） |
| `ssh <user> <host> <password> [port]` | パスワードで SSH 接続 |
| `key <text>` | SSH にキー入力を送る（`\e` = ESC。キーボードが無いときの入力手段） |
| `sshclose` | SSH 切断 |
| `conv <romaji>` | ローマ字→かな→漢字を試す |
| `term <text>` / `termtest` / `termscroll` | 端末描画の確認（`\e` でエスケープを送れる） |
| `bench` | 描画コストの実測（全画面 vs 1 文字） |
| `scroll [lines]` | スクロールバックを動かす |
| `kbd [off]` | 画面キーボードの表示切り替え |
| `wgtest` | WireGuard の暗号とハンドシェイクを実機で検証 |
| `wg <tunnel-ip> [pubkey] [host:port]` | WireGuard トンネルの netif |
| `wg stat` / `wg disco` / `wg down` | 統計・DISCO 状態・停止 |
| `ts <host> <authkey> [port] [capver]` | Tailscale / Headscale の制御プレーンに接続 |

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
