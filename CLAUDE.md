# M5Stack Tab5 Firmware

ESP32-P4 搭載 M5Stack Tab5 用ファームウェア。ESP-IDF (C/C++)。

## 必須機能

1. SSH クライアント（ターミナルコンソール）
2. VPN 接続：L2TP/IPsec と Tailscale
3. 日本語入力（かな漢字変換 IME）

## 開発フロー

**すべての変更は issue → branch → PR を通す。main への直 push は禁止。**

1. **Issue を切る** — 何を作る／直すかを 1 issue = 1 まとまりで。`gh issue create`
2. **ブランチを切る** — `feat/<issue番号>-<短い名前>` / `fix/<issue番号>-...`
3. **実装** — ローカルで `idf.py build` が通るまで
4. **Code review（PR 作成前・必須）** — `/code-review` を実行し、指摘を潰すか PR 本文で理由を明記
5. **PR を出す** — 本文に `Closes #<issue>`。テンプレのチェックリストを埋める
6. **実機 Reality Check（マージ前・必須）** — 下記
7. **マージ** — squash merge

### 実機 Reality Check

実機で動くことの**証跡**が PR にない限りマージしない。ビルドが通ることは動作確認ではない。

- `idf.py -p /dev/cu.usbmodem101 flash monitor` で書き込み・起動
- その PR が触った機能を実際に操作し、シリアルログを PR に貼る
- 画面が絡む変更は写真か画面キャプチャを添付
- クラッシュ・panic・WDT リセットが出ていないこと

判定は Reality Checker エージェントに投げる（デフォルトは NEEDS WORK、証跡が揃って初めて OK）。

## 環境

- ESP-IDF v5.5.1 (esp32p4)
- 実機: `/dev/cu.usbmodem101`（Espressif USB JTAG serial debug unit）
- ホスト: macOS (Apple Silicon)

## ビルドと実機確認

```sh
source ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
python tools/serial_log.py --seconds 20      # ログ採取
```

`idf.py monitor` は標準入力が TTY でないと動かないので、非対話環境では
`tools/serial_log.py` を使う（RTS でリセットしてから指定秒数だけ拾う）。

## ハード由来のハマりどころ

- **PSRAM は 200MHz が必須**。ESP32-P4 のデフォルトは 20MHz で、1280x720 の MIPI-DSI が
  フレームバッファを PSRAM から読み切れず `lcd.dsi.dpi: ... underrun happens` が出続ける。
  `CONFIG_SPIRAM_SPEED_200M` は `CONFIG_IDF_EXPERIMENTAL_FEATURES` に依存するので、
  依存も一緒に有効にしないと黙って 20MHz に落ちる
- **L2 キャッシュを増やしてはいけない**。`SRAM_HIGH_SIZE = 0x80000 - CONFIG_CACHE_L2_CACHE_SIZE`
  なので、512KB にすると内蔵 SRAM の上半分 384KB が消える（内蔵ヒープ 567KB → 178KB）。
  underrun 対策には効かない（DPI は DMA で PSRAM を直読みする）。既定の 128KB のままにする
- パネルはネイティブ縦 (720x1280)。横で使うなら `setRotation(1)`
- P4 に WiFi は無い。ESP32-C6 を esp-hosted (SDIO) 経由で使う

## コード方針

- 過剰な抽象化を作らない。実装が 1 つしかない interface、使われない config は書かない
- 非自明なロジックには最小の自己テストを残す
- 意図的な手抜きは `ponytail:` コメントで天井と昇格条件を明記
