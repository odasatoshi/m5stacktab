// Tab5 起動確認: 画面に文字を出し、ロット判別に必要な情報をログに残す。
#include <M5GFX.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_chip_info.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "boot";
static M5GFX display;

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: model=%d cores=%d rev=%d", chip.model, chip.cores, chip.revision);
    ESP_LOGI(TAG, "heap: internal=%u psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!display.init()) {
        ESP_LOGE(TAG, "display.init() failed");
        return;
    }
    // ターミナル用途なので横向き固定。パネルはネイティブ縦 (720x1280) で来る。
    display.setRotation(1);

    // getBoard() でどのボードとして認識されたかを残す。パネルの世代差 (ILI9881C+GT911 / ST7123)
    // は M5GFX 側が自動判別するので、判別結果を実機ログで確認できるようにしておく。
    ESP_LOGI(TAG, "board=%d panel=%dx%d colordepth=%d",
             (int)display.getBoard(), (int)display.width(), (int)display.height(),
             (int)display.getColorDepth());

    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setCursor(24, 24);
    display.printf("m5stacktab boot ok\n");
    display.setFont(&fonts::Font2);
    display.setCursor(24, 72);
    display.printf("panel   : %d x %d (depth %d, board %d)\n",
                   (int)display.width(), (int)display.height(),
                   (int)display.getColorDepth(), (int)display.getBoard());
    display.printf("psram   : %u KB free\n",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    display.printf("internal: %u KB free\n",
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    // 生存確認。WDT リセットや再起動ループを monitor で見分けるため。
    for (int sec = 0;; ++sec) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive %ds internal=%u psram=%u", sec * 10,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
