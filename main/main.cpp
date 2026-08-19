// Tab5 ファームウェアのエントリポイント。
// 今の段階では「画面にターミナルを描く土台」と「WiFi 接続」まで。
// SSH セッションを繋ぐのは #5 / #6。
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <M5GFX.h>
#include <esp_chip_info.h>
#include <esp_console.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#include <esp_partition.h>

#include "romaji.hpp"
#include "skk_dict.hpp"
#include "ssh.hpp"
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

ime::SkkDict                s_dict;
esp_partition_mmap_handle_t s_dict_mmap = 0;

// 辞書はフラッシュの dict パーティションを mmap してそのまま検索する（RAM に展開しない）。
bool dict_open()
{
    if (s_dict.is_open()) return true;
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "dict");
    if (!part) {
        std::printf("dict partition not found\n");
        return false;
    }
    const void* ptr = nullptr;
    esp_err_t   err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr,
                                         &s_dict_mmap);
    if (err != ESP_OK) {
        std::printf("mmap %u bytes failed: %s\n", (unsigned)part->size, esp_err_to_name(err));
        return false;
    }
    if (!s_dict.open(ptr, part->size)) {
        std::printf("dict not written yet or corrupt (mmap ok at %p)\n", ptr);
        return false;
    }
    std::printf("dict: %u entries mmapped from partition (%u bytes)\n", s_dict.count(),
                (unsigned)part->size);
    return true;
}

// ローマ字 → かな → 漢字 を一気に試す。
int cmd_conv(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: conv <romaji>    例: conv nihongo\n");
        return 1;
    }
    ime::Romaji r;
    std::string kana;
    for (const char* p = argv[1]; *p; ++p) r.input(*p, kana);
    r.flush(kana);
    std::printf("romaji: %s -> kana: %s\n", argv[1], kana.c_str());

    if (!dict_open()) return 1;

    std::vector<std::string> cands;
    const int64_t            t0 = esp_timer_get_time();
    const bool               hit = s_dict.lookup(kana, cands);
    const int64_t            us  = esp_timer_get_time() - t0;
    if (!hit) {
        std::printf("no kanji candidates (%lld us). katakana: %s\n", us,
                    ime::to_katakana(kana).c_str());
        return 0;
    }
    std::printf("lookup %lld us, %d candidates:", us, (int)cands.size());
    for (size_t i = 0; i < cands.size() && i < 8; ++i) std::printf(" %s", cands[i].c_str());
    std::printf("\n");

    // 画面にも出す（日本語表示の確認も兼ねる）
    std::string line = "\r\n" + std::string(argv[1]) + " -> " + kana + " -> ";
    for (size_t i = 0; i < cands.size() && i < 6; ++i) {
        line += cands[i];
        line += " ";
    }
    line += "\r\n";
    term->write(line);
    renderer->render(*term);
    return 0;
}

// スクロールバックの確認用（タッチのスワイプは #7 の UI で繋ぐ）。
int cmd_scroll(int argc, char** argv)
{
    const int delta = (argc > 1) ? atoi(argv[1]) : 3;
    const int moved = term->scroll_view(delta);
    renderer->render(*term, /*force=*/true);
    std::printf("moved %d (offset %d / %d lines held)\n", moved, term->view_offset(),
                term->scrollback_lines());
    return 0;
}

int cmd_ssh(int argc, char** argv)
{
    SshConfig cfg;
    if (argc == 1) {
        if (ssh_config_load(cfg) != ESP_OK || cfg.host.empty()) {
            std::printf("no saved connection. usage: ssh <user> <host> <password> [port]\n");
            return 1;
        }
    } else if (argc == 3) {
        // 秘密鍵 (sshkey パーティション) で認証する。パスワードは保存しない。
        cfg.user = argv[1];
        cfg.host = argv[2];
        if (esp_err_t err = ssh_config_save(cfg); err != ESP_OK) {
            std::printf("warning: could not save connection: %s\n", esp_err_to_name(err));
        }
    } else if (argc == 4 || argc == 5) {
        cfg.user     = argv[1];
        cfg.host     = argv[2];
        cfg.password = argv[3];
        cfg.port     = (argc == 5) ? (uint16_t)atoi(argv[4]) : 22;
        if (esp_err_t err = ssh_config_save(cfg); err != ESP_OK) {
            std::printf("warning: could not save connection: %s\n", esp_err_to_name(err));
        }
    } else {
        std::printf("usage: ssh <user> <host>                    # 秘密鍵で認証\n");
        std::printf("       ssh <user> <host> <password> [port]  # パスワードで認証\n");
        std::printf("       ssh                                  # 保存済み設定で再接続\n");
        return 1;
    }

    term->write("\r\n\033[33mconnecting to ");
    term->write(cfg.user + "@" + cfg.host + "...\033[m\r\n");
    renderer->render(*term);

    esp_err_t err = ssh_connect(cfg, renderer->cols(), renderer->rows());
    if (err != ESP_OK) {
        std::printf("connect failed: %s (%s)\n", esp_err_to_name(err), ssh_last_error());
        return 1;
    }
    std::printf("connecting... watch the screen\n");
    return 0;
}

int cmd_sshclose(int, char**)
{
    ssh_disconnect();
    std::printf("disconnected\n");
    return 0;
}

// キーボードが届くまでの入力手段。\e などのエスケープも送れる。
int cmd_key(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: key <text>   (\\e = ESC, \\n = LF, \\t = TAB)\n");
        return 1;
    }
    std::string out;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) out += ' ';
        out += unescape(argv[i]);
    }
    if (!ssh_is_connected()) {
        std::printf("not connected\n");
        return 1;
    }
    esp_err_t err = ssh_send(out.data(), out.size());
    if (err != ESP_OK) {
        std::printf("send failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

// 差分転送の効き目を測る。1 文字ずつ書いたときの再描画コストを見る。
int cmd_bench(int, char**)
{
    renderer->render(*term, /*force=*/true);
    const uint32_t full_us = renderer->last_render_us();
    const uint32_t full_px = renderer->last_pixels();

    // 1 文字入力を 20 回。実際のタイプ入力に相当する。
    uint32_t typing_us = 0, typing_px = 0;
    for (int i = 0; i < 20; ++i) {
        term->write("x");
        renderer->render(*term);
        typing_us += renderer->last_render_us();
        typing_px += renderer->last_pixels();
    }
    term->write("\r\n");
    renderer->render(*term);

    std::printf("full screen: %u us (%u px)\n", (unsigned)full_us, (unsigned)full_px);
    std::printf("20 keystrokes: %u us total, %u us each (%u px each)\n", (unsigned)typing_us,
                (unsigned)(typing_us / 20), (unsigned)(typing_px / 20));
    return 0;
}

void register_term_commands()
{
    const esp_console_cmd_t cmds[] = {
        {"term", "端末に文字列を流し込む (\\e で ESC)", "<text>", &cmd_term, nullptr, nullptr, nullptr},
        {"termtest", "色・全角・装飾のテストパターンを描画する", nullptr, &cmd_termtest, nullptr, nullptr, nullptr},
        {"termscroll", "40 行流してスクロールを見る", nullptr, &cmd_termscroll, nullptr, nullptr, nullptr},
        {"fonttest", "フォント描画経路の切り分け", nullptr, &cmd_fonttest, nullptr, nullptr, nullptr},
        {"ssh", "SSH 接続 (引数なしで保存済み設定)", "[<user> <host> <password> [port]]", &cmd_ssh,
         nullptr, nullptr, nullptr},
        {"sshclose", "SSH セッションを閉じる", nullptr, &cmd_sshclose, nullptr, nullptr, nullptr},
        {"key", "SSH にキー入力を送る", "<text>", &cmd_key, nullptr, nullptr, nullptr},
        {"conv", "ローマ字→かな→漢字を試す", "<romaji>", &cmd_conv, nullptr, nullptr, nullptr},
        {"scroll", "スクロールバックを動かす (正=過去へ)", "[lines]", &cmd_scroll, nullptr, nullptr,
         nullptr},
        {"bench", "描画コストを測る (全画面 vs 1 文字)", nullptr, &cmd_bench, nullptr, nullptr,
         nullptr},
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

    // スクロールバックは PSRAM に置く。1 行 = cols * sizeof(Cell) なので 1000 行で約 1.3MB。
    // コア側で確保させずにここで渡すのは、アロケータ (PSRAM) を選ぶため。
    constexpr int kScrollbackLines = 1000;
    auto* sb = static_cast<vt::Cell*>(heap_caps_malloc(
        sizeof(vt::Cell) * renderer->cols() * kScrollbackLines, MALLOC_CAP_SPIRAM));
    if (sb) {
        term->set_scrollback(sb, kScrollbackLines);
        ESP_LOGI(TAG, "scrollback: %d lines (%u KB in PSRAM)", kScrollbackLines,
                 (unsigned)(sizeof(vt::Cell) * renderer->cols() * kScrollbackLines / 1024));
    } else {
        ESP_LOGW(TAG, "scrollback disabled (PSRAM alloc failed)");
    }
    term->write("m5stacktab\r\n");
    char line[128];
    std::snprintf(line, sizeof(line), "grid %dx%d  cell %dx%d  panel %dx%d\r\n", renderer->cols(),
                  renderer->rows(), renderer->cell_w(), renderer->cell_h(), (int)display.width(),
                  (int)display.height());
    term->write(line);
    term->write("console: term / termtest / wifi / ssh / key\r\n");
    // DSR/CPR や DA の応答をリモートへ返す。vim などがこれを待つ。
    term->set_reply([](const std::string& s) {
        if (ssh_is_connected()) ssh_send(s.data(), s.size());
    });
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

        // リモートからの出力を端末に流す。term を触るのはこのループだけに保つ。
        uint8_t rx[1024];
        for (;;) {
            size_t n = ssh_receive(rx, sizeof(rx));
            if (n == 0) break;
            term->write(rx, n);
        }

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
