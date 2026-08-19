#include <cstdint>
#include "wg_netif.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>

namespace wg {
namespace {

const char* TAG = "wg_netif";

// Tailscale と同じ 1280。IPv6 の最小 MTU なので、経路上の MTU 問題を避けやすい。
constexpr uint16_t kTunnelMtu = 1280;
constexpr size_t   kMaxPacket = 2048;

struct State {
    struct netif        netif{};
    int                 sock       = -1;
    sockaddr_in         peer_addr{};
    bool                has_peer   = false;
    Handshake*          hs         = nullptr;
    Transport*          transport  = nullptr;
    SemaphoreHandle_t   lock       = nullptr;
    TaskHandle_t        rx_task    = nullptr;
    volatile bool       running    = false;
    uint8_t             static_priv[kKeyLen] = {};
    uint8_t             peer_pub[kKeyLen]    = {};
    uint32_t            local_index = 0;
    int64_t             last_handshake_us = 0;
    uint32_t            keepalive_sec = 25;
    NetifStats*         stats = nullptr;
};

State g_state;

// TAI64N のタイムスタンプ。WireGuard はリプレイ検出に使う。
// 実時間が取れない環境でも単調増加していれば通るので、起動からの経過時間を足す。
void make_timestamp(uint8_t out[kTimestampLen])
{
    const uint64_t secs = 0x400000000000000aULL + static_cast<uint64_t>(esp_timer_get_time() / 1000000);
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
    uint8_t msg1[148];
    // インデックスは自分で選ぶ。相手はこれを receiver として返してくる。
    if (!c.random_bytes(reinterpret_cast<uint8_t*>(&g_state.local_index), 4)) return false;
    if (!g_state.hs->create_initiation(msg1, g_state.local_index, ts)) return false;
    if (!send_to_peer(msg1, sizeof(msg1))) return false;
    if (g_state.stats) ++g_state.stats->handshakes;
    ESP_LOGI(TAG, "sent handshake initiation (index %08x)", (unsigned)g_state.local_index);
    return true;
}

// lwIP から呼ばれる送信パス。平文の IP パケットを暗号化して UDP で送る。
err_t wg_output(struct netif* nif, struct pbuf* p, const ip4_addr_t* ipaddr)
{
    (void)nif;
    (void)ipaddr;
    if (!g_state.transport || !g_state.transport->ready()) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_CONN;  // まだハンドシェイクが終わっていない
    }
    if (p->tot_len > kTunnelMtu) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_MEM;
    }

    uint8_t plain[kTunnelMtu];
    if (pbuf_copy_partial(p, plain, p->tot_len, 0) != p->tot_len) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_BUF;
    }

    uint8_t wire[kMaxPacket];
    size_t  n = 0;
    if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = g_state.transport->encrypt(wire, sizeof(wire), plain, p->tot_len);
        xSemaphoreGive(g_state.lock);
    }
    if (n == 0 || !send_to_peer(wire, n)) {
        if (g_state.stats) ++g_state.stats->tx_dropped;
        return ERR_IF;
    }
    if (g_state.stats) {
        ++g_state.stats->tx_packets;
        g_state.stats->tx_bytes += p->tot_len;
    }
    return ERR_OK;
}

err_t wg_netif_init(struct netif* nif)
{
    nif->name[0]   = 'w';
    nif->name[1]   = 'g';
    nif->output    = wg_output;
    nif->mtu       = kTunnelMtu;
    // ポイントツーポイントなので ARP は無い。ブロードキャストもしない。
    nif->flags     = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

// 復号したパケットを lwIP に渡す。
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
            continue;  // タイムアウト
        }
        const uint8_t type = buf[0];

        if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (type == kMsgResponse && n == 92) {
            Keypair kp;
            if (g_state.hs && g_state.hs->consume_response(buf, kp)) {
                g_state.transport->set_keypair(kp);
                g_state.last_handshake_us = esp_timer_get_time();
                ESP_LOGI(TAG, "handshake complete (peer index %08x)",
                         (unsigned)kp.remote_index);
            } else {
                ESP_LOGW(TAG, "handshake response rejected");
            }
        } else if (type == kMsgTransport && n >= static_cast<ssize_t>(kTransportHeader + kTagLen)) {
            bool         valid = false;
            const size_t got   = g_state.transport
                                   ? g_state.transport->decrypt(plain, sizeof(plain), buf,
                                                                static_cast<size_t>(n), &valid)
                                   : 0;
            xSemaphoreGive(g_state.lock);
            if (valid && got > 0) {
                inject_to_lwip(plain, got);
            } else if (!valid) {
                if (g_state.stats) ++g_state.stats->rx_dropped;
            }
            continue;  // ロックは解放済み
        } else if (type == kMsgInitiation) {
            // 相手からの再ハンドシェイク要求。応答側の実装はまだ持たないので数だけ記録する。
            ESP_LOGW(TAG, "received handshake initiation (responder role not implemented)");
        }
        xSemaphoreGive(g_state.lock);
    }
    g_state.rx_task = nullptr;
    vTaskDelete(nullptr);
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
    g_state.stats = &stats_;
    if (!g_state.lock) g_state.lock = xSemaphoreCreateMutex();
    if (!g_state.lock) {
        last_error_ = "mutex alloc failed";
        return ESP_ERR_NO_MEM;
    }
    if (!g_state.hs) g_state.hs = new Handshake(default_crypto());
    if (!g_state.transport) g_state.transport = new Transport(default_crypto());

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

    ip4_addr_t gw;
    ip4_addr_set_zero(&gw);
    if (!netif_add(&g_state.netif, &addr, &netmask, &gw, nullptr, wg_netif_init, tcpip_input)) {
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "netif_add failed";
        return ESP_FAIL;
    }
    netif_set_up(&g_state.netif);
    netif_set_link_up(&g_state.netif);

    g_state.running = true;
    // ハンドシェイク応答の処理で X25519 を 2 回呼ぶので、スタックは広く取る。
    if (xTaskCreate(&rx_task, "wg_rx", 16384, nullptr, 6, &g_state.rx_task) != pdPASS) {
        g_state.running = false;
        last_error_     = "rx task create failed";
        return ESP_ERR_NO_MEM;
    }

    netif_up_ = true;
    char addr_str[16], mask_str[16];
    std::snprintf(addr_str, sizeof(addr_str), "%s", ip4addr_ntoa(&addr));
    std::snprintf(mask_str, sizeof(mask_str), "%s", ip4addr_ntoa(&netmask));
    ESP_LOGI(TAG, "netif up: %s/%s mtu %u port %u", addr_str, mask_str, kTunnelMtu, listen_port);
    return ESP_OK;
}

void Netif::down()
{
    if (!netif_up_) return;
    g_state.running = false;
    for (int i = 0; i < 30 && g_state.rx_task; ++i) vTaskDelay(pdMS_TO_TICKS(50));
    netif_set_down(&g_state.netif);
    netif_remove(&g_state.netif);
    if (g_state.sock >= 0) {
        close(g_state.sock);
        g_state.sock = -1;
    }
    g_state.has_peer = false;
    netif_up_        = false;
    ESP_LOGI(TAG, "netif down");
}

esp_err_t Netif::set_peer(const PeerConfig& peer)
{
    if (!netif_up_) return ESP_ERR_INVALID_STATE;

    // "host:port" を分解する
    const size_t colon = peer.endpoint.rfind(':');
    if (colon == std::string::npos) {
        last_error_ = "endpoint must be host:port";
        return ESP_ERR_INVALID_ARG;
    }
    const std::string host = peer.endpoint.substr(0, colon);
    const int         port = std::atoi(peer.endpoint.c_str() + colon + 1);
    if (port <= 0 || port > 65535) {
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
    return g_state.transport && g_state.transport->ready();
}

}  // namespace wg
