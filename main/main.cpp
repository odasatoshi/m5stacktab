// Tab5 ファームウェアのエントリポイント。
// 今の段階では「画面にターミナルを描く土台」と「WiFi 接続」まで。
// SSH セッションを繋ぐのは #5 / #6。
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <M5GFX.h>
#include <esp_chip_info.h>
#include <esp_console.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include "term_render.hpp"
#include "vt100.hpp"
#include "wifi.hpp"

namespace {

const char* TAG = "boot";

M5GFX                          display;
std::unique_ptr<TermRenderer>  renderer;
std::unique_ptr<vt::Terminal>  term;

// M5GFX はパネル・タッチの判別結果を NVS にキャッシュする。NVS を初期化しておかないと
// 毎起動でフルプローブ（タッチ IC のファームウェア待ちを含む）が走る。
void init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// コンソールから任意のバイト列を端末に流し込むための最小のエスケープ展開。
std::string unescape(const char* src)
{
    std::string out;
    for (const char* p = src; *p; ++p) {
        if (*p != '\\') {
            out += *p;
            continue;
        }
        switch (*++p) {
            case 'e': out += '\033'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'a': out += '\a'; break;
            case '\\': out += '\\'; break;
            case '\0': return out;
            default: out += *p; break;
        }
    }
    return out;
}

int cmd_term(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: term <text>   (\\e = ESC, \\n \\r \\t \\a も使える)\n");
        return 1;
    }
    std::string s;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) s += ' ';
        s += unescape(argv[i]);
    }
    term->write(s);
    renderer->render(*term);
    std::printf("wrote %d bytes, redrew %d rows in %u us (draw %u / push %u)\n", (int)s.size(),
                renderer->last_rows_drawn(), (unsigned)renderer->last_render_us(),
                (unsigned)renderer->last_draw_us(), (unsigned)renderer->last_push_us());
    return 0;
}

// 色・全角・装飾・スクロールをまとめて出すテストパターン。実機の目視確認用。
int cmd_termtest(int, char**)
{
    std::string s = "\033[2J\033[H";
    s += "m5stacktab terminal renderer test\r\n";
    s += "\033[1mbold\033[m \033[4munderline\033[m \033[7mreverse\033[m \033[9mstrike\033[m\r\n";

    s += "16 colors: ";
    for (int i = 0; i < 16; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\033[48;5;%dm  ", i);
        s += buf;
    }
    s += "\033[m\r\n";

    s += "216 cube : ";
    for (int i = 16; i < 232; i += 3) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\033[48;5;%dm ", i);
        s += buf;
    }
    s += "\033[m\r\n";

    s += "grayscale: ";
    for (int i = 232; i < 256; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\033[48;5;%dm  ", i);
        s += buf;
    }
    s += "\033[m\r\n";

    s += "24bit    : ";
    for (int i = 0; i < 48; ++i) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "\033[48;2;%d;%d;%dm ", i * 5, 255 - i * 5, 128);
        s += buf;
    }
    s += "\033[m\r\n";

    s += "日本語    : 全角の桁が合っているか確認する。あいうえお漢字カタカナ１２３ＡＢＣ\r\n";
    s += "mixed    : abcあいうdefかきくghi\r\n";
    s += "罫線     : ┌───┬───┐ │ │ │ └───┴───┘ ━━━ ═══\r\n";
    s += "\033[32mgreen\033[m \033[38;5;208morange\033[m \033[38;2;255;0;255mmagenta24\033[m\r\n";
    s += "tab      : a\tb\tc\td\r\n";
    return cmd_term(2, (char**)(const char*[]){(char*)"term", (char*)s.c_str()}) == 0 ? 0 : 1;
}

int cmd_termscroll(int, char**)
{
    for (int i = 0; i < 40; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "scroll line %d あいう\r\n", i);
        term->write(buf);
    }
    renderer->render(*term);
    std::printf("redrew %d rows in %u us (draw %u / push %u)\n", renderer->last_rows_drawn(),
                (unsigned)renderer->last_render_us(), (unsigned)renderer->last_draw_us(),
                (unsigned)renderer->last_push_us());
    return 0;
}

// フォント描画の切り分け用。どの経路でグリフが壊れるかを画面で見る。
int cmd_fonttest(int, char**)
{
    display.fillScreen(TFT_BLACK);
    display.setFont(&fonts::efontJA_24);
    display.setTextColor(TFT_WHITE, TFT_BLACK);

    const char* sample = "ABCあいう漢字123";

    display.drawString("1 drawString(gfx)", 10, 10);
    display.drawString(sample, 320, 10);

    // drawChar を直接。戻り値は送り幅なので、それも表示する。
    display.drawString("2 drawChar(gfx)", 10, 40);
    int      x   = 320;
    uint32_t cps[] = {'A', 'B', 'C', 0x3042, 0x3044, 0x3046, 0x6F22, 0x5B57, '1', '2', '3'};
    std::string widths;
    for (uint32_t cp : cps) {
        size_t w = display.drawChar(static_cast<uint16_t>(cp), x, 40);
        x += (int)w;
        widths += std::to_string((int)w) + " ";
    }
    display.drawString(("3 advance: " + widths).c_str(), 10, 70);

    display.drawString("4 print(gfx)", 10, 100);
    display.setCursor(320, 100);
    display.print(sample);

    // スプライト経由 (renderer と同じ経路)
    M5Canvas sp(&display);
    sp.setPsram(false);
    sp.setColorDepth(16);
    sp.setFont(&fonts::efontJA_24);
    if (sp.createSprite(900, 30)) {
        sp.fillSprite(TFT_NAVY);
        sp.setTextColor(TFT_WHITE, TFT_NAVY);
        sp.drawString(sample, 0, 0);
        int sx = 300;
        for (uint32_t cp : cps) sx += (int)sp.drawChar(static_cast<uint16_t>(cp), sx, 0);
        sp.pushSprite(320, 130);
        sp.deleteSprite();
    }
    display.drawString("5 sprite drawString+drawChar", 10, 130);

    std::printf("fontWidth=%d fontHeight=%d textWidth(A)=%d textWidth(sample)=%d\n",
                (int)display.fontWidth(), (int)display.fontHeight(), (int)display.textWidth("A"),
                (int)display.textWidth(sample));
    return 0;
}

void register_term_commands()
{
    const esp_console_cmd_t cmds[] = {
        {"term", "端末に文字列を流し込む (\\e で ESC)", "<text>", &cmd_term, nullptr, nullptr, nullptr},
        {"termtest", "色・全角・装飾のテストパターンを描画する", nullptr, &cmd_termtest, nullptr, nullptr, nullptr},
        {"termscroll", "40 行流してスクロールを見る", nullptr, &cmd_termscroll, nullptr, nullptr, nullptr},
        {"fonttest", "フォント描画経路の切り分け", nullptr, &cmd_fonttest, nullptr, nullptr, nullptr},
    };
    for (const auto& c : cmds) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_console_cmd_register(&c));
}

}  // namespace

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
    ESP_LOGI(TAG, "board=%d panel=%dx%d colordepth=%d init=%lldms",
             (int)display.getBoard(), (int)display.width(), (int)display.height(),
             (int)display.getColorDepth(), (esp_timer_get_time() - t0) / 1000);

    display.fillScreen(TFT_BLACK);

    renderer = std::make_unique<TermRenderer>(display);
    if (!renderer->begin()) {
        ESP_LOGE(TAG, "renderer init failed");
        return;
    }
    term = std::make_unique<vt::Terminal>(renderer->cols(), renderer->rows());
    term->write("m5stacktab\r\n");
    char line[128];
    std::snprintf(line, sizeof(line), "grid %dx%d  cell %dx%d  panel %dx%d\r\n", renderer->cols(),
                  renderer->rows(), renderer->cell_w(), renderer->cell_h(), (int)display.width(),
                  (int)display.height());
    term->write(line);
    term->write("console: term / termtest / termscroll / wifi / wifi-status\r\n");
    renderer->render(*term, /*force=*/true);
    ESP_LOGI(TAG, "first full draw: %d rows in %u us (draw %u / push %u)",
             renderer->last_rows_drawn(), (unsigned)renderer->last_render_us(),
             (unsigned)renderer->last_draw_us(), (unsigned)renderer->last_push_us());

    // WiFi。display.init() が C6 の電源 (IO エクスパンダ経由) を入れているので、必ずこの後。
    if (esp_err_t err = wifi_start(); err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(console_start());
    register_term_commands();

    // 描画とタッチのポーリング。キーボードが来るまではタッチが唯一の直接入力手段。
    int  last_log_s = 0;
    bool touching   = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        if (term->any_dirty()) renderer->render(*term);

        lgfx::touch_point_t tp;
        if (display.getTouch(&tp, 1)) {
            if (!touching) {
                touching = true;
                ESP_LOGI(TAG, "touch down x=%d y=%d (cell %d,%d)", tp.x, tp.y,
                         tp.x / renderer->cell_w(), tp.y / renderer->cell_h());
            }
        } else if (touching) {
            touching = false;
            ESP_LOGI(TAG, "touch up");
        }

        int now_s = (int)(esp_timer_get_time() / 1000000);
        if (now_s - last_log_s >= 10) {
            last_log_s = now_s;
            ESP_LOGI(TAG, "alive %ds wifi=%d internal=%u psram=%u", now_s, (int)wifi_is_connected(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
    }
}
