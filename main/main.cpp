// Tab5 起動確認: 画面に文字を出し、ロット判別に必要な情報をログに残す。
#include <M5GFX.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <cstdio>

static const char* TAG = "boot";
static M5GFX display;

// M5GFX はパネル・タッチの判別結果を NVS にキャッシュする。NVS を初期化しておかないと
// 毎起動でフルプローブ（タッチ IC のファームウェア待ちを含む）が走って起動が遅くなる。
static void init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: model=%d cores=%d rev=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "heap: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    init_nvs();

    int64_t t0 = esp_timer_get_time();
    if (!display.init()) {
        ESP_LOGE(TAG, "display.init() failed");
        return;
    }
    // ターミナル用途なので横向き固定。パネルはネイティブ縦 (720x1280) で来る。
    display.setRotation(1);

    // getBoard() でどのボードとして認識されたかを残す。パネルの世代差 (ILI9881C+GT911 / ST7123)
    // は M5GFX 側が自動判別するので、判別結果を実機ログで確認できるようにしておく。
    ESP_LOGI(TAG, "board=%d panel=%dx%d colordepth=%d init=%lldms",
             (int)display.getBoard(), (int)display.width(), (int)display.height(),
             (int)display.getColorDepth(), (esp_timer_get_time() - t0) / 1000);

    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.drawString("m5stacktab boot ok", 24, 24);

    // LovyanGFX は改行でカーソル x を 0 に戻すので、左マージンを保つには行ごとに置き直す。
    display.setFont(&fonts::Font2);
    char line[96];
    std::snprintf(line, sizeof(line), "panel   : %d x %d (depth %d, board %d)",
                  (int)display.width(), (int)display.height(),
                  (int)display.getColorDepth(), (int)display.getBoard());
    display.drawString(line, 24, 72);
    std::snprintf(line, sizeof(line), "psram   : %u KB free",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    display.drawString(line, 24, 96);
    std::snprintf(line, sizeof(line), "internal: %u KB free",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    display.drawString(line, 24, 120);

    // 生存確認。WDT リセットや再起動ループを monitor で見分けるため。
    for (int i = 1;; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive %ds internal=%u psram=%u", i * 10,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
