#include "wifi.hpp"

#include <cstring>

#include <driver/i2c_master.h>
#include <esp_console.h>
#include <esp_event.h>
#include <esp_hosted.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <nvs.h>
#include <esp_check.h>
#include <cstdio>

namespace {

const char* TAG = "wifi";

// Tab5 の内部 I2C。BSP の BSP_I2C_SDA / BSP_I2C_SCL と同じ。
constexpr gpio_num_t kI2cSda = GPIO_NUM_31;
constexpr gpio_num_t kI2cScl = GPIO_NUM_32;

constexpr const char* kNvsNamespace = "wifi";
constexpr const char* kNvsKeySsid   = "ssid";
constexpr const char* kNvsKeyPass   = "pass";

EventGroupHandle_t s_events    = nullptr;
constexpr int      kConnected  = BIT0;
bool               s_started   = false;
int                s_retry     = 0;
constexpr int      kMaxRetry   = 5;

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* e = static_cast<wifi_event_sta_disconnected_t*>(data);
        xEventGroupClearBits(s_events, kConnected);
        if (s_retry < kMaxRetry) {
            ++s_retry;
            ESP_LOGW(TAG, "disconnected (reason=%d), retry %d/%d", e->reason, s_retry, kMaxRetry);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "disconnected (reason=%d), giving up after %d retries", e->reason, kMaxRetry);
        }
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
    wifi_config_t cfg = {};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password), pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");  // STA_START で connect する
        s_started = true;
    } else {
        esp_wifi_disconnect();
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
    return err == ESP_OK ? 0 : 1;
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

esp_err_t tab5_c6_power_on(void)
{
    // PI4IOE5V6408 @0x44 の pin0 が WLAN_PWR_EN。
    // 専用ドライバ (esp_io_expander_pi4ioe5v6408) は使わない:
    //   - set_dir が Hi-Z レジスタ (0x07) を解除しないので、出力方向にしても電流が出ない
    //     (このチップは既定で全ピン Hi-Z)
    //   - 初期化でチップリセットを撃つので、他のピン (USB 5V など) の設定も飛ぶ
    // 触るのは 3 レジスタの bit0 だけなので read-modify-write で済ませる。
    constexpr uint8_t kAddr     = 0x44;
    constexpr uint8_t kRegIoDir = 0x03;  // 1 = 出力
    constexpr uint8_t kRegOut   = 0x05;  // 出力レベル
    constexpr uint8_t kRegHiZ   = 0x07;  // 1 = Hi-Z (出力を切り離す)
    constexpr uint8_t kWlanBit  = 1 << 0;

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port                     = I2C_NUM_0;
    bus_cfg.sda_io_num                   = kI2cSda;
    bus_cfg.scl_io_num                   = kI2cScl;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = nullptr;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c_new_master_bus");

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = kAddr;
    dev_cfg.scl_speed_hz        = 100000;

    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);

    auto update = [&](uint8_t reg, uint8_t set_mask, uint8_t clear_mask) -> esp_err_t {
        uint8_t v = 0;
        ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100), TAG,
                            "read reg 0x%02x", reg);
        uint8_t next[2] = {reg, static_cast<uint8_t>((v | set_mask) & ~clear_mask)};
        return i2c_master_transmit(dev, next, sizeof(next), 100);
    };

    if (err == ESP_OK) err = update(kRegIoDir, kWlanBit, 0);         // 出力にする
    if (err == ESP_OK) err = update(kRegHiZ, 0, kWlanBit);           // Hi-Z を解除する
    if (err == ESP_OK) err = update(kRegOut, kWlanBit, 0);           // High = 電源 ON

    if (dev) i2c_master_bus_rm_device(dev);
    // M5GFX がタッチ IC のために同じ I2C を自前で扱うので、バスは掴んだままにしない。
    i2c_del_master_bus(bus);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 power on failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "C6 power enabled (PI4IOE5V6408@0x44 pin0, Hi-Z cleared)");
    // C6 のブートを待つ。esp-hosted が SDIO を叩く前に立ち上がっている必要がある。
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}

esp_err_t wifi_start(void)
{
    if (!s_events) s_events = xEventGroupCreate();

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
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&wifi_cmd), TAG, "reg wifi cmd");

    const esp_console_cmd_t status_cmd = {
        .command  = "wifi-status",
        .help     = "WiFi の接続状態を表示する",
        .hint     = nullptr,
        .func     = &cmd_wifi_status,
        .argtable = nullptr,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&status_cmd), TAG, "reg status cmd");

    return esp_console_start_repl(repl);
}
