#include <cstdint>
#include "wg_netif.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>

namespace wg {
namespace {

const char* TAG = "wg_netif";

// Tailscale と同じ 1280。IPv6 の最小 MTU なので経路 MTU の問題を避けやすい。
constexpr uint16_t kTunnelMtu   = 1280;
constexpr size_t   kMaxPacket   = 2048;
constexpr int      kTxQueueLen  = 16;
// WireGuard の既定値。REJECT_AFTER_TIME (180s) より前に鍵を作り直す。
constexpr int64_t  kRekeyAfterUs        = 120 * 1000000LL;
constexpr int64_t  kHandshakeRetryUs    = 5 * 1000000LL;

struct State {
    struct netif      netif{};
    int               sock       = -1;
    sockaddr_in       peer_addr{};
    bool              has_peer   = false;
    Handshake*        hs         = nullptr;
    Transport*        transport  = nullptr;
    SemaphoreHandle_t lock       = nullptr;
    QueueHandle_t     tx_queue   = nullptr;
    TaskHandle_t      rx_task    = nullptr;
    TaskHandle_t      tx_task    = nullptr;
    SemaphoreHandle_t rx_done    = nullptr;
    SemaphoreHandle_t tx_done    = nullptr;
    volatile bool     running    = false;
    uint8_t           static_priv[kKeyLen] = {};
    uint8_t           peer_pub[kKeyLen]    = {};
    uint32_t          local_index          = 0;
    int64_t           last_handshake_us    = 0;
    int64_t           last_initiation_us   = 0;
    int64_t           last_tx_us           = 0;
    uint32_t          keepalive_sec        = 25;
    NetifStats*       stats                = nullptr;
    Netif::TimestampStore ts_store         = nullptr;
    uint64_t          ts_seconds           = 0;  // 最後に送った TAI64N の秒部分
};

State g_state;

// TAI64N のタイムスタンプ。WireGuard はこれでリプレイを弾くので、
// **再起動をまたいで単調増加させる必要がある**（esp_timer は 0 に戻る）。
// 保存できる場合は前回値 +1 秒以上を使う。
void make_timestamp(uint8_t out[kTimestampLen])
{
    uint64_t saved = 0;
    if (g_state.ts_store && g_state.ts_store(&saved, /*write=*/false)) {
        g_state.ts_seconds = saved;
    }
    const uint64_t uptime = static_cast<uint64_t>(esp_timer_get_time() / 1000000);
    uint64_t       secs   = 0x400000000000000aULL + uptime;
    if (secs <= g_state.ts_seconds) secs = g_state.ts_seconds + 1;
    g_state.ts_seconds = secs;
    if (g_state.ts_store) g_state.ts_store(&secs, /*write=*/true);

    const uint32_t nano = static_cast<uint32_t>((esp_timer_get_time() % 1000000) * 1000);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(secs >> (8 * (7 - i)));
    for (int i = 0; i < 4; ++i) out[8 + i] = static_cast<uint8_t>(nano >> (8 * (3 - i)));
}

bool send_to_peer(const uint8_t* data, size_t len)
{
    if (g_state.sock < 0 || !g_state.has_peer) return false;
    const ssize_t n = sendto(g_state.sock, data, len, 0,
                             reinterpret_cast<sockaddr*>(&g_state.peer_addr),
                             sizeof(g_state.peer_addr));
    return n == static_cast<ssize_t>(len);
}

bool start_handshake_locked()
{
    if (!g_state.hs || !g_state.has_peer) return false;
    const auto& c = default_crypto();
    if (!g_state.hs->set_keys(g_state.static_priv, g_state.peer_pub)) return false;

    uint8_t ts[kTimestampLen];
    make_timestamp(ts);
    static uint8_t msg1[148];  // タスクスタックを節約する
    if (!c.random_bytes(reinterpret_cast<uint8_t*>(&g_state.local_index), 4)) return false;
    if (!g_state.hs->create_initiation(msg1, g_state.local_index, ts)) return false;
    if (!send_to_peer(msg1, sizeof(msg1))) return false;
    g_state.last_initiation_us = esp_timer_get_time();
    if (g_state.stats) ++g_state.stats->handshakes;
    ESP_LOGI(TAG, "sent handshake initiation (index %08x)", (unsigned)g_state.local_index);
    return true;
}

// lwIP から呼ばれる送信パス。**tcpip スレッドの上で動く**ので、
// ここで sendto() を呼んではいけない（core locking 無効の構成では
// tcpip スレッド自身がセマフォ待ちに入り、スタック全体がデッドロックする）。
// パケットをキューに渡すだけにして、暗号化と送信は専用タスクでやる。
err_t wg_output(struct netif* nif, struct pbuf* p, const ip4_addr_t* ipaddr)
{
    (void)nif;
    (void)ipaddr;
    if (!g_state.running || !g_state.tx_queue) return ERR_IF;
    if (!g_state.transport || !g_state.transport->ready()) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_CONN;  // まだハンドシェイクが終わっていない
    }
    if (p->tot_len > kTunnelMtu) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_MEM;
    }
    // キューに渡す間だけ参照を足す。解放は送信タスク側。
    pbuf_ref(p);
    if (xQueueSend(g_state.tx_queue, &p, 0) != pdTRUE) {
        pbuf_free(p);
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_MEM;
    }
    return ERR_OK;
}

err_t wg_netif_init(struct netif* nif)
{
    nif->name[0] = 'w';
    nif->name[1] = 'g';
    nif->output  = wg_output;
    // IPv6 は使わない。使うなら output_ip6 も実装しないと ip6_output_if が
    // NULL を呼ぶので、アドレスを付ける前に必ずここを埋めること。
    nif->mtu     = kTunnelMtu;
    // ポイントツーポイントなので ARP もブロードキャストも無い。
    nif->flags   = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

void tx_task(void*)
{
    // tcpip スレッドではなく自分のスタックなので大丈夫だが、
    // 2KB をスタックから外しておく（このタスク専用）。
    static uint8_t plain[kTunnelMtu];
    static uint8_t wire[kMaxPacket];

    while (g_state.running) {
        struct pbuf* p = nullptr;
        if (xQueueReceive(g_state.tx_queue, &p, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (!p) continue;

        const uint16_t len = p->tot_len;
        bool           ok  = (len <= sizeof(plain)) &&
                  (pbuf_copy_partial(p, plain, len, 0) == len);
        pbuf_free(p);
        if (!ok) {
            if (g_state.stats) ++g_state.stats->tx_dropped;
            continue;
        }

        size_t n = 0;
        if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (g_state.transport && g_state.transport->ready()) {
                n = g_state.transport->encrypt(wire, sizeof(wire), plain, len);
            }
            xSemaphoreGive(g_state.lock);
        }
        if (n == 0 || !send_to_peer(wire, n)) {
            if (g_state.stats) ++g_state.stats->tx_dropped;
            continue;
        }
        g_state.last_tx_us = esp_timer_get_time();
        if (g_state.stats) {
            ++g_state.stats->tx_packets;
            g_state.stats->tx_bytes += len;
        }
    }
    if (g_state.tx_done) xSemaphoreGive(g_state.tx_done);
    vTaskDelete(nullptr);
}

void inject_to_lwip(const uint8_t* data, size_t len)
{
    struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<uint16_t>(len), PBUF_POOL);
    if (!p) {
        if (g_state.stats) ++g_state.stats->rx_dropped;
        return;
    }
    if (pbuf_take(p, data, static_cast<uint16_t>(len)) != ERR_OK) {
        pbuf_free(p);
        if (g_state.stats) ++g_state.stats->rx_dropped;
        return;
    }
    // tcpip_input は tcpip スレッドにキューするのでこのタスクから呼んで良い。
    if (tcpip_input(p, &g_state.netif) != ERR_OK) {
        pbuf_free(p);
        if (g_state.stats) ++g_state.stats->rx_dropped;
        return;
    }
    if (g_state.stats) {
        ++g_state.stats->rx_packets;
        g_state.stats->rx_bytes += len;
    }
}

// keepalive と rekey。受信タイムアウトのたびに呼ぶ。
void tick_locked()
{
    if (!g_state.has_peer) return;
    const int64_t now = esp_timer_get_time();

    // 鍵がまだ無い場合は一定間隔で再送する（応答が来ないこともある）。
    if (!g_state.transport || !g_state.transport->ready()) {
        if (now - g_state.last_initiation_us > kHandshakeRetryUs) start_handshake_locked();
        return;
    }
    // WireGuard は 120 秒で鍵を作り直す。放っておくと 180 秒でピアが鍵を捨てて沈黙する。
    if (now - g_state.last_handshake_us > kRekeyAfterUs) {
        if (now - g_state.last_initiation_us > kHandshakeRetryUs) {
            ESP_LOGI(TAG, "rekeying");
            if (g_state.stats) ++g_state.stats->rekeys;
            start_handshake_locked();
        }
        return;
    }
    // 無通信が続くと NAT のマッピングが失効するので、空のパケットを送る。
    if (g_state.keepalive_sec > 0 &&
        now - g_state.last_tx_us > static_cast<int64_t>(g_state.keepalive_sec) * 1000000LL) {
        static uint8_t ka[kTransportHeader + kTagLen];
        const size_t   n = g_state.transport->encrypt(ka, sizeof(ka), nullptr, 0);
        if (n > 0 && send_to_peer(ka, n)) {
            g_state.last_tx_us = now;
            if (g_state.stats) ++g_state.stats->keepalives;
        }
    }
}

void rx_task(void*)
{
    // 4KB 分をスタックから外す。X25519 と同居させると足りない。
    static uint8_t buf[kMaxPacket];
    static uint8_t plain[kMaxPacket];

    while (g_state.running) {
        sockaddr_in from{};
        socklen_t   from_len = sizeof(from);
        const ssize_t n = recvfrom(g_state.sock, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) {
            if (!g_state.running) break;
            // タイムアウト。ここで keepalive と rekey を回す。
            if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                tick_locked();
                xSemaphoreGive(g_state.lock);
            }
            continue;
        }
        const uint8_t type = buf[0];

        if (type == kMsgTransport && n >= static_cast<ssize_t>(kTransportHeader + kTagLen)) {
            bool   valid = false;
            size_t got   = 0;
            if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                if (g_state.transport) {
                    got = g_state.transport->decrypt(plain, sizeof(plain), buf,
                                                     static_cast<size_t>(n), &valid);
                }
                xSemaphoreGive(g_state.lock);
            }
            if (valid && got > 0) {
                inject_to_lwip(plain, got);
            } else if (!valid) {
                if (g_state.stats) ++g_state.stats->rx_dropped;
            }
            continue;
        }

        if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (type == kMsgResponse && n == 92) {
            Keypair kp;
            if (g_state.hs && g_state.hs->consume_response(buf, kp)) {
                g_state.transport->set_keypair(kp);
                g_state.last_handshake_us = esp_timer_get_time();
                g_state.last_tx_us       = 0;  // すぐ keepalive を送って経路を開ける
                ESP_LOGI(TAG, "handshake complete (peer index %08x)", (unsigned)kp.remote_index);
            } else {
                ESP_LOGW(TAG, "handshake response rejected");
            }
        } else if (type == kMsgInitiation) {
            // 相手からの再ハンドシェイク要求。応答側の役はまだ実装していないので、
            // 自分から作り直して経路を復活させる。
            ESP_LOGW(TAG, "peer initiated a handshake; restarting ours");
            start_handshake_locked();
        }
        xSemaphoreGive(g_state.lock);
    }
    if (g_state.rx_done) xSemaphoreGive(g_state.rx_done);
    vTaskDelete(nullptr);
}

// netif の登録・解除は tcpip スレッドで行う。core locking 無効の構成では
// 他スレッドから netif_list を触ると、走査中のリストを壊す。
void netif_add_cb(void* arg)
{
    auto* cfg = static_cast<ip4_addr_t*>(arg);
    ip4_addr_t gw;
    ip4_addr_set_zero(&gw);
    if (netif_add(&g_state.netif, &cfg[0], &cfg[1], &gw, nullptr, wg_netif_init, tcpip_input)) {
        netif_set_up(&g_state.netif);
        netif_set_link_up(&g_state.netif);
    }
}

void netif_remove_cb(void*)
{
    netif_set_down(&g_state.netif);
    netif_remove(&g_state.netif);
}

}  // namespace

Netif& netif_instance()
{
    static Netif inst;
    return inst;
}

esp_err_t Netif::up(const uint8_t static_priv[kKeyLen], const ip4_addr_t& addr,
                    const ip4_addr_t& netmask, uint16_t listen_port)
{
    if (netif_up_) return ESP_ERR_INVALID_STATE;

    std::memcpy(g_state.static_priv, static_priv, kKeyLen);
    g_state.stats    = &stats_;
    g_state.ts_store = ts_store_;
    if (!g_state.lock) g_state.lock = xSemaphoreCreateMutex();
    if (!g_state.rx_done) g_state.rx_done = xSemaphoreCreateBinary();
    if (!g_state.tx_done) g_state.tx_done = xSemaphoreCreateBinary();
    if (!g_state.tx_queue) g_state.tx_queue = xQueueCreate(kTxQueueLen, sizeof(struct pbuf*));
    if (!g_state.lock || !g_state.rx_done || !g_state.tx_done || !g_state.tx_queue) {
        last_error_ = "sync primitive alloc failed";
        return ESP_ERR_NO_MEM;
    }
    if (!g_state.hs) g_state.hs = new Handshake(default_crypto());
    // 前回のセッション鍵を持ち越さない（持ち越すと死んだ鍵で送り続けてしまう）。
    delete g_state.transport;
    g_state.transport = new Transport(default_crypto());

    g_state.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_state.sock < 0) {
        last_error_ = "socket failed";
        return ESP_FAIL;
    }
    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(listen_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_state.sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "bind failed";
        return ESP_FAIL;
    }
    timeval tv{1, 0};
    setsockopt(g_state.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ip4_addr_t cfg[2] = {addr, netmask};
    if (tcpip_callback(netif_add_cb, cfg) != ERR_OK || g_state.netif.name[0] != 'w') {
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "netif_add failed";
        return ESP_FAIL;
    }

    g_state.running = true;
    if (xTaskCreate(&rx_task, "wg_rx", 16384, nullptr, 6, &g_state.rx_task) != pdPASS) {
        g_state.running = false;
        tcpip_callback(netif_remove_cb, nullptr);
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "rx task create failed";
        return ESP_ERR_NO_MEM;
    }
    // 送信タスクは X25519 を使わないのでスタックは小さくて足りる。
    if (xTaskCreate(&tx_task, "wg_tx", 4096, nullptr, 6, &g_state.tx_task) != pdPASS) {
        g_state.running = false;
        xSemaphoreTake(g_state.rx_done, pdMS_TO_TICKS(3000));
        tcpip_callback(netif_remove_cb, nullptr);
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "tx task create failed";
        return ESP_ERR_NO_MEM;
    }

    netif_up_ = true;
    char a[16], m[16];
    std::snprintf(a, sizeof(a), "%s", ip4addr_ntoa(&addr));
    std::snprintf(m, sizeof(m), "%s", ip4addr_ntoa(&netmask));
    ESP_LOGI(TAG, "netif up: %s/%s mtu %u port %u", a, m, kTunnelMtu, listen_port);
    return ESP_OK;
}

void Netif::down()
{
    if (!netif_up_) return;
    g_state.running = false;

    // タスクの終了を待ってからソケットと netif を片付ける。
    // 待たずに閉じると、解放した fd 番号が再利用された後に触りに行く。
    if (g_state.rx_done) xSemaphoreTake(g_state.rx_done, pdMS_TO_TICKS(5000));
    if (g_state.tx_done) xSemaphoreTake(g_state.tx_done, pdMS_TO_TICKS(3000));
    g_state.rx_task = nullptr;
    g_state.tx_task = nullptr;

    // キューに残った pbuf を解放する。
    if (g_state.tx_queue) {
        struct pbuf* p = nullptr;
        while (xQueueReceive(g_state.tx_queue, &p, 0) == pdTRUE) {
            if (p) pbuf_free(p);
        }
    }

    tcpip_callback(netif_remove_cb, nullptr);
    if (g_state.sock >= 0) {
        close(g_state.sock);
        g_state.sock = -1;
    }
    // セッション鍵を捨てる。残すと handshake_done() が嘘をつき、
    // 次に up() したとき死んだ鍵で送ってしまう。
    delete g_state.transport;
    g_state.transport = nullptr;
    g_state.has_peer  = false;
    netif_up_         = false;
    ESP_LOGI(TAG, "netif down");
}

esp_err_t Netif::set_peer(const PeerConfig& peer)
{
    if (!netif_up_) return ESP_ERR_INVALID_STATE;

    const size_t colon = peer.endpoint.rfind(':');
    if (colon == std::string::npos) {
        last_error_ = "endpoint must be host:port";
        return ESP_ERR_INVALID_ARG;
    }
    const std::string host = peer.endpoint.substr(0, colon);
    char*             end  = nullptr;
    const long        port = std::strtol(peer.endpoint.c_str() + colon + 1, &end, 10);
    if (!end || *end != '\0' || port <= 0 || port > 65535) {
        last_error_ = "bad port";
        return ESP_ERR_INVALID_ARG;
    }

    std::memset(&g_state.peer_addr, 0, sizeof(g_state.peer_addr));
    g_state.peer_addr.sin_family = AF_INET;
    g_state.peer_addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &g_state.peer_addr.sin_addr) != 1) {
        last_error_ = "endpoint must be a literal IPv4 address";
        return ESP_ERR_INVALID_ARG;
    }
    std::memcpy(g_state.peer_pub, peer.public_key, kKeyLen);
    g_state.keepalive_sec = peer.keepalive_sec;
    g_state.has_peer      = true;

    bool ok = false;
    if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        ok = start_handshake_locked();
        xSemaphoreGive(g_state.lock);
    }
    if (!ok) {
        last_error_ = "could not start handshake";
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool Netif::handshake_done() const
{
    return netif_up_ && g_state.transport && g_state.transport->ready();
}

}  // namespace wg
