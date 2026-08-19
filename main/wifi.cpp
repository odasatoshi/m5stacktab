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
        xEventGroupSetBits(s_events, kConnected);
        ESP_LOGI(TAG, "got ip: " IPSTR " gw=" IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
    }
}

esp_err_t load_credentials(char* ssid, size_t ssid_len, char* pass, size_t pass_len)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(kNvsNamespace, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_get_str(nvs, kNvsKeySsid, ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, kNvsKeyPass, pass, &pass_len);
        // パスワードなし (オープン AP) も許す
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';
            err     = ESP_OK;
        }
    }
    nvs_close(nvs);
    return err;
}

esp_err_t save_credentials(const char* ssid, const char* pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(nvs, kNvsKeySsid, ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, kNvsKeyPass, pass);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
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
    esp_err_t   err  = save_credentials(ssid, pass);
    if (err != ESP_OK) {
        std::printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    err = connect_with(ssid, pass);
    if (err != ESP_OK) {
        // WiFi 未初期化 (C6 が上がっていない等) はここに来る。黙って 1 を返すと理由が分からない。
        std::printf("connect failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

int cmd_wifi_status(int, char**)
{
    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
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

    char ssid[33] = {};
    char pass[65] = {};
    if (load_credentials(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0]) {
        return connect_with(ssid, pass);
    }
    ESP_LOGW(TAG, "no credentials in NVS. set them with: wifi <ssid> [password]");
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & kConnected);
}

esp_err_t console_start(void)
{
    esp_console_repl_t*       repl     = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt                    = "tab5>";
    repl_cfg.max_cmdline_length        = 256;
    // 既定 4KB では mbedTLS の ECP (X25519) がスタック保護フォルトを起こす。
    repl_cfg.task_stack_size           = 16384;

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

    return esp_console_start_repl(repl);
}
