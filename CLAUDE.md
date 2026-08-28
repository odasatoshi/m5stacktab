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

画面の証跡は実機の `screencap` + `tools/screencap.py` で PNG にできる
（フレームバッファを読むので、写真が撮れない環境でも「画面キャプチャ」の要件を満たせる）。
**PNG は `docs/screens/` にコミットして PR から参照する** — `gh` は PR 本文に画像を
上げられないので、リポジトリに置かないと「添付した」がチェックだけになる。

## ハード由来のハマりどころ

- **PSRAM は 200MHz が必須**。ESP32-P4 のデフォルトは 20MHz で、1280x720 の MIPI-DSI が
  フレームバッファを PSRAM から読み切れず `lcd.dsi.dpi: ... underrun happens` が出続ける。
  `CONFIG_SPIRAM_SPEED_200M` は `CONFIG_IDF_EXPERIMENTAL_FEATURES` に依存するので、
  依存も一緒に有効にしないと黙って 20MHz に落ちる
- **L2 キャッシュを増やしてはいけない**。`SRAM_HIGH_SIZE = 0x80000 - CONFIG_CACHE_L2_CACHE_SIZE`
  なので、512KB にすると内蔵 SRAM の上半分 384KB が消える（内蔵ヒープ 567KB → 178KB）。
  underrun 対策には効かない（DPI は DMA で PSRAM を直読みする）。既定の 128KB のままにする
- パネルはネイティブ縦 (720x1280)。**回転は PPA (ハードウェア) に任せる**。
  M5GFX の `setRotation(1)` はピクセル単位で座標変換するので 4.6 倍遅い
  （実測: 1280x24 の転送が pushSprite 4247us、PPA 924us）。
  そのため座標変換は `components/rotate` で自分で持ち、PPA でフレームバッファへ直接書く。
  実機の `rottest` で M5GFX の rotation 1 と向きが一致することを照合できる
  （ピクセルを読み戻して自動判定する。目視の「白点が円の中にあれば ok」では
  **180 度ずれに気づけなかった**。四隅が対角の色を読んでいても円の中には見える）。
  ただし **`rottest` は写像だけを見る。PPA の角度は見ていない**。
  角度は `ppatest`、**本番の描画経路は `termcheck`** で確かめる。
  `termcheck` は vt100 に色つきセルを書いてレンダラを通し、画素を読み戻す。
  push_row_ppa の角度がずれても rottest と ppatest は緑のまま通るので、
  実際の症状を捕まえられるのは `termcheck` だけ（実機で実証済み）。
  PPA の角度の定数は `TermRenderer::ppa_rotation_angle()` にしか置かない
  （複製すると片方が取り残される。実際に ppatest が取り残された）
- **`rot` の変換と PPA の `rotation_angle` は必ず対で直す**。PPA の角度は反時計回りなので、
  `landscape_to_native` の時計回り 90 度に対応するのは `PPA_SRM_ROTATION_ANGLE_270`。
  片方だけ変えるとブロックの位置は合うのに中身が 180 度回る。
  そして端末は PPA、キーボードは M5GFX の rotation 1 で描くので、食い違うと
  「端末とキーボードの天地が逆」になる（実機で発生）。
  M5GFX の `getTouch` も rotation 1 の座標で返すため、端末領域のタッチも一緒にずれる
- **`Panel_DSI::config_detail().buffer_length` は 0 が入っている**（M5GFX が埋めていない）。
  PPA に渡す `out.buffer_size` は自分で計算する（0 だと `ESP_ERR_INVALID_ARG`）
- PPA の入力は DMA するので **64B 境界の内蔵 RAM** に置く（`heap_caps_aligned_alloc`）
- **スプライトの色深度に `16` を渡すと M5GFX では swap565 になる**（メモリ上 byte0 = `RRRRRGGG`）。
  PPA やパネルはネイティブ LE の RGB565 を期待するので、生バイトを渡すと色が入れ替わる
  （赤が暗い青になる。**白と黒はスワップ不変なので通常のテキストでは気づけない**）。
  `lgfx::v1::color_depth_t::rgb565_nonswapped` を明示する
- **PPA は転送のたびに出力側のキャッシュを無効化する**（範囲は `pic_w * block_h * 2` で、
  フル行だとフレームバッファ全体）。M5GFX がキャッシュ経由で描いた内容と競合するので、
  **画面に触る経路は全部同じロックで守る**（端末の描画とキーボードの描画を別々に走らせてはいけない）
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
- **純正キーボード (A164) は USB ではなく I2C**。Ext.Port1 (G0=SDA / G1=SCL / INT=G50) に
  STM32 が居て、アドレスは 0x6D。**電源は自分で入れなくてよい** — Ext.Port1 の 5V は
  IO エクスパンダ 0x43 の bit2 (EXT5V_EN) で、M5GFX の Tab5 初期化が OUT_SET に
  `0b01110110` を書いて High にしている。ただし **display.init() より前には触れない**
  - **バスは `I2C_NUM_0`**。M5GFX が I2C_NUM_1 (G31/G32) を握っているので別ポートにする。
    ドライバは足さず `lgfx::i2c`（M5GFX が持っている）をそのまま使う
  - **STRING (Character) モードを使う。** 押したキーの文字がそのまま名前で降ってくるので、
    行列座標や HID キーコードの表を自分で持たなくて済む
    （行列が要るのは押し／離しの両方が要る用途。端末は押しだけで足りる）
  - **長さレジスタ 0x40 は「修飾バイト 1 + 文字列」のバイト数。NUL 終端ではない**
    （'a' で 2、'backspace' で 10）。上流のデモ (`M5Tab5-Keyboard-UserDemo`) は
    `len + 1` バイト読んで長さも `len` にしているので、末尾に 1 バイト余分が付く
  - **Aa (大文字) モードでは特殊キーの名前も大文字で来る** ("ENTER" / "BACKSPACE" / "UP")。
    小文字だけで比べると **Aa を押した後だけ Enter が効かない**という形でしか出ない
    （表は上流ファーム `M5Tab5-Keyboard-Internal-FW` の `key_value_map[5][14]`）
  - 修飾ビットは HID と同じ (**Ctrl=0x01 / Alt=0x04**)。Shift は名前側に既に効いている
  - **読み手は 1 タスクだけにする。** イベントは FIFO なので、2 か所から読むと
    取り合って「打っても出ないことがある」という再現しない形になる
- **キーボードを付けると本体の向きが逆さになる**ので画面を 180 度回す（`flip [0|1]`、
  起動時はキーボードの有無で自動）。**触るのは 4 箇所で、必ず対で直す**
  （片方だけだと位置は合うのに中身が 180 度ずれ、目視では気づけない）:
  1. **PPA の角度**（反時計回り）。通常 270 → 反転は 90（`TermRenderer::ppa_rotation_angle()`）
  2. **`rot::Panel::flipped`**（座標の対応。ホストテストで normal に 180 度回した座標と
     一致することを不変量にしてある）
  3. **M5GFX の `setRotation`**（1 → 3）。描画もタッチもここから来る
  4. **読み戻し**（`screencap` / `rottest` / `ppatest` / `termcheck`）も同じ rotation で。
     **1 に決め打ちすると反転時に四隅が入れ替わって偽の NG になる**
  - **物理的な向きはホストからは観測できない。** 読み戻しの経路が全部同じ rotation を
    通るので、画素は反転前後で同じに見える。**実機を見た人の報告が要る**
- **画面キーボードは段 (Mode) で切り替える** (#65): `なし → ASCII → かな → なし`。
  巡回はステータスバーの `MENU` の隣のボタン（`StatusBar::kKbdW`）。
  ASCII 面は 12 列 x 5 行で、キーは**純正キーボードと同じ「名前」**を返し
  `kbd_key_to_bytes` に通す（送るバイトの表を二重に持たない）
  - **段を変える経路は `set_keyboard_mode()` ひとつ**にする。`KeyboardUi::set_mode()` を
    直に呼ぶと画面だけ変わり、ステータスバーの表示・端末の行数・`s_kbd_face` が
    食い違う（実機で踏んだ: 面は ABC なのに `kbd` は かな に戻る）
  - **面のキーを送った後に描いてはいけない。** 送った先で 1 行入力が確定して段が戻る
    ことがあり（パスワードの Enter）、古い面のキーを新しい画面の上に描く。
    `mode_` を送信の前後で比べて、変わっていたら描かずに返る
  - **面の文字は efontJA に有る字だけを使う。** `⌫` (U+232B) と `⏎` (U+23CE) は
    豆腐 (□) になる（実機で確認。かな面の `⌫` はずっと豆腐だった）。
    矢印 `← ↑ ↓ →` は JIS X 0208 にあるので出る
  - `Shift` と `Ctrl` は**次の 1 打だけ効くラッチ**（指は押しっぱなしにできない）
  - **PAD 段だけは端末に重なる**（行数を削らない）。ここが他の段と違う:
    - `KeyboardUi::height()` は PAD で **0** を返す。`visible()` は true のままなので、
      **配分を張り直すかどうかは visible() ではなく height() で見る**
      （`ABC → PAD` は visible() が変わらないのに 13 行 → 29 行に戻る）
    - **端末を描いたら必ず重ね直す。** `TermRenderer::render()` は変わったセルだけ
      塗るので、下の文字が動いた分だけ PAD に穴が開く。`renderer->render()` を直に
      呼ばず `render_term_raw()` を通す（`last_pixels() == 0` なら重ね直さない）
    - 薄表示は**市松（4 画素に 1 つ透かす）を焼いたスプライトを透過色つきで重ねる**。
      **フレームバッファを読んで平均する方法は使えない** — 差分描画なので読むのは
      「前回薄くした画素」で、重ねるたびに暗くなっていく。市松はべき等
    - **押下の色を塗ってよいのは、スプライト側が不透明な画素だけ**（枠 2px と
      文字の背景箱）。キー全面を塗ると、離して重ね直しても**透かした画素に
      押下色が残る**（市松がべき等なのは「下が端末の画素のまま」の間だけ）
    - **対話ログインの QR は PAD を塗り潰す**（QR は `height()` の分しか避けない）。
      見えないのに当たり判定だけ生きると右下のタップが `Esc` を送って承認待ちを
      中止するので、QR 表示中は PAD にタッチを渡さない
    - **`screencap` の縮小は点サンプリング**なので、市松の PAD は実物よりずっと薄く
      写る（step が偶数だと透過画素だけを拾って**丸ごと消えて見える**）。
      PAD の見た目を判定するときは `screencap 1 <x> <y> <w> <h>` で等倍に吸う
    - 重ね直しは **6〜8ms**（352x136 の透過 pushSprite、実測）。`bench` が出す。
      ponytail: 端末が塗った行と重なるときだけ出せば減らせるが、下端の行は
      たいてい塗られるので効きは薄い
    - **PAD に `Ctrl` 単体を置かない。** 面に文字キーが無いので掛ける先が無い
      （`kbd_key_to_bytes` の Ctrl は 1 文字のキー名にしか効かない）。
      `^C` を 1 キーにして `AsciiKey::send_mod` で修飾を持たせてある
- **`esp_wifi_scan_start` は接続試行と重なると 0 件で返る**（実機で確認）。繋がっている
  ときは問題ない。**保存済みの AP が無い場所で「新規追加」を開くと必ずリトライ中**なので、
  リトライを止めてから探して、探し終わったら戻す（戻さないと、スキャンしただけで
  元の AP に繋ぎ直さなくなる）
- **WiFi の設定は 5 件まで NVS の blob 1 つ**（`wifi/nets`）に持つ (#56)。1 件しか持てなかった
  頃の `ssid` / `pass` からは起動時に自動で引き継ぐ。**画面から消したら実際に切断する**ので、
  `s_reconfiguring` を立ててから `esp_wifi_disconnect` を呼ぶ（立てないと切断イベントで
  再接続が走り、消した AP に繋ぎ直しに行く）
- **WiFi の設定変更と RPC は `wifijob` タスク (8KB) に載せる**。パスワード入力の Enter は
  **kbd タスク (8KB) の上で、しかも `s_term_lock` を握ったまま**返ってくるので、そこから
  `nvs_set_blob` と `esp_wifi_set_config` を呼ぶと、スタックも足りないうえ描画ループが
  TermGuard の 2 秒タイムアウトに落ちる（SSH / VPN を `connect` タスクに逃がしてあるのと同じ理由）
- **`esp_wifi_scan_start(&cfg, true)` は数秒ブロックする。専用タスクに載せる**。
  メインループや kbd タスク (8KB) の上で走らせると、その間画面が固まる。
  esp-hosted の RPC が深いので **4096 では残り 1696 バイトしかなかった**（実測）。8192 にしてある
- **SD カード (SDMMC 4 線, CLK=G43 / CMD=G44 / D0-D3=G39-42) は on-chip LDO の VO4 を
  開けないと一切応答しない**（`sd_pwr_ctrl_new_on_chip_ldo`）。実装は `main/sdcard.cpp`
- **FatFs の長いファイル名は既定で無効**。`CONFIG_FATFS_LFN_NONE` のままだと 8.3 形式しか
  読めず、`profiles.json` はカードに置いてあるのに `stat` が ENOENT を返す
  （「が無い」と表示されて、配線もマウントも疑うことになる）。
  `CONFIG_FATFS_LFN_HEAP` + `CONFIG_FATFS_API_ENCODING_UTF_8` を入れる
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
失敗理由は文字列で出る（`CONFIG_MBEDTLS_ERROR_STRINGS` は ESP-IDF の既定が `y`。`sdkconfig.defaults` に明示してあるのは意図を残すためで、これを消しても出る）。**`CONFIG_MBEDTLS_ERROR_C` という symbol は存在しない** ので、書いても `sdkconfig` 再生成のときに `unknown kconfig symbol` の警告が出るだけ。

## static_assert の発火を確かめるときの罠

**定数を書き換えて戻すときに `mv file.bak file` を使ってはいけない。** `mv` は元の
mtime ごと戻すので、**書き換えたまま作られたオブジェクトのほうが新しくなり、ninja が
再コンパイルを飛ばす**。戻したはずの定数が焼かれていない実行ファイルができる。

実際に踏んだ (#56): `kMaxWifiNets` を 5 → 10 にして static_assert の発火を確かめ、
`mv wifi.hpp.bak wifi.hpp` で戻したところ、`wifi.cpp.obj` だけ 10 のまま残った。
`grep` も `git show` も 5 なのに、**実機では 6 件目が保存できる**という形でしか出ない
（他の .cpp は static_assert でコンパイル自体が失敗したので 5 のまま = 食い違う）。

戻したら **`touch` するか `git checkout -- <file>` を使い、確認の後は `idf.py fullclean`** を挟む。

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
  WireGuard の受信タスクも 16KB 必要。受信バッファは static にしてスタックから外す。
  **UI から VPN を上げる経路も同じ制約にかかる**（kbd タスクもメインループも 8KB しかない）。
  接続は専用のワーカタスク (`connect`, 32KB) に載せる — 直接呼ぶと
  「メニューから VPN を選ぶと落ちる」という形でしか出ない

## Tailscale の対話ログイン (#59)

- **端末側に OAuth は要らない。** 制御プレーンが `AuthURL` を返し、人間が手元のブラウザで
  認証する。端末は URL を QR で見せて、承認されるまで register を投げ直すだけ
- **HTTP/2 のストリーム id は増える奇数**。register を投げ直すのに固定の 1 を使うと
  2 回目が protocol error になる。`take_sid()` で連番にしてあり、**map のストリームも
  同じ連番から取る**（register を N 回投げた後に map が来るので、3 に決め打ちできない）
- QR は `espressif/qrcode` を使う。自前で書かない（生成多項式とマスク選択が要る）。
  **静穏帯 (quiet zone) を 4 モジュール取る**。無いと読めない端末がある
- **Headscale は register を投げ直すたびに別の `AuthURL` を発行する**（SaaS と違う）。
  「URL が変わったら UI に通知する」と書くと 3 秒ごとに発火し、**QR がスキャン中に
  描き変わり、`show_auth_qr()` がメニューを畳むので承認待ちの間メニューが開けない**
  （`menu show` は「menu shown」と返すのに画面は QR のまま、という形でしか出ない）。
  **承認待ちの間は最初の 1 本を保持する**。古い URL も承認に使える（実機で 4.5 分後に確認）
- **QR が読めることは目視で確かめない。** `screencap 1 <x> <y> <w> <h>` で等倍に吸って
  デコーダに通す（macOS なら CoreImage で 15 行。追加インストール不要）。
  ログの URL と一致するかまで見る
- **`ts-status` の `state` は数字だけで読まない。** `ClientStatus::State` に値を挿すと
  過去のログの `state=5` が黙って別の意味になる（`kAuthPending` を挿して実際に起きた）。
  `ts::state_name()` が名前も出す
- **QR を畳む処理を `TermGuard` の中に閉じ込めない。** ロックは 2 秒でタイムアウトするので、
  取れなかった一回で `s_auth_qr_active` が true のまま残り、以後 `render_term()` が
  永久に早期 return する。**ロックが要るのは再描画だけ**

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
- **DERP のエントリが最低 1 つ要る**。無いと `initial DERPMap is empty` で即終了する。
  外に出たくないので `derp.server.enabled: true`（埋め込み DERP）にしておく
- **v0.29.3 は capver 113 以上しか受け付けない**（`/key?v=<capver>` が
  `unsupported client version` を返す）。`ts-login` の既定は 131 なので素通りする
- **config と DB は消えない場所に置く。** scratchpad に置くとセッションをまたいで
  掃除され、**「起動はするが users も nodes も消えている」**という形で出る
- **ポートフォワードが localhost 限定かは毎回確かめる。** Rancher Desktop でも
  `*:8080` で待つことがある（`lsof -nP -iTCP:8080 -sTCP:LISTEN` で見る）。
  LAN の IP に curl が通るなら中継プロキシは要らない
- 実機・ホストの IP は変わるので、接続先は毎回確認する
- **素の Headscale では「スマホで QR を読んでブラウザで承認」を検証できない。**
  `/register/...` が返すのは承認画面ではなく「サーバで `headscale auth register
  --auth-id ... --user ...` を実行しろ」という案内ページで、**OIDC を設定しない限り
  人間がブラウザで完了する経路が無い**。URL が `http://` なのでスマホ側が弾く問題もある
  （URL を決めるのは制御プレーンなので端末側に直す場所は無い）。
  この経路は SaaS か OIDC 付き Headscale でしか通せない

## ホストテストの依存

```sh
brew install mbedtls@3 cjson   # mbedtls は 3.x を使う（4.x は API が合わない）
```

CI (Ubuntu) では `libmbedtls-dev` と `libcjson-dev`。

## コード方針

- 過剰な抽象化を作らない。実装が 1 つしかない interface、使われない config は書かない
- 非自明なロジックには最小の自己テストを残す
- 意図的な手抜きは `ponytail:` コメントで天井と昇格条件を明記
