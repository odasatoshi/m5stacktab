#pragma once
// SSH クライアント。libssh2 を 1 本のタスクで回し、受信データはストリームバッファ経由で
// 呼び出し側 (メインループ) に渡す。vt::Terminal を触るタスクを 1 つに保つための構造。
#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_err.h>

struct SshConfig {
    std::string host;
    std::string user;
    std::string password;
    uint16_t    port = 22;
    // 秘密鍵 (PEM)。空なら `sshkey` パーティションの鍵を使う。
    // 接続先 (#49) はここに NVS の keys/ から読んだ鍵を入れる。
    std::string key_pem;
};

// 秘密鍵 PEM と、続けて書かれた `.pub` を切り分ける (#75)。pub は無ければ空。
// **ECDSA は公開鍵を渡さないと認証できない** — libssh2 の mbedTLS バックエンドは
// 秘密鍵からの導出を RSA 決め打ちで実装しているため。
void ssh_key_split(const std::string& blob, std::string* priv, std::string* pub);

// EC の秘密鍵を本番と同じ手順で DER に詰め直す (#75)。EC でなければ false。
// **`keytest` が本番経路を通るためにある。**
bool ssh_key_ec_to_der(const std::string& pem, const std::string& passphrase, std::string* der);

// 鍵の切り分け（秘密鍵 PEM + 続けて書いた .pub）の自己テスト (#75)。
// `keytest` が呼ぶ。合成入力なので実機の鍵には触らない。
bool ssh_key_split_selftest(std::string* detail);

// NVS から接続先を読む / 書く（パスワードも NVS。画面から入力できるまでの手段）。
esp_err_t ssh_config_load(SshConfig& out);

// 覚えているホスト鍵を忘れる（#35）。サーバを作り直して鍵が正当に変わった場合、
// これが無いとそのホストに永久に繋げない（TOFU は「変わったら拒否」しかしない）。
// 見つからなければ ESP_ERR_NOT_FOUND。
esp_err_t ssh_forget_host_key(const char* host, uint16_t port);
esp_err_t ssh_config_save(const SshConfig& cfg);

// 接続してリモートシェルを開く。cols/rows は PTY のサイズ。
esp_err_t ssh_connect(const SshConfig& cfg, int cols, int rows);
void      ssh_disconnect(void);
bool      ssh_is_connected(void);

// キー入力をリモートへ送る。
esp_err_t ssh_send(const void* data, size_t len);

// 受信済みデータを取り出す。戻り値は取り出したバイト数（0 = 今は無い）。
size_t ssh_receive(void* buf, size_t max_len);

// 端末サイズが変わったことを伝える。
esp_err_t ssh_resize(int cols, int rows);

// 直近のエラーメッセージ（UI 表示用）。
const char* ssh_last_error(void);
