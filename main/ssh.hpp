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
};

// NVS から接続先を読む / 書く（パスワードも NVS。画面から入力できるまでの手段）。
esp_err_t ssh_config_load(SshConfig& out);
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
