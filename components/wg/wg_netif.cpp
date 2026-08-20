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
    // 応答側として使う別インスタンス。開始側の状態を壊さないように分ける
    // （同じ Handshake を使い回すと、進行中の自分のハンドシェイクが消える）。
    Handshake*        responder = nullptr;
    // 相手から受け取った最後のタイムスタンプ。巻き戻ったものはリプレイとして拒否する。
    uint8_t           peer_timestamp[kTimestampLen] = {};
    bool              have_peer_timestamp = false;
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
    Netif::ForeignPacketHandler foreign     = nullptr;
    Netif::TimestampStore ts_store         = nullptr;
    uint64_t          ts_seconds           = 0;  // 最後に送った TAI64N の秒部分
};

State g_state;

void handle_initiation_locked(const uint8_t* pkt, size_t len, const sockaddr_in& from);

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

// 相手からのハンドシェイク要求に応答する。ロックを持った状態で呼ぶ。
// from は受信元。**認証が通ってから** endpoint を更新する（本家 wg と同じ）。
void handle_initiation_locked(const uint8_t* pkt, size_t len, const sockaddr_in& from)
{
    if (len != 148 || !g_state.responder || !g_state.has_peer || !g_state.transport) return;

    if (!g_state.responder->set_keys(g_state.static_priv, g_state.peer_pub)) return;

    uint8_t learned_static[kKeyLen];
    uint8_t timestamp[kTimestampLen];
    if (!g_state.responder->consume_initiation(pkt, learned_static, timestamp)) {
        ESP_LOGW(TAG, "initiation rejected (wrong peer or bad mac1)");
        return;
    }
    // タイムスタンプが巻き戻っていたらリプレイ。WireGuard 仕様どおり拒否する。
    if (g_state.have_peer_timestamp &&
        std::memcmp(timestamp, g_state.peer_timestamp, kTimestampLen) <= 0) {
        if (g_state.stats) ++g_state.stats->stale_initiations;
        ESP_LOGW(TAG, "initiation replayed (stale timestamp)");
        return;
    }

    uint32_t local_index = 0;
    if (!default_crypto().random_bytes(reinterpret_cast<uint8_t*>(&local_index), 4)) return;

    static uint8_t msg2[92];
    Keypair        kp;
    if (!g_state.responder->create_response(msg2, local_index, kp)) {
        ESP_LOGW(TAG, "could not build handshake response");
        return;
    }
    // **認証が通った時点で endpoint を送信元に更新する。**
    // 設定時の宛先に返し続けると、ピアの NAT マッピングが変わったときに
    // msg2 が死んだ宛先へ飛び、しかも新しい鍵に張り替えてしまうので
    // 受信方向まで死んで復帰しなくなる（本家 wg は認証済みパケットの送信元を使う）。
    if (from.sin_addr.s_addr != g_state.peer_addr.sin_addr.s_addr ||
        from.sin_port != g_state.peer_addr.sin_port) {
        char ip[16];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "peer endpoint moved to %s:%u", ip, (unsigned)ntohs(from.sin_port));
        g_state.peer_addr = from;
    }
    if (!send_to_peer(msg2, sizeof(msg2))) return;

    std::memcpy(g_state.peer_timestamp, timestamp, kTimestampLen);
    g_state.have_peer_timestamp = true;
    // kp.initiator == false なので Transport は「未確認」として扱う。
    // 相手はまだ msg2 を処理していない可能性があり、ここで新しい鍵に切り替えて
    // 送ると相手側で復号できずに落ちる。相手から新しい鍵のデータが届いた時点で昇格する。
    g_state.transport->set_keypair(kp, esp_timer_get_time());
    netif_instance().set_handshake_ok(true);
    // **応答側は last_handshake_us を更新しない（#30）。** 更新すると rekey タイマが
    // 120 秒先に押し出されるが、相手が msg2 を受け取ったかは分からないので、
    // 未確認の鍵で送り続けたまま 180 秒を越えて沈黙する可能性がある。
    // 更新しなければタイマが走り続け、こちらから作り直して復帰できる。
    // すぐ keepalive を送って、相手から見た経路を開ける。
    g_state.last_tx_us = 0;
    if (g_state.stats) ++g_state.stats->responses_sent;
    ESP_LOGI(TAG, "responded to peer handshake (our index %08x)", (unsigned)local_index);
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
    // 寿命を過ぎた「1 つ前」の鍵を捨てる。相手はもう破棄しているので、
    // 使い続けると送っているのに届かない状態になる（#30）。
    if (g_state.transport->expire_previous(now)) {
        ESP_LOGI(TAG, "dropped the previous keypair (older than reject-after)");
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
        // WireGuard のメッセージは「型 + 予約 3 バイトが 0 + 長さが固定」。
        // 先頭 1 バイトだけで判定すると、STUN Binding Response (0x01 0x01 ...) が
        // Initiation (型 1) と衝突し、**先頭バイトが 0x01 の任意のデータグラム 1 発で
        // 確立済みセッションを張り直させられる**。
        const uint8_t type      = buf[0];
        const bool    reserved_ok = (n >= 4) && buf[1] == 0 && buf[2] == 0 && buf[3] == 0;
        const bool    is_wg =
            reserved_ok &&
            ((type == kMsgInitiation && n == 148) || (type == kMsgResponse && n == 92) ||
             (type == kMsgCookie && n == 64) ||
             (type == kMsgTransport && n >= static_cast<ssize_t>(kTransportHeader + kTagLen)));

        if (is_wg && type == kMsgTransport) {
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

        if (!is_wg) {
            // WireGuard ではない。DISCO や STUN の応答が同じポートに来る。
            // ハンドラは時間がかかるのでロックを持たずに呼ぶ。
            if (g_state.foreign) {
                g_state.foreign(buf, static_cast<size_t>(n), from.sin_addr.s_addr,
                                ntohs(from.sin_port));
            } else if (g_state.stats) {
                ++g_state.stats->rx_dropped;
            }
            continue;
        }

        if (xSemaphoreTake(g_state.lock, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (type == kMsgResponse) {
            Keypair kp;
            if (g_state.hs && g_state.transport && g_state.hs->consume_response(buf, kp)) {
                g_state.transport->set_keypair(kp, esp_timer_get_time());
                netif_instance().set_handshake_ok(true);
                g_state.last_handshake_us = esp_timer_get_time();
                g_state.last_tx_us       = 0;  // すぐ keepalive を送って経路を開ける
                ESP_LOGI(TAG, "handshake complete (peer index %08x)", (unsigned)kp.remote_index);
            } else {
                ESP_LOGW(TAG, "handshake response rejected");
            }
        } else if (type == kMsgInitiation) {
            // 相手からの（再）ハンドシェイク。応答側として返す。
            // WireGuard は両側から rekey するので、これを実装しないと相手主導の
            // 鍵更新に追随できない（相手は 180 秒で鍵を捨てるので通信が止まる）。
            handle_initiation_locked(buf, static_cast<size_t>(n), from);
        }
        xSemaphoreGive(g_state.lock);
    }
    if (g_state.rx_done) xSemaphoreGive(g_state.rx_done);
    vTaskDelete(nullptr);
}

// netif の登録・解除は tcpip スレッドで行う。core locking 無効の構成では
// 他スレッドから netif_list を触ると、走査中のリストを壊す。
//
// **`tcpip_callback` ではなく `tcpip_callback_wait` を使う。** 前者は非同期なので、
// (1) 直後に netif.name を見ても、まだ登録されていなくて失敗と誤判定する
//     （実機で `netif up failed: netif_add failed` が出た）、
// (2) 引数をスタックに置いたまま渡すと、コールバックが走る前に呼び出し元が
//     戻ってスコープが消える、の 2 つが起きる。
// down() 側も同じで、非同期だと remove の前にソケットと transport を壊してしまう。
// どちらもコンソールタスクから呼ばれるので、待って構わない
// （tcpip スレッド自身から呼ぶとデッドロックするが、その経路は無い）。
struct AddArgs {
    ip4_addr_t addr;
    ip4_addr_t netmask;
    bool       ok;
};

void netif_add_cb(void* arg)
{
    auto*      a = static_cast<AddArgs*>(arg);
    ip4_addr_t gw;
    ip4_addr_set_zero(&gw);
    a->ok = netif_add(&g_state.netif, &a->addr, &a->netmask, &gw, nullptr, wg_netif_init,
                      tcpip_input) != nullptr;
    if (a->ok) {
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
    std::lock_guard<std::mutex> cfg_guard(cfg_mu_);
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
    if (!g_state.responder) g_state.responder = new Handshake(default_crypto());
    // 前回のセッション鍵を持ち越さない（持ち越すと死んだ鍵で送り続けてしまう）。
    delete g_state.transport;
    g_state.transport = new Transport(default_crypto());
    handshake_ok_     = false;  // 新しい netif には鍵がまだ無い

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

    AddArgs args{addr, netmask, false};
    if (tcpip_callback_wait(netif_add_cb, &args) != ERR_OK || !args.ok) {
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "netif_add failed";
        return ESP_FAIL;
    }

    g_state.running = true;
    if (xTaskCreate(&rx_task, "wg_rx", 16384, nullptr, 6, &g_state.rx_task) != pdPASS) {
        g_state.running = false;
        tcpip_callback_wait(netif_remove_cb, nullptr);
        close(g_state.sock);
        g_state.sock = -1;
        last_error_  = "rx task create failed";
        return ESP_ERR_NO_MEM;
    }
    // 送信タスクは X25519 を使わないのでスタックは小さくて足りる。
    if (xTaskCreate(&tx_task, "wg_tx", 4096, nullptr, 6, &g_state.tx_task) != pdPASS) {
        g_state.running = false;
        xSemaphoreTake(g_state.rx_done, pdMS_TO_TICKS(3000));
        tcpip_callback_wait(netif_remove_cb, nullptr);
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
    std::lock_guard<std::mutex> cfg_guard(cfg_mu_);
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

    tcpip_callback_wait(netif_remove_cb, nullptr);
    if (g_state.sock >= 0) {
        close(g_state.sock);
        g_state.sock = -1;
    }
    // セッション鍵を捨てる。残すと handshake_done() が嘘をつき、
    // 次に up() したとき死んだ鍵で送ってしまう。
    delete g_state.transport;
    g_state.transport = nullptr;
    g_state.has_peer            = false;
    g_state.have_peer_timestamp = false;
    handshake_ok_               = false;
    netif_up_                   = false;
    ESP_LOGI(TAG, "netif down");
}

// **up() の後に呼んでも効くようにする。** 以前はメンバに置くだけで、
// g_state.ts_store に入るのは up() の中だけだった。上がった後に呼ぶと
// 黙って無視される API になっていて罠だった。
void Netif::set_timestamp_store(TimestampStore fn)
{
    std::lock_guard<std::mutex> cfg_guard(cfg_mu_);
    ts_store_ = fn;
    if (netif_up_) g_state.ts_store = fn;
}

esp_err_t Netif::set_peer(const PeerConfig& peer)
{
    std::lock_guard<std::mutex> cfg_guard(cfg_mu_);
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
    // ピアが変わったらリプレイ判定の状態も捨てる。残すと、実クロックを持つ相手から
    // uptime 基準のタイムスタンプを出す相手に切り替えたときに、
    // 新しいピアの initiation が全部 stale 扱いになって永久に応答しなくなる。
    if (std::memcmp(g_state.peer_pub, peer.public_key, kKeyLen) != 0) {
        g_state.have_peer_timestamp = false;
        std::memset(g_state.peer_timestamp, 0, sizeof(g_state.peer_timestamp));
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

void Netif::set_foreign_handler(ForeignPacketHandler fn) { g_state.foreign = fn; }

bool Netif::send_raw(const uint8_t* data, size_t len, uint32_t dst_ip, uint16_t dst_port)
{
    if (g_state.sock < 0) return false;
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(dst_port);
    dst.sin_addr.s_addr = dst_ip;
    const ssize_t n = sendto(g_state.sock, data, len, 0, reinterpret_cast<sockaddr*>(&dst),
                             sizeof(dst));
    return n == static_cast<ssize_t>(len);
}

bool Netif::handshake_done() const
{
    // **生ポインタを触らない。** 以前は g_state.transport を読んで ->ready() を
    // 呼んでいたが、down()/up() がロック無しで delete/new するので、
    // 別タスク（画面のステータス表示）から毎秒呼ぶと use-after-free になる。
    // 鍵が確定したかどうかは bool にして、書く側（ロックの内側）が更新する。
    return netif_up_ && handshake_ok_;
}

}  // namespace wg
