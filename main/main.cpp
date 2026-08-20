// Tab5 ファームウェアのエントリポイント。
// 今の段階では「画面にターミナルを描く土台」と「WiFi 接続」まで。
// SSH セッションを繋ぐのは #5 / #6。
#include <cstdint>
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

#include <esp_netif.h>
#include <ping/ping_sock.h>
#include <arpa/inet.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <esp_partition.h>
#include <nvs.h>

#include "romaji.hpp"
#include "skk_dict.hpp"
#include "ssh.hpp"
#include "ts_client.hpp"
#include "disco.hpp"
#include "disco_responder.hpp"
#include "salsa20.hpp"
#include "netmap.hpp"
#include <driver/ppa.h>
#include <esp_memory_utils.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>

#include "rotate.hpp"
#include "ts_control.hpp"
#include "wg_netif.hpp"
#include "blake2s.hpp"
#include "kbd_ui.hpp"
#include "menu_ui.hpp"
#include "status_bar.hpp"
#include "noise.hpp"
#include "transport.hpp"
#include "term_render.hpp"
#include "vt100.hpp"
#include "wifi.hpp"

namespace {

const char* TAG = "boot";

// vt::Terminal と TermRenderer は **メインループとコンソールタスクの両方から触られる**
// （コンソールコマンドが term に書いて描画し、メインループも SSH の受信を流して描画する）。
// 保護しないと画面が壊れるうえ、描画時間の計測値も混ざって信用できなくなる。
SemaphoreHandle_t s_term_lock = nullptr;

class TermGuard {
public:
    TermGuard()
    {
        if (!s_term_lock) return;
        taken_ = xSemaphoreTake(s_term_lock, pdMS_TO_TICKS(2000)) == pdTRUE;
        if (!taken_) {
            // 黙って通すと、ロックを入れた目的（画面の破壊と計測値の混入）がそのまま再発する。
            // 呼び出し側は ok() を見て中断する。
            ESP_LOGE(TAG, "terminal lock timeout - skipping this operation");
        }
    }
    ~TermGuard() { if (taken_) xSemaphoreGive(s_term_lock); }
    bool ok() const { return taken_; }

private:
    bool taken_ = false;
};

// mbedtls_pk_parse_key は EC 鍵で RNG を要求する（座標ブラインディング）。
mbedtls_ctr_drbg_context* ts_drbg()
{
    static mbedtls_ctr_drbg_context ctr;
    static mbedtls_entropy_context  ent;
    static bool                     ready = false;
    if (!ready) {
        mbedtls_ctr_drbg_init(&ctr);
        mbedtls_entropy_init(&ent);
        static const char* pers = "keytest";
        ready = (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent,
                                       reinterpret_cast<const unsigned char*>(pers),
                                       std::strlen(pers)) == 0);
    }
    return ready ? &ctr : nullptr;
}

// cmd_ts から呼ぶので前方宣言する（定義は DISCO のセクション）。
void register_disco_peers(const ts::NetMap& map);
// DISCO のレスポンダ（定義は下）。ts の鍵設定から先に触る。
extern ts::DiscoResponder s_disco;
void maybe_bring_up_tunnel(const ts::NetMap& map, const std::string& assigned);
void wire_netif(wg::Netif& nif);

M5GFX                          display;
std::unique_ptr<TermRenderer>  renderer;
std::unique_ptr<vt::Terminal>  term;
std::unique_ptr<KeyboardUi>    keyboard;
std::unique_ptr<StatusBar>     status_bar;
std::unique_ptr<MenuUi>        menu;
// ステータスバーに使う高さ。セル 1 行ぶんにして端末のグリッドに合わせる。
int                            s_status_h = 0;

// 画面の配分を張り直す（定義は下）。ステータスバー / 端末 / キーボードの
// 高さの計算はここ 1 箇所しかない。分散させると片方だけ直して重なる。
void apply_layout();
// メニューの表示を切り替える。キーボードの表示と端末の行数も一緒に動く。
void set_menu_visible(bool show);
// メニュー表示中は端末を描かない（メニューの矩形を上書きしてしまう）。
void render_term(bool force = false);

// Tailscale の 3 つの鍵。node_priv は WireGuard の秘密鍵でもあるので、
// netmap が来たときにトンネルを張るために map handler からも読む。
uint8_t s_ts_machine_priv[32] = {};
uint8_t s_ts_node_priv[32]    = {};
uint8_t s_ts_disco_priv[32]   = {};
bool    s_ts_keys_ready       = false;

// トンネルの netif がどの鍵で上がっているか。Netif::up() は秘密鍵をコピーするので、
// 上がった後に差し替える手段が無い。張り替えの判断に使う。
enum class NetifKey { kNone, kOwn, kNode };
NetifKey    s_netif_key = NetifKey::kNone;
// 今トンネルを張っている相手。netmap ごとに選び直さないために覚える。
std::string s_tunnel_peer_key;
std::string s_tunnel_endpoint;
bool        s_tunnel_peer_valid = false;

// ステータスバーを見に行った最後の時刻。C6 への RPC なので毎フレームは叩かない。
int64_t s_last_status_us = 0;
// ステータスバーのタップ判定の高さ。描画は s_status_h (24px) だが、指で当てるには
// 狭すぎるので判定だけ広げる（24px は 3.5mm しかない）。
constexpr int kStatusTapH = 56;

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

// コンソールから任意のバイト列を端末やリモートに送るための最小のエスケープ展開。
//
// **コンソールで打つときはバックスラッシュを二重にする（`\\r`）。**
// esp_console_split_argv が argv を作る時点で 1 段はがすので、`\r` と書くと
// ここには `r` しか届かない（実機で確認: `key echo X\r` が送るのは "echo X" で
// CR が落ちる）。ヘルプがこれを書いていなかったせいで「SSH は繋がるのに
// コマンドが実行できない」の原因究明に時間がかかった。
//
// `^M` 形式も一度入れたが落とした。CR は `\\r` で送れるので新しい記法は
// 要らない一方、`^` を食う副作用で `key grep ^root ...` が黙って壊れる
// （`^r` が 0x12 になる）。任意のテキストをリモートに送る道具でそれは困る。
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

// 端末の内容をシリアルに吐く。実機の画面を見られない状況（CI、遠隔、
// エージェント実行）で SSH の出力や描画を検証する唯一の手段なので、
// 見た目ではなくセルの中身をそのまま出す。
//
// 出すのはライブ画面（cell）。scroll でスクロールバックを見ている間は
// 画面に出ているもの（view_cell）とずれる。
int cmd_termdump(int, char**)
{
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    std::printf("--- term %dx%d cursor=(%d,%d)%s ---\n", term->cols(), term->rows(),
                term->cursor_x(), term->cursor_y(), term->cursor_visible() ? "" : " hidden");
    // UTF-8 化は vt100 の row_text に任せる（ホストテストで全角の境界まで固めてある）。
    for (int y = 0; y < term->rows(); ++y) {
        std::printf("%2d|%s\n", y, term->row_text(y).c_str());
    }
    std::printf("--- end (bell=%u) ---\n", (unsigned)term->bell_count());
    return 0;
}

int cmd_term(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: term <text>   (\\\\e=ESC \\\\r=CR \\\\n \\\\t \\\\a"
                    " — コンソールでは二重にする)\n");
        return 1;
    }
    std::string s;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) s += ' ';
        s += unescape(argv[i]);
    }
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    term->write(s);
    render_term();
    // 計測値もロックの中で読む。外に出すとメインループの render に上書きされる。
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
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    for (int i = 0; i < 40; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "scroll line %d あいう\r\n", i);
        term->write(buf);
    }
    render_term();
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
    std::printf("dict: %u entries mmapped from partition (%u bytes)\n",
                (unsigned)s_dict.count(), (unsigned)part->size);
    return true;
}

// ローマ字 → かな → 漢字 を一気に試す。
// ローマ字を IME に通して、その結果を SSH に送る（かなまで。漢字変換はしない）。
// シリアルコンソールは非 ASCII を argv に通さない（実機で確認: `key echo 日本語` は
// echo に空の引数を渡す）ので、日本語を送る経路を ASCII だけで叩くために用意した。
// タッチ → フリックの層は通らないが、IME の出力から先（ssh_send → リモート →
// 端末描画）という未検証の継ぎ目はこれで通る。
int cmd_keyj(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: keyj <romaji>...   例: keyj nihongo tsukuba\n");
        return 1;
    }
    if (!ssh_is_connected()) {
        std::printf("not connected\n");
        return 1;
    }
    std::string out;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) out += ' ';
        ime::Romaji r;
        std::string kana;
        for (const char* p = argv[i]; *p; ++p) r.input(*p, kana);
        r.flush(kana);
        out += kana;
    }
    std::printf("sending %d bytes:", (int)out.size());
    for (unsigned char ch : out) std::printf(" %02x", ch);
    std::printf("\n");
    esp_err_t err = ssh_send(out.data(), out.size());
    if (err != ESP_OK) {
        std::printf("send failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

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
    {
        TermGuard guard;
        term->write(line);
        render_term();
    }
    return 0;
}

// スクロールバックの確認用（タッチのスワイプは #7 の UI で繋ぐ）。
int cmd_scroll(int argc, char** argv)
{
    const int delta = (argc > 1) ? atoi(argv[1]) : 3;
    TermGuard guard;
    const int moved = term->scroll_view(delta);
    render_term(/*force=*/true);
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
        // host:port も受ける。これがないと 22 番以外では鍵認証が使えない
        // （パスワード認証の 4 引数形式にしかポート指定が無かった）。
        // **コロンが 1 個のときだけ**解析する。無条件に rfind すると
        // IPv6 リテラル（fd7a:115c:a1e0::5）を切り刻む。getaddrinfo は
        // AF_UNSPEC なので IPv6 は実際に解決できる経路。
        if (const size_t colon = cfg.host.rfind(':');
            colon != std::string::npos && cfg.host.find(':') == colon) {
            const int port = atoi(cfg.host.c_str() + colon + 1);
            if (port > 0 && port <= 65535) {
                cfg.port = (uint16_t)port;
                cfg.host.resize(colon);
            }
        }
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
        std::printf("usage: ssh <user> <host>[:<port>]           # 秘密鍵で認証\n");
        std::printf("       ssh <user> <host> <password> [port]  # パスワードで認証\n");
        std::printf("       ssh                                  # 保存済み設定で再接続\n");
        return 1;
    }

    {
        TermGuard guard;
        term->write("\r\n\033[33mconnecting to ");
        term->write(cfg.user + "@" + cfg.host + "...\033[m\r\n");
        render_term();
    }

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
        std::printf("usage: key <text>   (\\\\r=CR \\\\e=ESC \\\\t=TAB"
                    " — コンソールでは二重にする)\n");
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
    // 計測中に別タスクが描画すると値が混ざるので、区間全体を押さえる。
    TermGuard guard;
    render_term(/*force=*/true);
    const uint32_t full_us = renderer->last_render_us();
    const uint32_t full_px = renderer->last_pixels();

    // 1 文字入力を 20 回。実際のタイプ入力に相当する。
    uint32_t typing_us = 0, typing_px = 0;
    for (int i = 0; i < 20; ++i) {
        term->write("x");
        render_term();
        typing_us += renderer->last_render_us();
        typing_px += renderer->last_pixels();
    }
    term->write("\r\n");
    render_term();

    std::printf("full screen: %u us (%u px)\n", (unsigned)full_us, (unsigned)full_px);
    std::printf("20 keystrokes: %u us total, %u us each (%u px each)\n", (unsigned)typing_us,
                (unsigned)(typing_us / 20), (unsigned)(typing_px / 20));
    return 0;
}

int cmd_kbd(int argc, char** argv)
{
    const bool show = (argc < 2) || (std::string(argv[1]) != "off");
    TermGuard  guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    if (menu && menu->visible()) {
        // メニュー表示中はキーボードを隠している。ここで出すと配分が食い違う。
        std::printf("menu is shown (menu hide してから)\n");
        return 1;
    }
    keyboard->set_visible(show);
    apply_layout();
    keyboard->draw();
    renderer->render(*term, /*force=*/true);
    std::printf("keyboard %s, terminal %dx%d\n", show ? "shown" : "hidden", renderer->cols(),
                renderer->rows());
    return 0;
}

// WireGuard の暗号とハンドシェイクを実機で検証する（ホストテストと同じ流れ）。
int cmd_wgtest(int, char**)
{
    const auto& c = wg::default_crypto();

    // BLAKE2s の既知値
    uint8_t h[32];
    wg::blake2s(h, 32, reinterpret_cast<const uint8_t*>("abc"), 3);
    const char* want = "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982";
    char        got[65] = {};
    for (int i = 0; i < 32; ++i) std::snprintf(got + i * 2, 3, "%02x", h[i]);
    std::printf("blake2s(abc): %s\n", std::strcmp(got, want) == 0 ? "ok" : got);

    // X25519 (RFC 7748 のベクタ)
    uint8_t priv[32] = {0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1,
                        0x72, 0x51, 0xb2, 0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0,
                        0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
    uint8_t pub[32];
    int64_t t0 = esp_timer_get_time();
    bool    ok = c.dh_pubkey(pub, priv);
    int64_t dh_us = esp_timer_get_time() - t0;
    for (int i = 0; i < 32; ++i) std::snprintf(got + i * 2, 3, "%02x", pub[i]);
    const char* want_pub = "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
    std::printf("x25519 pubkey: %s (%lld us)\n",
                (ok && std::strcmp(got, want_pub) == 0) ? "ok" : got, dh_us);

    // ハンドシェイクを自分同士で往復させる
    uint8_t a_priv[32], b_priv[32], a_pub[32], b_pub[32];
    if (!c.random_bytes(a_priv, 32) || !c.random_bytes(b_priv, 32) ||
        !c.dh_pubkey(a_pub, a_priv) || !c.dh_pubkey(b_pub, b_priv)) {
        std::printf("key generation failed (no entropy?)\n");
        return 1;
    }

    wg::Handshake ini(c), res(c);
    if (!ini.set_keys(a_priv, b_pub) || !res.set_keys(b_priv, a_pub)) {
        std::printf("set_keys failed\n");
        return 1;
    }
    const uint8_t ts[12] = {0x40};
    uint8_t       m1[148], m2[92], st[32], tsout[12];
    wg::Keypair   ik, rk;

    t0 = esp_timer_get_time();
    bool hs = ini.create_initiation(m1, 0x1234, ts) && res.consume_initiation(m1, st, tsout) &&
              res.create_response(m2, 0x5678, rk) && ini.consume_response(m2, ik);
    const int64_t hs_us = esp_timer_get_time() - t0;
    const bool    match = hs && std::memcmp(ik.send, rk.recv, 32) == 0 &&
                       std::memcmp(ik.recv, rk.send, 32) == 0;
    std::printf("noise IK handshake: %s (%lld us for both sides)\n", match ? "ok" : "FAILED",
                hs_us);

    if (!match) return 1;

    // トランスポートの往復とリプレイ拒否
    wg::Transport tx(c), rx(c);
    tx.set_keypair(ik);
    rx.set_keypair(rk);
    uint8_t      pkt[256], out[256];
    const char*  msg = "wireguard on esp32-p4";
    t0 = esp_timer_get_time();
    const size_t n = tx.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>(msg),
                                std::strlen(msg));
    bool         valid = false;
    const size_t got_len = rx.decrypt(out, sizeof(out), pkt, n, &valid);
    const int64_t rt_us = esp_timer_get_time() - t0;
    const bool    replay_rejected = rx.decrypt(out, sizeof(out), pkt, n, &valid) == 0;
    std::printf("transport: %s (%lld us round trip), replay rejected: %s\n",
                (got_len == std::strlen(msg) && std::memcmp(out, msg, got_len) == 0) ? "ok"
                                                                                     : "FAILED",
                rt_us, replay_rejected ? "yes" : "NO");
    return 0;
}

// Tailscale / Headscale の制御プレーンに繋いでみる。
// 鍵は NVS に保存して再利用する（毎回新しい鍵だとノードが増え続ける）。
// ts は別タスクで走らせる（下の cmd_ts のコメント参照）。
TaskHandle_t s_ts_task    = nullptr;
ts::Client*  s_ts_client  = nullptr;
int64_t      s_ts_started_us = 0;

void ts_task(void* arg)
{
    auto* client = static_cast<ts::Client*>(arg);
    const bool ok = client->run_once();
    // run_once() から戻ったあとなので、この参照を書き換える者はもういない。
    const auto& st = client->status();
    ESP_LOGI(TAG, "ts finished: %s state=%d registered=%d map_messages=%u", ok ? "ok" : "failed",
             (int)st.state, st.registered ? 1 : 0, (unsigned)st.map_messages);
    // X25519 と HTTP/2 のバッファが重なる経路なので、残量は記録しておく
    // （CLAUDE.md がスタック不足を名指しのハマりどころにしている）。
    ESP_LOGI(TAG, "ts task stack headroom: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    if (!st.assigned_address.empty()) {
        std::string line = "\r\n\033[32mtailscale: " + st.assigned_address + "\033[m\r\n";
        TermGuard guard;
        if (guard.ok()) {
            term->write(line);
            render_term();
        }
    }
    if (!st.error.empty()) ESP_LOGW(TAG, "ts: %s", st.error.c_str());
    s_ts_task = nullptr;
    vTaskDelete(nullptr);
}

int cmd_ts(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: ts <host> <authkey> [port] [capver]\n");
        return 1;
    }
    // **共有オブジェクトを触る前に弾く。** ts::Client は関数ローカル static で
    // 実体が 1 つしかないので、long-poll 中に set_config / set_map_handler を
    // 呼ぶと走行中のタスクが読んでいる std::string と std::function を
    // 差し替えることになる（use-after-free）。
    if (s_ts_task) {
        std::printf("already running (ts-status で状態、ts-stop で停止)\n");
        return 1;
    }
    // machine / node / disco の 3 つは別の鍵にする（役割ごとに分離する）。
    // node_priv は **WireGuard の秘密鍵でもある**（Tailscale の設計）。
    // netmap が来たときにトンネルを張るので、map handler から見えるところに置く。
    uint8_t* machine_priv = s_ts_machine_priv;
    uint8_t* node_priv    = s_ts_node_priv;
    uint8_t* disco_priv   = s_ts_disco_priv;
    nvs_handle_t   nvs;
    bool           have_keys = false;
    if (nvs_open("ts", NVS_READWRITE, &nvs) == ESP_OK) {
        auto load = [&](const char* key, uint8_t* out) {
            size_t len = 32;
            return nvs_get_blob(nvs, key, out, &len) == ESP_OK && len == 32;
        };
        have_keys = load("mkey", machine_priv) && load("nkey", node_priv) &&
                    load("dkey", disco_priv);
        if (!have_keys) {
            const auto& c = wg::default_crypto();
            if (!c.random_bytes(machine_priv, 32) || !c.random_bytes(node_priv, 32) ||
                !c.random_bytes(disco_priv, 32)) {
                std::printf("key generation failed (no entropy)\n");
                nvs_close(nvs);
                return 1;
            }
            // 保存に失敗したら黙って続けない。毎回新しい鍵で登録するとノードが増え続ける。
            esp_err_t err = nvs_set_blob(nvs, "mkey", machine_priv, 32);
            if (err == ESP_OK) err = nvs_set_blob(nvs, "nkey", node_priv, 32);
            if (err == ESP_OK) err = nvs_set_blob(nvs, "dkey", disco_priv, 32);
            if (err == ESP_OK) err = nvs_commit(nvs);
            if (err != ESP_OK) {
                std::printf("could not save keys: %s (refusing to register with throwaway keys)\n",
                            esp_err_to_name(err));
                nvs_close(nvs);
                return 1;
            }
            std::printf("generated and saved new machine/node/disco keys\n");
            have_keys = true;
        }
        nvs_close(nvs);
    }
    if (!have_keys) {
        std::printf("could not load or create keys\n");
        return 1;
    }
    // これ以降 map handler が node_priv を WireGuard の秘密鍵として使う。
    s_ts_keys_ready = true;
    // **DISCO の鍵はここで入れる。** map handler は register_disco_peers を
    // 先に呼ぶので、鍵が無いと add_peer が全部拒否されて peers=0 になる
    // （実機で踏んだ: pings が来ても unknown として捨てられる）。
    if (!s_disco.set_key(disco_priv)) {
        std::printf("disco key setup failed\n");
        s_ts_keys_ready = false;
        return 1;
    }

    static ts::Client client;
    ts::ClientConfig  cfg;
    cfg.host     = argv[1];
    cfg.auth_key = argv[2];
    cfg.port     = (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 80;
    cfg.capability_version = (argc > 4) ? static_cast<uint16_t>(atoi(argv[4])) : 131;
    cfg.hostname = "m5stack-tab5";
    // 自分のエンドポイント。
    // ponytail: STUN も portmap も実装していないので LAN アドレスしか申告しない。
    // かつ DERP を持たない（PreferredDERP=0 = ホーム無し）ので、相手から
    // 中継経由で最初の Ping を投げてもらう手段もない。**つまり同一 LAN 限定で、
    // NAT 越えは成立しない。** 越えたくなったら STUN と DERP のどちらかが必要 → #11 の続き。
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            char ep[32];
            std::snprintf(ep, sizeof(ep), IPSTR ":41641", IP2STR(&ip.ip));
            cfg.endpoints.push_back(ep);
        }
    }
    if (!client.set_keys(machine_priv, node_priv, disco_priv)) {
        std::printf("public key derivation failed\n");
        s_ts_keys_ready = false;
        return 1;
    }
    client.set_config(cfg);
    // netmap が来たらピアの disco 公開鍵を登録する。これが DISCO の前提。
    client.set_map_handler([](const std::string& json) {
        ts::NetMap map;
        if (!ts::parse_netmap(json, &map)) return;
        if (map.keepalive) return;
        register_disco_peers(map);
        // 自分のアドレスは status 側で拾っている（netmap の Addresses）。
        const std::string assigned = s_ts_client ? s_ts_client->snapshot().assigned_address : "";
        if (!assigned.empty()) maybe_bring_up_tunnel(map, assigned);
    });

    // **別タスクで走らせる。** run_once() は map の long-poll を最長 600 秒
    // 保持する（それが正しい動作で、DISCO の往復はこのストリームが開いている
    // 間に起きる）。コンソールタスクから呼ぶと 10 分間 REPL が死んで、
    // ストリームを開けたまま `wg disco` を見ることすらできない。
    std::printf("connecting to %s:%u (capver %u) in background...\n", cfg.host.c_str(), cfg.port,
                cfg.capability_version);
    // 起動できてから公開する。先に入れると、鍵導出や xTaskCreate で失敗した後に
    // ts-status が「一度も動いていないのに finished」と表示する。
    s_ts_started_us = esp_timer_get_time();
    s_ts_client     = &client;
    // X25519 が 1 回で 10KB 近く使い、HTTP/2 のバッファも重なるので広く取る。
    if (xTaskCreate(&ts_task, "ts", 32768, &client, 5, &s_ts_task) != pdPASS) {
        s_ts_task       = nullptr;
        s_ts_client     = nullptr;
        s_ts_keys_ready = false;
        std::printf("xTaskCreate failed\n");
        return 1;
    }
    return 0;
}

// ts の状態を見る。long-poll を保持している間も見られる必要がある。
int cmd_ts_status(int, char**)
{
    if (!s_ts_client) {
        std::printf("ts: not started\n");
        return 1;
    }
    // **参照ではなく値で受ける。** ts タスクが st_ の std::string を書くので、
    // 参照のまま c_str() を printf に渡すと読んでいる最中に解放され得る。
    const ts::ClientStatus st = s_ts_client->snapshot();
    std::printf("ts: %s (%lld ms)\n", s_ts_task ? "running" : "finished",
                (esp_timer_get_time() - s_ts_started_us) / 1000);
    std::printf("  state=%d registered=%d map_messages=%u keepalives=%u\n", (int)st.state,
                st.registered ? 1 : 0, (unsigned)st.map_messages, (unsigned)st.keepalives);
    if (!st.assigned_address.empty()) {
        std::printf("  assigned address: %s\n", st.assigned_address.c_str());
    }
    if (!st.domain.empty()) std::printf("  domain: %s\n", st.domain.c_str());
    if (!st.error.empty()) std::printf("  error: %s\n", st.error.c_str());
    return 0;
}

int cmd_ts_stop(int, char**)
{
    if (!s_ts_client || !s_ts_task) {
        std::printf("ts: not running\n");
        return 1;
    }
    s_ts_client->stop();
    std::printf("stop requested\n");
    return 0;
}

// ステータスバーとメニューに出す状態を集める。
// 状態の出所（WiFi / ts / wg）が増えてもここだけ触れば済むようにする。
StatusBar::Info gather_status()
{
    StatusBar::Info info{};
    info.menu_open = (menu && menu->visible());
    info.wifi_up = wifi_status(info.ssid, sizeof(info.ssid), &info.rssi, info.ip, sizeof(info.ip));

    auto& nif = wg::netif_instance();
    // **値で受ける。** ts タスクが st_ の std::string を書くので、参照のまま
    // c_str() を触ると読んでいる最中に解放され得る。
    const ts::ClientStatus ts = s_ts_client ? s_ts_client->snapshot() : ts::ClientStatus{};
    // **registered は走っている間だけ見る。** st_.registered は false に戻らないので、
    // そのまま渡すと ts を一度動かしただけで「VPN ...」が永久に黄色のまま残り、
    // 同じ画面のメニュー側 ("Tailscale: stopped") と食い違う。
    const bool ts_running = (s_ts_task != nullptr);
    info.vpn = ui::vpn_state(ts_running, ts_running && ts.registered, nif.is_up(),
                             nif.handshake_done());
    if (!ts.assigned_address.empty()) {
        std::snprintf(info.vpn_ip, sizeof(info.vpn_ip), "%s", ts.assigned_address.c_str());
    }
    return info;
}

MenuUi::Info gather_menu_info(const StatusBar::Info& si)
{
    MenuUi::Info mi{};
    SshConfig    cfg;
    if (ssh_config_load(cfg) == ESP_OK && !cfg.host.empty()) {
        std::snprintf(mi.ssh_target, sizeof(mi.ssh_target), "%s@%s:%u", cfg.user.c_str(),
                      cfg.host.c_str(), (unsigned)cfg.port);
    }
    if (s_ts_client) {
        const ts::ClientStatus st = s_ts_client->snapshot();
        const std::string      addr = st.assigned_address.empty() ? "" : " " + st.assigned_address;
        std::snprintf(mi.ts_state, sizeof(mi.ts_state), "%s%s",
                      s_ts_task ? "running" : (st.registered ? "stopped" : "off"), addr.c_str());
    } else {
        std::snprintf(mi.ts_state, sizeof(mi.ts_state), "off");
    }
    auto& nif = wg::netif_instance();
    std::snprintf(mi.wg_state, sizeof(mi.wg_state), "%s", !nif.is_up() ? "down"
                                                          : nif.handshake_done() ? "up"
                                                                                 : "no handshake");
    std::snprintf(mi.wifi, sizeof(mi.wifi), "%s", si.wifi_up ? si.ssid : "(未接続)");
    return mi;
}

// 画面の配分を張り直す。
//
// **高さの計算はここだけに置く。** 以前は起動時と `kbd` コマンドで別々に
// 計算していて、`kbd` 側がステータスバーの分を引いていなかったため、
// 端末の最下行が IME の候補帯に 24px 重なっていた。
//
// メニューを出している間はキーボードを隠す。隠さないと、メニューはキーボードの
// 領域を覆わないので指でキーを押せてしまい、その出力が端末経由でメニューの
// 矩形に描かれて表示が崩れる（かつ押した文字は見えないまま端末に入る）。
void apply_layout()
{
    if (!renderer || !keyboard) return;
    // **キーボードの表示はここで決めない。** 決めると `kbd off` を上書きしてしまう。
    // 表示を変えたい側（set_menu_visible / cmd_kbd）が先に set_visible してから呼ぶ。
    const int avail = (int)display.height() - s_status_h;
    const int rows  = keyboard->visible()
                          ? (avail - keyboard->height()) / renderer->cell_h()
                          : avail / renderer->cell_h();
    renderer->set_rows(rows);
    if (term) term->resize(renderer->cols(), rows);
    if (ssh_is_connected()) ssh_resize(renderer->cols(), rows);
    if (menu) menu->set_area(s_status_h, avail - keyboard->height());
}

void render_term(bool force)
{
    if (!renderer || !term) return;
    // メニューが開いている間は描かない。term->write は続けるので内容は
    // 失われず、閉じるときの force render で追いつく。
    if (menu && menu->visible()) return;
    renderer->render(*term, force);
}

void set_menu_visible(bool show)
{
    if (!menu) return;
    menu->set_visible(show);
    // メニューはキーボードの領域を覆わないので、隠さないと指でキーを押せてしまう。
    // ponytail: 端末に戻るときは必ずキーボードを出す（`kbd off` の状態は覚えない）。
    // メニューから端末に入る流れでは打ちたいはずなので、今はこれで足りる。
    keyboard->set_visible(!show);
    if (show) menu->set_info(gather_menu_info(gather_status()));
    apply_layout();
    if (show) {
        menu->draw(/*force=*/true);
    } else {
        keyboard->draw();
        renderer->render(*term, /*force=*/true);
    }
    // ラベルを MENU / CLOSE に切り替える。開閉のたびに必ず描き直す。
    status_bar->draw(gather_status(), /*force=*/true);
}

// メニューの操作。#15 の純正キーボードが来るまでは、上下や Enter を送る手段が
// タップ以外に無いのでここから叩く。キーの解釈は ui::Key に集約してあるので、
// キーボードが来たらそのキーコードをここと同じ enum に写すだけで済む。
int cmd_menu(int argc, char** argv)
{
    if (!menu) {
        std::printf("menu not ready\n");
        return 1;
    }
    const std::string a = (argc > 1) ? argv[1] : "show";
    TermGuard         guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    if (a == "show" || a == "hide") {
        const bool show = (a == "show");
        set_menu_visible(show);
        std::printf("menu %s, terminal %dx%d\n", show ? "shown" : "hidden", renderer->cols(),
                    renderer->rows());
        return 0;
    }
    if (!menu->visible()) {
        std::printf("menu is hidden (menu show)\n");
        return 1;
    }
    ui::Key k;
    if (a == "up") k = ui::Key::kUp;
    else if (a == "down") k = ui::Key::kDown;
    else if (a == "enter") k = ui::Key::kEnter;
    else if (a == "esc" || a == "back") k = ui::Key::kEsc;
    else if (a == "left") k = ui::Key::kLeft;
    else if (a == "right") k = ui::Key::kRight;
    else {
        std::printf("usage: menu [show|hide|up|down|enter|esc|left|right]\n");
        return 1;
    }
    menu->set_info(gather_menu_info(gather_status()));
    menu->refresh();
    menu->key(k);
    menu->draw();
    return 0;
}

// DISCO の Ping/Pong を実機で往復させる。相手機が無くても、自分を peer として登録して
// UDP ループバックで投げ合えば「レスポンダが正しく応答するか」を確かめられる。
int cmd_discoloop(int, char**)
{
    const auto& c = wg::default_crypto();

    // 2 者ぶんの disco 鍵を作る（片方が Tab5 のレスポンダ、もう片方が仮想のピア）。
    uint8_t me_priv[32], me_pub[32], peer_priv[32], peer_pub[32];
    if (!c.random_bytes(me_priv, 32) || !c.random_bytes(peer_priv, 32) ||
        !c.dh_pubkey(me_pub, me_priv) || !c.dh_pubkey(peer_pub, peer_priv)) {
        std::printf("key generation failed\n");
        return 1;
    }

    ts::DiscoResponder resp;
    if (!resp.set_key(me_priv) || !resp.add_peer(peer_pub)) {
        std::printf("responder setup failed\n");
        return 1;
    }

    // ピア側の共有鍵（beforenm）を作る
    uint8_t dh[32], peer_shared[32];
    if (!c.dh(dh, peer_priv, me_pub)) {
        std::printf("dh failed\n");
        return 1;
    }
    wg::box_beforenm(peer_shared, dh);

    // UDP でループバックに投げ合う
    const int sp = socket(AF_INET, SOCK_DGRAM, 0);
    const int sm = socket(AF_INET, SOCK_DGRAM, 0);
    if (sp < 0 || sm < 0) {
        std::printf("socket failed\n");
        if (sp >= 0) close(sp);
        if (sm >= 0) close(sm);
        return 1;
    }
    sockaddr_in peer_addr{}, me_addr{};
    peer_addr.sin_family      = me_addr.sin_family = AF_INET;
    peer_addr.sin_addr.s_addr = me_addr.sin_addr.s_addr = htonl(0x7f000001);
    peer_addr.sin_port        = htons(41651);
    me_addr.sin_port          = htons(41652);
    bool ok = bind(sp, reinterpret_cast<sockaddr*>(&peer_addr), sizeof(peer_addr)) == 0 &&
              bind(sm, reinterpret_cast<sockaddr*>(&me_addr), sizeof(me_addr)) == 0;
    timeval tv{2, 0};
    setsockopt(sp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sm, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (!ok) {
        std::printf("bind failed\n");
        close(sp);
        close(sm);
        return 1;
    }

    // ピアが Ping を送る
    uint8_t tx_id[ts::kDiscoTxIdLen], nonce[ts::kDiscoNonceLen];
    if (!c.random_bytes(tx_id, sizeof(tx_id)) || !c.random_bytes(nonce, sizeof(nonce))) {
        close(sp);
        close(sm);
        return 1;
    }
    static uint8_t pkt[512];
    const int64_t  t0 = esp_timer_get_time();
    const size_t   plen =
        ts::disco_build_ping(pkt, sizeof(pkt), peer_pub, peer_shared, tx_id, nullptr, nonce);
    if (plen == 0) {
        std::printf("build_ping failed\n");
        close(sp);
        close(sm);
        return 1;
    }
    sendto(sp, pkt, plen, 0, reinterpret_cast<sockaddr*>(&me_addr), sizeof(me_addr));

    // Tab5 側が受けて Pong を返す（wg_netif の foreign ハンドラと同じ処理）
    sockaddr_in from{};
    socklen_t   from_len = sizeof(from);
    ssize_t     n = recvfrom(sm, pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n <= 0) {
        std::printf("ping not received\n");
        close(sp);
        close(sm);
        return 1;
    }
    static uint8_t pong[512];
    const size_t   mlen = resp.handle(pkt, static_cast<size_t>(n), from.sin_addr.s_addr,
                                    ntohs(from.sin_port), pong, sizeof(pong));
    if (mlen == 0) {
        std::printf("responder did not answer (peers=%u unknown=%u)\n", (unsigned)resp.peer_count(),
                    (unsigned)resp.unknown_peers());
        close(sp);
        close(sm);
        return 1;
    }
    sendto(sm, pong, mlen, 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
    resp.note_send_result(true);

    // ピアが Pong を受けて中身を確認する
    n = recvfrom(sp, pkt, sizeof(pkt), 0, nullptr, nullptr);
    const int64_t rt_us = esp_timer_get_time() - t0;
    if (n <= 0) {
        std::printf("pong not received\n");
        close(sp);
        close(sm);
        return 1;
    }
    ts::DiscoType type{};
    bool          opened = ts::disco_open(pkt, static_cast<size_t>(n), peer_shared, &type, nullptr);
    bool          tx_ok  = false;
    if (opened && type == ts::DiscoType::kPong) {
        // TxID と、返ってきた送信元アドレスを確かめる
        static uint8_t plain[512];
        const size_t   box_len = static_cast<size_t>(n) - ts::kDiscoHeaderLen;
        if (wg::secretbox_open(plain, pkt + ts::kDiscoHeaderLen, box_len,
                               pkt + ts::kDiscoMagicLen + ts::kDiscoKeyLen, peer_shared)) {
            tx_ok = std::memcmp(plain + 2, tx_id, sizeof(tx_id)) == 0;
            const uint8_t* ip = plain + 2 + ts::kDiscoTxIdLen;
            const uint16_t port = static_cast<uint16_t>((ip[16] << 8) | ip[17]);
            std::printf("pong says our address is %u.%u.%u.%u:%u\n", ip[12], ip[13], ip[14], ip[15],
                        port);
        }
    }
    std::printf("disco ping/pong over udp loopback: %s (%lld us round trip)\n",
                (opened && type == ts::DiscoType::kPong && tx_ok) ? "ok" : "FAILED", rt_us);
    std::printf("  pings=%u pongs=%u unknown=%u\n", (unsigned)resp.pings_received(),
                (unsigned)resp.pongs_sent(), (unsigned)resp.unknown_peers());
    close(sp);
    close(sm);
    return (opened && tx_ok) ? 0 : 1;
}

// WireGuard のハンドシェイクと暗号化を、実機のループバック相手に往復させる。
// 相手機や WiFi が無くても「トンネルとして成立するか」を確かめられる。
// 2 つの独立した鍵ペアを作り、UDP でお互いに投げ合う（実装は wg_netif とは別経路）。
int cmd_wgloop(int, char**)
{
    const auto& c = wg::default_crypto();
    uint8_t a_priv[32], a_pub[32], b_priv[32], b_pub[32];
    if (!c.random_bytes(a_priv, 32) || !c.random_bytes(b_priv, 32) ||
        !c.dh_pubkey(a_pub, a_priv) || !c.dh_pubkey(b_pub, b_priv)) {
        std::printf("key generation failed\n");
        return 1;
    }

    // 2 つの UDP ソケットを作って localhost で向き合わせる。
    const int sa = socket(AF_INET, SOCK_DGRAM, 0);
    const int sb = socket(AF_INET, SOCK_DGRAM, 0);
    if (sa < 0 || sb < 0) {
        std::printf("socket failed\n");
        if (sa >= 0) close(sa);
        if (sb >= 0) close(sb);
        return 1;
    }
    sockaddr_in addr_a{}, addr_b{};
    addr_a.sin_family      = addr_b.sin_family = AF_INET;
    addr_a.sin_addr.s_addr = addr_b.sin_addr.s_addr = htonl(0x7f000001);  // 127.0.0.1
    addr_a.sin_port        = htons(51820);
    addr_b.sin_port        = htons(51821);
    bool ok = bind(sa, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a)) == 0 &&
              bind(sb, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b)) == 0;
    timeval tv{2, 0};
    setsockopt(sa, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sb, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (!ok) {
        std::printf("bind failed\n");
        close(sa);
        close(sb);
        return 1;
    }

    wg::Handshake ha(c), hb(c);
    if (!ha.set_keys(a_priv, b_pub) || !hb.set_keys(b_priv, a_pub)) {
        std::printf("set_keys failed\n");
        close(sa);
        close(sb);
        return 1;
    }

    // A が initiation を送る
    static uint8_t buf[2048];
    uint8_t        ts[12] = {0x40, 0, 0, 0, 0x67, 0x89, 0xab, 0xcd, 0, 0, 0, 1};
    const int64_t  t0 = esp_timer_get_time();
    if (!ha.create_initiation(buf, 0x1111, ts)) {
        std::printf("create_initiation failed\n");
        close(sa);
        close(sb);
        return 1;
    }
    sendto(sa, buf, 148, 0, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b));

    // B が受けて応答する
    ssize_t n = recvfrom(sb, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n != 148) {
        std::printf("initiation not received (%d)\n", (int)n);
        close(sa);
        close(sb);
        return 1;
    }
    uint8_t     learned[32], tsout[12];
    wg::Keypair kb, ka;
    if (!hb.consume_initiation(buf, learned, tsout) || !hb.create_response(buf, 0x2222, kb)) {
        std::printf("responder side failed\n");
        close(sa);
        close(sb);
        return 1;
    }
    sendto(sb, buf, 92, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));

    // A が応答を受けて鍵を確定する
    n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
    if (n != 92 || !ha.consume_response(buf, ka)) {
        std::printf("response not accepted (%d)\n", (int)n);
        close(sa);
        close(sb);
        return 1;
    }
    const int64_t hs_us = esp_timer_get_time() - t0;
    const bool    keys_match = std::memcmp(ka.send, kb.recv, 32) == 0 &&
                            std::memcmp(ka.recv, kb.send, 32) == 0;
    std::printf("handshake over udp loopback: %s (%lld us)\n", keys_match ? "ok" : "KEY MISMATCH",
                hs_us);

    // 確定した鍵で実際にパケットを往復させる
    wg::Transport ta(c), tb(c);
    ta.set_keypair(ka);
    tb.set_keypair(kb);
    const char*  payload = "tunnel payload over loopback";
    const size_t plen    = std::strlen(payload);
    const int64_t t1 = esp_timer_get_time();
    const size_t  wlen =
        ta.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>(payload), plen);
    if (wlen == 0) {
        // 暗号化の失敗と経路の不通を区別できるようにする（検証コマンドなので重要）。
        std::printf("encrypt failed\n");
        close(sa);
        close(sb);
        return 1;
    }
    sendto(sa, buf, wlen, 0, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b));
    n = recvfrom(sb, buf, sizeof(buf), 0, nullptr, nullptr);
    static uint8_t out[2048];
    bool           valid = false;
    const size_t   got = (n > 0) ? tb.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid)
                                 : 0;
    const int64_t rt_us    = esp_timer_get_time() - t1;
    const bool    data_ok  = valid && got == plen && std::memcmp(out, payload, plen) == 0;
    std::printf("data over udp loopback: %s (%lld us round trip)\n", data_ok ? "ok" : "FAILED",
                rt_us);

    // 逆方向も確認する（応答側の鍵で送れること）
    const size_t rlen = tb.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("reply"), 5);
    bool         rev_ok = false;
    if (rlen == 0) {
        std::printf("reverse encrypt failed\n");
    } else {
        sendto(sb, buf, rlen, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));
        n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
        const size_t got2 =
            (n > 0) ? ta.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
        rev_ok = valid && got2 == 5 && std::memcmp(out, "reply", 5) == 0;
    }
    std::printf("reverse direction: %s\n", rev_ok ? "ok" : "FAILED");

    // rekey が交差したときに断が出ないこと（#29）。
    // transport パケットの宛先インデックス = どの鍵世代で送られたか。
    auto recv_index = [](const uint8_t* pkt) {
        return static_cast<uint32_t>(pkt[4]) | (static_cast<uint32_t>(pkt[5]) << 8) |
               (static_cast<uint32_t>(pkt[6]) << 16) | (static_cast<uint32_t>(pkt[7]) << 24);
    };
    // 世代 2 以降の鍵はハンドシェイクをもう一度回さずに作る（ハンドシェイク自体は上で
    // 確認済み。ここで見たいのは「古い鍵で飛んでいたパケットを取りこぼさないか」だけ）。
    // 両側の send/recv を同じ値で XOR するので、鍵の対応関係は保たれる。
    // initiator フラグは元の鍵のまま引き継ぐので、ka* は確認済み・kb* は未確認になる。
    auto next_gen = [](wg::Keypair a, wg::Keypair b, uint8_t x, uint32_t ia, uint32_t ib,
                       wg::Keypair* out_a, wg::Keypair* out_b) {
        for (int i = 0; i < 32; ++i) {
            a.send[i] ^= x;
            a.recv[i] ^= x;
            b.send[i] ^= x;
            b.recv[i] ^= x;
        }
        a.local_index  = ia;
        a.remote_index = ib;
        b.local_index  = ib;
        b.remote_index = ia;
        *out_a         = a;
        *out_b         = b;
    };
    wg::Keypair ka2, kb2, ka3, kb3, ka4, kb4;
    next_gen(ka, kb, 0x5a, 0xa2a2a2a2, 0xb2b2b2b2, &ka2, &kb2);
    next_gen(ka, kb, 0xa5, 0xa3a3a3a3, 0xb3b3b3b3, &ka3, &kb3);
    next_gen(ka, kb, 0x3c, 0xa4a4a4a4, 0xb4b4b4b4, &ka4, &kb4);

    bool rekey_ok = false;
    // (1) A が世代 1 で送ったパケットが飛んでいる最中に、B が世代 2 に張り替える。
    const size_t ilen = ta.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("inflight"), 8);
    if (ilen == 0) {
        std::printf("rekey: inflight encrypt failed\n");
    } else {
        sendto(sa, buf, ilen, 0, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b));
        tb.set_keypair(kb2);  // kb2.initiator == false なので未確認として入る
        valid = false;
        n = recvfrom(sb, buf, sizeof(buf), 0, nullptr, nullptr);
        const size_t g1 =
            (n > 0) ? tb.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
        const bool inflight_ok = valid && g1 == 8 && std::memcmp(out, "inflight", 8) == 0;
        std::printf("  old-key packet after rekey: %s\n", inflight_ok ? "ok" : "LOST");

        // (2) 未確認のうちは B は古い鍵で送る。新しい鍵で送ると A 側で全部落ちる。
        const size_t olen = tb.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("pre"), 3);
        bool         pre_ok = false;
        if (olen > 0 && recv_index(buf) == kb2.remote_index) {
            std::printf("  BUG: unconfirmed side sent on the new key\n");
        } else if (olen > 0) {
            sendto(sb, buf, olen, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));
            valid = false;
            n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
            const size_t g2 =
                (n > 0) ? ta.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
            pre_ok = valid && g2 == 3;
        }
        std::printf("  unconfirmed sends on old key: %s\n", pre_ok ? "ok" : "FAILED");

        // (3) A も世代 2 に移ってデータを送ると、B は世代 2 を確認済みに昇格させる。
        ta.set_keypair(ka2);  // ka2.initiator == true なので確認済みで入る
        bool         post_ok = false;
        const size_t nlen = ta.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("new"), 3);
        if (nlen > 0) {
            sendto(sa, buf, nlen, 0, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b));
            valid = false;
            n = recvfrom(sb, buf, sizeof(buf), 0, nullptr, nullptr);
            const size_t g3 =
                (n > 0) ? tb.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
            post_ok = valid && g3 == 3 && tb.current_confirmed();
        }
        // (4) 昇格後は B も世代 2 で送る。
        // A は世代 1 も持っているので、宛先インデックスまで見ないと
        // 「まだ古い鍵で送っている」のを成功と誤認する。
        bool         final_ok = false;
        const size_t flen = tb.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("fin"), 3);
        if (flen > 0 && recv_index(buf) == kb2.remote_index) {
            sendto(sb, buf, flen, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));
            valid = false;
            n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
            const size_t g4 =
                (n > 0) ? ta.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
            final_ok = valid && g4 == 3;
        }
        std::printf("  after confirmation both on new key: %s\n",
                    (post_ok && final_ok) ? "ok" : "FAILED");

        // (5) 未確認の世代が 2 連続で来る場合（こちらの msg2 が落ちてピアが msg1 を
        // 再送した状況）。確認済みの世代 2 を押し出してはいけない。押し出すと
        // 「ピアが一度も持っていない世代」で送り続けて上りが全損する。
        tb.set_keypair(kb3);
        tb.set_keypair(kb4);
        bool         retry_ok = false;
        const size_t rtlen = tb.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("rty"), 3);
        if (rtlen > 0 && recv_index(buf) == kb2.remote_index) {
            sendto(sb, buf, rtlen, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));
            valid = false;
            n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
            const size_t g5 =
                (n > 0) ? ta.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
            retry_ok = valid && g5 == 3;
        }
        std::printf("  two unconfirmed rekeys keep the live key: %s\n", retry_ok ? "ok" : "FAILED");
        rekey_ok = inflight_ok && pre_ok && post_ok && final_ok && retry_ok;
    }
    std::printf("rekey crossover (no drop): %s\n", rekey_ok ? "ok" : "FAILED");

    close(sa);
    close(sb);
    // 全部が通って初めて成功。検証手順から戻り値で判定できるようにする。
    return (keys_match && data_ok && rev_ok && rekey_ok) ? 0 : 1;
}

// DISCO レスポンダ。WireGuard と同じ UDP ポートに来るので、netif の
// 「WireGuard 以外のパケット」ハンドラから呼ぶ。
ts::DiscoResponder s_disco;

void on_foreign_packet(const uint8_t* pkt, size_t len, uint32_t src_ip, uint16_t src_port)
{
    static uint8_t out[256];  // このハンドラは受信タスクからのみ呼ばれる
    const size_t   n = s_disco.handle(pkt, len, src_ip, src_port, out, sizeof(out));
    if (n > 0) {
        // 応答は同じソケットから返す。別ソケットだと NAT のマッピングがずれる。
        // 送れたかを伝える（送れていないのに送信済みと数えると誤診断につながる）。
        s_disco.note_send_result(wg::netif_instance().send_raw(out, n, src_ip, src_port));
    }
}

// netmap から得たピアの disco 公開鍵をレスポンダに登録する。
// **これを繋がないと、Ping は全部 unknown として捨てられて Pong が一度も返らない。**
void register_disco_peers(const ts::NetMap& map)
{
    auto add = [](const std::string& key_str) {
        uint8_t pub[32];
        if (!ts::key_from_string(key_str, "discokey:", pub)) return false;
        return s_disco.add_peer(pub);
    };
    int added = 0;
    for (const auto& p : map.peers) {
        if (!p.disco_key.empty() && add(p.disco_key)) ++added;
    }
    for (const auto& p : map.peers_changed) {
        if (!p.disco_key.empty() && add(p.disco_key)) ++added;
    }
    if (added > 0) {
        ESP_LOGI(TAG, "registered %d disco peers (total %u)", added,
                 (unsigned)s_disco.peer_count());
    }
}

// netif を上げる前に必ずやる配線。
//
// **片方の経路だけに置くと、もう片方で DISCO が死ぬ。** 実機で踏んだ:
// netmap 経由で上げた netif には foreign handler が無く、DISCO のパケットが
// 全部 rx_dropped になって Ping に応答しなくなった（`wg stat` が
// `rx=0 pkt (drop 17)`）。`cmd_wg` と `maybe_bring_up_tunnel` の両方から呼ぶ。
void wire_netif(wg::Netif& nif)
{
    // WireGuard のタイムスタンプは再起動をまたいで単調増加させる必要がある。
    // 巻き戻るとピアがリプレイとして無視し、原因の分からない無応答になる。
    nif.set_timestamp_store([](uint64_t* seconds, bool write) -> bool {
        nvs_handle_t h;
        if (nvs_open("wg", NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = false;
        if (write) {
            ok = (nvs_set_u64(h, "ts", *seconds) == ESP_OK) && (nvs_commit(h) == ESP_OK);
        } else {
            ok = (nvs_get_u64(h, "ts", seconds) == ESP_OK);
        }
        nvs_close(h);
        return ok;
    });

    // DISCO の鍵は ts コマンドと同じ NVS の "dkey" を使う（netmap に載る鍵と一致させる）。
    if (s_disco.has_key()) {
        nif.set_foreign_handler(&on_foreign_packet);
        return;
    }
    uint8_t      dkey[32];
    nvs_handle_t h;
    bool         ok = false;
    if (nvs_open("ts", NVS_READONLY, &h) == ESP_OK) {
        size_t len = 32;
        ok         = (nvs_get_blob(h, "dkey", dkey, &len) == ESP_OK && len == 32);
        nvs_close(h);
    }
    if (ok && s_disco.set_key(dkey)) {
        nif.set_foreign_handler(&on_foreign_packet);
    } else {
        ESP_LOGW(TAG, "no disco key yet (run `ts` first) - DISCO disabled");
    }
}

// netmap が来たら、トンネルを張れる材料が揃っているかを見て張る。
//
// **Tailscale では WireGuard の秘密鍵 = node key。** 以前は `wg` コマンドが
// NVS に自前の鍵を作って使っていたので、ピアは Tab5 の node key を公開鍵として
// 期待するのに Tab5 は別の鍵でハンドシェイクを投げ、必ず弾かれていた（#37）。
//
// ponytail: Netif は 1 ピアしか持てないので、**エンドポイントを申告している
// オンラインのピアのうち最初の 1 つ**だけを相手にする。tailnet に 2 台以上いる
// 構成が必要になったら Netif を複数ピア対応にする（そのときは AllowedIPs で
// 宛先を振り分ける必要がある）。
void maybe_bring_up_tunnel(const ts::NetMap& map, const std::string& assigned)
{
    if (!s_ts_keys_ready) return;
    auto& nif = wg::netif_instance();

    // **is_up() の外で呼ぶ。** 内側に置くと、`wg` を先に打って netif が
    // 上がっている場合に配線されず、foreign handler が null のままになって
    // DISCO のパケットが全部 rx_dropped になる（実機で踏んだ形と同じ）。
    wire_netif(nif);

    // "100.64.0.5/32" から自分のアドレスを取る。
    const std::string addr_str = assigned.substr(0, assigned.find('/'));
    ip4_addr_t        addr, mask;
    if (addr_str.empty() || !ip4addr_aton(addr_str.c_str(), &addr)) return;
    // Tailscale のアドレスは 100.64.0.0/10 に収まる。マスクを /10 にすれば
    // tailnet 宛だけがこの netif に向く（lwIP にポリシールーティングは無い）。
    ip4addr_aton("255.192.0.0", &mask);

    // **node key 以外で上がっている netif は張り替える。** Netif::up() は
    // 秘密鍵をコピーするので、上がった後に差し替える手段が無い。`wg` の独自鍵で
    // 上がっていると、ピアは node key を期待するので必ず弾かれる（#37 そのもの）。
    if (nif.is_up() && s_netif_key != NetifKey::kNode) {
        ESP_LOGW(TAG, "netif is up with a non-node key; bringing it down to re-key");
        nif.down();
        s_netif_key = NetifKey::kNone;
        s_tunnel_peer_valid = false;
    }
    if (!nif.is_up()) {
        if (esp_err_t err = nif.up(s_ts_node_priv, addr, mask); err != ESP_OK) {
            ESP_LOGE(TAG, "tunnel netif up failed: %s (%s)", esp_err_to_name(err),
                     nif.last_error());
            return;
        }
        s_netif_key         = NetifKey::kNode;
        s_tunnel_peer_valid = false;
        ESP_LOGI(TAG, "tunnel netif up with the node key: %s", addr_str.c_str());
    }

    // 相手を 1 つ選ぶ。オンラインでエンドポイントを申告しているものだけ。
    //
    // **一度選んだピアに貼り付く。** 差分 netmap には Peers が無く
    // PeersChanged だけが来るので、毎回選び直すと peers[0] と
    // peers_changed[0] の間でトンネルがフラップする（`Netif` は 1 ピアしか
    // 持てないので、切り替わるたびにハンドシェイクをやり直すことになる）。
    auto usable = [](const ts::Peer& p) {
        return p.online && !p.endpoints.empty() && !p.node_key.empty();
    };
    const ts::Peer* peer = nullptr;
    auto find_in = [&](const std::vector<ts::Peer>& peers) {
        for (const auto& p : peers) {
            if (!usable(p)) continue;
            if (s_tunnel_peer_valid && p.node_key != s_tunnel_peer_key) continue;
            peer = &p;
            return;
        }
    };
    find_in(map.peers);
    if (!peer) find_in(map.peers_changed);
    if (!peer && s_tunnel_peer_valid) return;  // 選んだピアがこの netmap に出てこないだけ
    if (!peer) {
        // まだ誰も選んでいないので、条件を満たす最初のピアを取る。
        for (const auto& list : {&map.peers, &map.peers_changed}) {
            for (const auto& p : *list) {
                if (usable(p)) {
                    peer = &p;
                    break;
                }
            }
            if (peer) break;
        }
    }
    if (!peer) return;

    wg::PeerConfig cfg;
    // ピアの node key が、そのまま WireGuard の公開鍵。
    if (!ts::key_from_string(peer->node_key, "nodekey:", cfg.public_key)) {
        ESP_LOGW(TAG, "peer node key not parseable: %s", peer->node_key.c_str());
        return;
    }
    // 自分のサブネットは WiFi の netif から取る（トンネルの netif ではない）。
    uint32_t            my_addr = 0, my_mask = 0;
    esp_netif_t*        sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        my_addr = ip.ip.addr;
        my_mask = ip.netmask.addr;
    }
    cfg.endpoint = ts::pick_endpoint(peer->endpoints, my_addr, my_mask);
    if (cfg.endpoint.empty()) {
        ESP_LOGW(TAG, "no usable IPv4 endpoint for %s", peer->name.c_str());
        return;
    }

    // **同じピア・同じエンドポイントなら何もしない。** set_peer は無条件に
    // ハンドシェイクを始めるので、netmap ごとに呼ぶと確立済みのセッションを
    // 毎回張り替える。しかも X25519 を g_state.lock を握ったまま 2 回回すので
    // （実測 72ms x 2）、rx/tx タスクの 200ms タイムアウトを踏んでパケットが落ちる。
    if (s_tunnel_peer_valid && peer->node_key == s_tunnel_peer_key &&
        cfg.endpoint == s_tunnel_endpoint) {
        return;
    }
    if (esp_err_t err = nif.set_peer(cfg); err != ESP_OK) {
        ESP_LOGE(TAG, "set_peer failed: %s (%s)", esp_err_to_name(err), nif.last_error());
        return;
    }
    s_tunnel_peer_key   = peer->node_key;
    s_tunnel_endpoint   = cfg.endpoint;
    s_tunnel_peer_valid = true;
    ESP_LOGI(TAG, "tunnel peer: %s at %s", peer->name.c_str(), cfg.endpoint.c_str());
}

// ICMP echo を投げる。トンネル越しの到達性を確かめる手段が無いと、
// 「netif は上がったが本当に通っているのか」が分からない（#9 の完了条件）。
int cmd_ping(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: ping <ip> [count]\n");
        return 1;
    }
    ip_addr_t target{};
    if (!ipaddr_aton(argv[1], &target)) {
        std::printf("bad address: %s\n", argv[1]);
        return 1;
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr       = target;
    // **0 を渡してはいけない。** esp_ping では 0 = 無限で、下の待ちが必ず
    // タイムアウトしてコールバックが解放済みスタックを触る。atoi は非数値でも
    // 0 を返すので、ここでクランプする。
    const int want = (argc > 2) ? atoi(argv[2]) : 4;
    cfg.count      = (uint32_t)((want < 1) ? 1 : (want > 100) ? 100 : want);
    cfg.timeout_ms = 2000;
    cfg.interval_ms = 500;
    // 既定のスタックは 2048 + extra しかなく、フル newlib の printf を
    // このタスクで呼ぶので広げる（この repo は何度もスタック保護フォルトを踏んでいる）。
    cfg.task_stack_size = 4096;
    // トンネルの MTU は 1280 なので、既定の 64 バイトなら断片化しない。

    // 結果はコールバックで来る。終わるまで待つのでセマフォで同期する。
    struct Ctx {
        SemaphoreHandle_t done = nullptr;
        uint32_t          replies = 0;
    } ctx;
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        std::printf("no memory\n");
        return 1;
    }

    esp_ping_callbacks_t cbs{};
    cbs.cb_args = &ctx;
    cbs.on_ping_success = [](esp_ping_handle_t h, void* args) {
        auto*    c   = static_cast<Ctx*>(args);
        uint32_t seq = 0, ms = 0;
        uint8_t  ttl = 0;
        ip_addr_t addr{};
        esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
        esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof(ms));
        esp_ping_get_profile(h, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
        esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &addr, sizeof(addr));
        std::printf("  reply from %s: seq=%u ttl=%u time=%u ms\n", ipaddr_ntoa(&addr),
                    (unsigned)seq, (unsigned)ttl, (unsigned)ms);
        ++c->replies;
    };
    cbs.on_ping_timeout = [](esp_ping_handle_t h, void*) {
        uint32_t seq = 0;
        esp_ping_get_profile(h, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
        std::printf("  timeout seq=%u\n", (unsigned)seq);
    };
    cbs.on_ping_end = [](esp_ping_handle_t, void* args) {
        xSemaphoreGive(static_cast<Ctx*>(args)->done);
    };

    esp_ping_handle_t ping = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        std::printf("esp_ping_new_session failed\n");
        vSemaphoreDelete(ctx.done);
        return 1;
    }
    esp_ping_start(ping);
    // count * (interval + timeout) より少し長く待つ。
    // **pdMS_TO_TICKS は ms * configTICK_RATE_HZ を uint32 で計算する。**
    // count を絞らずに 3600 を渡したら 9,002,000ms でオーバーフローし、
    // 412 秒で早期タイムアウトして解放済みスタックに書き込み、panic した（実機）。
    // count が 100 以下なら 252,000ms で収まる。
    const uint32_t   wait_ms = cfg.count * (cfg.interval_ms + cfg.timeout_ms) + 2000;
    const TickType_t wait    = pdMS_TO_TICKS(wait_ms);
    const bool       ok   = (xSemaphoreTake(ctx.done, wait) == pdTRUE);
    uint32_t         sent = 0, recv = 0;
    esp_ping_get_profile(ping, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(ping, ESP_PING_PROF_REPLY, &recv, sizeof(recv));
    esp_ping_stop(ping);
    // **stop は旗を立てるだけで、ping タスクを待たない**（ESP-IDF の
    // ping_sock.c を確認）。走行中の recv を終えてから on_ping_end が呼ばれるので、
    // ここで待たずに ctx とセマフォを捨てると解放済みスタックに書かれる。
    if (!ok) xSemaphoreTake(ctx.done, pdMS_TO_TICKS(cfg.timeout_ms + cfg.interval_ms + 1000));
    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);
    std::printf("%u sent, %u received%s\n", (unsigned)sent, (unsigned)recv,
                ok ? "" : " (timed out waiting)");
    return (recv > 0) ? 0 : 1;
}

// WireGuard のトンネル netif を上げる。ピア指定は任意（無ければ netif だけ作る）。
int cmd_wg(int argc, char** argv)
{
    auto& nif = wg::netif_instance();

    if (argc >= 2 && std::string(argv[1]) == "down") {
        nif.down();
        s_netif_key         = NetifKey::kNone;
        s_tunnel_peer_valid = false;
        std::printf("netif down\n");
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "disco") {
        // 自分の disco 公開鍵とピア登録状況を見る。
        if (!s_disco.has_key()) {
            std::printf("disco: disabled (no key yet - run `ts` first)\n");
            return 0;
        }
        std::printf("disco pub: ");
        for (int i = 0; i < 32; ++i) std::printf("%02x", s_disco.public_key()[i]);
        std::printf("\n  peers=%u pings=%u pongs=%u (failed %u) unknown=%u\n",
                    (unsigned)s_disco.peer_count(), (unsigned)s_disco.pings_received(),
                    (unsigned)s_disco.pongs_sent(), (unsigned)s_disco.pongs_failed(),
                    (unsigned)s_disco.unknown_peers());
        return 0;
    }
    if (argc >= 2 && std::string(argv[1]) == "stat") {
        const auto& st = nif.stats();
        std::printf("up=%d handshake=%d tx=%u pkt/%u B (drop %u) rx=%u pkt/%u B (drop %u)\n",
                    nif.is_up() ? 1 : 0, nif.handshake_done() ? 1 : 0, (unsigned)st.tx_packets,
                    (unsigned)st.tx_bytes, (unsigned)st.tx_dropped, (unsigned)st.rx_packets,
                    (unsigned)st.rx_bytes, (unsigned)st.rx_dropped);
        std::printf("  handshakes=%u rekeys=%u keepalives=%u responses=%u stale=%u\n",
                    (unsigned)st.handshakes, (unsigned)st.rekeys, (unsigned)st.keepalives,
                    (unsigned)st.responses_sent, (unsigned)st.stale_initiations);
        return 0;
    }
    if (argc < 2) {
        std::printf("usage: wg <tunnel-ip> [peer-pubkey-hex] [peer-endpoint]\n");
        std::printf("       wg stat | wg down\n");
        return 1;
    }

    // トンネルの鍵は NVS に保存して使い回す（相手に公開鍵を登録するため）。
    static uint8_t priv[32], pub[32];
    nvs_handle_t   nvs;
    bool           have = false;
    if (nvs_open("wg", NVS_READWRITE, &nvs) == ESP_OK) {
        size_t len = 32;
        have = (nvs_get_blob(nvs, "priv", priv, &len) == ESP_OK && len == 32);
        if (!have) {
            if (!wg::default_crypto().random_bytes(priv, 32)) {
                std::printf("key generation failed\n");
                nvs_close(nvs);
                return 1;
            }
            esp_err_t err = nvs_set_blob(nvs, "priv", priv, 32);
            if (err == ESP_OK) err = nvs_commit(nvs);
            if (err != ESP_OK) {
                std::printf("could not save key: %s\n", esp_err_to_name(err));
                nvs_close(nvs);
                return 1;
            }
            have = true;
        }
        nvs_close(nvs);
    }
    if (!have || !wg::default_crypto().dh_pubkey(pub, priv)) {
        std::printf("no tunnel key\n");
        return 1;
    }
    std::printf("tunnel public key (register this on the peer): ");
    for (int i = 0; i < 32; ++i) std::printf("%02x", pub[i]);
    std::printf("\n");

    ip4_addr_t addr, mask;
    if (!ip4addr_aton(argv[1], &addr)) {
        std::printf("bad tunnel ip\n");
        return 1;
    }
    // Tailscale のアドレスは 100.64.0.0/10 に収まる。マスクを /10 にすれば
    // tailnet 宛だけがこの netif に向く（lwIP にポリシールーティングは無い）。
    ip4addr_aton("255.192.0.0", &mask);

    wire_netif(nif);

    if (!nif.is_up()) {
        const esp_err_t err = nif.up(priv, addr, mask);
        if (err == ESP_OK) s_netif_key = NetifKey::kOwn;
        if (err != ESP_OK) {
            std::printf("netif up failed: %s (%s)\n", esp_err_to_name(err), nif.last_error());
            return 1;
        }
    }

    if (argc == 3) {
        // 公開鍵だけ渡されても接続できない。黙って netif だけ上げると原因が分からない。
        std::printf("peer endpoint is missing: wg <tunnel-ip> <pubkey> <host:port>\n");
        return 1;
    }
    if (argc >= 4) {
        wg::PeerConfig peer;
        const std::string hex = argv[2];
        // std::stoul は例外を投げる。例外を捕まえていないので、打ち間違いで abort してしまう。
        if (hex.size() != 64) {
            std::printf("peer pubkey must be 64 hex chars\n");
            return 1;
        }
        for (int i = 0; i < 32; ++i) {
            int hi = -1, lo = -1;
            auto nib = [](char ch) {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            hi = nib(hex[i * 2]);
            lo = nib(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                std::printf("peer pubkey must be hex\n");
                return 1;
            }
            peer.public_key[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        peer.endpoint = argv[3];
        const esp_err_t err = nif.set_peer(peer);
        if (err != ESP_OK) {
            std::printf("set_peer failed: %s (%s)\n", esp_err_to_name(err), nif.last_error());
            return 1;
        }
        std::printf("handshake started with %s\n", peer.endpoint.c_str());
    }
    return 0;
}

// sshkey パーティションの鍵を mbedTLS で直接パースして、失敗理由を表示する。
// libssh2 経由だと LIBSSH2_ERROR_FILE (-16) しか分からないため（#17）。
int cmd_keytest(int, char**)
{
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), "sshkey");
    if (!part) {
        std::printf("sshkey partition not found\n");
        return 1;
    }
    static char buf[8192];
    if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK) {
        std::printf("partition read failed\n");
        return 1;
    }
    // 未書き込み領域は 0xFF。そこまでを鍵とみなす。
    size_t len = 0;
    while (len < sizeof(buf) - 1 && static_cast<uint8_t>(buf[len]) != 0xFF) ++len;
    buf[len] = '\0';
    if (len == 0) {
        std::printf("sshkey partition is empty\n");
        return 1;
    }
    const char* nl = std::strchr(buf, '\n');
    std::printf("key: %d bytes, header=%.*s\n", (int)len, (int)(nl ? nl - buf : 0), buf);

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    // mbedTLS は鍵データの末尾が NUL であることを要求する（pkparse.c の実装）。
    const int rc = mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char*>(buf), len + 1,
                                        nullptr, 0, mbedtls_ctr_drbg_random, ts_drbg());
    if (rc == 0) {
        std::printf("parse ok: type=%s bits=%u\n", mbedtls_pk_get_name(&pk),
                    (unsigned)mbedtls_pk_get_bitlen(&pk));
    } else {
        char err[128] = {};
        mbedtls_strerror(rc, err, sizeof(err));
        std::printf("parse failed: -0x%04x (%s)\n", (unsigned)(-rc), err);
    }
    mbedtls_pk_free(&pk);
    return rc == 0 ? 0 : 1;
}

// PPA でフレームバッファに直接書けるかを確かめる。
// Panel_DSI は config_detail().buffer でフレームバッファを公開している。
int cmd_ppatest(int, char**)
{
    // フレームバッファに直接書くので、他のタスクの描画と重ならないようにする。
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    auto* panel = static_cast<lgfx::Panel_DSI*>(display.getPanel());
    if (!panel) {
        std::printf("panel is not Panel_DSI\n");
        return 1;
    }
    const auto& cfg = panel->config_detail();
    std::printf("frame buffer: %p (%u bytes), panel %dx%d depth %d\n", cfg.buffer,
                (unsigned)cfg.buffer_length, (int)display.width(), (int)display.height(),
                (int)display.getColorDepth());
    if (!cfg.buffer) {
        std::printf("no frame buffer exposed\n");
        return 1;
    }
    // PSRAM 上のはず（1280x720x2 = 1.84MB）
    const bool in_psram = esp_ptr_external_ram(cfg.buffer);
    std::printf("  in psram: %d, expected size for 720x1280x2: %d\n", in_psram ? 1 : 0,
                720 * 1280 * 2);

    // PPA クライアントを登録して、1 行ぶんを 90 度回転して書いてみる。
    ppa_client_config_t pc = {};
    pc.oper_type          = PPA_OPERATION_SRM;
    ppa_client_handle_t client = nullptr;
    esp_err_t err = ppa_register_client(&pc, &client);
    if (err != ESP_OK) {
        std::printf("ppa_register_client failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    // 内蔵 RAM に 1 行ぶん（1280x24）のソースを作る。DMA するので 64B 境界に置く。
    constexpr int kW = 1280, kH = 24;
    auto* src = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, kW * kH * 2,
                                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!src) {
        std::printf("src alloc failed\n");
        ppa_unregister_client(client);
        return 1;
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) src[y * kW + x] = (x < kW / 2) ? 0xF800 : 0x001F;  // 赤 | 青
    }

    rot::Panel rp;
    int nx = 0, ny = 0, nw = 0, nh = 0;
    rot::landscape_rect_to_native(rp, 0, 0, kW, kH, &nx, &ny, &nw, &nh);

    ppa_srm_oper_config_t op = {};
    op.in.buffer            = src;
    op.in.pic_w             = kW;
    op.in.pic_h             = kH;
    op.in.block_w           = kW;
    op.in.block_h           = kH;
    op.in.srm_cm            = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = cfg.buffer;
    // M5GFX は buffer_length を埋めていない（0 が入っている）ので自分で計算する。
    // 0 を渡すと ppa_do_scale_rotate_mirror が ESP_ERR_INVALID_ARG を返す。
    op.out.buffer_size = static_cast<size_t>(rp.native_w) * rp.native_h * 2;
    op.out.pic_w            = rp.native_w;
    op.out.pic_h            = rp.native_h;
    op.out.block_offset_x   = nx;
    op.out.block_offset_y   = ny;
    op.out.srm_cm           = PPA_SRM_COLOR_MODE_RGB565;
    // 定数は TermRenderer に 1 つだけ置いてある（複製すると片方が取り残される）。
    op.rotation_angle       = static_cast<ppa_srm_rotation_angle_t>(
        TermRenderer::ppa_rotation_angle());
    op.scale_x              = 1.0f;
    op.scale_y              = 1.0f;
    op.mode                 = PPA_TRANS_MODE_BLOCKING;

    const int64_t t0 = esp_timer_get_time();
    err = ppa_do_scale_rotate_mirror(client, &op);
    const int64_t us = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        std::printf("ppa_do_scale_rotate_mirror failed: %s\n", esp_err_to_name(err));
    } else {
        std::printf("ppa rotate 1280x24 -> native(%d,%d) %dx%d: %lld us\n", nx, ny, nw, nh, us);

        // **PPA の回転角が rot の変換と合っているかを画素で判定する。**
        // rottest は写像だけを見るので、角度を間違えてもすり抜ける
        // （実際にこのコマンド自身が ANGLE_90 のまま取り残されていた）。
        // ソースは左半分が赤・右半分が青と非対称なので、180 度回れば入れ替わる。
        // PPA は転送前に出力窓のキャッシュを無効化するので、読み戻しは PSRAM から来る。
        display.setRotation(1);
        const uint16_t left  = display.readPixel(100, kH / 2);
        const uint16_t right = display.readPixel(kW - 100, kH / 2);
        const bool     ok    = (left == 0xF800 && right == 0x001F);
        std::printf("ppa angle check: left %04x (want f800) right %04x (want 001f) %s\n", left,
                    right, ok ? "ok" : (left == 0x001F && right == 0xF800)
                                           ? "SWAPPED - rotation_angle が rot と食い違っている"
                                           : "MISMATCH");
        if (!ok) err = ESP_FAIL;

        // 比較用に M5GFX の pushSprite も測る
        M5Canvas sp(&display);
        sp.setPsram(false);
        sp.setColorDepth(16);
        if (sp.createSprite(kW, kH)) {
            sp.fillSprite(TFT_GREEN);
            display.setRotation(1);
            const int64_t t1 = esp_timer_get_time();
            sp.pushSprite(0, kH);
            std::printf("m5gfx pushSprite same size (rotation 1): %lld us\n",
                        esp_timer_get_time() - t1);
            sp.deleteSprite();
        }
    }
    heap_caps_free(src);
    ppa_unregister_client(client);
    // フレームバッファを直接叩いたのでステータスバーを描き直す。
    status_bar->draw(gather_status(), /*force=*/true);
    return err == ESP_OK ? 0 : 1;
}

// 画面をシリアル経由で吸い出す。CLAUDE.md が「画面が絡む変更は写真か画面キャプチャ」を
// 求めているが、遠隔・エージェント実行では写真が撮れない。フレームバッファを読めば
// キャプチャは作れるし、視差も照明もないぶん写真より正確に判定できる。
//
// rotation 1（キーボードが描くのと同じ向き = 物理的に正しい向き）で読むので、
// 出てくる絵はユーザーが見ているものと同じになる。端末だけが 180 度ずれていれば
// その通りに写る。
//
// 115200 baud なので間引く。step=4 で 320x180、16 進で約 230KB / 20 秒。
int cmd_screencap(int argc, char** argv)
{
    const int step = (argc > 1) ? atoi(argv[1]) : 4;
    // 範囲を絞れば文字が読める解像度でも現実的な時間で吸い出せる。
    const int x0 = (argc > 2) ? atoi(argv[2]) : 0;
    const int y0 = (argc > 3) ? atoi(argv[3]) : 0;
    const int rw = (argc > 4) ? atoi(argv[4]) : (int)display.width() - x0;
    const int rh = (argc > 5) ? atoi(argv[5]) : (int)display.height() - y0;
    if (step < 1 || step > 32 || x0 < 0 || y0 < 0 || rw <= 0 || rh <= 0 ||
        x0 + rw > (int)display.width() || y0 + rh > (int)display.height()) {
        std::printf("usage: screencap [step] [x] [y] [w] [h]   (step 1-32, 既定 4 で全画面)\n");
        return 1;
    }
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    // **自分で rotation 1 にする。** コメントで「rotation 1 で読む」と言いながら
    // 現在の rotation で読んでいると、直前のコマンドが 0 を残していた場合に
    // 黙って回った PNG が出る（この PR が直したのと同じクラスの失敗）。
    const uint8_t prev_rotation = display.getRotation();
    display.setRotation(1);
    const int w = rw / step;
    const int h = rh / step;
    std::printf("SCREENCAP %d %d %d rotation=1\n", w, h, step);
    // 1 行ぶんをまとめて組んでから出す。1 画素ずつ printf すると桁違いに遅い。
    static char line[1281 * 4 + 8];
    for (int y = 0; y < h; ++y) {
        char* out = line;
        for (int x = 0; x < w; ++x) {
            const uint16_t px = display.readPixel(x0 + x * step, y0 + y * step);
            static const char kHex[] = "0123456789abcdef";
            *out++ = kHex[(px >> 12) & 0xF];
            *out++ = kHex[(px >> 8) & 0xF];
            *out++ = kHex[(px >> 4) & 0xF];
            *out++ = kHex[px & 0xF];
        }
        *out = '\0';
        std::printf("%s\n", line);
    }
    std::printf("SCREENCAP END\n");
    display.setRotation(prev_rotation);
    return 0;
}

// **本番のレンダラ経路を通して**回転を判定する。
//
// rottest も ppatest も term_render.cpp の回転を守れない。rottest は純関数の
// 写像だけを見るし、ppatest は PPA の設定を自前で組む。だから push_row_ppa の
// 角度だけを 180 度ずらしても、あの 2 つは緑のまま通る（実機で確認済み）。
// ユーザーが報告した症状を捕まえられるのはこのコマンドだけ。
//
// 判定には色つきの背景しか使わない。グリフの形に依存させるとフォントを
// 変えたときに壊れるし、単色でも 180 度ずれれば位置が変わるので十分捕まる。
// エスケープは C++ 側で組む。コンソール経由だと argv 分割で剥がれて再現できない。
int cmd_termcheck(int, char**)
{
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    const int cw = renderer->cell_w();
    const int ch = renderer->cell_h();
    if (cw <= 0 || ch <= 0 || renderer->cols() < 4 || renderer->rows() < 3) {
        std::printf("unexpected cell/grid size (%dx%d, %dx%d cells)\n", cw, ch, renderer->cols(),
                    renderer->rows());
        return 1;
    }
    const int far = renderer->cols() - 2;  // 右端の少し内側（グリッド幅に追随させる）

    // 行 0: セル 0 が赤、セル 1 が緑。行 1: セル 0 が青。ほかは空。
    term->write("\033[2J\033[H\033[41m \033[42m \033[m\r\n\033[44m \033[m");
    // **render_term ではなく直接呼ぶ。** これは本番経路を実際に通すための診断なので、
    // メニュー表示中でも端末を描かないと、メニューの画素を読んで FAILED になる
    // （実機で踏んだ）。終わったらメニューを描き直す。
    const bool menu_was_shown = menu && menu->visible();
    renderer->render(*term, /*force=*/true);

    // 期待色は kBase16 (xterm 標準 16 色) と同じ変換で作る。
    const uint16_t red   = display.color565(205, 0, 0);
    const uint16_t green = display.color565(0, 205, 0);
    const uint16_t blue  = display.color565(0, 0, 238);

    struct Probe {
        int         lx, ly;
        uint16_t    want;
        const char* what;
    };
    // **端末の原点はステータスバーの下。** ここを足し忘れると 1 行ぶん上を読んで
    // ステータスバーの背景色が返る（実機で踏んだ）。
    const int oy = renderer->origin_y();
    const Probe probes[] = {
        {cw / 2, oy + ch / 2, red, "row0 cell0 = red"},
        {cw + cw / 2, oy + ch / 2, green, "row0 cell1 = green (red の右)"},
        // 横方向に反転していれば、ここに赤が来て左端が黒になる。
        {far * cw + cw / 2, oy + ch / 2, 0x0000, "row0 右端付近 = 黒"},
        {cw / 2, oy + ch + ch / 2, blue, "row1 cell0 = blue (row0 の下)"},
        {cw / 2, oy + 2 * ch + ch / 2, 0x0000, "row2 = 黒"},
    };

    int bad = 0;
    for (const auto& pr : probes) {
        const uint16_t got = display.readPixel(pr.lx, pr.ly);
        const bool     ok  = (got == pr.want);
        if (!ok) ++bad;
        std::printf("  (%4d,%3d) want %04x got %04x  %-34s %s\n", pr.lx, pr.ly, pr.want, got,
                    pr.what, ok ? "ok" : "MISMATCH");
    }
    std::printf("render path (vt100 -> sprite -> %s -> framebuffer): %s\n",
                renderer->ppa_enabled() ? "PPA" : "pushSprite", bad == 0 ? "ok" : "FAILED");
    status_bar->draw(gather_status(), /*force=*/true);
    if (menu_was_shown) menu->draw(/*force=*/true);
    return bad == 0 ? 0 : 1;
}

// 横向き座標の画素を読む。PPA でフレームバッファに直接書いた内容が
// 正しい位置・正しい向きに出ているかを、目視なしで確かめるために使う。
int cmd_pix(int argc, char** argv)
{
    if (argc < 3 || (argc - 1) % 2 != 0) {
        std::printf("usage: pix <lx> <ly> [<lx> <ly> ...]   横向き座標の色を読む\n");
        return 1;
    }
    // 横向き座標で読むので rotation 1 が前提。範囲外は readPixel が 0 を返すので、
    // 座標を間違えたときに「黒」と誤読しないよう rotation も出す。
    std::printf("  (rotation %d)\n", (int)display.getRotation());
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        const int lx = atoi(argv[i]);
        const int ly = atoi(argv[i + 1]);
        std::printf("  (%4d,%4d) = %04x\n", lx, ly, (unsigned)display.readPixel(lx, ly));
    }
    return 0;
}

// 座標変換が M5GFX の setRotation(1) と同じ向きかを実機で照合する。
// 横向きの座標に印を描き、rotation 0 に切り替えて計算した位置に別の色で印を描く。
// 2 つが重なれば変換が正しい。
int cmd_rottest(int, char**)
{
    // rotation を一時的に変えるので、他のタスクが描画しないよう押さえる。
    // 変えている最中に render されると、スプライトのストライドとタッチ座標がずれる。
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    rot::Panel panel;
    panel.native_w = 720;
    panel.native_h = 1280;

    struct Point { int lx, ly; uint16_t color; const char* name; };
    const Point pts[] = {
        {40, 40, TFT_RED, "top-left"},
        {1240, 40, TFT_GREEN, "top-right"},
        {40, 680, TFT_BLUE, "bottom-left"},
        {1240, 680, TFT_YELLOW, "bottom-right"},
    };

    // 目視に頼らず、描いたピクセルを読み戻して判定する。
    // rotation 1 で置いた色が、変換したネイティブ座標にあるかを確かめる。
    // ここが合っていないと、端末 (PPA + rot) とキーボード (M5GFX rotation 1) の
    // 向きが 180 度食い違う。見た目では「天地が逆」になる。
    display.setRotation(1);
    display.fillScreen(TFT_BLACK);
    for (const auto& pt : pts) display.fillCircle(pt.lx, pt.ly, 18, pt.color);
    display.setRotation(0);
    int bad = 0;
    for (const auto& pt : pts) {
        int nx = 0, ny = 0;
        rot::landscape_to_native(panel, pt.lx, pt.ly, &nx, &ny);
        const uint16_t got = display.readPixel(nx, ny);
        const bool     ok  = (got == pt.color);
        if (!ok) ++bad;
        std::printf("  %-13s landscape(%4d,%3d) -> native(%3d,%4d) want %04x got %04x %s\n",
                    pt.name, pt.lx, pt.ly, nx, ny, pt.color, got, ok ? "ok" : "MISMATCH");
    }
    display.setRotation(1);
    if (bad == 0) {
        std::printf("rotation matches setRotation(1): ok\n");
    } else {
        // どの向きなら合うのかも出す。原因を当てる手間が要らなくなる。
        // 実際に起こる回帰は「90 度を逆向きに取る」= 変換後の点が 180 度ずれる形。
        // 旧式をハードコードすると、旧式に戻したときに主判定と同じ点を読んで
        // 情報がゼロになる。変換結果を 180 度回した点を試せば向きに依存しない。
        std::printf("rotation MISMATCH (%d/4). 180 度回した点を試す:\n", bad);
        display.setRotation(0);
        int alt_bad = 0;
        for (const auto& pt : pts) {
            int nx = 0, ny = 0;
            rot::landscape_to_native(panel, pt.lx, pt.ly, &nx, &ny);
            const int nx2 = panel.native_w - 1 - nx;
            const int ny2 = panel.native_h - 1 - ny;
            const uint16_t got = display.readPixel(nx2, ny2);
            if (got != pt.color) ++alt_bad;
            std::printf("  %-13s native(%3d,%4d) want %04x got %04x\n", pt.name, nx2, ny2,
                        pt.color, got);
        }
        display.setRotation(1);
        std::printf("  180 度回した点なら %d/4 一致\n", 4 - alt_bad);
    }
    // 画面を塗り潰したのでステータスバーを描き直す（誰も描き直さない）。
    status_bar->draw(gather_status(), /*force=*/true);
    std::printf("run `termtest` or type to restore the terminal\n");
    return bad == 0 ? 0 : 1;
}

void register_term_commands()
{
    const esp_console_cmd_t cmds[] = {
        {"term", "端末に文字列を流し込む (\\\\e で ESC)", "<text>", &cmd_term, nullptr, nullptr, nullptr},
        {"termdump", "端末の内容をシリアルに出す（画面を見られないとき用）", nullptr,
         &cmd_termdump, nullptr, nullptr, nullptr},
        {"termtest", "色・全角・装飾のテストパターンを描画する", nullptr, &cmd_termtest, nullptr, nullptr, nullptr},
        {"termscroll", "40 行流してスクロールを見る", nullptr, &cmd_termscroll, nullptr, nullptr, nullptr},
        {"fonttest", "フォント描画経路の切り分け", nullptr, &cmd_fonttest, nullptr, nullptr, nullptr},
        {"ssh", "SSH 接続 (引数なしで保存済み設定)", "[<user> <host> <password> [port]]", &cmd_ssh,
         nullptr, nullptr, nullptr},
        {"sshclose", "SSH セッションを閉じる", nullptr, &cmd_sshclose, nullptr, nullptr, nullptr},
        {"key", "SSH にキー入力を送る", "<text>", &cmd_key, nullptr, nullptr, nullptr},
        {"conv", "ローマ字→かな→漢字を試す", "<romaji>", &cmd_conv, nullptr, nullptr, nullptr},
        {"keyj", "ローマ字を IME に通して SSH に送る", "<romaji>...", &cmd_keyj, nullptr, nullptr,
         nullptr},
        {"scroll", "スクロールバックを動かす (正=過去へ)", "[lines]", &cmd_scroll, nullptr, nullptr,
         nullptr},
        {"bench", "描画コストを測る (全画面 vs 1 文字)", nullptr, &cmd_bench, nullptr, nullptr,
         nullptr},
        {"kbd", "画面キーボードの表示切り替え", "[off]", &cmd_kbd, nullptr, nullptr, nullptr},
        {"menu", "初期メニューの操作", "[show|hide|up|down|enter|esc|left|right]", &cmd_menu,
         nullptr, nullptr, nullptr},
        {"wgtest", "WireGuard の暗号とハンドシェイクを実機で検証", nullptr, &cmd_wgtest, nullptr,
         nullptr, nullptr},
        {"wgloop", "UDP ループバックでトンネルを往復させる（相手機不要）", nullptr, &cmd_wgloop,
         nullptr, nullptr, nullptr},
        {"discoloop", "UDP ループバックで DISCO の Ping/Pong を往復させる", nullptr,
         &cmd_discoloop, nullptr, nullptr, nullptr},
        {"keytest", "sshkey パーティションの鍵を mbedTLS で直接パースする", nullptr, &cmd_keytest,
         nullptr, nullptr, nullptr},
        {"screencap", "画面をシリアルに吸い出す（PNG は tools/screencap.py）", "[step]",
         &cmd_screencap, nullptr, nullptr, nullptr},
        {"termcheck", "本番のレンダラ経路で描いて画素で回転を判定する", nullptr, &cmd_termcheck,
         nullptr, nullptr, nullptr},
        {"pix", "横向き座標の画素の色を読む", "<lx> <ly> ...", &cmd_pix, nullptr, nullptr, nullptr},
        {"rottest", "座標変換が setRotation(1) と一致するか実機で照合", nullptr, &cmd_rottest,
         nullptr, nullptr, nullptr},
        {"ppatest", "PPA でフレームバッファに直接回転転送してみる", nullptr, &cmd_ppatest, nullptr,
         nullptr, nullptr},
        {"ts", "Tailscale/Headscale の制御プレーンに接続（別タスク）",
         "<host> <authkey> [port] [capver]", &cmd_ts, nullptr, nullptr, nullptr},
        {"ts-status", "ts の状態を見る", nullptr, &cmd_ts_status, nullptr, nullptr, nullptr},
        {"ts-stop", "ts の long-poll を止める", nullptr, &cmd_ts_stop, nullptr, nullptr, nullptr},
        {"ping", "ICMP echo を投げる（トンネル越しの到達性確認）", "<ip> [count]",
         &cmd_ping, nullptr, nullptr, nullptr},
        {"wg", "WireGuard トンネルの netif を操作", "<tunnel-ip> [pubkey] [endpoint] | stat | down",
         &cmd_wg, nullptr, nullptr, nullptr},
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

    s_term_lock = xSemaphoreCreateMutex();
    if (!s_term_lock) {
        ESP_LOGE(TAG, "could not create the terminal lock");
        return;
    }

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
    // 画面上端の 1 行ぶんをステータスバーに譲る。端末はその下から描く。
    s_status_h = renderer->cell_h();
    renderer->set_origin_y(s_status_h);
    status_bar = std::make_unique<StatusBar>(display);
    status_bar->begin(s_status_h);

    // 画面下部をキーボードに使うので、端末の行数はその分減らす。
    keyboard = std::make_unique<KeyboardUi>(display, *renderer);
    // キーボードの上端は端末のセル境界に合わせる。合わせないと誰も描かない帯が残る。
    constexpr int kWantKeyboardH = 320;  // 4 行 x 72px + ステータス帯 32px 程度
    const int avail      = display.height() - s_status_h;
    const int term_rows  = (avail - kWantKeyboardH) / renderer->cell_h();
    const int keyboard_h = avail - term_rows * renderer->cell_h();
    keyboard->begin(keyboard_h);
    renderer->set_rows(term_rows);
    // ここで出すのは「キーボードを出したときの配分」。起動直後はメニューを出して
    // キーボードを隠すので、実際の行数はこれと違う（下で apply_layout が決める）。
    ESP_LOGI(TAG, "layout with keyboard: status %d px, terminal %d rows, keyboard %d px (no gap)",
             s_status_h, term_rows, keyboard_h);

    // 回転を PPA に任せる（使えなければ従来の経路にとどまる）。
    if (renderer->enable_ppa()) {
        ESP_LOGI(TAG, "renderer: using PPA for rotation");
    } else {
        ESP_LOGW(TAG, "renderer: PPA unavailable, falling back to pushSprite");
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

    // キーボードの出力はそのまま SSH へ流す。未接続なら端末にエコーして動作確認できるようにする。
    // 電源投入時はメニューを出す。端末は選んでから入る。
    menu = std::make_unique<MenuUi>(display);
    menu->set_action([](MenuUi::Action a) {
        switch (a) {
            case MenuUi::Action::kOpenSsh: {
                menu->set_visible(false);
                render_term(/*force=*/true);
                // 保存済みの設定で繋ぐ。無ければ端末にそう出す。
                SshConfig cfg;
                if (ssh_config_load(cfg) != ESP_OK || cfg.host.empty()) {
                    term->write("\r\n\033[33mno saved connection: use `ssh <user> <host>`\033[m\r\n");
                    render_term();
                    break;
                }
                term->write("\r\n\033[33mconnecting to " + cfg.user + "@" + cfg.host +
                            "...\033[m\r\n");
                render_term();
                if (esp_err_t err = ssh_connect(cfg, renderer->cols(), renderer->rows());
                    err != ESP_OK) {
                    ESP_LOGE(TAG, "ssh_connect failed: %s (%s)", esp_err_to_name(err),
                             ssh_last_error());
                }
                break;
            }
            case MenuUi::Action::kShowTerminal:
                menu->set_visible(false);
                render_term(/*force=*/true);
                break;
            case MenuUi::Action::kTsConnect:
            case MenuUi::Action::kWgUp:
                // ponytail: 接続先を NVS に持っていないので、まだ画面からは繋げない。
                // 保存できるようにしたら menu_ui 側の項目を enabled にする。
                break;
        }
    });
    menu->set_info(gather_menu_info(gather_status()));
    menu->set_visible(true);
    // 描画はキーボードの表示が決まってから（下の apply_layout で行う）。

    keyboard->set_output([](const std::string& s) {
        if (ssh_is_connected()) {
            ssh_send(s.data(), s.size());
        } else {
            TermGuard guard;
            term->write(s);
            render_term();
        }
    });

    // WiFi。display.init() が C6 の電源 (IO エクスパンダ経由) を入れているので、必ずこの後。
    if (esp_err_t err = wifi_start(); err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start failed: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(console_start());
    register_term_commands();

    // 辞書が書き込まれていればキーボードの変換に使う。
    if (dict_open()) keyboard->set_dict(&s_dict);
    // 電源投入時はメニュー。配分（キーボードを隠す・端末の行数）と描画は
    // set_menu_visible が全部やる。ここで先に描くと、配分が決まる前の高さで
    // 描いた操作説明が下に取り残される（実機で確認）。
    set_menu_visible(true);
    ESP_LOGI(TAG, "boot state: menu shown, terminal %dx%d", renderer->cols(), renderer->rows());

    // 描画とタッチのポーリング。キーボードが来るまではタッチが唯一の直接入力手段。
    int  last_log_s = 0;
    bool touching   = false;
    int  last_x = 0, last_y = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // **状態集めはロックの外でやる。** gather_status は C6 への SDIO RPC、
        // gather_menu_info は NVS 読みを含む。s_term_lock を握ったままやると、
        // コンソール側の描画コマンドが TermGuard の 2 秒タイムアウトに落ちるし、
        // 同じループでポーリングしているタッチも取りこぼす。
        const int64_t   now_us = esp_timer_get_time();
        const bool      poll   = (now_us - s_last_status_us > 1000000);
        StatusBar::Info si;
        MenuUi::Info    mi;
        if (poll) {
            s_last_status_us = now_us;
            si               = gather_status();
            mi               = gather_menu_info(si);
        }

        // リモートからの出力を端末に流す。コンソールタスクも term を触るのでロックする。
        {
            TermGuard guard;
            if (guard.ok()) {
                // **1 フレームで飲み込む量に上限を付ける (#40)。** 上限が無いと、
                // 高速に流れる出力（`seq 1 20000` など）でこのタスクが数秒 CPU を
                // 離さず、IDLE0 が回らなくて task_wdt が出る（実機で確認）。
                // 上限を超えた分は SSH のストリームバッファに残り、ssh タスクが
                // xStreamBufferSend で待つので**取りこぼしにはならない**（backpressure）。
                // 1024B x 16 = 16KB/フレーム、50Hz で 800KB/s。端末には十分。
                uint8_t           rx[1024];
                constexpr int     kMaxDrainPerFrame = 16;
                for (int i = 0; i < kMaxDrainPerFrame; ++i) {
                    size_t n = ssh_receive(rx, sizeof(rx));
                    if (n == 0) break;
                    term->write(rx, n);
                }
                // 描画は中身が変わったときだけ（PPA は転送のたびに出力側の
                // キャッシュを無効化するので、毎フレーム描くと端末の描画と競合する）。
                if (poll && status_bar->draw(si) && menu->visible()) {
                    // 状態が変わったときだけメニューの表示も作り直す。
                    menu->set_info(mi);
                    menu->refresh();
                }
                if (menu->visible()) {
                    menu->draw();
                } else if (term->any_dirty()) {
                    render_term();
                }
            }
        }

        // キーボードの描画も同じロックで守る。PPA は転送のたびにフレームバッファの
        // キャッシュを無効化するので、M5GFX がキャッシュ経由で描いた内容と競合する。
        TermGuard touch_guard;
        lgfx::touch_point_t tp;
        if (touch_guard.ok() && display.getTouch(&tp, 1)) {
            if (!touching) {
                touching = true;
                last_x   = tp.x;
                last_y   = tp.y;
                if (tp.y < kStatusTapH) {
                    // ステータスバーをタップでメニューを開閉する。純正キーボードが
                    // 来るまで、指だけでメニューに戻れる経路はここだけなので、
                    // 当たり判定は描画（24px）より広く取る。24px は 3.5mm しかない。
                    set_menu_visible(!menu->visible());
                } else if (menu->visible()) {
                    // メニュー表示中はキーボードを隠しているので、ここで全部食う。
                    if (menu->touch_down(tp.x, tp.y)) menu->draw();
                } else if (!keyboard->touch_down(tp.x, tp.y)) {
                    // キーボード外 = 端末領域。
                    // ponytail: スワイプでスクロールバックを見る動作は未実装。
                    // 今は座標をログに出すだけ（スクロールは `scroll` コマンドから）。
                    // キーボードが届いて画面を触る頻度が上がったら実装する。
                    ESP_LOGI(TAG, "touch down x=%d y=%d (cell %d,%d)", tp.x, tp.y,
                             tp.x / renderer->cell_w(), tp.y / renderer->cell_h());
                }
            } else {
                // メニュー表示中はドラッグをキーボードに渡さない（見えていないので）。
                if (!menu->visible()) keyboard->touch_move(tp.x, tp.y);
                last_x = tp.x;
                last_y = tp.y;
            }
        } else if (touching && touch_guard.ok()) {
            touching = false;
            if (menu->visible()) {
                // メニュー表示中は押した時点で決まっているので、離すのは無視する。
            } else if (!keyboard->touch_up(last_x, last_y)) {
                ESP_LOGI(TAG, "touch up");
            }
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
