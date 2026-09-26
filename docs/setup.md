# 新規セットアップ

まっさらな Tab5 と新品の SD カードから、Tailscale 経由で SSH するまでの手順。
2026-09-26 に、フラッシュを全消去した実機で最初から最後まで通したものだけを書いている。

## 必要なもの

- M5Stack Tab5 と USB Type-C ケーブル（シリアルは `/dev/cu.usbmodem101`）
- microSD カード（128GB で確認）と、Mac に挿すためのカードリーダ
- macOS + ESP-IDF v5.5.1（`source ~/esp/esp-idf/export.sh` が通ること）
- SSH の秘密鍵（ECDSA P-256 か RSA の PEM。作り方は下の「鍵」の節）

以降のコマンドは、`source ~/esp/esp-idf/export.sh` を済ませたシェルで、リポジトリ直下から叩く。
`python` は ESP-IDF の環境の中のものを使う（外の python には `serial` が無い）。

## 1. 実機を全消去する（任意）

使い回しの実機で、前の設定を残したくないときだけやる。

```sh
idf.py -p /dev/cu.usbmodem101 erase-flash
```

- ファーム、NVS（WiFi・接続先・Tailscale の状態）、辞書、SSH 鍵がすべて消える
- **WiFi チップ (ESP32-C6) のフラッシュは消えない。** C6 が覚えている古い AP の設定は、
  ファーム側で使わないようにしてある (#94)。気にしなくてよい

## 2. SD カードを MBR + FAT32 でフォーマットする

**新品の SD カードは、そのままでは使えない。** 64GB 以上のカード (SDXC) は exFAT で
フォーマットされて出荷されるが、ファームは exFAT を読めない（ESP-IDF の FatFs が
`ffconf.h` で `FF_FS_EXFAT 0`）。

Mac に挿して、ディスク番号を確かめる。

```sh
diskutil list external physical
```

**消すディスクを取り違えないこと。** 容量と `external, physical` を見て特定する。
例では `/dev/disk4`（123.8GB）だった。

```sh
diskutil eraseDisk FAT32 TAB5 MBRFormat /dev/disk4
mkdir -p /Volumes/TAB5/tab5/keys
```

- 32GB を超えるカードでも、`diskutil` なら FAT32 で作れる（クラスタ 32KB になった）
- `MBRFormat` を付ける。GPT は読めない（同じく `FF_LBA64 0`。FatFs は GPT を LBA64 有効時にしか扱わない）
- macOS が `._keys` のような `._` ファイルを作るが、ファームは名前を指定して開くので害はない

## 3. ビルドして書き込む

```sh
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

辞書（SKK-JISYO.L をダウンロードして変換する。約 6.2MB）:

```sh
python tools/build_dict.py --out build/dict.bin
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name dict --input build/dict.bin
```

SSH の秘密鍵:

```sh
python $IDF_PATH/components/partition_table/parttool.py --port /dev/cu.usbmodem101 \
    write_partition --partition-name sshkey --input ~/.ssh/id_ecdsa_tab5.pem
```

> zsh では `PT="python .../parttool.py --port ..."; $PT write_partition ...` のように
> 変数に入れて呼ぶと、単語に分割されず `no such file or directory` になる。そのまま書く。

### 鍵

まだ無ければ作る。**ed25519 と OpenSSH 形式は使えない**（libssh2 の mbedTLS バックエンドの制約）。
ECDSA は **openssl で named curve の P-256** を作る（`ssh-keygen -t ecdsa -m PEM` の鍵は読めない）。

```sh
openssl ecparam -name prime256v1 -genkey -noout -out ~/.ssh/id_ecdsa_tab5.pem
chmod 600 ~/.ssh/id_ecdsa_tab5.pem
ssh-keygen -y -f ~/.ssh/id_ecdsa_tab5.pem    # この 1 行を接続先の ~/.ssh/authorized_keys に足す
```

公開鍵は端末が秘密鍵から組み立てるので、秘密鍵だけ書けばよい (#84)。
RSA なら `ssh-keygen -t rsa -b 2048 -m PEM -N '' -f ~/.ssh/id_rsa_tab5`。

**接続先に登録できたかは、Tab5 より先に Mac から確かめる。** Tab5 の
`publickey auth failed ... Username/PublicKey combination invalid` と区別がつかない:

```sh
ssh -i ~/.ssh/id_ecdsa_tab5.pem -o IdentitiesOnly=yes -o BatchMode=yes <user>@<host> true
```

## 4. 起動を確かめる

```sh
python tools/serial_log.py --send "conv nihongo" --send "keytest" --send-delay 8 --seconds 15
```

```
romaji: nihongo -> kana: にほんご
lookup 176 us, 1 candidates: 日本語                  ← 辞書が読めている
公開鍵: 無し -> 秘密鍵から組み立てられる (160 バイト)   ← 鍵が読めている（ECDSA）
  ecdsa-sha2-nistp256 AAAA...                          ← ssh-keygen -y と同じになる
```

- 辞書が無いと `dict not written yet or corrupt` が出る
- 初回は `E (...) ssh: ssh_config_load(577): nvs_open` が 1 秒ごとに出るが、無視してよい。
  NVS が空なだけ（#93）

## 5. WiFi に繋ぐ（画面から）

`MENU → Settings → WiFi → Create new wifi setting` → SSID を選ぶ → パスワード → Enter。

- 端末に緑で `connected to "<SSID>" (192.168.x.x)` と出れば成功。
  パスワードを間違えると 20 秒後に赤で `… に 20 秒繋がらない` と出る
- 画面から打てない環境なら、コンソールで `wifi <ssid> <password>`

## 6. 接続先を SD から取り込む

Mac で SD に `tab5/profiles.json` を書く（書式は `docs/profiles.example.json`）。
鍵を使う接続先は、鍵ファイルを `tab5/keys/` に置いて `"key": "<ファイル名>"` と書く。

Tailscale 本家に対話ログイン（QR で承認）し、tailnet のマシンに SSH する例:

```json
{
  "version": 1,
  "profiles": [
    { "name": "tailscale", "type": "tailscale", "control": "https://controlplane.tailscale.com" },
    { "name": "host1", "type": "ssh", "host": "host1.<tailnet>.ts.net", "user": "<user>",
      "auth": "key", "key": "id_ecdsa_tab5", "via": "tailscale" }
  ]
}
```

- `authkey` を書かないと対話ログインになる
- `via` は種別ではなく **Tailscale の接続先の `name`**
- `host` は MagicDNS の名前（短い名前 `host1` や `100.x.y.z` でもよい）
- 鍵は `tab5/keys/id_ecdsa_tab5` に置く。**名前は 13 文字まで**（NVS のキー名の制約）なので
  `.pem` を外す

Mac から取り出して (`diskutil eject /Volumes/TAB5`)
Tab5 に挿し、**コンソールから**取り込む（メニューには取り込みが無い）。
起動した後に挿してよい — 取り込むときにマウントする。

```sh
python tools/serial_log.py --no-reset --send "profiles import" --seconds 8
```

```
I (...) sd: mounted /sdcard in 55ms: SE128 118040MB
取り込んだ: 2 件、鍵 1 本（SD は抜いてよい）
  [0] tailscale        tailscale  https://controlplane.tailscale.com
  [1] host1            ssh        <user>@host1.<tailnet>.ts.net:22 key=id_ecdsa_tab5 via tailscale
```

- `ldo: The voltage value 0 is out of the recommended range` の警告が出るが、マウントできていれば問題ない
- 取り込んだものは NVS に入る。**SD は抜いてよい**

## 7. Tailscale にログインする

`MENU → VPN → tailscale → 接続`。画面に QR が出るので、スマホで読んで Tailscale のアカウントで承認する。
Esc で中止できる（放っておくと 5 分で諦める）。

```
I (...) boot: auth url: https://login.tailscale.com/a/...
I (...) boot: registered 7 disco peers (total 7)
I (...) boot: tunnel netif up with the node key: 100.70.71.77
```

`ts-status` で `registered=1` と `assigned address` が見えれば登録できている。

- **WireGuard の設定は要らない。** ログの `wg_netif` は Tailscale が内部で張るトンネル
- QR が出ずに `control plane returned a non-200 status` になるのは #96 で直した不具合

## 8. Tailscale 経由で SSH する

`MENU → SSH → host1`。Tailscale に繋がっていなければ先に繋ぎ（初回は手順 7 の QR）、
相手に DISCO ping を打って経路を確かめてから SSH する。**最大で 2 分ほどかかる**。

```
I (...) boot: disco ping -> host1.<tailnet>.ts.net. at 192.168.0.x:41641: sent
I (...) wg_netif: handshake complete (peer index ...)
W (...) ssh: new host key for 100.x.y.z:22 (sha256:...) - remembered
```

tailnet の他のマシンから見えているかは、そちらで確かめられる:

```sh
tailscale ping <Tab5 のホスト名>     # pong ... via 192.168.0.x:41641 なら DISCO が通っている
tailscale status                   # Tab5 が active; direct ... になる
```

## まだできないこと

- **LAN の外の tailnet ピアには繋がらない。** DERP も STUN も実装していないので、
  同じ LAN に居るピアとだけ直接繋がる → #11
