#include <cstdint>
#include "ssh.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
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

// NVS のキー名は 15 文字までなので、ホスト名そのままでは入らない。
// 先頭 11 文字で切ると 192.168.0.101 と 192.168.0.102 が同じ枠を共有し、
// 別のホストが「鍵が変わった」として理由なく拒否される。ハッシュして
// 12 桁の 16 進にする（fail-closed だが誤検知は困る）。
//
// **ポートも含める。** 同じホストの別ポートに別のサーバを置くのは普通なので、
// host だけでハッシュすると今度はそこで衝突する。
// 固定長にすると長いホスト名が黙って切られて、先頭が同じホスト同士でまた
// 衝突するので std::string にして切らない。
std::string host_ident(const char* host, uint16_t port)
{
    return std::string(host) + ":" + std::to_string(port);
}

void host_key_name(const std::string& ident, char out[16])
{
    uint8_t digest[32] = {};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(ident.data()), ident.size(), digest, 0);
    std::snprintf(out, 16, "hk_%02x%02x%02x%02x%02x%02x", digest[0], digest[1], digest[2],
                  digest[3], digest[4], digest[5]);
}

// ホスト鍵の TOFU 検証。初回は覚え、変わったら拒否する。
bool verify_host_key(LIBSSH2_SESSION* session, const char* host, uint16_t port)
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
    const std::string ident = host_ident(host, port);
    char              key_name[16];
    host_key_name(ident, key_name);

    uint8_t   saved[32];
    size_t    saved_len = sizeof(saved);
    esp_err_t err       = nvs_get_blob(nvs, key_name, saved, &saved_len);
    bool      ok        = true;
    if (err == ESP_OK && saved_len == sizeof(digest)) {
        if (std::memcmp(saved, digest, sizeof(digest)) != 0) {
            set_error("HOST KEY CHANGED for %s - refusing to connect", ident.c_str());
            ok = false;
        }
    } else {
        nvs_set_blob(nvs, key_name, digest, sizeof(digest));
        nvs_commit(nvs);
        char hex[65] = {};
        for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
        ESP_LOGW(TAG, "new host key for %s (sha256:%s) - remembered", ident.c_str(), hex);
    }
    nvs_close(nvs);
    return ok;
}

// 秘密鍵 PEM の後ろに `.pub` の行が続いていれば切り分ける (#75)。
//
// **PEM の終端で切る。** 行数や「空行が区切り」では切らない — PEM の本体には
// 改行しか無く、`ssh-keygen -y` が吐く公開鍵は 1 行なので、終端マーカーが
// 唯一の確実な境目になる。
//
// 公開鍵が無い鍵（今までの `sshkey` パーティション）では pub が空になり、
// 呼び出し側が nullptr を渡す = 従来どおりの動作。
void split_key_blob(const std::string& blob, std::string* priv, std::string* pub)
{
    priv->assign(blob);
    pub->clear();
    const size_t end = blob.rfind("-----END ");
    if (end == std::string::npos) return;
    const size_t eol = blob.find('\n', end);
    if (eol == std::string::npos) return;

    std::string rest = blob.substr(eol + 1);
    // 前後の空白を落とす。改行だけが残っている（= 公開鍵は無い）のが普通。
    const size_t b = rest.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return;
    const size_t e = rest.find_last_not_of(" \t\r\n");
    rest = rest.substr(b, e - b + 1);
    // **1 行目だけを取る。** `memory_read_publickey` は「最初の空白の次から次の空白まで」を
    // base64 として読むので、2 行目が続いていると `AAAA\nssh-rsa` を食わせることになり、
    // `Invalid key data, not base64 encoded` で鍵認証ごと落ちる。
    rest = rest.substr(0, rest.find('\n'));
    // 行末の CR も落とす（CRLF で書かれた鍵）。
    while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
    // **`.pub` の行に見えるものだけ受ける。** ここに PEM の続きや案内文が
    // 入っていると、libssh2 に渡した時点で鍵ごと弾かれる。
    if (rest.compare(0, 4, "ssh-") != 0 && rest.compare(0, 6, "ecdsa-") != 0) return;

    priv->assign(blob, 0, eol + 1);
    pub->assign(rest);
}

// 鍵のパースに要る乱数源。EC は座標ブラインディングで f_rng を必須にしている。
// 初回だけ種をまく。失敗したら nullptr（呼び出し側は PEM のまま進む）。
mbedtls_ctr_drbg_context* ssh_drbg()
{
    static mbedtls_ctr_drbg_context ctr;
    static mbedtls_entropy_context  ent;
    static bool                     ready = false;
    static bool                     tried = false;
    if (!tried) {
        tried = true;
        mbedtls_ctr_drbg_init(&ctr);
        mbedtls_entropy_init(&ent);
        static const char* pers = "ssh-key";
        ready = (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent,
                                       reinterpret_cast<const unsigned char*>(pers),
                                       std::strlen(pers)) == 0);
    }
    return ready ? &ctr : nullptr;
}

// EC の秘密鍵を PEM から DER に詰め直す (#75)。詰め替えたら true。
//
// **libssh2 の ECDSA frommemory 経路が PEM を受け取れない。**
// `_libssh2_mbedtls_ecdsa_new_private_frommemory` (mbedtls.c:1335) は
// `data_len + 1` を `LIBSSH2_ALLOC`（= malloc）して `data_len` バイトしか埋めず、
// `data_len + 1` で `mbedtls_pk_parse_key` に渡す。mbedTLS は
// 「`key[keylen-1] != '\0'` なら PEM ではない」と判断するので、**未初期化の
// 1 バイト**で結果が決まる（実機では常に 0x4f が入り、毎回 `-0x3d00` で失敗した）。
// **同じファイルの RSA 版は `mbedtls_calloc` を使っていて無事**なので、
// 誰かが RSA だけ直して ECDSA を直し忘れている。上流の master では修正済み
// (f7fa81ca5956) だが、リリースにも ESP のコンポーネントにも入っていない。
//
// **DER なら ASN.1 の SEQUENCE 長から終端を引き直すので、その 1 バイトを読まない。**
// 渡すバッファは libssh2 が確保するので、呼び出し側から末尾バイトは触れない。
// これが「ライブラリに手を入れずに済む唯一の経路」。
//
// **RSA は触らない。** libssh2 側が無事で、PEM のまま動いているものを変える理由がない。
bool ec_pem_to_der(const std::string& pem, const std::string& passphrase, std::string* der)
{
    // EC でなければ何もしない（安い先読み。パースしてから判定すると RSA でも毎回
    // 鍵を展開することになる）。
    if (pem.find("EC PRIVATE KEY") == std::string::npos &&
        pem.find("BEGIN PRIVATE KEY") == std::string::npos) {
        return false;
    }
    // mbedTLS は鍵データの末尾が NUL であることを要求する。std::string の data() は
    // data()[size()] が '\0' なので、長さに +1 して渡せばよい。
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_context* drbg = ssh_drbg();
    if (!drbg ||
        mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(pem.data()),
                             pem.size() + 1,
                             passphrase.empty() ? nullptr
                                                : reinterpret_cast<const unsigned char*>(
                                                      passphrase.data()),
                             passphrase.size(), mbedtls_ctr_drbg_random, drbg) != 0) {
        // **ここで黙って返ってはいけない。** EC の鍵だと分かっているのに展開できないのは
        // 「曲線パラメータが展開形式」か「パスフレーズが違う」。PEM のまま進むと
        // libssh2 の未初期化 1 バイト経路に落ちて `-0x3d00` になり、
        // **CLAUDE.md が「この番号を曲線形式のせいだと決めつけるな」と書いた罠を自分で踏む**。
        ESP_LOGW(TAG, "EC key: cannot parse (named curve でないか、パスフレーズが違う)");
        mbedtls_pk_free(&pk);
        return false;
    }
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY) {
        mbedtls_pk_free(&pk);
        return false;
    }
    // mbedtls_pk_write_key_der は**バッファの末尾から前向きに**書き、長さを返す。
    unsigned char buf[1024];
    const int     len = mbedtls_pk_write_key_der(&pk, buf, sizeof(buf));
    mbedtls_pk_free(&pk);
    if (len > 0) {
        der->assign(reinterpret_cast<const char*>(buf) + sizeof(buf) - len,
                    static_cast<size_t>(len));
    } else {
        ESP_LOGW(TAG, "EC key: cannot re-encode to DER (%d)", len);
    }
    // **平文の秘密鍵をスタックに残さない。** この領域は戻った先で libssh2 の
    // 送受信バッファに使い回される。
    mbedtls_platform_zeroize(buf, sizeof(buf));
    return len > 0;
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
    // 接続先が鍵を持っていればそれを使う（SD の profiles.json の `key`）。
    // 無ければ従来どおり sshkey パーティション。
    const std::string key = cfg.key_pem.empty() ? load_private_key() : cfg.key_pem;
    if (!key.empty()) {
        // **公開鍵が続けて書いてあれば渡す (#75)。** 渡さないと libssh2 は秘密鍵から
        // 公開鍵を導出する経路に入るが、mbedTLS バックエンドのその実装
        // (`_libssh2_mbedtls_pub_priv_key`) は **RSA 決め打ち**で、ECDSA を
        // "Key type not supported" で弾く（`LIBSSH2_ECDSA` は 1 で、署名側の
        // `_libssh2_mbedtls_ecdsa_sign` は実装されているのに、ここだけが塞いでいる）。
        // 渡せば導出を飛ばし、種別は公開鍵の側から決まる。
        //
        // 無ければ今までどおり nullptr。RSA はそれで動いているので壊さない。
        std::string priv, pub;
        split_key_blob(key, &priv, &pub);
        // **EC だけ DER に詰め替える (#75)。** 理由は ec_pem_to_der のコメント。
        std::string der;
        if (ec_pem_to_der(priv, cfg.password, &der)) {
            ESP_LOGI(TAG, "EC key: re-encoded PEM -> DER (%d bytes) for libssh2", (int)der.size());
            priv = der;
        }
        // パスフレーズ付きの鍵なら cfg.password をパスフレーズとして使う。
        const int rc = libssh2_userauth_publickey_frommemory(
            session, cfg.user.c_str(), cfg.user.size(), pub.empty() ? nullptr : pub.data(),
            pub.size(), priv.data(), priv.size(),
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
    if (key.empty()) set_error("no private key (sshkey partition / profile key) and no password given");
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
        if (!verify_host_key(session, s_cfg.host.c_str(), s_cfg.port)) break;

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

// 秘密鍵 PEM と、続けて書かれた `.pub` を切り分ける (#75)。切り方は上の
// split_key_blob にしかない（`keytest` が同じ関数を見るようにするための口）。
void ssh_key_split(const std::string& blob, std::string* priv, std::string* pub)
{
    split_key_blob(blob, priv, pub);
}

// EC の鍵を本番と同じ手順で DER に詰め直せるか確かめる口 (#75)。
// **`keytest` が本番経路を通るためにある** — 生 PEM を mbedTLS に通すだけでは
// 「parse ok なのに繋ぐと落ちる」を取りこぼす。
bool ssh_key_ec_to_der(const std::string& pem, const std::string& passphrase, std::string* der)
{
    return ec_pem_to_der(pem, passphrase, der);
}

// 鍵の切り分けの自己テスト (#75)。**切り方を間違えると「鍵が壊れている」と
// しか見えない**（libssh2 は秘密鍵ごと弾く）ので、合成入力で固めておく。
// `keytest` から呼ぶ。失敗したら最初に落ちたケースを detail に入れる。
bool ssh_key_split_selftest(std::string* detail)
{
    const std::string pem = "-----BEGIN EC PRIVATE KEY-----\nMHcC\n-----END EC PRIVATE KEY-----\n";
    const std::string pub = "ecdsa-sha2-nistp256 AAAAE2Vj tab5";
    struct Case {
        const char* what;
        std::string blob;
        std::string want_priv;
        std::string want_pub;
    };
    const Case cases[] = {
        {"公開鍵なし（従来の鍵）", pem, pem, ""},
        {"公開鍵つき", pem + pub + "\n", pem, pub},
        {"公開鍵つき（空行を挟む）", pem + "\n\n" + pub, pem, pub},
        {"ssh-rsa の公開鍵", pem + "ssh-rsa AAAAB3 x", pem, "ssh-rsa AAAAB3 x"},
        // **公開鍵に見えないゴミは渡さない。** 渡すと鍵ごと弾かれる。
        {"末尾がゴミ", pem + "# memo\n", pem + "# memo\n", ""},
        // **2 行目は捨てる。** 続けて書かれていると base64 に改行が混ざって鍵認証ごと落ちる。
        {"公開鍵の後ろに 1 行", pem + pub + "\nssh-rsa BBB\n", pem, pub},
        {"CRLF", pem + pub + "\r\n", pem, pub},
        {"PEM の終端が無い", "garbage", "garbage", ""},
    };
    for (const Case& c : cases) {
        std::string priv, p;
        split_key_blob(c.blob, &priv, &p);
        if (priv == c.want_priv && p == c.want_pub) continue;
        if (detail) *detail = std::string(c.what) + ": priv=" + std::to_string(priv.size()) +
                              "B pub=\"" + p + "\"";
        return false;
    }
    return true;
}

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

esp_err_t ssh_forget_host_key(const char* host, uint16_t port)
{
    if (!host || !*host) return ESP_ERR_INVALID_ARG;
    const std::string ident = host_ident(host, port);
    char              key_name[16];
    host_key_name(ident, key_name);

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_erase_key(nvs, key_name);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) ESP_LOGW(TAG, "forgot the host key for %s", ident.c_str());
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
