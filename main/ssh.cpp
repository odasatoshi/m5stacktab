#include "ssh.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/sha256.h>
#include <errno.h>
#include <esp_check.h>
#include <esp_partition.h>
#include <nvs.h>

#include <libssh2.h>

namespace {

const char* TAG = "ssh";

constexpr const char* kNvsNamespace = "ssh";
constexpr size_t      kRxBufSize    = 8192;
constexpr size_t      kTxBufSize    = 1024;

StreamBufferHandle_t s_rx     = nullptr;  // リモート → 端末
StreamBufferHandle_t s_tx     = nullptr;  // 端末 → リモート
TaskHandle_t         s_task   = nullptr;
volatile bool        s_run    = false;
volatile bool        s_online = false;
char                 s_error[128] = {};
SshConfig            s_cfg;
int                  s_cols = 80;
int                  s_rows = 24;
volatile bool        s_resize_pending = false;

void set_error(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_error, sizeof(s_error), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", s_error);
}

// ホスト鍵の TOFU 検証。初回は覚え、変わったら拒否する。
bool verify_host_key(LIBSSH2_SESSION* session, const char* host)
{
    size_t      len  = 0;
    int         type = 0;
    const char* key  = libssh2_session_hostkey(session, &len, &type);
    if (!key) {
        set_error("host key unavailable");
        return false;
    }
    uint8_t digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(key), len, digest, 0);

    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) {
        set_error("nvs_open failed for host key");
        return false;
    }
    char key_name[24];
    // NVS のキー名は 15 文字までなので、ホスト名そのままではなく指紋の先頭で識別する。
    std::snprintf(key_name, sizeof(key_name), "hk_%.11s", host);

    uint8_t   saved[32];
    size_t    saved_len = sizeof(saved);
    esp_err_t err       = nvs_get_blob(nvs, key_name, saved, &saved_len);
    bool      ok        = true;
    if (err == ESP_OK && saved_len == sizeof(digest)) {
        if (std::memcmp(saved, digest, sizeof(digest)) != 0) {
            set_error("HOST KEY CHANGED for %s - refusing to connect", host);
            ok = false;
        }
    } else {
        nvs_set_blob(nvs, key_name, digest, sizeof(digest));
        nvs_commit(nvs);
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        ESP_LOGW(TAG, "new host key for %s (sha256:%s) - remembered", host, hex);
    }
    nvs_close(nvs);
    return ok;
}

// 秘密鍵は専用パーティションから読む。NVS の blob 長制限も base64 変換も要らない。
std::string load_private_key()
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "sshkey");
    if (!part) return {};

    std::string buf(std::min<size_t>(part->size, 8192), '\0');
    if (esp_partition_read(part, 0, buf.data(), buf.size()) != ESP_OK) return {};

    // 未書き込み領域は 0xFF。そこから先は捨てる。
    const size_t end = buf.find('\xFF');
    if (end != std::string::npos) buf.resize(end);
    while (!buf.empty() && (buf.back() == '\0' || buf.back() == '\n' || buf.back() == '\r')) {
        buf.pop_back();
    }
    if (buf.find("PRIVATE KEY") == std::string::npos) {
        ESP_LOGW(TAG, "sshkey partition has no PEM key (%d bytes read)", (int)buf.size());
        return {};
    }
    buf += '\n';  // PEM は最終行の改行を期待する実装があるので付けておく
    // 先頭行は鍵の種類が分かるだけで秘密ではないので、切り分けのために出す。
    const size_t nl = buf.find('\n');
    ESP_LOGI(TAG, "private key: %d bytes, header=%.*s", (int)buf.size(),
             (int)(nl == std::string::npos ? 0 : nl), buf.c_str());
    return buf;
}

// 鍵があれば公開鍵認証、なければ (または失敗したら) パスワード認証。
bool authenticate(LIBSSH2_SESSION* session, const SshConfig& cfg)
{
    const std::string key = load_private_key();
    if (!key.empty()) {
        // 公開鍵は渡さない。OpenSSH 形式の秘密鍵には公開鍵が含まれているため libssh2 が導出する。
        // パスフレーズ付きの鍵なら cfg.password をパスフレーズとして使う。
        const int rc = libssh2_userauth_publickey_frommemory(
            session, cfg.user.c_str(), cfg.user.size(), nullptr, 0, key.data(), key.size(),
            cfg.password.empty() ? nullptr : cfg.password.c_str());
        if (rc == 0) {
            ESP_LOGI(TAG, "authenticated with private key (%d bytes)", (int)key.size());
            return true;
        }
        char* msg = nullptr;
        int   msg_len = 0;
        const int last = libssh2_session_last_error(session, &msg, &msg_len, 0);
        ESP_LOGW(TAG, "publickey auth failed: rc=%d last=%d msg=%.*s", rc, last, msg_len,
                 msg ? msg : "");
        // mbedTLS バックエンドは ed25519 非対応 (LIBSSH2_ED25519=0)。ECDSA か RSA の鍵が必要。
        if (key.find("OPENSSH PRIVATE KEY") != std::string::npos) {
            ESP_LOGW(TAG, "note: ed25519 keys are not supported by the mbedTLS backend; "
                          "use ecdsa or rsa");
        }
    }
    if (!cfg.password.empty()) {
        const int rc =
            libssh2_userauth_password(session, cfg.user.c_str(), cfg.password.c_str());
        if (rc == 0) {
            ESP_LOGI(TAG, "authenticated with password");
            return true;
        }
        set_error("authentication failed: %d", rc);
        return false;
    }
    if (key.empty()) set_error("no private key in the sshkey partition and no password given");
    return false;
}

int tcp_connect(const char* host, uint16_t port)
{
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", port);

    addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res     = nullptr;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        set_error("cannot resolve %s", host);
        return -1;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        set_error("socket() failed: %d", errno);
        freeaddrinfo(res);
        return -1;
    }
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        set_error("connect to %s:%u failed: %d", host, port, errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // 対話操作なので遅延を避ける
    return sock;
}

// EAGAIN のときはソケットが読み書き可能になるまで待つ。ビジーループを避ける。
void wait_socket(int sock, LIBSSH2_SESSION* session, int timeout_ms)
{
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    const int dir = libssh2_session_block_directions(session);
    if (!dir || (dir & LIBSSH2_SESSION_BLOCK_INBOUND)) FD_SET(sock, &rd);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(sock, &wr);
    timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    select(sock + 1, &rd, &wr, nullptr, &tv);
}

void ssh_task(void*)
{
    int               sock    = -1;
    LIBSSH2_SESSION*  session = nullptr;
    LIBSSH2_CHANNEL*  channel = nullptr;

    do {
        sock = tcp_connect(s_cfg.host.c_str(), s_cfg.port);
        if (sock < 0) break;

        session = libssh2_session_init();
        if (!session) {
            set_error("libssh2_session_init failed");
            break;
        }
        libssh2_session_set_blocking(session, 1);  // ハンドシェイクまではブロッキングで単純に
        if (int rc = libssh2_session_handshake(session, sock); rc) {
            set_error("handshake failed: %d", rc);
            break;
        }
        if (!verify_host_key(session, s_cfg.host.c_str())) break;

        if (!authenticate(session, s_cfg)) break;
        channel = libssh2_channel_open_session(session);
        if (!channel) {
            set_error("channel open failed");
            break;
        }
        // 端末型は自前の VT100 実装が対応している範囲に合わせる。
        if (int rc = libssh2_channel_request_pty_ex(channel, "xterm-256color", 14, nullptr, 0,
                                                   s_cols, s_rows, 0, 0);
            rc) {
            set_error("pty request failed: %d", rc);
            break;
        }
        if (int rc = libssh2_channel_shell(channel); rc) {
            set_error("shell request failed: %d", rc);
            break;
        }
        libssh2_session_set_blocking(session, 0);  // ここからは送受信を 1 タスクで多重化する
        s_online = true;
        s_error[0] = '\0';
        ESP_LOGI(TAG, "connected to %s@%s:%u (pty %dx%d)", s_cfg.user.c_str(), s_cfg.host.c_str(),
                 s_cfg.port, s_cols, s_rows);

        char buf[1024];
        while (s_run) {
            bool idle = true;

            if (s_resize_pending) {
                s_resize_pending = false;
                libssh2_channel_request_pty_size(channel, s_cols, s_rows);
            }

            // リモート → 端末
            ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
            if (n > 0) {
                idle = false;
                // 端末が詰まっているときは捨てずに待つ（画面が壊れるので取りこぼしは許さない）。
                size_t sent = 0;
                while (sent < static_cast<size_t>(n) && s_run) {
                    sent += xStreamBufferSend(s_rx, buf + sent, n - sent, pdMS_TO_TICKS(100));
                }
            } else if (n == LIBSSH2_ERROR_EAGAIN) {
                // 何もない
            } else if (n < 0) {
                set_error("channel read error: %d", (int)n);
                break;
            }
            if (libssh2_channel_eof(channel)) {
                ESP_LOGI(TAG, "remote closed the channel");
                break;
            }

            // 端末 → リモート
            size_t len = xStreamBufferReceive(s_tx, buf, sizeof(buf), 0);
            if (len > 0) {
                idle       = false;
                size_t off = 0;
                while (off < len && s_run) {
                    ssize_t w = libssh2_channel_write(channel, buf + off, len - off);
                    if (w == LIBSSH2_ERROR_EAGAIN) {
                        wait_socket(sock, session, 50);
                        continue;
                    }
                    if (w < 0) {
                        set_error("channel write error: %d", (int)w);
                        off = len;
                        s_run = false;
                        break;
                    }
                    off += w;
                }
            }

            if (idle) wait_socket(sock, session, 50);
        }
    } while (false);

    s_online = false;
    if (channel) {
        libssh2_channel_close(channel);
        libssh2_channel_free(channel);
    }
    if (session) {
        libssh2_session_disconnect(session, "bye");
        libssh2_session_free(session);
    }
    if (sock >= 0) close(sock);
    ESP_LOGI(TAG, "session finished");
    s_run  = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t ssh_config_load(SshConfig& out)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READONLY, &nvs), TAG, "nvs_open");
    char   buf[128];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(nvs, "host", buf, &len);
    if (err == ESP_OK) {
        out.host = buf;
        len      = sizeof(buf);
        if (nvs_get_str(nvs, "user", buf, &len) == ESP_OK) out.user = buf;
        len = sizeof(buf);
        if (nvs_get_str(nvs, "pass", buf, &len) == ESP_OK) out.password = buf;
        uint16_t port = 22;
        if (nvs_get_u16(nvs, "port", &port) == ESP_OK) out.port = port;
    }
    nvs_close(nvs);
    return err;
}

esp_err_t ssh_config_save(const SshConfig& cfg)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(nvs, "host", cfg.host.c_str());
    if (err == ESP_OK) err = nvs_set_str(nvs, "user", cfg.user.c_str());
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", cfg.password.c_str());
    if (err == ESP_OK) err = nvs_set_u16(nvs, "port", cfg.port);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t ssh_connect(const SshConfig& cfg, int cols, int rows)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

    if (!s_rx) s_rx = xStreamBufferCreate(kRxBufSize, 1);
    if (!s_tx) s_tx = xStreamBufferCreate(kTxBufSize, 1);
    if (!s_rx || !s_tx) {
        set_error("stream buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    xStreamBufferReset(s_rx);
    xStreamBufferReset(s_tx);

    static bool inited = false;
    if (!inited) {
        if (int rc = libssh2_init(0); rc) {
            set_error("libssh2_init failed: %d", rc);
            return ESP_FAIL;
        }
        inited = true;
    }

    s_cfg  = cfg;
    s_cols = cols;
    s_rows = rows;
    s_run  = true;
    // libssh2 + mbedTLS のハンドシェイクはスタックを食うので広く取る。
    if (xTaskCreate(&ssh_task, "ssh", 16384, nullptr, 5, &s_task) != pdPASS) {
        s_run = false;
        set_error("xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ssh_disconnect(void)
{
    s_run = false;
    for (int i = 0; i < 50 && s_task; ++i) vTaskDelay(pdMS_TO_TICKS(20));
}

bool ssh_is_connected(void) { return s_online; }

esp_err_t ssh_send(const void* data, size_t len)
{
    if (!s_online || !s_tx) return ESP_ERR_INVALID_STATE;
    size_t sent = xStreamBufferSend(s_tx, data, len, pdMS_TO_TICKS(100));
    return sent == len ? ESP_OK : ESP_ERR_TIMEOUT;
}

size_t ssh_receive(void* buf, size_t max_len)
{
    if (!s_rx) return 0;
    return xStreamBufferReceive(s_rx, buf, max_len, 0);
}

esp_err_t ssh_resize(int cols, int rows)
{
    s_cols           = cols;
    s_rows           = rows;
    s_resize_pending = true;
    return ESP_OK;
}

const char* ssh_last_error(void) { return s_error; }
