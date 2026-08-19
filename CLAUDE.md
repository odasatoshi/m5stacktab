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
- パネルはネイティブ縦 (720x1280)。横で使うなら `setRotation(1)`。
  **ただし回転すると転送が 3.3 倍遅くなる**（実測: 1280x24 の pushSprite が rot=0 で 1.21ms、
  rot=1 で 3.96ms）。回転時は M5GFX がピクセル単位で座標変換するため。
  全画面書き換えは 111ms（≒9fps）。差分更新なら 1 行 4ms で足りる。
  横向きのまま速くするには ESP32-P4 の PPA (Pixel Processing Accelerator) で回転させる必要がある → #16
- `Panel_DSI` はフレームバッファを `config_detail().buffer` で公開している（`Panel_FrameBufferBase` 派生）
- P4 に WiFi は無い。ESP32-C6 を esp-hosted (SDIO) 経由で使う。ここに罠が 2 つある:
  - **C6 の電源は I2C の IO エクスパンダ (PI4IOE5V6408 @0x44) の pin0**。しかもこのチップは
    既定で全ピン Hi-Z なので、方向を出力にするだけでは電流が出ない。**Hi-Z レジスタ (0x07) の
    該当ビットを 0 にする**必要がある（`esp_io_expander_pi4ioe5v6408` ドライバはこれをやらない）
  - esp_hosted は既定で `__attribute__((constructor))` により **app_main より前に** SDIO を
    叩き始める。C6 の電源が入る前に列挙が失敗し、以後リセットもかからず永久に失敗する。
    `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` にして順序を自分で作る
- `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` は有効にしない。TX mempool の 1600B ストライドが
  128B キャッシュラインと合わず CMD53 がアライメント検査で弾かれる

## コード方針

- 過剰な抽象化を作らない。実装が 1 つしかない interface、使われない config は書かない
- 非自明なロジックには最小の自己テストを残す
- 意図的な手抜きは `ponytail:` コメントで天井と昇格条件を明記
