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
- **M5GFX の `drawChar(uniCode, x, y)` は使えない**。送り幅は正しく返すが、指定した座標に描かない
  （実機で確認: 別の行に重なって出る）。`drawString` は正常なので、セル描画は
  「同じ見た目が続く区間を UTF-8 文字列にまとめて `drawString`」でやる。まとめる分だけ速くもなる
- P4 に WiFi は無い。ESP32-C6 を esp-hosted (SDIO) 経由で使う。**順序が決定的に重要**:
  - C6 の電源は P4 の GPIO ではなく I2C の IO エクスパンダ (PI4IOE5V6408 @0x44) の pin0 にある。
    ここは **`display.init()` (M5GFX の Tab5 初期化) が出力 High にしている**（実機で確認）。
    つまり自分で叩く必要はないが、**必ず display.init() を先に呼ぶ**こと
  - esp_hosted は既定で `__attribute__((constructor))` により **app_main より前に** SDIO を
    叩き始める。C6 の電源が入る前に列挙が失敗し (`send_op_cond returned 0x107`)、
    以後リセットもかからず永久に失敗する。
    `CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n` にして、
    「display.init() → esp_hosted_init() → connect_to_slave() → esp_wifi_init()」の順を自分で作る
  - GPIO31/32 の I2C は M5GFX が `I2C_NUM_1` で握っている。別のポートで同じピンに
    `i2c_new_master_bus` すると GPIO マトリクスの出力選択を奪い合うので、
    このピンを触りたいときは M5GFX 側の I2C を使う
- `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM` は有効にしない。TX mempool の 1600B ストライドが
  128B キャッシュラインと合わず CMD53 がアライメント検査で弾かれる

## lwIP を触るときの制約

`CONFIG_LWIP_TCPIP_CORE_LOCKING` は無効なので、次を守る必要がある。

- **`netif->output` の中で `sendto()` を呼んではいけない**。output は tcpip スレッドの上で
  動くので、`sendto` → `tcpip_send_msg_wait_sem` で tcpip スレッド自身がセマフォを待ち、
  **TCP/IP スタック全体がデッドロックする**（WiFi も SSH も止まる）。
  パケットはキューに渡して別タスクで送る
- **tcpip タスクのスタックは 3072B しかない**（`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE`）。
  output の中で KB 単位のローカル変数を取ると溢れる
- **`netif_add` / `netif_remove` は `tcpip_callback` 経由で呼ぶ**。他スレッドから
  `netif_list` を触ると、走査中のリストを壊す（WiFi 側は esp_netif が内部でやっている）
- `tcpip_input` は tcpip スレッドにキューするだけなので、どのタスクから呼んでもよい

## SSH の鍵

秘密鍵は `sshkey` パーティションに置く（NVS の blob 長制限と base64 経由を避けるため）。

```sh
ssh-keygen -t rsa -b 2048 -m PEM -N '' -f ~/.ssh/id_rsa_tab5
cat ~/.ssh/id_rsa_tab5.pub >> ~/.ssh/authorized_keys   # 接続先で
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name sshkey --input ~/.ssh/id_rsa_tab5
```

**鍵の形式に制約がある**（libssh2 の mbedTLS バックエンド）:

- **ed25519 は使えない**（`LIBSSH2_ED25519 = 0`）
- **OpenSSH 形式 (`-----BEGIN OPENSSH PRIVATE KEY-----`) は使えない**。mbedTLS は PKCS#1 / SEC1 の
  PEM しか解釈しないので `ssh-keygen -m PEM` が必須
- **RSA (PEM) は動作確認済み**
- **ECDSA は「named curve」形式でないと通らない**。`ssh-keygen -t ecdsa -m PEM` が作る鍵は
  曲線パラメータを**明示的に展開**した形（ASN.1 に `prime-field` から全部入る）で、
  mbedTLS は named curve（OID）しか解釈できず `-0x3d00 (PK - Invalid key tag or value)` になる。
  openssl で作るか変換すれば通る:

```sh
openssl ecparam -name prime256v1 -genkey -noout -out key.pem          # 新規作成
openssl ec -in ssh_key -out key.pem -param_enc named_curve            # 既存を変換
```

鍵のパースだけを確かめたいときは実機の `keytest` コマンドを使う。
`CONFIG_MBEDTLS_ERROR_C=y` を入れてあるので、失敗理由が文字列で出る。

## ホストテストで使う mbedTLS

WireGuard の暗号 (`components/wg`) はホストでもテストする。ホスト側は **mbedTLS 3.x が必要**
（`brew install mbedtls@3`）。既定の `mbedtls` は 4.x で ChaCha20-Poly1305 が private API に
移動しており、ESP-IDF (3.x) と API が合わない。

```sh
M3=/opt/homebrew/opt/mbedtls@3
c++ -std=c++17 -Wall -Wextra -Werror -O1 -I$M3/include -I components/wg \
    -o /tmp/test_noise components/wg/test_noise.cpp components/wg/noise.cpp \
    components/wg/blake2s.cpp components/wg/crypto_mbedtls.cpp components/wg/transport.cpp \
    -L$M3/lib -lmbedcrypto && /tmp/test_noise
```

## 暗号まわりのハマりどころ

- **X25519 は mbedTLS の `mbedtls_ecp_mul` では自動でクランプされない**。RFC 7748 のとおり
  `k[0] &= 248; k[31] &= 127; k[31] |= 64` を自分でやる
- **Montgomery カーブの `mbedtls_ecp_mul` は `f_rng` が必須**（座標ブラインディング）。NULL だと失敗する
- **X25519 は 1 回 72ms かかる**（実測）。P4 のハードウェア ECC は SECP192R1 / SECP256R1 のみ
  対応で、Curve25519 はソフト実装に落ちるため。`MBEDTLS_ECP_FIXED_POINT_OPTIM` は効かない。
  radix 2^16 の専用実装（TweetNaCl 方式）を自作して測ったが 98ms でかえって遅かった
  （32bit CPU では 64bit 乗算 256 回が重い）。頻度が低い（rekey は 120 秒ごと、
  DISCO の共有鍵はピアごとに 1 回）ので現状で足りている → #19
- **X25519 は 1 回で 10KB 近くスタックを使う**。既定 4KB では即スタック保護フォルト。
  コンソールタスクは 32KB（鍵導出と netif 初期化が重なる経路があるため 16KB でも足りなかった）、
  WireGuard の受信タスクも 16KB 必要。受信バッファは static にしてスタックから外す

## Tailscale / Headscale の開発環境

制御プレーンは **ローカルの Headscale** を相手に開発する。SaaS だと「なぜ弾かれたか」が
分からないため（プロトコル自体は SaaS と 1 バイトも変わらない）。

```sh
docker run -d --name headscale-tab5 -p 8080:8080 \
  -v <config>:/etc/headscale -v <data>:/var/lib/headscale \
  headscale/headscale:latest serve
docker exec headscale-tab5 headscale users create tab5
docker exec headscale-tab5 headscale preauthkeys create --user 1 --reusable --expiration 720h
```

- `dns.override_local_dns: false` と `dns.nameservers.global` を設定しないと起動しない
- **Rancher Desktop のポートフォワードは localhost 限定**なので、実機から届かない。
  `tools/` には置いていないが `0.0.0.0` で待って `127.0.0.1` に中継するだけの
  TCP プロキシを挟むと通る
- 実機・ホストの IP は変わるので、接続先は毎回確認する

## ホストテストの依存

```sh
brew install mbedtls@3 cjson   # mbedtls は 3.x を使う（4.x は API が合わない）
```

CI (Ubuntu) では `libmbedtls-dev` と `libcjson-dev`。

## コード方針

- 過剰な抽象化を作らない。実装が 1 つしかない interface、使われない config は書かない
- 非自明なロジックには最小の自己テストを残す
- 意図的な手抜きは `ponytail:` コメントで天井と昇格条件を明記
