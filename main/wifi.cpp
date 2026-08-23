#include <cstdint>
#include "wifi.hpp"

#include <cstring>

#include <esp_console.h>
#include <esp_event.h>
#include <esp_hosted.h>
#include <esp_log.h>
#include <algorithm>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <nvs.h>
#include <esp_check.h>
#include <cstdio>
#include <cstdlib>

namespace {

const char* TAG = "wifi";

constexpr const char* kNvsNamespace = "wifi";
constexpr const char* kNvsKeySsid   = "ssid";
constexpr const char* kNvsKeyPass   = "pass";

EventGroupHandle_t  s_events   = nullptr;
constexpr int       kConnected = BIT0;
bool                s_started  = false;
int                 s_retry    = 0;
// 自分で切ったときの切断イベントで再接続を走らせないための印。
bool                s_reconfiguring = false;
esp_timer_handle_t  s_retry_timer   = nullptr;

// --- 保存済みの接続先 (#56) ---
//
// 実体は blob 1 つ。**RAM に持ってから配る** — メニューは毎秒 refresh するので、
// 表示のたびに NVS を開くと SDIO の RPC と重なって描画が詰まる。
struct Net {
    char ssid[33];
    char pass[65];
};
constexpr const char* kNvsKeyNets = "nets";
constexpr const char* kNvsKeyLast = "last";

Net    s_nets[kMaxWifiNets] = {};
size_t s_net_count          = 0;
// **配列はコンソールタスクと UI タスクの両方から触られる。**
// 一覧を開いたままシリアルで `wifi-del` すると、詰め直しの途中を読んでしまう。
SemaphoreHandle_t s_nets_lock = nullptr;

// 再帰にしてある（wifi_net_add が中で save/load を呼ぶ）。
struct NetLock {
    NetLock()
    {
        if (!s_nets_lock) s_nets_lock = xSemaphoreCreateRecursiveMutex();
        if (s_nets_lock) xSemaphoreTakeRecursive(s_nets_lock, portMAX_DELAY);
    }
    ~NetLock() { if (s_nets_lock) xSemaphoreGiveRecursive(s_nets_lock); }
    NetLock(const NetLock&)            = delete;
    NetLock& operator=(const NetLock&) = delete;
};
// 今つないでいる設定。切断しただけでは -1 にしない（再接続の対象なので）。
// 消したときだけ -1 に戻す。
int    s_current = -1;

void load_nets()
{
    s_net_count = 0;
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return;
    size_t len = sizeof(s_nets);
    const esp_err_t got = nvs_get_blob(nvs, kNvsKeyNets, s_nets, &len);
    if (got == ESP_OK) {
        s_net_count = std::min(len / sizeof(Net), kMaxWifiNets);
    } else if (got == ESP_ERR_NVS_INVALID_LENGTH) {
        // **上限を下げた後や、上限を上げたビルドで書いた blob。** 黙って
        // 「設定なし」に落ちると、一覧が空になった理由が読めない。
        ESP_LOGW(TAG, "nets blob が大きすぎる（上限 %d 件ぶんに収まらない）。無視する",
                 (int)kMaxWifiNets);
        nvs_close(nvs);
        return;
    } else {
        // **1 件しか持てなかった頃の設定を引き継ぐ。** 引き継がないと、
        // 更新した瞬間に今つながっている AP を忘れてオフラインになる。
        char   ssid[33] = {};
        char   pass[65] = {};
        size_t sl = sizeof(ssid), pl = sizeof(pass);
        if (nvs_get_str(nvs, kNvsKeySsid, ssid, &sl) == ESP_OK && ssid[0]) {
            if (nvs_get_str(nvs, kNvsKeyPass, pass, &pl) != ESP_OK) pass[0] = '\0';
            std::snprintf(s_nets[0].ssid, sizeof(s_nets[0].ssid), "%s", ssid);
            std::snprintf(s_nets[0].pass, sizeof(s_nets[0].pass), "%s", pass);
            s_net_count = 1;
            ESP_LOGI(TAG, "migrated legacy credentials for \"%s\"", ssid);
        }
    }
    nvs_close(nvs);
    // 終端されていない blob を掴んでも文字列として扱わないように、末尾を潰す。
    for (size_t i = 0; i < s_net_count; ++i) {
        s_nets[i].ssid[sizeof(s_nets[i].ssid) - 1] = '\0';
        s_nets[i].pass[sizeof(s_nets[i].pass) - 1] = '\0';
    }
}

esp_err_t save_nets()
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_blob(nvs, kNvsKeyNets, s_nets, s_net_count * sizeof(Net));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

void save_last(int index)
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, kNvsKeyLast, (uint8_t)index);
    nvs_commit(nvs);
    nvs_close(nvs);
}

int load_last()
{
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &nvs) != ESP_OK) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(nvs, kNvsKeyLast, &v) != ESP_OK) v = 0;
    nvs_close(nvs);
    return (v < s_net_count) ? (int)v : 0;
}

// AP の再起動などで一時的に落ちても必ず戻ってくるように、諦めずに指数バックオフで粘る。
// ネットワーク端末が「5 回失敗したら電源を入れ直すまで永久にオフライン」では使えない。
void schedule_reconnect()
{
    const int shift = std::min(s_retry, 5);
    const uint32_t delay_ms = std::min<uint32_t>(30000, 500u << shift);
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, static_cast<uint64_t>(delay_ms) * 1000);
    }
    ESP_LOGW(TAG, "reconnect in %u ms (attempt %d)", (unsigned)delay_ms, s_retry);
}

void retry_timer_cb(void*)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        schedule_reconnect();
    }
}

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* e = static_cast<wifi_event_sta_disconnected_t*>(data);
        if (s_events) xEventGroupClearBits(s_events, kConnected);
        if (s_reconfiguring) {
            // 設定変更で自分から切ったぶん。次の connect は呼び出し側が出している。
            s_reconfiguring = false;
            return;
        }
        ESP_LOGW(TAG, "disconnected (reason=%d)", e->reason);
        ++s_retry;
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        s_retry = 0;
        // **繋がったものだけを覚える。** 選んだ時点で覚えると、繋がらない設定を
        // 一度選んだだけで次の起動もそれで始まる。
        if (s_current >= 0) save_last(s_current);
        xEventGroupSetBits(s_events, kConnected);
        ESP_LOGI(TAG, "got ip: " IPSTR " gw=" IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
    }
}

esp_err_t connect_with(const char* ssid, const char* pass)
{
    // ssid[32] / password[64] は最大長では NUL 終端されない仕様なので、配列いっぱいまで入れる。
    // strncpy に sizeof-1 を渡すと 32 文字 SSID や 64 桁の PSK が 1 バイト切れて必ず失敗する。
    wifi_config_t cfg = {};
    std::memcpy(cfg.sta.ssid, ssid, std::min(std::strlen(ssid), sizeof(cfg.sta.ssid)));
    std::memcpy(cfg.sta.password, pass, std::min(std::strlen(pass), sizeof(cfg.sta.password)));
    // 閾値は下限なので WPA2 を指定すると WPA/TKIP のみの AP がスキャン結果から外れる。
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    s_retry = 0;
    // 削除で立てた印が残っていることがある。明示的に繋ぐ時点で必ず落とす
    // （残すと、この接続が切れたときの再接続が 1 回だけ黙って飛ぶ）。
    s_reconfiguring = false;
    // **「繋がっている」印もここで落とす。** 別の設定へ移る時点で前の接続は
    // 手放しているのに、切断イベントが来るまで印が残る。その間に一覧を出すと
    // **繋がっていない設定に `*` が付く**（実機で確認）。
    if (s_events) xEventGroupClearBits(s_events, kConnected);
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");  // STA_START で connect する
        s_started = true;
    } else {
        // 自分で切った切断イベントで再接続ハンドラが動くと、この connect を潰してしまう。
        s_reconfiguring = true;
        esp_err_t err   = esp_wifi_disconnect();
        if (err != ESP_OK) {
            s_reconfiguring = false;
            ESP_LOGW(TAG, "esp_wifi_disconnect: %s", esp_err_to_name(err));
        }
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect");
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    return ESP_OK;
}

int cmd_wifi(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::printf("usage: wifi <ssid> [password]\n");
        return 1;
    }
    const char* ssid = argv[1];
    const char* pass = (argc == 3) ? argv[2] : "";
    esp_err_t   err  = wifi_net_add(ssid, pass);
    if (err != ESP_OK) {
        // 満杯なら消す先を出す。黙って 1 を返すと「打ち間違えた」と読める。
        std::printf("save failed: %s\n", esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) std::printf("保存済みが %d 件で満杯 (`wifi-list` / `wifi-del`)\n",
                                               (int)kMaxWifiNets);
        return 1;
    }
    const int i = wifi_net_find(ssid);
    if (i < 0) return 1;
    err = wifi_net_connect((size_t)i);
    if (err != ESP_OK) {
        // WiFi 未初期化 (C6 が上がっていない等) はここに来る。
        std::printf("connect failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

int cmd_wifi_list(int, char**)
{
    if (s_net_count == 0) {
        std::printf("保存済みなし (`wifi <ssid> [password]`)\n");
        return 0;
    }
    // **画面と同じ経路で並べる。** 別々に数えると片方だけ直したときに食い違う。
    WifiNetInfo  nets[kMaxWifiNets];
    const size_t n = wifi_net_snapshot(nets, kMaxWifiNets);
    for (size_t i = 0; i < n; ++i) {
        std::printf("%c %d: %s\n", nets[i].active ? '*' : ' ', (int)i, nets[i].ssid);
    }
    return 0;
}

int cmd_wifi_del(int argc, char** argv)
{
    if (argc != 2) {
        std::printf("usage: wifi-del <index>   (一覧は `wifi-list`)\n");
        return 1;
    }
    const int      i   = std::atoi(argv[1]);
    const esp_err_t err = (i >= 0) ? wifi_net_remove((size_t)i) : ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) {
        std::printf("delete failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

int cmd_wifi_scan(int, char**)
{
    WifiScanEntry aps[16];
    const int     n = wifi_scan(aps, 16);
    if (n < 0) {
        std::printf("scan failed\n");
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        std::printf("%2d: %-32s %4d dBm %s\n", i, aps[i].ssid, aps[i].rssi,
                    aps[i].secure ? "secure" : "open");
    }
    return 0;
}

int cmd_wifi_status(int, char**)
{
    wifi_ap_record_t ap;
    // ステータスバーが 1 秒ごとに叩く経路なので、所要時間を出せるようにしておく。
    const int64_t t0 = esp_timer_get_time();
    const bool    ok = wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    std::printf("esp_wifi_sta_get_ap_info: %lld us\n", esp_timer_get_time() - t0);
    if (ok) {
        esp_netif_ip_info_t ip{};
        esp_netif_t*        netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) esp_netif_get_ip_info(netif, &ip);
        std::printf("connected ssid=%s rssi=%d ch=%d ip=" IPSTR "\n", (char*)ap.ssid, ap.rssi,
                    ap.primary, IP2STR(&ip.ip));
    } else {
        std::printf("not connected\n");
    }
    return 0;
}

}  // namespace

esp_err_t wifi_start(void)
{
    if (!s_events) {
        s_events = xEventGroupCreate();
        if (!s_events) {
            ESP_LOGE(TAG, "xEventGroupCreate failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_retry_timer) {
        const esp_timer_create_args_t args = {
            .callback = &retry_timer_cb, .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK, .name = "wifi_retry", .skip_unhandled_events = true};
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_retry_timer), TAG, "timer_create");
    }

    // 自動初期化を切っているので、C6 の電源が入ったこの時点で明示的に立ち上げる。
    int rc = esp_hosted_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %d", rc);
        return ESP_FAIL;
    }
    rc = esp_hosted_connect_to_slave();
    if (rc != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave failed: %d (C6 が起動していない可能性)", rc);
        return ESP_FAIL;
    }
    esp_hosted_coprocessor_fwver_t fw{};
    if (esp_hosted_get_coprocessor_fwversion(&fw) == 0) {
        ESP_LOGI(TAG, "C6 firmware: %d.%d.%d", fw.major1, fw.minor1, fw.patch1);
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event_loop");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");  // 内部で SDIO が起動する
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr),
        TAG, "reg wifi event");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr),
        TAG, "reg ip event");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");

    load_nets();
    if (s_net_count > 0) {
        // 前回つながったものから始める。**どれに繋ぐかを自動で選び直しはしない** —
        // ponytail: 別の場所へ移ったら一覧から選ぶ。自動で総当たりするなら
        // スキャンしてから決める形にする（今は繋がらない理由が読めるほうを取る）。
        return wifi_net_connect((size_t)load_last());
    }
    ESP_LOGW(TAG, "no credentials in NVS. set them with: wifi <ssid> [password]");
    return ESP_OK;
}

bool wifi_status(char* ssid, size_t ssid_len, int* rssi, char* ip, size_t ip_len)
{
    if (!wifi_is_connected()) return false;
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    if (ssid && ssid_len) std::snprintf(ssid, ssid_len, "%s", (const char*)ap.ssid);
    if (rssi) *rssi = ap.rssi;
    if (ip && ip_len) {
        ip[0] = '\0';
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info{};
        if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            std::snprintf(ip, ip_len, IPSTR, IP2STR(&info.ip));
        }
    }
    return true;
}

bool wifi_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & kConnected);
}

// --- 保存済みの接続先 (#56) ---

size_t wifi_net_count(void)
{
    NetLock lock;
    return s_net_count;
}

int wifi_net_current(void)
{
    NetLock lock;
    return s_current;
}

size_t wifi_net_snapshot(WifiNetInfo* out, size_t max)
{
    if (!out || max == 0) return 0;
    // **1 回のロックで全部取る。** 1 件ずつ引くと、その間に消えて
    // 「名前は消したもの、印は別の行」という並びが出る。
    NetLock lock;
    const size_t n = std::min(s_net_count, max);
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(out[i].ssid, sizeof(out[i].ssid), "%s", s_nets[i].ssid);
        // 繋がっているものだけに印を付ける（選んだだけでは付けない）。
        out[i].active = ((int)i == s_current) && wifi_is_connected();
    }
    return n;
}

bool wifi_net_ssid(size_t i, char* out, size_t len)
{
    NetLock lock;
    if (i >= s_net_count || !out || len == 0) return false;
    std::snprintf(out, len, "%s", s_nets[i].ssid);
    return true;
}

esp_err_t wifi_net_add(const char* ssid, const char* pass)
{
    NetLock lock;
    if (!pass) pass = "";
    if (!ssid || !ssid[0] || std::strlen(ssid) >= sizeof(s_nets[0].ssid) ||
        std::strlen(pass) >= sizeof(s_nets[0].pass)) {
        return ESP_ERR_INVALID_ARG;
    }
    // **同じ SSID はパスワードを差し替えるだけ。** 枠を食うと、打ち間違えて
    // 入れ直しただけで「5 件で満杯」になる。
    for (size_t i = 0; i < s_net_count; ++i) {
        if (std::strcmp(s_nets[i].ssid, ssid) != 0) continue;
        std::snprintf(s_nets[i].pass, sizeof(s_nets[i].pass), "%s", pass);
        const esp_err_t err = save_nets();
        if (err != ESP_OK) load_nets();
        return err;
    }
    if (s_net_count >= kMaxWifiNets) return ESP_ERR_NO_MEM;
    std::snprintf(s_nets[s_net_count].ssid, sizeof(s_nets[0].ssid), "%s", ssid);
    std::snprintf(s_nets[s_net_count].pass, sizeof(s_nets[0].pass), "%s", pass);
    ++s_net_count;
    const esp_err_t err = save_nets();
    // **書けなかったものを RAM に残さない。** 残すと一覧には出るのに
    // 再起動で消える（「保存したはずなのに」になる）。
    if (err != ESP_OK) load_nets();
    return err;
}

esp_err_t wifi_net_remove(size_t i)
{
    NetLock lock;
    if (i >= s_net_count) return ESP_ERR_INVALID_ARG;
    const bool was_current = ((int)i == s_current);
    const int  prev_current = s_current;
    for (size_t j = i + 1; j < s_net_count; ++j) s_nets[j - 1] = s_nets[j];
    --s_net_count;
    std::memset(&s_nets[s_net_count], 0, sizeof(s_nets[0]));
    // 詰めたぶん index がずれる。ずらさないと、消した後に別の設定を
    // 「今つながっている」と表示する。
    if (was_current) s_current = -1;
    else if (s_current > (int)i) --s_current;

    const esp_err_t err = save_nets();
    if (err != ESP_OK) {
        // NVS に書けなかったら RAM を NVS に合わせ直す。**s_current も戻す** —
        // 戻さないと、一覧は元に戻るのに `*` だけ別の設定に付く。
        load_nets();
        s_current = prev_current;
        return err;
    }
    // **次回の起動先も詰め直す。** RAM だけ直して NVS の `last` を放っておくと、
    // 消した位置より後ろに繋いでいたとき、次の起動で別の設定に繋ぐ。
    save_last(s_current >= 0 ? s_current : 0);
    if (was_current) {
        // **実際に切る。** 消したのに繋がったままだと、一覧と実態が食い違う。
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        // 切断イベントで再接続が走らないようにしてから切る。
        s_reconfiguring = true;
        // **未接続でも呼ぶ。** 繋ぎに行っている最中（リトライ中）に消したとき、
        // 呼ばないと STA の config に消した SSID が残ったままアソシエーションが
        // 成功し得る（一覧に無い AP に繋がる）。印も立てっぱなしになる。
        if (esp_wifi_disconnect() != ESP_OK) s_reconfiguring = false;
        ESP_LOGI(TAG, "removed the active network; disconnected");
    }
    return ESP_OK;
}

int wifi_net_find(const char* ssid)
{
    NetLock lock;
    if (!ssid) return -1;
    for (size_t i = 0; i < s_net_count; ++i) {
        if (std::strcmp(s_nets[i].ssid, ssid) == 0) return (int)i;
    }
    return -1;
}

esp_err_t wifi_net_connect(size_t i)
{
    NetLock lock;
    if (i >= s_net_count) return ESP_ERR_INVALID_ARG;
    const int prev  = s_current;
    s_current       = (int)i;
    const esp_err_t err = connect_with(s_nets[i].ssid, s_nets[i].pass);
    // **失敗したら戻す。** 進めたままだと、生きている古い接続が IP を取り直した
    // ときに「繋がっていない設定」の index を次回起動先として覚えてしまう。
    if (err != ESP_OK) s_current = prev;
    return err;
}

int wifi_scan(WifiScanEntry* out, int max)
{
    if (!out || max <= 0) return -1;
    if (!s_started) {
        // 保存済みが 0 件だと起動時に繋ぎに行っていないので、まだ start していない。
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_start for scan: %s", esp_err_to_name(err));
            return -1;
        }
        s_started = true;
    }
    // **繋ぎに行っている最中はスキャンが 0 件で返る**（実機で確認）。しかも
    // **保存済みの AP が無い場所で「新規追加」を開くと必ずこの状態**なので、
    // 直さないと目玉の経路がいつも「AP が見つからない」になる。
    // 繋がっているときのスキャンは問題ないので、リトライ中だけ止める。
    const bool was_retrying = !wifi_is_connected() && s_current >= 0;
    if (was_retrying) {
        if (s_retry_timer) esp_timer_stop(s_retry_timer);
        s_reconfiguring = true;  // この切断で再接続を走らせない
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));  // 切断イベントが流れるのを待つ
    }

    wifi_scan_config_t cfg = {};
    cfg.show_hidden        = false;  // 隠し SSID は手入力で足す
    esp_err_t err          = esp_wifi_scan_start(&cfg, /*block=*/true);
    // **探し終わったらリトライを戻す。** 戻さないと、スキャンしただけで
    // 元の AP に繋ぎ直さなくなる（AP が戻ってきても永久にオフライン）。
    struct Resume {
        bool on;
        ~Resume()
        {
            if (!on) return;
            s_retry         = 0;
            s_reconfiguring = false;
            esp_wifi_connect();
        }
    } resume{was_retrying};
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan_start: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) return 0;
    // 1 件 80 バイト弱。全部拾うと数 KB になるので上限を切る（強い順に返る）。
    constexpr uint16_t kMaxAp = 24;
    if (found > kMaxAp) found = kMaxAp;
    auto* recs = static_cast<wifi_ap_record_t*>(std::malloc(found * sizeof(wifi_ap_record_t)));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    err   = esp_wifi_scan_get_ap_records(&found, recs);
    int n = 0;
    if (err == ESP_OK) {
        // **RSSI の降順で返る**ので、同じ SSID は最初の 1 つだけ残せば強いほうが残る
        // （2.4G と 5G、中継器で同じ名前が何度も出る）。
        for (uint16_t k = 0; k < found && n < max; ++k) {
            const char* ssid = reinterpret_cast<const char*>(recs[k].ssid);
            if (!ssid[0]) continue;
            bool dup = false;
            for (int j = 0; j < n; ++j) {
                if (std::strcmp(out[j].ssid, ssid) == 0) { dup = true; break; }
            }
            if (dup) continue;
            std::snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
            out[n].rssi   = recs[k].rssi;
            out[n].secure = (recs[k].authmode != WIFI_AUTH_OPEN);
            ++n;
        }
    } else {
        ESP_LOGE(TAG, "scan_get_ap_records: %s", esp_err_to_name(err));
    }
    std::free(recs);
    ESP_LOGI(TAG, "scan: %d ap (%u raw)", n, (unsigned)found);
    return (err == ESP_OK) ? n : -1;
}

esp_err_t console_start(void)
{
    esp_console_repl_t*       repl     = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt                    = "tab5>";
    repl_cfg.max_cmdline_length        = 256;
    // 既定 4KB では mbedTLS の ECP (X25519) がスタック保護フォルトを起こす。
    // X25519 は 1 回で 10KB 近く使うので、鍵導出と netif の初期化が重なる経路
    // (wg コマンド) では 16KB でも足りなかった。
    repl_cfg.task_stack_size           = 32768;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl), TAG,
                        "new_repl");

    const esp_console_cmd_t wifi_cmd = {
        .command  = "wifi",
        .help     = "WiFi の SSID とパスワードを NVS に保存して接続する",
        .hint     = "<ssid> [password]",
        .func     = &cmd_wifi,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&wifi_cmd), TAG, "reg wifi cmd");

    const esp_console_cmd_t status_cmd = {
        .command  = "wifi-status",
        .help     = "WiFi の接続状態を表示する",
        .hint     = nullptr,
        .func     = &cmd_wifi_status,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&status_cmd), TAG, "reg status cmd");

    // 画面が出ない状態でも一覧・削除・スキャンができるようにしておく (#56)。
    struct { const char* name; const char* help; const char* hint; esp_console_cmd_func_t fn; }
        extra[] = {
            {"wifi-list", "保存済みの WiFi を並べる", nullptr, &cmd_wifi_list},
            {"wifi-del", "保存済みの WiFi を消す", "<index>", &cmd_wifi_del},
            {"wifi-scan", "周りの AP を探す", nullptr, &cmd_wifi_scan},
        };
    for (const auto& e : extra) {
        const esp_console_cmd_t c = {
            .command = e.name, .help = e.help, .hint = e.hint, .func = e.fn,
            .argtable = nullptr, .func_w_context = nullptr, .context = nullptr,
        };
        ESP_RETURN_ON_ERROR(esp_console_cmd_register(&c), TAG, "reg wifi cmd");
    }

    return esp_console_start_repl(repl);
}
