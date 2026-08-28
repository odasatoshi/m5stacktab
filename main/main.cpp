// Tab5 ファームウェアのエントリポイント。
// 今の段階では「画面にターミナルを描く土台」と「WiFi 接続」まで。
// SSH セッションを繋ぐのは #5 / #6。
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <algorithm>
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
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <esp_partition.h>
#include <nvs.h>

#include "romaji.hpp"
#include "skk_dict.hpp"
#include "ssh.hpp"
#include <qrcode.h>

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
#include "kbd_hw.hpp"
#include "kbd_keys.hpp"
#include "kbd_ui.hpp"
#include "menu_ui.hpp"
#include "nvs_store.hpp"
#include "profiles.hpp"
#include "sdcard.hpp"
#include "status_bar.hpp"
#include "tap_gesture.hpp"
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

// **再帰ミューテックス。** 画面に触る経路は入れ子になる: タッチのループが
// ロックを持ったまま touch_up_at -> 画面キーボードの emit -> send_input と降りてきて、
// その先でもう一度 TermGuard を取る。非再帰だと自分のロックを 2 秒待って諦め、
// **ロック無しで端末を書いて描く**（ロックを入れた目的がそのまま再発する）。
class TermGuard {
public:
    TermGuard()
    {
        if (!s_term_lock) return;
        taken_ = xSemaphoreTakeRecursive(s_term_lock, pdMS_TO_TICKS(2000)) == pdTRUE;
        if (!taken_) {
            // 黙って通すと、ロックを入れた目的（画面の破壊と計測値の混入）がそのまま再発する。
            // 呼び出し側は ok() を見て中断する。
            ESP_LOGE(TAG, "terminal lock timeout - skipping this operation");
        }
    }
    ~TermGuard() { if (taken_) xSemaphoreGiveRecursive(s_term_lock); }
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
// 画面キーボードの出し入れ（コンソールもダブルタップも通す唯一の経路）。
void set_keyboard_visible(bool show);
// メニューの表示を切り替える。キーボードの表示と端末の行数も一緒に動く。
void set_menu_visible(bool show);
// 対話ログインの QR (#59)。定義は ts の側（ずっと下）。
extern bool        s_auth_qr_active;  // 出している間 true
extern std::string s_auth_qr_url;     // 出し直せるように URL は畳んでも残す
void               hide_auth_qr(bool redraw = true);
void               show_auth_qr(const std::string& url);
// メニュー表示中は端末を描かない（メニューの矩形を上書きしてしまう）。
void render_term(bool force = false);
// ステータスバーに出す内容を集める（定義は下）。
StatusBar::Info gather_status();
// 入力を SSH（未接続なら端末）へ送る（定義は下）。
void send_input(const std::string& s);
// 打鍵の行き先（`kbdinject` の報告と、未対応キーの警告を分けるため）。
enum class KeyResult { kMenu, kSent, kUnknown };
// 打鍵を 1 つ処理する（定義は下）。実キーも `kbdinject` もここを通す。
KeyResult kbd_handle_key(const std::string& name, uint8_t mod);
// 実タッチの生座標をログに出すか。四隅の照合（描画側とタッチ側の回転が
// 一致しているか）を指で取るときに使う。既定は off（毎タップでログが出ると邪魔）。
bool s_touch_log = false;

// タッチのルーティング（実タッチと tap / swipe コマンドで共有）。
void touch_down_at(int x, int y);
void touch_move_at(int x, int y);
void touch_up_at(int x, int y);

// Tailscale の 3 つの鍵。node_priv は WireGuard の秘密鍵でもあるので、
// netmap が来たときにトンネルを張るために map handler からも読む。
uint8_t s_ts_machine_priv[32] = {};
uint8_t s_ts_node_priv[32]    = {};
uint8_t s_ts_disco_priv[32]   = {};
bool    s_ts_keys_ready       = false;

// トンネルの netif がどの鍵で上がっているか。Netif::up() は秘密鍵をコピーするので、
// 上がった後に差し替える手段が無い。張り替えの判断に使う。
// トンネルの netmask。Tailscale のアドレスは 100.64.0.0/10 に収まるので /10 に
// すれば tailnet 宛だけがこの netif に向く（lwIP にポリシールーティングは無い）。
constexpr const char* kTunnelMask = "255.192.0.0";

enum class NetifKey { kNone, kOwn, kNode };
NetifKey    s_netif_key = NetifKey::kNone;
// 今上がっているトンネルの設定。**上げ直さずに 2 本目を張ろうとしたのを捕まえる**
// ためだけに持つ（Netif::up は鍵をコピーするので後から差し替えられない）。
uint8_t     s_wg_priv[32] = {};
ip4_addr_t  s_wg_addr{};
ip4_addr_t  s_wg_mask{};
bool        s_wg_live = false;
// 今トンネルを張っている相手。netmap ごとに選び直さないために覚える。
std::string s_tunnel_peer_key;
std::string s_tunnel_endpoint;
bool        s_tunnel_peer_valid = false;

// --- SD カードの接続先 (#49) ---
//
// **SD の中身は信用しない。** パーサが上限と書式を見ているので、ここは
// 「読めたか」と「その理由」だけを持つ。読めなくても NVS の 1 件で繋げる。
constexpr const char* kProfilesPath = "/sdcard/tab5/profiles.json";
constexpr const char* kKeysDir      = "/sdcard/tab5/keys/";

prof::Config s_profiles;
// 画面とコンソールに出す 1 行。読み込みのたびに書き換える。
char s_profiles_status[64] = "not loaded";

// ステータスバーを見に行った最後の時刻。C6 への RPC なので毎フレームは叩かない。
int64_t s_last_status_us = 0;
// ステータスバーのタップ判定の高さ。描画は s_status_h (24px) だが、指で当てるには
// 狭すぎるので判定だけ広げる（24px は 3.5mm しかない）。
constexpr int kStatusTapH = 56;

// 端末領域の縦スワイプでスクロールバックを見る。押した位置と、そのときの
// view_offset を覚えて、指の移動量から絶対位置を出す（相対だと取りこぼしで
// ずれていく）。-1 は「端末領域を掴んでいない」。
int s_swipe_start_y      = -1;
int s_swipe_start_offset = 0;

// 端末領域のダブルタップで画面キーボードを出し入れする (#54)。
// 判定そのものは tap_gesture.hpp（ホストテスト付き）に置いてある。
TapState s_tap;

// ユーザが選んだ画面キーボードの表示状態。**メニューから端末へ戻るときはこれに従う。**
// 以前は無条件で出していたが、指で切り替えられる以上、メニューを 1 回開くたびに
// 勝手に戻るのは困る。
bool s_keyboard_wanted = true;

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
// 出すのは「今画面に見えているもの」（view_offset を反映する）。
int cmd_termdump(int, char**)
{
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    std::printf("--- term %dx%d cursor=(%d,%d)%s scrollback=%d/%d ---\n", term->cols(),
                term->rows(), term->cursor_x(), term->cursor_y(),
                term->cursor_visible() ? "" : " hidden", term->view_offset(),
                term->scrollback_lines());
    if (term->scrollback_stalled()) {
        std::printf("!! scrollback stalled: 確保時の桁数と食い違っているので積んでいない\n");
    }
    // UTF-8 化は vt100 に任せる（ホストテストで全角の境界まで固めてある）。
    // **view_row_text を使う。** スクロールバックを見ている間、ライブ画面を出すと
    // 画面と食い違って「dump したのに見えているものと違う」ことになる。
    for (int y = 0; y < term->rows(); ++y) {
        std::printf("%2d|%s\n", y, term->view_row_text(y).c_str());
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

// 鍵を読む。**正式な置き場は NVS** (#57) — SD は取り込み元でしかないので、
// ここでは SD を見ない（挿していなくても繋がる必要がある）。
bool read_key(const std::string& name, std::string* out, std::string* err)
{
    if (name.empty()) {
        *err = "鍵のファイル名が空";
        return false;
    }
    switch (esp_err_t e = nvs_key_load(name, out)) {
        case ESP_OK: return true;
        case ESP_ERR_NVS_NOT_FOUND:
            *err = "鍵 \"" + name + "\" が取り込まれていない（`profiles import`）";
            return false;
        case ESP_ERR_INVALID_ARG:
            *err = "鍵の名前が長すぎる: \"" + name + "\" (" +
                   std::to_string(kNvsKeyNameMax) + " 文字まで)";
            return false;
        default: *err = std::string("鍵 \"") + name + "\": " + esp_err_to_name(e); return false;
    }
}

// NVS の接続先を読み直す。**落ちないこと**が第一で、読めない理由は
// s_profiles_status に残して画面にもコンソールにも出す。
void load_profiles()
{
    s_profiles = prof::Config{};
    std::string json;
    switch (esp_err_t err = nvs_profiles_load(&json)) {
        case ESP_OK: break;
        case ESP_ERR_NVS_NOT_FOUND:
            std::snprintf(s_profiles_status, sizeof(s_profiles_status),
                          "未設定 (`profiles import` で SD から取り込む)");
            return;
        default:
            std::snprintf(s_profiles_status, sizeof(s_profiles_status), "読めない (%s)",
                          esp_err_to_name(err));
            return;
    }
    s_profiles = prof::parse(json);
    if (!s_profiles.error.empty()) {
        std::snprintf(s_profiles_status, sizeof(s_profiles_status), "%s",
                      s_profiles.error.c_str());
    } else {
        std::snprintf(s_profiles_status, sizeof(s_profiles_status), "%u 件 (飛ばした %u 件)",
                      (unsigned)s_profiles.profiles.size(), (unsigned)s_profiles.warnings.size());
    }
    ESP_LOGI(TAG, "profiles: %s", s_profiles_status);
    for (const auto& w : s_profiles.warnings) ESP_LOGW(TAG, "profiles: %s", w.c_str());
}

// NVS に置ける profiles.json の大きさ。**パーサの上限 (64KB) ではなく NVS の天井で見る** —
// パーティションは 24KB しかないので、64KB を通してから nvs_set_blob で
// ESP_ERR_NVS_NOT_ENOUGH_SPACE を出しても、ユーザーには生のエラー名しか見えない。
constexpr size_t kMaxImportBytes = 8 * 1024;

// SD から NVS へ取り込む本体。**SD のマウントは呼び出し側が畳む。**
int import_from_sd()
{
    std::string json;
    switch (esp_err_t err = sd_read_file(kProfilesPath, kMaxImportBytes, &json)) {
        case ESP_OK: break;
        case ESP_ERR_NOT_FOUND: std::printf("%s が無い\n", kProfilesPath); return 1;
        case ESP_ERR_INVALID_SIZE:
            std::printf("profiles.json が大きすぎる (NVS に置ける上限 %uKB)\n",
                        (unsigned)(kMaxImportBytes / 1024));
            return 1;
        default: std::printf("読めない: %s\n", esp_err_to_name(err)); return 1;
    }
    // **書き込む前に検証する。** 壊れた JSON をそのまま NVS に入れると、
    // 次の起動から「読めない」状態が居座る（SD を抜いても直せない）。
    const prof::Config cfg = prof::parse(json);
    if (!cfg.error.empty()) {
        std::printf("取り込まない: %s\n", cfg.error.c_str());
        return 1;
    }
    for (const auto& w : cfg.warnings) std::printf("  ! %s\n", w.c_str());

    // **NVS に触る前に、要るものを全部メモリに読む。** 途中で失敗したときに
    // 「新しい鍵 + 古い接続先」という食い違った状態を残さないため。
    std::vector<std::pair<std::string, std::string>> keys;
    int                                              missing = 0;
    for (const auto& name : prof::referenced_keys(cfg)) {
        if (name.size() > kNvsKeyNameMax) {
            std::printf("  ! 鍵の名前が長すぎる: \"%s\" (%u 文字まで)。飛ばした\n", name.c_str(),
                        (unsigned)kNvsKeyNameMax);
            ++missing;
            continue;
        }
        const std::string path = std::string(kKeysDir) + name;
        std::string       data;
        // 秘密鍵は 8KB もあれば足りる（RSA 4096 の PEM で約 3.2KB）。
        if (esp_err_t e = sd_read_file(path.c_str(), 8192, &data); e != ESP_OK) {
            // **既に NVS にある鍵なら、消す前に諦める。** カードの一時的な不調で
            // 読めなかっただけのときに、正常な写しごと消してしまわないように。
            std::string have;
            if (nvs_key_load(name, &have) == ESP_OK) {
                std::printf("鍵 \"%s\" を SD から読めない (%s)。**NVS には入っている**ので、"
                            "消さずに中止した\n",
                            name.c_str(), esp_err_to_name(e));
                return 1;
            }
            std::printf("  ! %s: %s。飛ばした\n", path.c_str(), esp_err_to_name(e));
            ++missing;
            continue;
        }
        keys.emplace_back(name, std::move(data));
    }

    // **消す前に「入るか」を見る。** 書いている途中で NVS が尽きると、
    // 消した後なので接続先が全部無くなり、**もう一度 import しても同じ所で落ちて戻せない**。
    // ここでは今 prof が使っている分を空きに数えない（消せば増えるぶんは見込まない）ので
    // 厳しめに出る。断られたら `profiles clear` してから取り込めば通る。
    size_t need = nvs_entries_for(json.size());
    for (const auto& [name, data] : keys) need += nvs_entries_for(data.size());
    const size_t have = nvs_free_entries();
    // **成功時も数字を出す。** 出さないと「検査が走ったか」を後から確かめられない。
    ESP_LOGI(TAG, "import: %u エントリ要る / 空き %u", (unsigned)need, (unsigned)have);
    if (need > have) {
        std::printf("NVS が足りない: %u エントリ要るが空きは %u（1 エントリ 32B）\n",
                    (unsigned)need, (unsigned)have);
        std::printf("  `profiles clear` してから取り込むか、鍵を減らす（`nvsstat` で内訳）\n");
        return 1;
    }

    // **先に消してから書く。** 上書きだけだと、鍵の名前を変えたり接続先を消したりした
    // ときに古い秘密鍵が NVS に residue として残り続ける（`profiles` の一覧にも出る）。
    // 24KB しかないので容量も食う。**入るかは上で確かめてある**ので、ここから先で
    // 容量不足に落ちることは無い（SD の読みも全部終わっている）。
    if (esp_err_t e = nvs_profiles_clear(); e != ESP_OK) {
        std::printf("古い設定を消せない: %s\n", esp_err_to_name(e));
        return 1;
    }
    auto no_space = [](esp_err_t e) {
        if (e != ESP_ERR_NVS_NOT_ENOUGH_SPACE) return;
        std::printf("  NVS が足りない。`nvsstat` で使用量を見て、要らないものを減らす\n");
    };
    for (const auto& [name, data] : keys) {
        if (esp_err_t e = nvs_key_store(name, data); e != ESP_OK) {
            std::printf("鍵 \"%s\" を保存できない: %s\n", name.c_str(), esp_err_to_name(e));
            no_space(e);
            return 1;
        }
    }
    if (esp_err_t e = nvs_profiles_store(json); e != ESP_OK) {
        std::printf("profiles.json を保存できない: %s\n", esp_err_to_name(e));
        no_space(e);
        return 1;
    }
    load_profiles();
    std::printf("取り込んだ: %u 件、鍵 %u 本", (unsigned)cfg.profiles.size(),
                (unsigned)keys.size());
    if (missing) std::printf("（%d 本は取り込めなかった。上の ! を見る）", missing);
    std::printf("（SD は抜いてよい）\n");
    return 0;
}

// SD から NVS へ取り込む (#57)。**取り込んだら SD は抜ける。**
int import_profiles()
{
    if (esp_err_t err = sd_mount(); err != ESP_OK) {
        std::printf("SD をマウントできない: %s\n", esp_err_to_name(err));
        return 1;
    }
    const int rc = import_from_sd();
    // **必ず畳む。** sd_mount() は「もう開いている」と即 ESP_OK を返すので、
    // 畳まないままカードを差し替えると、次の import が死んだ VFS を読みに行く
    // （再起動するまで直らない）。
    sd_unmount();
    return rc;
}

// SD の接続先を見る／読み直す。**飛ばした理由をここで出す** — 画面には
// 1 行しか出ないので、書き損じを直すにはこれが要る。
int cmd_profiles(int argc, char** argv)
{
    const std::string sub = (argc > 1) ? argv[1] : "";
    if (argc > 2 || (argc == 2 && sub != "reload" && sub != "import" && sub != "clear")) {
        std::printf("usage: profiles [reload|import|clear]\n");
        std::printf("  import: SD の %s と keys/ を NVS に取り込む（以後 SD は不要）\n",
                    kProfilesPath);
        std::printf("  clear : 取り込んだ接続先と鍵を全部消す\n");
        return 1;
    }
    // **ロックを取る。** load_profiles は s_profiles を作り直すので、
    // メニューの描画（1 秒ごとの refresh も）が同じ vector を走査していると
    // 読んでいる最中に解放される。
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    if (sub == "import") {
        if (import_profiles() != 0) return 1;
    } else if (sub == "clear") {
        if (esp_err_t e = nvs_profiles_clear(); e != ESP_OK) {
            std::printf("消せない: %s\n", esp_err_to_name(e));
            return 1;
        }
        load_profiles();
        std::printf("消した\n");
        return 0;
    } else if (sub == "reload") {
        load_profiles();
    }
    std::printf("NVS: %s\n", s_profiles_status);
    for (size_t i = 0; i < s_profiles.profiles.size(); ++i) {
        const prof::Profile& p = s_profiles.profiles[i];
        std::printf("  [%u] %-16s %-10s ", (unsigned)i, p.name.c_str(),
                    prof::type_name(p.type));
        switch (p.type) {
            case prof::Type::kSsh:
                std::printf("%s@%s:%u%s%s%s", p.user.c_str(), p.host.c_str(), (unsigned)p.port,
                            p.key.empty() ? "" : " key=", p.key.c_str(),
                            p.ask_password ? " (パスワードは接続時に入力)" : "");
                if (!p.via.empty()) std::printf(" via %s", p.via.c_str());
                break;
            case prof::Type::kWireGuard:
                std::printf("%s -> %s", p.address.c_str(), p.peer.endpoint.c_str());
                break;
            case prof::Type::kTailscale:
                std::printf("%s:%u", p.control.c_str(), (unsigned)p.port);
                break;
        }
        std::printf("\n");
    }
    for (const auto& w : s_profiles.warnings) std::printf("  ! %s\n", w.c_str());
    if (const auto names = nvs_key_names(); !names.empty()) {
        std::printf("  鍵:");
        for (const auto& n : names) std::printf(" %s", n.c_str());
        std::printf("\n");
    }
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

// 覚えているホスト鍵を忘れる（#35）。サーバを作り直して鍵が正当に変わったとき、
// これが無いとそのホストに永久に繋げない。**ハッシュから元のホスト名は復元できない**
// ので、`ssh` と同じ形（host[:port]）で指定してもらう。
int cmd_ssh_forget(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: ssh-forget <host>[:<port>]\n");
        return 1;
    }
    std::string host = argv[1];
    uint16_t    port = 22;
    if (const size_t colon = host.rfind(':');
        colon != std::string::npos && host.find(':') == colon) {
        const int p = atoi(host.c_str() + colon + 1);
        if (p > 0 && p <= 65535) {
            port = (uint16_t)p;
            host.resize(colon);
        }
    }
    const esp_err_t err = ssh_forget_host_key(host.c_str(), port);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        std::printf("no stored host key for %s:%u\n", host.c_str(), (unsigned)port);
        return 1;
    }
    if (err != ESP_OK) {
        std::printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    std::printf("forgot the host key for %s:%u (next connect will remember the new one)\n",
                host.c_str(), (unsigned)port);
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

// 打鍵を合成する。**遠隔ではキーを打てない**ので、I2C の読み以外（メニューへの分岐、
// キー名の変換、DECCKM の切り替え、SSH / 端末への送出）を実キーと同じ関数で確かめる。
// バイト列まで見たいときは `kbdlog` を on にしてから呼ぶ。
int cmd_kbdinject(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: kbdinject <key-name> [mod]   mod: 1=Ctrl 4=Alt\n");
        return 1;
    }
    const uint8_t mod = (argc > 2) ? (uint8_t)std::strtoul(argv[2], nullptr, 0) : 0;
    switch (kbd_handle_key(argv[1], mod)) {
        case KeyResult::kUnknown:
            std::printf("未対応のキー名: \"%s\"\n", argv[1]);
            return 1;
        case KeyResult::kMenu:
            std::printf("\"%s\" はメニューが処理した\n", argv[1]);
            return 0;
        case KeyResult::kSent:
            std::printf("\"%s\" mod=0x%02X を送った (%s)\n", argv[1], mod,
                        ssh_is_connected() ? "SSH へ" : "端末へエコー");
            return 0;
    }
    return 0;
}

// 1 行入力中は打鍵をそこへ回す（#49 / #56）。定義はずっと下（プロファイル接続の側）。
bool line_prompt_input(const std::string& in);
// 対話ログインの QR を出している間の打鍵 (#59)。定義は ts の側。
bool auth_qr_input(const std::string& in);

// 入力を送る唯一の経路。画面キーボードも純正キーボードもここを通す。
// 未接続なら端末にエコーして、繋がなくても打鍵の確認ができるようにする。
void send_input(const std::string& s)
{
    if (s.empty()) return;
    // 対話ログインの QR を出している間は Esc で中止する (#59)。
    // **矢印キーも ESC で始まる**ので、単独の ESC だけを見る。
    if (auth_qr_input(s)) return;
    // **SSH へ送る前に見る。** 入力中のパスワードをリモートに漏らさない。
    if (line_prompt_input(s)) return;
    if (ssh_is_connected()) {
        ssh_send(s.data(), s.size());
        return;
    }
    // **未接続のエコーだけ手当てする。** リモートへ送るバイトは正しい
    // （Enter = CR、Backspace = DEL 0x7F）。それをそのまま端末に書くと、
    // CR は行頭に戻るだけで改行せず、DEL は VT の仕様どおり無視されるので、
    // 「Enter で次の行に行かない」「Backspace が効かない」に見える。
    // 普段は端末エミュレータの外（リモートの pty の ICRNL と行編集）がやっている仕事。
    // ponytail: 桁 0 より前へは消し戻さない。エコーの見た目だけの話なので足りている。
    std::string echo;
    echo.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\r') {
            echo += "\r\n";
        } else if (c == '\x7F') {
            echo += "\b \b";
        } else {
            echo += c;
        }
    }
    TermGuard guard;
    // **取れなかったら描かない。** render_term は PPA 転送を出すので、
    // ロックの外で走らせると他の描画とキャッシュを取り合って画面が壊れる。
    // 打鍵は 1 つ落ちるが、壊れた画面より落ちたエコーの方がまし。
    if (!guard.ok()) {
        ESP_LOGW(TAG, "send_input: 端末のロックが取れずエコーを捨てた (%d バイト)", (int)s.size());
        return;
    }
    term->write(echo);
    render_term();
}

// 打鍵をログに出すか（`kbdlog`）。**読み手は kbd タスクひとつに保つ** —
// I2C の FIFO を 2 つのタスクで取り合うと、イベントが片方に吸われて
// 「打っても出ないことがある」という再現しない形になる。
bool         s_kbd_log  = false;
TaskHandle_t s_kbd_task = nullptr;

// 純正キーボードを読んで送る。**INT (G50) は使わずポーリングしている。**
// 打鍵の間隔に対して 15ms は十分速く、GPIO の ISR と I2C の排他を足す理由がない。
// ponytail: 取りこぼしや消費電力が問題になったら INT の立ち下がりで起こす。
// メニューが出ている間はキーをメニューへ回す。**端末に流してはいけない** —
// メニュー表示中は render_term が描かないので、押した分が見えないまま溜まり、
// メニューを閉じた瞬間にまとめて噴き出す。
// 消費したら true（未対応のキーもメニュー中は捨てる）。
bool menu_consumed_key(const std::string& name)
{
    if (!menu || !menu->visible()) return false;
    ui::Key k;
    if (kbd_name_is(name, "up")) {
        k = ui::Key::kUp;
    } else if (kbd_name_is(name, "down")) {
        k = ui::Key::kDown;
    } else if (kbd_name_is(name, "enter")) {
        k = ui::Key::kEnter;
    } else if (kbd_name_is(name, "esc")) {
        k = ui::Key::kEsc;
    } else if (kbd_name_is(name, "left")) {
        k = ui::Key::kLeft;
    } else if (kbd_name_is(name, "right")) {
        k = ui::Key::kRight;
    } else {
        return true;
    }
    // 項目の文字列はメインループが定期的に作り直しているので、ここでは触らない
    // （set_info の材料集めは NVS と C6 への RPC を含むので、ロックの中でやらない）。
    TermGuard guard;
    if (!guard.ok()) return true;
    menu->key(k);
    menu->draw();
    return true;
}

// 打鍵を 1 つ処理する。**タスクも `kbdinject` もここを通す** —
// 分けると「合成では通るのに実キーでは通らない」（またはその逆）になって検証にならない。
// 送るものが無い（未対応のキー名）なら false。
KeyResult kbd_handle_key(const std::string& name, uint8_t mod)
{
    // **メニューの開閉はキーボードだけで往復できる必要がある (#51)。**
    // ステータスバーのタップは残す（キーボードを外したときの唯一の手段）。
    if (kbd_is_menu_key(name, mod)) {
        // **menu の null は見る。** 隣の menu_consumed_key も cmd_kbdhw も見ているし、
        // set_menu_visible 自身も見ている。ここだけ素で引くと不変条件が食い違う。
        if (!menu) return KeyResult::kMenu;
        TermGuard guard;
        if (!guard.ok()) return KeyResult::kMenu;
        set_menu_visible(!menu->visible());
        if (s_kbd_log) {
            std::printf("kbd: menu %s\n", menu->visible() ? "open" : "close");
        }
        return KeyResult::kMenu;
    }
    if (menu_consumed_key(name)) {
        if (s_kbd_log) std::printf("kbd: \"%s\" -> メニュー\n", name.c_str());
        return KeyResult::kMenu;
    }
    // 矢印の形は端末のモード (DECCKM) で変わる。vim や less がこれを切り替える。
    const bool        app_cursor = term && term->app_cursor_keys();
    const std::string out        = kbd_key_to_bytes(name, mod, app_cursor);
    if (s_kbd_log) {
        std::printf("kbd: \"%s\" mod=0x%02X ->", name.c_str(), mod);
        for (char c : out) std::printf(" %02X", (unsigned char)c);
        std::printf("\n");
    }
    if (out.empty()) return KeyResult::kUnknown;
    send_input(out);
    return KeyResult::kSent;
}

void kbd_task(void*)
{
    for (;;) {
        char    buf[16] = {};
        uint8_t mod     = 0;
        const int n     = kbd_hw::poll(buf, sizeof(buf) - 1, &mod);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        if (kbd_handle_key(std::string(buf, n), mod) == KeyResult::kUnknown) {
            ESP_LOGW(TAG, "未対応のキー名: \"%s\" (mod=0x%02X)", buf, mod);
        }
    }
}

// 純正キーボードの状態を見る。**ここでは読まない** — 読むのは kbd タスクだけ。
// 後から挿したときはここでタスクを立て直せる。
int cmd_kbdhw(int, char**)
{
    // **タスクが動いているならバスに触らない。** `lgfx::i2c` はポートごとの状態を
    // 持っていて再入できないので、別タスクと START/STOP を混ぜると転送が壊れる。
    // begin() はモードを書き直して FIFO も捨てるので、打鍵も黙って消える。
    if (s_kbd_task) {
        std::printf("キーボード fw=0x%02X、読み取りタスクは動いている"
                    "（打鍵を見るなら `kbdlog`）\n",
                    kbd_hw::version());
        return 0;
    }
    if (!kbd_hw::begin()) {
        std::printf("キーボードが見つからない (0x6D)。バスを舐める:\n");
        kbd_hw::scan();
        return 1;
    }
    std::printf("キーボード検出 fw=0x%02X\n", kbd_hw::version());
    // **失敗を成功と報告しない。** 立たなかったのに「立てた」と出すと、
    // 打鍵が来ない理由を配線と I2C の側で探すことになる（内蔵ヒープは狭い）。
    if (xTaskCreate(&kbd_task, "kbd", 8192, nullptr, 4, &s_kbd_task) != pdPASS) {
        s_kbd_task = nullptr;
        std::printf("読み取りタスクを立てられなかった（内蔵メモリ不足）\n");
        return 1;
    }
    if (menu) menu->set_has_keyboard(true);  // 操作説明をキーの案内に切り替える (#51)
    std::printf("読み取りタスクを立てた（`flip 1` で画面も回せる）\n");
    return 0;
}

// 打鍵をシリアルに出す。画面を見られないときの証跡。
int cmd_kbdlog(int argc, char** argv)
{
    s_kbd_log = (argc < 2) || (std::string(argv[1]) != "off");
    std::printf("kbd log: %s\n", s_kbd_log ? "on" : "off");
    return 0;
}

// 画面の向きを手で変える（キーボードを後から挿した / 検出に失敗したとき）。
int cmd_flip(int argc, char** argv)
{
    if (argc >= 2) {
        // **`off` も受ける。** 隣に並んでいる `kbd [off]` / `kbdlog [off]` と
        // 同じ表で README に載っているので、`flip off` は自然に打たれる。
        // 先頭 1 文字だけ見ていると `off` が「反転を有効にする」になっていた。
        const std::string a = argv[1];
        const bool        want_off = (a == "0" || a == "off" || a.empty());
        const bool        want_on  = (a == "1" || a == "on");
        if (!want_off && !want_on) {
            std::printf("usage: flip [0|1] (off/on も可)\n");
            return 1;
        }
        const bool on = want_on;
        if (on != screen::flipped()) {
            TermGuard guard;
            if (!guard.ok()) {
                std::printf("busy\n");
                return 1;
            }
            screen::set_flipped(on);
            display.setRotation(screen::rotation());
            display.fillScreen(TFT_BLACK);
            // 全部描き直す。**同じロックの中でやる**（PPA は転送のたびに出力側の
            // キャッシュを潰すので、端末とキーボードを別々に走らせてはいけない）。
            status_bar->draw(gather_status(), /*force=*/true);
            if (menu->visible()) {
                menu->draw(/*force=*/true);
            } else {
                if (keyboard->visible()) keyboard->draw();
                hide_auth_qr(/*redraw=*/false);  // 塗り直すので QR は畳む (#59)
                renderer->render(*term, /*force=*/true);
            }
        }
    }
    std::printf("画面: %s (rotation %d)\n", screen::flipped() ? "180 度反転" : "通常",
                (int)screen::rotation());
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
    set_keyboard_visible(show);
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
    tx.set_keypair(ik, esp_timer_get_time());
    rx.set_keypair(rk, esp_timer_get_time());
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
    // **QR を必ず畳む。** 承認されずにタイムアウトした / GOAWAY を食らった場合、
    // 畳まないと死んだ QR が残り、端末も更新されなくなる (#59)。
    // **畳むことをロックに依存させない。** TermGuard は 2 秒でタイムアウトする
    // （メニューの再描画は NVS 読みを含むので実際に長い）ので、取れたときだけ
    // 畳むと、取れなかった一回で s_auth_qr_active が true のまま ts タスクが
    // 消える。以後 render_term() は永久に早期 return し、auth_qr_input() が
    // ESC 以外の打鍵を全部食う。**ロックの外に出す必要があるのはフラグだけ。**
    // s_auth_qr_url は他の全アクセスがロックの下（show_auth_qr / set_menu_visible）で、
    // しかも show_auth_qr は &s_auth_qr_url を描画コールバックに渡すので、
    // 外から clear() すると生成中に解放済みバッファを読ませることになる。
    // 取れなくても実害は無い: run_once() が抜けるとき state が kFailed になるので、
    // メニューを開閉しても「まだ承認待ち」の条件が成立せず出し直されない。
    {
        TermGuard guard;
        if (guard.ok()) s_auth_qr_url.clear();
        hide_auth_qr(/*redraw=*/guard.ok());
    }
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

// Tailscale を上げる。**コンソールもメニューもここを通す** (#49) —
// 分けると「シリアルからは繋がるのに一覧から選ぶと繋がらない」になる。
// 対話ログイン (#59) の QR。定義はこの下（ts の状態表示の側）。
void term_note(const char* color, const std::string& text);

bool ts_start(const std::string& host, const std::string& authkey, uint16_t port, uint16_t capver,
              bool interactive = false)
{
    // **共有オブジェクトを触る前に弾く。** ts::Client は関数ローカル static で
    // 実体が 1 つしかないので、long-poll 中に set_config / set_map_handler を
    // 呼ぶと走行中のタスクが読んでいる std::string と std::function を
    // 差し替えることになる（use-after-free）。
    if (s_ts_task) {
        std::printf("already running (ts-status で状態、ts-stop で停止)\n");
        return false;
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
                return false;
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
                return false;
            }
            std::printf("generated and saved new machine/node/disco keys\n");
            have_keys = true;
        }
        nvs_close(nvs);
    }
    if (!have_keys) {
        std::printf("could not load or create keys\n");
        return false;
    }
    // これ以降 map handler が node_priv を WireGuard の秘密鍵として使う。
    s_ts_keys_ready = true;
    // **DISCO の鍵はここで入れる。** map handler は register_disco_peers を
    // 先に呼ぶので、鍵が無いと add_peer が全部拒否されて peers=0 になる
    // （実機で踏んだ: pings が来ても unknown として捨てられる）。
    if (!s_disco.set_key(disco_priv)) {
        std::printf("disco key setup failed\n");
        s_ts_keys_ready = false;
        return false;
    }

    static ts::Client client;
    ts::ClientConfig  cfg;
    cfg.host     = host;
    cfg.auth_key = authkey;
    cfg.port     = port;
    cfg.capability_version = capver;
    cfg.interactive = interactive;
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
        return false;
    }
    client.set_config(cfg);
    // 対話ログイン (#59)。**ts タスクの上で呼ばれる**ので、描くだけにして待たない。
    client.set_auth_url_handler([](const std::string& url) { show_auth_qr(url); });
    // netmap が来たらピアの disco 公開鍵を登録する。これが DISCO の前提。
    client.set_map_handler([](const std::string& json) {
        // netmap が来た = 登録が通った。**QR を畳む** — 残すと承認後も出たままになる。
        {
            TermGuard guard;
            if (guard.ok()) hide_auth_qr();
        }
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
        return false;
    }
    return true;
}

int cmd_ts(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: ts <host> <authkey> [port] [capver]\n");
        return 1;
    }
    return ts_start(argv[1], argv[2],
                    (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 80,
                    (argc > 4) ? static_cast<uint16_t>(atoi(argv[4])) : 131)
               ? 0
               : 1;
}

// authkey 無しで参加する (#59)。制御プレーンが返す AuthURL を QR で出して、
// 人間がブラウザで承認するまで register を投げ直す。
int cmd_ts_login(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: ts-login <host> [port] [capver]\n");
        return 1;
    }
    return ts_start(argv[1], /*authkey=*/"",
                    (argc > 2) ? static_cast<uint16_t>(atoi(argv[2])) : 80,
                    (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 131,
                    /*interactive=*/true)
               ? 0
               : 1;
}



// --- 対話ログインの QR (#59) ---
//
// **端末は URL を見せて待つだけ。** 認証は人間が手元のスマホ／PC のブラウザで
// 済ませる（Google なり GitHub なり）ので、ここに OAuth クライアントも
// 証明書検証も要らない。URL を画面から手で打たせるのは無理なので QR にする。
//
// **メニューと同じ M5GFX で、同じロックの中から描く**（PPA は転送のたびに
// 出力側のキャッシュを無効化するので、経路を分けると競合する）。
bool        s_auth_qr_active = false;
std::string s_auth_qr_url;

void draw_auth_qr_modules(esp_qrcode_handle_t qr, void* user_data)
{
    const int  n   = esp_qrcode_get_size(qr);
    const auto url = static_cast<const std::string*>(user_data);
    if (n <= 0) return;

    const int top   = s_status_h;
    const int avail = (int)display.height() - s_status_h - keyboard->height();
    display.fillRect(0, top, display.width(), avail, TFT_BLACK);
    display.setFont(&fonts::efontJA_24);
    display.setTextDatum(textdatum_t::top_left);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("Tailscale: ブラウザで開いて承認する", 24, top + 12);

    // 静穏帯 (quiet zone) を 4 モジュール取る。無いと読み取れない端末がある。
    constexpr int kQuiet = 4;
    const int     total  = n + kQuiet * 2;
    // 文字 2 行ぶん (上 40 / 下 64) を除いて入る大きさにする。
    const int scale =
        std::max(2, std::min((avail - 104) / total, static_cast<int>(display.width()) / total));
    const int side  = total * scale;
    const int qx    = ((int)display.width() - side) / 2;
    const int qy    = top + 40;

    // **白地に黒**で描く。反転すると読めない。
    display.fillRect(qx, qy, side, side, TFT_WHITE);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (!esp_qrcode_get_module(qr, x, y)) continue;
            display.fillRect(qx + (x + kQuiet) * scale, qy + (y + kQuiet) * scale, scale, scale,
                             TFT_BLACK);
        }
    }
    // QR が読めない場合のために URL も出す（打つのは辛いが、手がかりにはなる）。
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString(url->c_str(), 24, qy + side + 12);
    display.setTextColor(0x8410, TFT_BLACK);
    display.drawString("Esc で中止", 24, top + avail - 32);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
}

// QR を畳む。**端末領域を全部塗り直す経路は必ずここを通すこと** —
// 通さずに端末を描くと QR は消えるのにフラグが残り、以後 render_term() が
// 早期 return し続けて**端末が二度と更新されなくなる**（打鍵も食われ続ける）。
// redraw=false は「呼び出し側がこの後すぐ描く」場合（二度塗りを避ける）。
void hide_auth_qr(bool redraw)
{
    if (!s_auth_qr_active) return;
    s_auth_qr_active = false;
    // **URL は残す。** メニューから戻ったときに出し直せるようにする
    // （ts::Client は同じ URL では二度と handler を呼ばない）。
    if (redraw && renderer && term) renderer->render(*term, /*force=*/true);
}

bool auth_qr_input(const std::string& in)
{
    if (!s_auth_qr_active) return false;
    // **矢印キーで中止しない。** 特殊キーは ESC で始まる複数バイトなので、
    // 先頭だけ見ると「QR を見ている間に ↑ を押すと黙って中止」になる。
    if (in.size() == 1 && in[0] == '\033') {
        TermGuard guard;
        if (!guard.ok()) return true;
        // **URL も消す。** 消さないと、stop() から ts タスクが実際に抜けるまでの
        // 間にメニューを開閉するだけで、中止したはずの QR が描き直される
        // （state はまだ kAuthPending のため）。
        s_auth_qr_url.clear();
        hide_auth_qr();
        // **待っている ts タスクを止める。** 止めないと、承認されないまま
        // 5 分間 (kAuthTimeoutSec) register を投げ続ける。
        if (s_ts_client) s_ts_client->stop();
        term_note("33", "Tailscale の対話ログインを中止した");
        return true;
    }
    // それ以外の打鍵は捨てる（QR の上に文字が流れても読めなくなるだけ）。
    return true;
}

void show_auth_qr(const std::string& url)
{
    TermGuard guard;
    if (!guard.ok()) {
        // 描けなくても URL は残す。コンソールから `ts-status` で読める。
        ESP_LOGW(TAG, "auth url: %s (画面のロックが取れなかった)", url.c_str());
        return;
    }
    // メニューが出ていると重なるので閉じる。
    if (menu && menu->visible()) set_menu_visible(false);
    s_auth_qr_active = true;
    s_auth_qr_url    = url;
    esp_qrcode_config_t cfg = {};
    cfg.display_func_with_cb = &draw_auth_qr_modules;
    cfg.user_data            = &s_auth_qr_url;
    // URL は 60 文字前後。version 6 (41x41) まで見ておけば足りる。
    cfg.max_qrcode_version   = 6;
    cfg.qrcode_ecc_level     = ESP_QRCODE_ECC_MED;
    if (esp_qrcode_generate(&cfg, url.c_str()) != ESP_OK) {
        // 生成できなくても止めない。URL だけでも出す。
        s_auth_qr_active = false;
        ESP_LOGE(TAG, "QR を作れなかった: %s", url.c_str());
        term_note("33", "Tailscale: ブラウザで開いて承認する: " + url);
    }
    ESP_LOGI(TAG, "auth url: %s", url.c_str());
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
    // **名前も出す。** 数字だけだと、State に値を挿したときに過去のログの
    // `state=5` が別の意味になる（実際 kAuthPending を挿して繰り上がった）。
    std::printf("  state=%d (%s) registered=%d map_messages=%u keepalives=%u\n", (int)st.state,
                ts::state_name(st.state), st.registered ? 1 : 0, (unsigned)st.map_messages,
                (unsigned)st.keepalives);
    if (!st.assigned_address.empty()) {
        std::printf("  assigned address: %s\n", st.assigned_address.c_str());
    }
    if (!st.domain.empty()) std::printf("  domain: %s\n", st.domain.c_str());
    // **対話ログインの URL はここから読める。** 画面のロックが取れず QR を
    // 描けなかったときの唯一の手がかりなので、必ず出す (#59)。
    if (!st.auth_url.empty()) std::printf("  auth url: %s\n", st.auth_url.c_str());
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
    // 対話ログインの QR を出したまま止めると、端末が更新されないまま残る (#59)。
    // ts_task の後始末と同じ理由で、畳むこと自体はロックに依存させない
    // （URL はロックの下でだけ触る。理由は ts_task 側のコメント）。
    {
        TermGuard guard;
        if (guard.ok()) s_auth_qr_url.clear();
        hide_auth_qr(/*redraw=*/guard.ok());
    }
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
    std::snprintf(mi.sd, sizeof(mi.sd), "%s", s_profiles_status);
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

// タッチのルーティング。**実タッチと `tap` / `swipe` コマンドで同じ経路を通す。**
// 分けると「指だと動くがコマンドだと動かない」（または逆）になって、
// 検証にならない。ロックは呼び出し側が取っている。
// 画面キーボードを出し入れする。**コンソール (`kbd`) もダブルタップもここを通す** —
// 分けると「指では消えるがコマンドでは消えない」（またはその逆）になって検証にならない。
// ロックは呼び出し側が持っている。
void set_keyboard_visible(bool show)
{
    if (!keyboard || !renderer || !term) return;
    s_keyboard_wanted = show;
    if (keyboard->visible() == show) return;
    keyboard->set_visible(show);  // 出すときは KeyboardUi 側が描く
    apply_layout();
    // **QR は端末領域に描いてある。** 畳まずに塗り直すと、消えたのにフラグが
    // 残って端末が二度と更新されなくなる。
    hide_auth_qr(/*redraw=*/false);
    // 隠すときはキーボードが居た帯まで端末の行が伸びるので、force で全部塗り直す。
    renderer->render(*term, /*force=*/true);
    ESP_LOGI(TAG, "keyboard %s, terminal %dx%d", show ? "shown" : "hidden", renderer->cols(),
             renderer->rows());
}

void touch_down_at(int x, int y)
{
    if (y < kStatusTapH) {
        // ステータスバーをタップでメニューを開閉する。純正キーボードが来るまで、
        // 指だけでメニューに戻れる経路はここだけなので、当たり判定は描画（24px）
        // より広く取る。24px は 3.5mm しかない。
        set_menu_visible(!menu->visible());
    } else if (menu->visible()) {
        // メニュー表示中はキーボードを隠しているので、ここで全部食う。
        if (menu->touch_down(x, y)) menu->draw();
    } else if (!keyboard->touch_down(x, y)) {
        // キーボード外 = 端末領域。縦スワイプでスクロールバックを見る。
        s_swipe_start_y      = y;
        s_swipe_start_offset = term->view_offset();
        tap_down(&s_tap, x, y, esp_timer_get_time(), s_swipe_start_offset);
    }
}

void touch_move_at(int x, int y)
{
    if (s_swipe_start_y >= 0) {
        // 端末領域のドラッグ。**下に引いたら過去に戻る**（紙を下に引く感覚）。
        // 移動量をセル単位に落として絶対位置で当てる。相対で足していくと
        // 20ms ごとのポーリングの取りこぼしでずれていく。
        const int cells = (y - s_swipe_start_y) / renderer->cell_h();
        const int want  = s_swipe_start_offset + cells;
        if (const int delta = want - term->view_offset(); delta != 0) {
            if (term->scroll_view(delta) != 0) render_term();
        }
        return;
    }
    // メニュー表示中はドラッグをキーボードに渡さない（見えていないので）。
    if (!menu->visible()) keyboard->touch_move(x, y);
}

void touch_up_at(int x, int y)
{
    if (s_swipe_start_y >= 0) {
        ESP_LOGI(TAG, "scrollback offset %d / %d lines", term->view_offset(),
                 term->scrollback_lines());
        s_swipe_start_y = -1;
        // **キーボードの帯は数えない。** 帯にはキーの無い隙間（左右の余白と
        // 上端の候補表示）があり、そこは touch_down が false を返して
        // ここへ流れてくる。数えると入力中にキーボードが消える。
        const int keyboard_top = (int)display.height() - keyboard->height();
        if (tap_up(&s_tap, x, y, esp_timer_get_time(), term->view_offset(), keyboard_top)) {
            set_keyboard_visible(!keyboard->visible());
        }
        return;
    }
    if (menu->visible()) return;  // 押した時点で決まっているので、離すのは無視する
    if (!keyboard->touch_up(x, y)) ESP_LOGI(TAG, "touch up");
}

void render_term(bool force)
{
    if (!renderer || !term) return;
    // メニューが開いている間は描かない。term->write は続けるので内容は
    // 失われず、閉じるときの force render で追いつく。
    if (menu && menu->visible()) return;
    // QR を出している間も同じ（上書きすると読めなくなる）(#59)。
    if (s_auth_qr_active) return;
    renderer->render(*term, force);
}

void cancel_line_prompt();
void refresh_wifi_nets();

void set_menu_visible(bool show)
{
    if (!menu) return;
    // 入力途中でメニューへ逃げると、以後の打鍵が全部プロンプトに吸われて
    // SSH セッションに届かなくなる（`*` だけが出る）。
    cancel_line_prompt();
    s_swipe_start_y = -1;  // 掴んだままメニューに移ると、次のドラッグが飛ぶ
    if (show) hide_auth_qr(/*redraw=*/false);  // QR とメニューが重なる (#59)
    menu->set_visible(show);
    // メニューはキーボードの領域を覆わないので、隠さないと指でキーを押せてしまう。
    // 端末に戻るときは**ユーザが選んだ状態**に戻す（#54 でダブルタップから
    // 切り替えられるようになったので、開くたびに勝手に出すと選択が消える）。
    keyboard->set_visible(!show && s_keyboard_wanted);
    if (show) {
        menu->set_info(gather_menu_info(gather_status()));
        refresh_wifi_nets();  // `*` の位置は繋ぎ直しで変わる (#56)
    }
    apply_layout();
    if (show) {
        menu->draw(/*force=*/true);
    } else {
        // キーボードは set_visible(true) が中で描いている。ここで描くと二度塗り。
        renderer->render(*term, /*force=*/true);
        // **まだ承認待ちなら QR を出し直す (#59)。** ts::Client は同じ URL では
        // 二度と handler を呼ばないので、ここで戻さないと二度と出せない。
        if (!s_auth_qr_url.empty() && s_ts_client &&
            s_ts_client->snapshot().state == ts::ClientStatus::State::kAuthPending) {
            show_auth_qr(s_auth_qr_url);
        }
    }
    // ラベルを MENU / CLOSE に切り替える。開閉のたびに必ず描き直す。
    status_bar->draw(gather_status(), /*force=*/true);
}

// タッチを合成する。**実タッチと同じ関数を呼ぶ**ので、経路の検証になる
// （タッチ IC そのものは実機で反応することを確認済み。残るのは座標から先の
// 振り分けで、それがここで測れる）。指で触れない環境でも UI を検証できる。
// **描画側 (components/rotate) とタッチ側 (M5GFX) の回転が一致しているかを、
// 指なしで照合する。**
//
// M5GFX の `convertRawXY` は「生座標 → アフィン変換 → 回転」を適用する公開関数なので、
// 生座標を合成して通せる。rotation 1 では swap してから ty を反転するので、
// 生座標がネイティブ画素と一致するなら (ny, native_w-1-nx) になる。
// components/rotate の native_to_landscape も同じ式なので、一致すれば
// 2 つの実装が同じ向きを指していることになる。
//
// **これでも残るもの**: 「生座標 (0,0) が物理的にパネルの左上か」は M5GFX の
// ボード固有のキャリブレーション（アフィン係数）の話で、こちらのコードではない。
// そこは指で押して確かめるしかない（touchlog を使う）。
int cmd_touchmap(int, char**)
{
    rot::Panel panel;  // 既定値が 720x1280
    panel.flipped = screen::flipped();

    const uint8_t prev = display.getRotation();
    display.setRotation(screen::rotation());
    std::printf("M5GFX の convertRawXY と components/rotate を照合 (rotation %d):\n",
                (int)screen::rotation());

    // **ビット一致は要求しない。** タッチ側は float のアフィン変換を通り
    // （`_affine[0] * (float)x + ...` を int32 に切り捨てる）、そのあとの
    // swap と反転は整数。描画側 (rot::native_to_landscape) は最初から整数なので、
    // 差は「アフィン係数の誤差 x 座標」＋切り捨てになる。
    // **誤差はスケール由来なので座標が大きいところで最大になる。** だから最端
    // (719 / 1279) を必ず踏む。四隅を別扱いで印字するだけにすると、
    // 一番効いてほしい点が判定から外れる。
    constexpr int kTolerance = 1;
    int max_dx = 0, max_dy = 0, bad = 0, checked = 0;
    int wx_nx = 0, wx_ny = 0, wy_nx = 0, wy_ny = 0;

    auto check = [&](int nx, int ny) {
        lgfx::touch_point_t tp{};
        tp.x = (int16_t)nx;
        tp.y = (int16_t)ny;
        display.convertRawXY(&tp, 1);
        int lx = 0, ly = 0;
        rot::native_to_landscape(panel, nx, ny, &lx, &ly);
        const int dx = std::abs((int)tp.x - lx);
        const int dy = std::abs((int)tp.y - ly);
        ++checked;
        // dx と dy で別々に最悪点を覚える。1 点に混ぜると、印字した座標と
        // 数字の出所が食い違う。
        if (dx > max_dx) {
            max_dx = dx;
            wx_nx  = nx;
            wx_ny  = ny;
        }
        if (dy > max_dy) {
            max_dy = dy;
            wy_nx  = nx;
            wy_ny  = ny;
        }
        if (dx > kTolerance || dy > kTolerance) {
            ++bad;
            std::printf("  native (%3d,%4d) -> m5gfx(%4d,%3d) rotate(%4d,%3d) OVER TOLERANCE\n",
                        nx, ny, (int)tp.x, (int)tp.y, lx, ly);
        }
        return std::pair<int, int>{(int)tp.x, (int)tp.y};
    };

    // 全域を格子で舐める。最端を必ず含めるため、刻みを進めたあとに端へ丸める。
    for (int nx = 0; nx < panel.native_w; nx = (nx + 37 >= panel.native_w - 1 && nx != panel.native_w - 1)
                                                   ? panel.native_w - 1
                                                   : nx + 37) {
        for (int ny = 0; ny < panel.native_h;
             ny = (ny + 53 >= panel.native_h - 1 && ny != panel.native_h - 1) ? panel.native_h - 1
                                                                              : ny + 53) {
            check(nx, ny);
            if (ny == panel.native_h - 1) break;
        }
        if (nx == panel.native_w - 1) break;
    }

    // 四隅は個別にも出す（読み手が一番見たい値）。**判定は上の格子に含まれている。**
    const struct { int nx, ny; const char* name; } corners[] = {
        {0, 0, "左上"},
        {panel.native_w - 1, 0, "右上"},
        {0, panel.native_h - 1, "左下"},
        {panel.native_w - 1, panel.native_h - 1, "右下"},
    };
    for (const auto& c : corners) {
        lgfx::touch_point_t tp{};
        tp.x = (int16_t)c.nx;
        tp.y = (int16_t)c.ny;
        display.convertRawXY(&tp, 1);
        int lx = 0, ly = 0;
        rot::native_to_landscape(panel, c.nx, c.ny, &lx, &ly);
        std::printf("  %s native (%3d,%4d) -> m5gfx(%4d,%3d) rotate(%4d,%3d) 差 (%d,%d)\n", c.name,
                    c.nx, c.ny, (int)tp.x, (int)tp.y, lx, ly, std::abs((int)tp.x - lx),
                    std::abs((int)tp.y - ly));
    }
    display.setRotation(prev);

    const int cw = renderer ? renderer->cell_w() : 0;
    const int ch = renderer ? renderer->cell_h() : 0;
    std::printf("grid %d 点 (最端を含む): 最大ずれ dx=%d (native %d,%d) dy=%d (native %d,%d)\n",
                checked, max_dx, wx_nx, wx_ny, max_dy, wy_nx, wy_ny);
    std::printf("touch/render rotation agreement: %s (許容 %d 画素, 超え %d 点)\n",
                bad == 0 ? "ok" : "MISMATCH", kTolerance, bad);
    // セル境界では 1 画素のずれで隣のセルになる（x=11 が 12 と報告されれば隣）。
    // 「別のセルにならない」は偽なので、「隣までで済む」と書く。
    std::printf("  ずれは float のアフィン変換の切り捨て。セルは %dx%d なので、"
                "ずれても隣のセルまで（誤差はセル 1 個未満）\n",
                cw, ch);
    std::printf("  残るのは「IC が物理的な角で報告する生座標の範囲」= パネルと IC の"
                "個体差。touchlog で指で確かめる\n");
    return bad == 0 ? 0 : 1;
}

// 実タッチの座標をログに出す。**描画側 (components/rotate) とタッチ側 (M5GFX) は
// 回転の実装が別物**なので、この 2 系がずれていないかは指で押して座標を見るしか
// 確かめる方法がない（tap / swipe は M5GFX の変換より下流に注入するので届かない）。
int cmd_touchlog(int argc, char** argv)
{
    s_touch_log = (argc < 2) || (std::string(argv[1]) != "off");
    std::printf("touch log %s. 画面を押すと (x,y) が出ます。\n", s_touch_log ? "on" : "off");
    if (s_touch_log) {
        std::printf("  四隅の期待値: 左上 (0,0) 付近 / 右上 (%d,0) / 左下 (0,%d) / 右下 (%d,%d)\n",
                    (int)display.width() - 1, (int)display.height() - 1, (int)display.width() - 1,
                    (int)display.height() - 1);
    }
    return 0;
}

int cmd_tap(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: tap <x> <y>\n");
        return 1;
    }
    const int x = atoi(argv[1]);
    const int y = atoi(argv[2]);
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    touch_down_at(x, y);
    touch_up_at(x, y);
    std::printf("tap (%d,%d): menu=%s keyboard=%s terminal=%dx%d scrollback=%d/%d\n", x, y,
                menu->visible() ? "shown" : "hidden", keyboard->visible() ? "shown" : "hidden",
                renderer->cols(), renderer->rows(), term->view_offset(),
                term->scrollback_lines());
    return 0;
}

// 縦スワイプを合成する。y1 から y2 まで、実タッチと同じ刻み（20ms ごとの
// ポーリング相当）で動かす。
int cmd_swipe(int argc, char** argv)
{
    if (argc < 4) {
        std::printf("usage: swipe <x> <y_from> <y_to> [x_to]\n");
        std::printf("       x_to を付けると斜め・横方向になる（IME の横フリック用）\n");
        return 1;
    }
    const int x  = atoi(argv[1]);
    const int y1 = atoi(argv[2]);
    const int y2 = atoi(argv[3]);
    // **横方向も出せるようにする。** x 固定だと ime::Flick の kLeft / kRight に
    // 到達できず、IME の入力面の半分が指でしか試せない。
    const int x2 = (argc > 4) ? atoi(argv[4]) : x;
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    const int before = term->view_offset();
    // **メニューの状態は dispatch の前に読む。** 後で読むと、メニュー項目が発火して
    // 閉じた後の状態を見てしまい「menu に食われた」を取り逃がす（実機で踏んだ）。
    const bool menu_was_shown = menu->visible();
    touch_down_at(x, y1);
    // どこに食われたかを出す。「0 -> 0」の理由が分からないと、メニューに食われた・
    // 代替画面で固定された・本当に動かない の区別がつかない（実際に取り違えた）。
    const char* target = (s_swipe_start_y >= 0) ? "terminal"
                         : menu_was_shown       ? "menu"
                                                : "keyboard/status";
    // 実タッチと同じ刻み（20ms ごとのポーリング相当）で動かす。
    constexpr int kSteps = 8;
    for (int i = 1; i <= kSteps; ++i) {
        touch_move_at(x + (x2 - x) * i / kSteps, y1 + (y2 - y1) * i / kSteps);
    }
    touch_up_at(x2, y2);
    std::printf("swipe (%d,%d)->(%d,%d) [%s]: scrollback %d -> %d / %d lines%s\n", x, y1, x2, y2,
                target, before, term->view_offset(), term->scrollback_lines(),
                term->alt_screen() ? " (alt screen: scrollback is pinned)" : "");
    return 0;
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
    ta.set_keypair(ka, esp_timer_get_time());
    tb.set_keypair(kb, esp_timer_get_time());
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
        tb.set_keypair(kb2, esp_timer_get_time());  // kb2.initiator == false なので未確認として入る
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
        ta.set_keypair(ka2, esp_timer_get_time());  // ka2.initiator == true なので確認済みで入る
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
        tb.set_keypair(kb3, esp_timer_get_time());
        tb.set_keypair(kb4, esp_timer_get_time());
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
    ip4addr_aton(kTunnelMask, &mask);

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
// 16 進文字列 -> バイト列。長さは呼び出し側が確かめる。
bool hex_to_bytes(const std::string& hex, uint8_t* out, size_t n)
{
    if (hex.size() != n * 2) return false;
    auto nib = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = nib(hex[i * 2]);
        const int lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

// WireGuard の鍵 (32 バイト) を読む。**base64 が標準の表記**（`wg genkey` の出力）
// なので base64 を先に試し、駄目なら 16 進も受ける。前後の空白と改行は捨てる。
bool decode_key32(const std::string& text, uint8_t out[32])
{
    std::string t;
    for (char c : text) {
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') t += c;
    }
    if (t.empty()) return false;
    size_t len = 0;
    if (mbedtls_base64_decode(out, 32, &len, reinterpret_cast<const unsigned char*>(t.data()),
                              t.size()) == 0 &&
        len == 32) {
        return true;
    }
    return hex_to_bytes(t, out, 32);
}

// トンネルを上げてピアを設定する。**コンソール (`wg`) も SD の接続先 (#49) も
// ここを通す** — 分けると片方だけ wire_netif を呼び忘れて DISCO が死ぬ
// （実機で踏んだのと同じ形）。addr/mask は点表記、peer_pub は 32 バイト、
// endpoint が空ならピアは設定しない（netif を上げるだけ）。
bool wg_bring_up(const uint8_t priv[32], const char* addr_str, const char* mask_str,
                 const uint8_t* peer_pub, const std::string& endpoint, std::string* err)
{
    auto& nif = wg::netif_instance();
    ip4_addr_t addr, mask;
    if (!ip4addr_aton(addr_str, &addr)) {
        *err = std::string("bad tunnel ip: ") + addr_str;
        return false;
    }
    if (!ip4addr_aton(mask_str, &mask)) {
        *err = std::string("bad netmask: ") + mask_str;
        return false;
    }

    wire_netif(nif);

    if (nif.is_up()) {
        // **上がっているものと食い違ったら断る。** Netif::up は鍵をコピーするので
        // 上がった後に差し替える手段が無く、黙って続けると「1 本目の鍵と
        // アドレスのまま 2 本目のピアに握手する」ことになる（原因が分からない）。
        if (!s_wg_live || std::memcmp(s_wg_priv, priv, 32) != 0 ||
            s_wg_addr.addr != addr.addr || s_wg_mask.addr != mask.addr) {
            *err = "別の設定でトンネルが上がっている（先に `wg down`）";
            return false;
        }
    } else {
        const esp_err_t e = nif.up(priv, addr, mask);
        if (e != ESP_OK) {
            *err = std::string("netif up failed: ") + esp_err_to_name(e) + " (" +
                   nif.last_error() + ")";
            return false;
        }
        s_netif_key = NetifKey::kOwn;
        std::memcpy(s_wg_priv, priv, 32);
        s_wg_addr = addr;
        s_wg_mask = mask;
        s_wg_live = true;
    }
    if (endpoint.empty() || !peer_pub) return true;

    wg::PeerConfig peer;
    std::memcpy(peer.public_key, peer_pub, 32);
    peer.endpoint = endpoint;
    if (const esp_err_t e = nif.set_peer(peer); e != ESP_OK) {
        *err = std::string("set_peer failed: ") + esp_err_to_name(e) + " (" + nif.last_error() + ")";
        return false;
    }
    return true;
}

int cmd_wg(int argc, char** argv)
{
    auto& nif = wg::netif_instance();

    if (argc >= 2 && std::string(argv[1]) == "down") {
        nif.down();
        s_netif_key         = NetifKey::kNone;
        s_tunnel_peer_valid = false;
        s_wg_live           = false;
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

    if (argc == 3) {
        // 公開鍵だけ渡されても接続できない。黙って netif だけ上げると原因が分からない。
        std::printf("peer endpoint is missing: wg <tunnel-ip> <pubkey> <host:port>\n");
        return 1;
    }
    uint8_t     peer_pub[32] = {};
    std::string endpoint;
    if (argc >= 4) {
        const std::string hex = argv[2];
        // std::stoul は例外を投げる。例外を捕まえていないので、打ち間違いで abort してしまう。
        if (hex.size() != 64 || !hex_to_bytes(hex, peer_pub, 32)) {
            std::printf("peer pubkey must be 64 hex chars\n");
            return 1;
        }
        endpoint = argv[3];
    }

    std::string err;
    if (!wg_bring_up(priv, argv[1], kTunnelMask, endpoint.empty() ? nullptr : peer_pub, endpoint,
                     &err)) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    if (!endpoint.empty()) std::printf("handshake started with %s\n", endpoint.c_str());
    return 0;
}

// --- SD の接続先に繋ぐ (#49) ---
//
// **メニューもコンソール (`connect`) もここを通す。** 分けると
// 「一覧から選ぶと繋がらないがシリアルからは繋がる」という形の食い違いが出る。

// 端末に 1 行出す。**ロックは自分で取る** — 接続はワーカタスクで走るので、
// 呼び出し側が持っている保証がない（s_term_lock は再帰なので入れ子でも安全）。
void term_note(const char* color, const std::string& text)
{
    if (!term) return;
    TermGuard guard;
    if (!guard.ok()) return;
    term->write("\r\n\033[" + std::string(color) + "m" + text + "\033[m\r\n");
    render_term();
}

bool connect_vpn_profile(const prof::Profile& p, std::string* err)
{
    if (p.type == prof::Type::kTailscale) {
        std::string authkey;
        if (!read_key(p.authkey, &authkey, err)) return false;
        // ファイルなので末尾の改行が付く。そのまま送るとヘッダが壊れる。
        while (!authkey.empty() && (authkey.back() == '\n' || authkey.back() == '\r' ||
                                    authkey.back() == ' ')) {
            authkey.pop_back();
        }
        if (authkey.empty()) {
            *err = "authkey \"" + p.authkey + "\" が空";
            return false;
        }
        if (!ts_start(p.control, authkey, p.port, 131)) {
            *err = "tailscale を起動できなかった（詳細はシリアル）";
            return false;
        }
        return true;
    }

    // WireGuard。
    std::string priv_text;
    if (!read_key(p.private_key, &priv_text, err)) return false;
    uint8_t priv[32];
    if (!decode_key32(priv_text, priv)) {
        *err = "鍵 \"" + p.private_key + "\" が 32 バイトの鍵ではない";
        return false;
    }
    uint8_t peer_pub[32];
    if (!decode_key32(p.peer.pubkey, peer_pub)) {
        *err = "peer.pubkey が 32 バイトの鍵ではない";
        return false;
    }
    std::string addr;
    int         prefix = 32;
    if (!prof::split_cidr(p.address, &addr, &prefix)) {
        *err = "address が IPv4/CIDR ではない: " + p.address;
        return false;
    }
    // **経路は netif のアドレス 1 本ぶんしか持てない。** lwIP にポリシー
    // ルーティングは無いので、allowed_ips の先頭をネットマスクとして使う。
    // ponytail: 2 本目以降は無視する。複数レンジが要るなら netif に経路を
    // 足す仕組みから作ることになる → その時に #49 の続きとして切る。
    int mask_prefix = prefix;
    if (!p.peer.allowed_ips.empty()) {
        std::string ignored;
        if (!prof::split_cidr(p.peer.allowed_ips[0], &ignored, &mask_prefix)) {
            *err = "allowed_ips[0] が IPv4/CIDR ではない: " + p.peer.allowed_ips[0];
            return false;
        }
    }
    const std::string mask = prof::prefix_to_mask(mask_prefix);
    if (!wg_bring_up(priv, addr.c_str(), mask.c_str(), peer_pub, p.peer.endpoint, err)) {
        return false;
    }
    return true;
}

// 画面から 1 行入力させる (#49 / #56)。**SD は抜けば誰でも読める**ので、
// SSH の `auth: "password"` で password を書いていなければここに来る。
// WiFi のパスワードと隠し SSID も同じ入り口を使う。
//
// 打鍵は画面キーボードも純正キーボードも send_input を通るので、そこで横取りする。
bool        s_prompt_active = false;
bool        s_prompt_mask   = false;  // 伏せ字にする（肩越しに見えないように）
// プロンプトの間だけ画面キーボードを abc にするので、元のモードを覚えておく。
bool        s_prompt_prev_direct = false;
std::string s_prompt_buf;
// Enter で呼ぶ。**呼ぶ前にプロンプトを畳む**ので、この中から次の入力を始めてよい
// （隠し SSID は SSID → パスワードと 2 回続けて聞く）。
std::function<void(const std::string&)> s_prompt_done;

void start_connect(int index, const std::string& password, bool have_password);

// プロンプトを畳む。**メニューを開閉したときにも呼ぶ** — 入力途中で
// メニューに逃げると、以後の打鍵が全部 s_prompt_buf に吸われて
// SSH セッションに届かなくなる（`*` だけが出る）。
void cancel_line_prompt()
{
    if (!s_prompt_active) return;
    s_prompt_active = false;
    s_prompt_mask   = false;
    s_prompt_buf.clear();
    // **続きの処理も捨てる。** 残すと、次に別のプロンプトを開いたときに
    // 前回の続きが動く（SSID を聞いていたつもりが VPN に繋ぎに行く）。
    s_prompt_done = nullptr;
    if (keyboard) keyboard->set_direct(s_prompt_prev_direct);
    // **畳んだことを端末に書く。** 伏せ字だけが残っていると、入力が
    // まだ続いているように見える（実際には次の打鍵は端末へ流れる）。
    term_note("33", "canceled");
}

void start_line_prompt(const std::string& label, bool mask,
                       std::function<void(const std::string&)> done)
{
    // **かな（フリック）のままでは打てない。** パスワードも SSID も英数なので、
    // 開いている間だけ abc にして、終わったら元のモードへ戻す。
    if (keyboard && !s_prompt_active) {
        s_prompt_prev_direct = keyboard->direct();
        keyboard->set_direct(true);
    }
    s_prompt_active = true;
    s_prompt_mask   = mask;
    s_prompt_buf.clear();
    s_prompt_done   = std::move(done);
    term_note("33", label);
}

// 入力を食ったら true。
bool line_prompt_input(const std::string& in)
{
    if (!s_prompt_active) return false;
    // **矢印キーで中止しない。** DECCKM を含め特殊キーは ESC で始まる複数バイトなので、
    // 先頭の ESC だけ見ると「パスワード入力中に ↑ を押すと黙って中止」になる。
    if (in.size() > 1 && in[0] == '\033') return true;
    TermGuard guard;
    if (!guard.ok()) return true;  // 食ったことにする（端末に漏らさない）
    for (char c : in) {
        if (c == '\r' || c == '\n') {
            // **先に畳んでから呼ぶ。** 続きの中で次のプロンプトを開くので、
            // 後で畳むとそれを消してしまう。
            auto              done = std::move(s_prompt_done);
            const std::string line = s_prompt_buf;
            s_prompt_active = false;
            s_prompt_mask   = false;
            s_prompt_buf.clear();
            s_prompt_done = nullptr;
            // **続きが次のプロンプトを開くなら、その中でまた abc にする。**
            // ここで戻しておかないと、2 段目 (SSID → パスワード) を抜けたときに
            // 元のモードが失われる。
            if (keyboard) keyboard->set_direct(s_prompt_prev_direct);
            term->write("\r\n");
            render_term();
            if (done) done(line);
            return true;
        }
        if (c == '\033') {  // Esc 単独で中止
            cancel_line_prompt();
            return true;
        }
        if (c == '\x7F' || c == '\b') {
            if (!s_prompt_buf.empty()) {
                s_prompt_buf.pop_back();
                term->write("\b \b");
            }
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) continue;  // 制御文字は捨てる
        if (s_prompt_buf.size() < 128) {
            s_prompt_buf += c;
            term->write(s_prompt_mask ? "*" : std::string(1, c));
        }
    }
    render_term();
    return true;
}

// SSH のパスワードを聞く。**プロファイルは index で持つ** — 値でコピーして持つと、
// 待っている間に `profiles reload` されたときに古い設定に繋いでしまう。
void start_password_prompt(int index, const std::string& user, const std::string& host)
{
    start_line_prompt("password for " + user + "@" + host + " (Enter で接続 / Esc で中止):",
                      /*mask=*/true, [index](const std::string& pw) {
                          start_connect(index, pw, /*have_password=*/true);
                      });
}

// via の解決結果。**s_profiles をロックの外で引かない**ために、呼び出し側が
// ロックの中でコピーして渡す（`profiles import` / `clear` が同じ vector を
// 再代入するので、参照のまま持つと読んでいる最中に解放され得る）。
struct ViaTarget {
    bool          present = false;  // via が指定されていて、解決できた
    bool          named   = false;  // via が指定されていた
    prof::Profile profile;
};

void connect_ssh_profile(const prof::Profile& p, int index, const ViaTarget& via)
{
    if (p.ask_password) {
        start_password_prompt(index, p.user, p.host);
        return;
    }
    // 先に VPN を張る（`via`）。**張れなければ繋ぎに行かない** —
    // VPN 越しの相手に素の経路で繋ぎに行くと、無関係の相手に当たり得る。
    if (via.named) {
        if (!via.present) {
            term_note("31", "via \"" + p.via + "\" が見つからない");
            return;
        }
        const prof::Profile* v = &via.profile;
        // **Tailscale だけ「動いていれば飛ばす」。** ts::Client は実体が 1 つで、
        // 走行中に設定を差し替えると use-after-free になる。WireGuard 側は
        // wg_bring_up が食い違いを見るので、そのまま通してよい。
        const bool skip = (v->type == prof::Type::kTailscale) && (s_ts_task != nullptr);
        if (!skip) {
            term_note("33", "bringing up " + p.via + " (" + prof::type_name(v->type) + ")...");
            std::string err;
            if (!connect_vpn_profile(*v, &err)) {
                term_note("31", "via " + p.via + ": " + err);
                return;
            }
        }
    }

    SshConfig cfg;
    cfg.host     = p.host;
    cfg.user     = p.user;
    cfg.port     = p.port;
    cfg.password = p.password;
    if (!p.key.empty()) {
        std::string err;
        if (!read_key(p.key, &cfg.key_pem, &err)) {
            term_note("31", err);
            return;
        }
    }
    char line[128];
    std::snprintf(line, sizeof(line), "connecting to %s@%s:%u...", cfg.user.c_str(),
                  cfg.host.c_str(), (unsigned)cfg.port);
    term_note("33", line);
    if (esp_err_t err = ssh_connect(cfg, renderer->cols(), renderer->rows()); err != ESP_OK) {
        ESP_LOGE(TAG, "ssh_connect failed: %s (%s)", esp_err_to_name(err), ssh_last_error());
        term_note("31", std::string("connect failed: ") + ssh_last_error());
    }
}

// --- 接続はワーカタスクで走らせる ---
//
// **X25519 は 1 回で 10KB 近くスタックを使う**（CLAUDE.md）。VPN を上げる経路は
// ハンドシェイクの生成と鍵導出を含むので、呼び出し元（kbd タスク 8KB /
// メインループ 8KB）の上で走らせるとスタック保護フォルトになる。
// コンソールの `connect` からも同じタスクに載せる（経路を 1 本に保つ）。
struct ConnectReq {
    int         index    = -1;
    std::string password;
    bool        have_password = false;
};
ConnectReq   s_connect_req;
TaskHandle_t s_connect_task = nullptr;

void connect_worker(void*)
{
    const ConnectReq req = s_connect_req;
    // **index はここで引き直す。** 待っている間に `profiles reload` が
    // 走っていれば、そのときは黙って何もしない方がよい。
    prof::Profile p;
    ViaTarget     via;
    bool          found = false;
    {
        TermGuard guard;
        if (guard.ok() && req.index >= 0 &&
            req.index < static_cast<int>(s_profiles.profiles.size())) {
            p     = s_profiles.profiles[req.index];
            found = true;
            // **via もここで値にする。** ロックを離した後に s_profiles を引くと、
            // コンソールの `profiles import` / `clear` による再代入と競合する。
            if (!p.via.empty()) {
                via.named = true;
                if (const prof::Profile* v = prof::find(s_profiles, p.via)) {
                    if (v->type != prof::Type::kSsh) {
                        via.present = true;
                        via.profile = *v;
                    }
                }
            }
        }
    }
    if (!found) {
        term_note("31", "接続先が見つからない（読み直された？）");
        s_connect_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    if (req.have_password) {
        p.password     = req.password;
        p.ask_password = false;
    }

    if (p.type == prof::Type::kSsh) {
        connect_ssh_profile(p, req.index, via);
    } else {
        term_note("33", "bringing up " + p.name + " (" + prof::type_name(p.type) + ")...");
        std::string err;
        if (!connect_vpn_profile(p, &err)) {
            term_note("31", p.name + ": " + err);
        } else {
            term_note("32", p.name + ": started (詳細は `ts-status` / `wg stat`)");
        }
    }
    ESP_LOGI(TAG, "connect task stack headroom: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_connect_task = nullptr;
    vTaskDelete(nullptr);
}

void start_connect(int index, const std::string& password, bool have_password)
{
    if (s_connect_task) {
        term_note("31", "接続処理が走っている（終わるまで待つ）");
        return;
    }
    s_connect_req = {index, password, have_password};
    // **端末を見せてから始める。** メニューが出ている間は render_term が描かないので、
    // 進行と失敗の行が端末バッファに溜まるだけで画面には出ない。
    set_menu_visible(false);
    if (xTaskCreate(&connect_worker, "connect", 32768, nullptr, 4, &s_connect_task) != pdPASS) {
        s_connect_task = nullptr;
        term_note("31", "接続タスクを作れなかった（メモリ不足）");
    }
}

// --- WiFi の一覧 (#56) ---
//
// 表示に使う文字列は main が持つ。**MenuUi は参照しか持たない**ので、
// 生きている間ずっと差し替えないこと。触るのは「メニューの描画・タッチ・キー」と
// 下のワーカだけで、どれも s_term_lock (TermGuard) の中で動く。
std::vector<std::string>   s_wifi_nets;
std::vector<std::string>   s_wifi_scan_rows;
std::vector<WifiScanEntry> s_wifi_scan_aps;  // 選ばれたときに SSID を引く

void refresh_wifi_nets()
{
    WifiNetInfo  nets[kMaxWifiNets];
    const size_t n = wifi_net_snapshot(nets, kMaxWifiNets);
    s_wifi_nets.clear();
    for (size_t i = 0; i < n; ++i) {
        s_wifi_nets.emplace_back(std::string(nets[i].active ? "* " : "  ") + nets[i].ssid);
    }
}

// --- WiFi の変更は全部ワーカタスクでやる ---
//
// **UI / kbd タスクの上でやってはいけない。** esp-hosted の RPC は深く
// (scan は 4096B で残り 1696B しかなかった)、kbd タスクもメインループも 8KB しか
// ない。しかも呼び出し元は s_term_lock を握っているので、そのまま走らせると
// 描画ループが TermGuard の 2 秒タイムアウトに落ちる。SSH / VPN の接続を
// `connect` タスクに逃がしてあるのと同じ理由。
enum class WifiJob { kScan, kAddConnect, kConnect, kRemove };

struct WifiReq {
    WifiJob     job   = WifiJob::kScan;
    int         index = -1;
    std::string ssid;
    std::string pass;
};
WifiReq      s_wifi_req;
TaskHandle_t s_wifi_task = nullptr;

// ワーカから画面に一言返す。**ロックが取れないと結果が消える**ので少し粘る
// （黙って諦めると「スキャン中...」が出たまま止まって見える）。
void wifi_ui_result(const std::string& note, bool show_scan)
{
    for (int i = 0; i < 3; ++i) {
        TermGuard guard;
        if (!guard.ok()) continue;
        menu->set_wifi_note(note);
        refresh_wifi_nets();
        // **今も一覧に居るときだけ移る。** 数秒待つ間に Back で抜けていたら、
        // ルートメニューからいきなりスキャン結果へ飛ばすことになる。
        if (show_scan && menu->visible() && menu->on_wifi_list()) menu->show_wifi_scan();
        if (menu->visible()) {
            menu->refresh();
            menu->draw();
        }
        return;
    }
    ESP_LOGW(TAG, "wifi: 端末のロックが取れず画面に反映できなかった: %s", note.c_str());
}

// 足して繋ぐ。**入力は端末でやる**ので、結果も端末に書く（メニューは閉じている）。
void wifi_add_and_connect(const std::string& ssid, const std::string& pass)
{
    const esp_err_t err = wifi_net_add(ssid.c_str(), pass.c_str());
    if (err != ESP_OK) {
        term_note("31", err == ESP_ERR_NO_MEM
                            ? "保存済みが " + std::to_string(kMaxWifiNets) +
                                  " 件で満杯。消してから追加する"
                            : "保存できない: " + std::string(esp_err_to_name(err)));
        return;
    }
    const int i = wifi_net_find(ssid.c_str());
    if (i < 0) {
        term_note("31", "保存したはずの \"" + ssid + "\" が見つからない");
        return;
    }
    term_note("33", "connecting to \"" + ssid + "\"...");
    if (const esp_err_t e = wifi_net_connect(static_cast<size_t>(i)); e != ESP_OK) {
        term_note("31", std::string("connect failed: ") + esp_err_to_name(e));
    }
    TermGuard guard;
    if (guard.ok()) refresh_wifi_nets();
}

void wifi_worker(void*)
{
    const WifiReq req = s_wifi_req;
    switch (req.job) {
        case WifiJob::kScan: {
            WifiScanEntry aps[kMaxWifiScanRows];
            const int     n = wifi_scan(aps, kMaxWifiScanRows);
            {
                TermGuard guard;
                if (guard.ok()) {
                    s_wifi_scan_aps.assign(aps, aps + (n > 0 ? n : 0));
                    s_wifi_scan_rows.clear();
                    for (int i = 0; i < n; ++i) {
                        char buf[80];
                        std::snprintf(buf, sizeof(buf), "%-20s %4d dBm%s", aps[i].ssid,
                                      aps[i].rssi, aps[i].secure ? "" : "  (open)");
                        s_wifi_scan_rows.emplace_back(buf);
                    }
                }
            }
            // **失敗と 0 件を区別して出す。** どちらも空の一覧になるので、
            // 理由を書かないと「AP が無い」と読める。
            wifi_ui_result(n < 0 ? "スキャンできない（WiFi が起動していない？）"
                                 : (n == 0 ? "AP が見つからない" : ""),
                           /*show_scan=*/n >= 0);
            break;
        }
        case WifiJob::kAddConnect:
            wifi_add_and_connect(req.ssid, req.pass);
            break;
        case WifiJob::kConnect: {
            const esp_err_t err = wifi_net_connect(static_cast<size_t>(req.index));
            wifi_ui_result(err == ESP_OK ? "connecting to \"" + req.ssid + "\"..."
                                         : std::string("connect failed: ") + esp_err_to_name(err),
                           /*show_scan=*/false);
            break;
        }
        case WifiJob::kRemove: {
            const esp_err_t err = wifi_net_remove(static_cast<size_t>(req.index));
            wifi_ui_result(err == ESP_OK ? "削除した: " + req.ssid
                                         : std::string("削除できない: ") + esp_err_to_name(err),
                           /*show_scan=*/false);
            break;
        }
    }
    ESP_LOGI(TAG, "wifi job task stack headroom: %u bytes",
             (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    s_wifi_task = nullptr;
    vTaskDelete(nullptr);
}

// 走らせる。前のが終わっていなければ何もしない（重ねると RPC が競合する）。
bool start_wifi_job(const WifiReq& req)
{
    if (s_wifi_task) return false;
    s_wifi_req = req;
    if (xTaskCreate(&wifi_worker, "wifijob", 8192, nullptr, 4, &s_wifi_task) != pdPASS) {
        s_wifi_task = nullptr;
        return false;
    }
    return true;
}

// パスワードを聞いてから足す。オープンな AP は聞かない。
// **足すのも繋ぐのもワーカに載せる** — Enter は kbd タスク (8KB) の上で、
// しかも s_term_lock を握ったまま返ってくる。
void wifi_ask_password(const std::string& ssid, bool secure)
{
    auto submit = [](const std::string& id, const std::string& pw) {
        if (!start_wifi_job({WifiJob::kAddConnect, -1, id, pw})) {
            term_note("31", "WiFi の処理中（終わるまで待つ）");
        }
    };
    if (!secure) {
        submit(ssid, "");
        return;
    }
    start_line_prompt("password for \"" + ssid + "\" (Enter で接続 / Esc で中止):",
                      /*mask=*/true,
                      [ssid, submit](const std::string& pw) { submit(ssid, pw); });
}

// メニューの WiFi の項目から呼ぶ。index の意味は Action ごとに違う。
void wifi_menu_action(MenuUi::Action a, int index)
{
    // 満杯の理由。**パスワードを聞く前に断る**（聞いてから断ると、打った後で捨てる）。
    const std::string full = "保存済みが " + std::to_string(kMaxWifiNets) +
                             " 件で満杯。消してから追加する";
    const char* busy = "WiFi の処理中（終わるまで待つ）";

    switch (a) {
        case MenuUi::Action::kWifiScan:
            if (wifi_net_count() >= kMaxWifiNets) {
                menu->set_wifi_note(full);
                menu->draw();
                return;
            }
            if (!start_wifi_job({WifiJob::kScan, -1, "", ""})) {
                menu->set_wifi_note(busy);
                menu->draw();
            }
            break;
        case MenuUi::Action::kWifiAddScanned: {
            if (index < 0 || index >= static_cast<int>(s_wifi_scan_aps.size())) return;
            // **ここでも見る。** スキャン結果を開いたままシリアルの `wifi` で
            // 埋まると、聞いてから断ることになる（既に保存済みなら差し替えなので通す）。
            if (wifi_net_count() >= kMaxWifiNets &&
                wifi_net_find(s_wifi_scan_aps[index].ssid) < 0) {
                menu->set_wifi_note(full);
                menu->draw();
                return;
            }
            const std::string ssid(s_wifi_scan_aps[index].ssid);
            const bool        secure = s_wifi_scan_aps[index].secure;
            // 入力は端末に出るので、先に端末へ移る。
            set_menu_visible(false);
            wifi_ask_password(ssid, secure);
            break;
        }
        case MenuUi::Action::kWifiAddManual:
            if (wifi_net_count() >= kMaxWifiNets) {
                menu->set_wifi_note(full);
                menu->draw();
                return;
            }
            set_menu_visible(false);
            // 隠し SSID。**スキャンに出ないので打つしかない。**
            start_line_prompt("hidden SSID (Enter で次へ / Esc で中止):", /*mask=*/false,
                              [](const std::string& ssid) {
                                  if (ssid.empty()) {
                                      term_note("31", "SSID が空");
                                      return;
                                  }
                                  wifi_ask_password(ssid, /*secure=*/true);
                              });
            break;
        case MenuUi::Action::kWifiConnect:
        case MenuUi::Action::kWifiDelete: {
            if (index < 0 || index >= static_cast<int>(wifi_net_count())) return;
            char ssid[33] = {};
            wifi_net_ssid(static_cast<size_t>(index), ssid, sizeof(ssid));
            const bool del = (a == MenuUi::Action::kWifiDelete);
            const bool ok  = start_wifi_job(
                {del ? WifiJob::kRemove : WifiJob::kConnect, index, ssid, ""});
            menu->set_wifi_note(ok ? (del ? "削除中..." : "接続中...") : busy);
            // **一覧へ戻してから待つ。** 消したものの詳細画面に残っていると、
            // 終わった後に「もう無い設定」の接続 / 削除を押せてしまう。
            if (ok) menu->show_wifi_list();
            menu->draw();
            break;
        }
        default: break;
    }
}

// index 番目の接続先に繋ぐ。SSH も VPN もワーカタスクに載せる。
void connect_profile(int index)
{
    if (index < 0 || index >= static_cast<int>(s_profiles.profiles.size())) return;
    start_connect(index, "", /*have_password=*/false);
}

// 一覧から選ぶのと同じ経路を、指も画面も無しで叩く。
int cmd_connect(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: connect <name|index>   (一覧は `profiles`)\n");
        return 1;
    }
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    int index = -1;
    if (const prof::Profile* p = prof::find(s_profiles, argv[1])) {
        index = static_cast<int>(p - s_profiles.profiles.data());
    } else {
        char*      end = nullptr;
        const long v   = std::strtol(argv[1], &end, 10);
        if (end && *end == '\0' && v >= 0 && v < (long)s_profiles.profiles.size()) {
            index = static_cast<int>(v);
        }
    }
    if (index < 0) {
        std::printf("no such profile: %s\n", argv[1]);
        return 1;
    }
    connect_profile(index);
    return 0;
}

// NVS の使用量を見る。**接続先と鍵を NVS に置けるか (#57) は、ここの実測で決まる。**
// パーティションは既定の 24KB (partitions.csv) しかなく、ページ単位 (4KB) で
// 使うので「blob が何バイトまで」だけを見ても足りない。
int cmd_nvsstat(int, char**)
{
    nvs_stats_t st{};
    if (esp_err_t err = nvs_get_stats(nullptr, &st); err != ESP_OK) {
        std::printf("nvs_get_stats failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    std::printf("nvs: used=%u free=%u total=%u entries, namespaces=%u\n", (unsigned)st.used_entries,
                (unsigned)st.free_entries, (unsigned)st.total_entries,
                (unsigned)st.namespace_count);
    // エントリは 32 バイト固定なので、バイト換算も出す（どれだけ置けるかの目安）。
    std::printf("     おおよそ used=%uB free=%uB (1 エントリ 32B)\n",
                (unsigned)st.used_entries * 32, (unsigned)st.free_entries * 32);

    nvs_iterator_t it  = nullptr;
    esp_err_t      err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        std::printf("  %-16s %-16s type=%d\n", info.namespace_name, info.key, (int)info.type);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
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
    rp.flipped = screen::flipped();
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
        display.setRotation(screen::rotation());
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
            display.setRotation(screen::rotation());
            const int64_t t1 = esp_timer_get_time();
            sp.pushSprite(0, kH);
            std::printf("m5gfx pushSprite same size (rotation %d): %lld us\n",
                        (int)screen::rotation(),
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
    display.setRotation(screen::rotation());
    const int w = rw / step;
    const int h = rh / step;
    std::printf("SCREENCAP %d %d %d rotation=%d\n", w, h, step, (int)screen::rotation());
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

    // **スクロールバックを見ている状態を解除する。** 見たままだと探査点が
    // 1 セル分ずれて、健全な描画経路に対して FAILED と言ってしまう
    // （スワイプを足したせいで、その状態に入るのが簡単になった）。
    if (term->view_offset() != 0) term->scroll_view(-term->view_offset());
    // 行 0: セル 0 が赤、セル 1 が緑。行 1: セル 0 が青。ほかは空。
    term->write("\033[2J\033[H\033[41m \033[42m \033[m\r\n\033[44m \033[m");
    // **render_term ではなく直接呼ぶ。** これは本番経路を実際に通すための診断なので、
    // メニュー表示中でも端末を描かないと、メニューの画素を読んで FAILED になる
    // （実機で踏んだ）。終わったらメニューを描き直す。
    const bool menu_was_shown = menu && menu->visible();
    hide_auth_qr(/*redraw=*/false);
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
    panel.flipped  = screen::flipped();
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
    display.setRotation(screen::rotation());
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
    display.setRotation(screen::rotation());
    if (bad == 0) {
        std::printf("rotation matches setRotation(%d): ok\n", (int)screen::rotation());
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
        display.setRotation(screen::rotation());
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
        {"profiles", "接続先 (NVS) の一覧／SD からの取り込み／消去", "[reload|import|clear]",
         &cmd_profiles, nullptr, nullptr, nullptr},
        {"connect", "接続先に繋ぐ（メニューから選ぶのと同じ経路）", "<name|index>",
         &cmd_connect, nullptr, nullptr, nullptr},
        {"ssh-forget", "覚えているホスト鍵を忘れる", "<host>[:<port>]", &cmd_ssh_forget, nullptr,
         nullptr, nullptr},
        {"key", "SSH にキー入力を送る", "<text>", &cmd_key, nullptr, nullptr, nullptr},
        {"conv", "ローマ字→かな→漢字を試す", "<romaji>", &cmd_conv, nullptr, nullptr, nullptr},
        {"keyj", "ローマ字を IME に通して SSH に送る", "<romaji>...", &cmd_keyj, nullptr, nullptr,
         nullptr},
        {"scroll", "スクロールバックを動かす (正=過去へ)", "[lines]", &cmd_scroll, nullptr, nullptr,
         nullptr},
        {"bench", "描画コストを測る (全画面 vs 1 文字)", nullptr, &cmd_bench, nullptr, nullptr,
         nullptr},
        {"flip", "画面を 180 度反転する (純正キーボード用)", "[0|1]", &cmd_flip, nullptr,
         nullptr, nullptr},
        {"kbdinject", "打鍵を合成して送出経路を確かめる", "<key-name> [mod]", &cmd_kbdinject,
         nullptr, nullptr, nullptr},
        {"kbdhw", "純正キーボード (Ext.Port1 I2C) の状態を見る／読み取りを立て直す", nullptr,
         &cmd_kbdhw, nullptr, nullptr, nullptr},
        {"kbdlog", "打鍵をシリアルに出す", "[off]", &cmd_kbdlog, nullptr, nullptr, nullptr},
        {"kbd", "画面キーボードの表示切り替え", "[off]", &cmd_kbd, nullptr, nullptr, nullptr},
        {"menu", "初期メニューの操作", "[show|hide|up|down|enter|esc|left|right]", &cmd_menu,
         nullptr, nullptr, nullptr},
        {"tap", "タッチを合成する（実タッチと同じ経路）", "<x> <y>", &cmd_tap, nullptr, nullptr,
         nullptr},
        {"touchlog", "実タッチの座標をログに出す（四隅の照合用）", "[off]", &cmd_touchlog, nullptr,
         nullptr, nullptr},
        {"touchmap", "描画側とタッチ側の回転が一致するか照合（指なし）", nullptr, &cmd_touchmap,
         nullptr, nullptr, nullptr},
        {"swipe", "縦スワイプを合成する", "<x> <y_from> <y_to>", &cmd_swipe, nullptr, nullptr,
         nullptr},
        {"wgtest", "WireGuard の暗号とハンドシェイクを実機で検証", nullptr, &cmd_wgtest, nullptr,
         nullptr, nullptr},
        {"wgloop", "UDP ループバックでトンネルを往復させる（相手機不要）", nullptr, &cmd_wgloop,
         nullptr, nullptr, nullptr},
        {"discoloop", "UDP ループバックで DISCO の Ping/Pong を往復させる", nullptr,
         &cmd_discoloop, nullptr, nullptr, nullptr},
        {"keytest", "sshkey パーティションの鍵を mbedTLS で直接パースする", nullptr, &cmd_keytest,
         nullptr, nullptr, nullptr},
        {"nvsstat", "NVS の使用量と中身を見る（#57 の設計用）", nullptr, &cmd_nvsstat, nullptr,
         nullptr, nullptr},
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
        {"ts-login", "authkey 無しで参加する（AuthURL を QR で出す）", "<host> [port] [capver]",
         &cmd_ts_login, nullptr, nullptr, nullptr},
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

    s_term_lock = xSemaphoreCreateRecursiveMutex();
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
    // 純正キーボードを付けると本体の向きが逆さになるので、挿さっていれば画面を 180 度回す。
    // **display.init() の後に見る**（Ext.Port1 の 5V は M5GFX の Tab5 初期化が入れている）。
    if (kbd_hw::begin()) {
        ESP_LOGI(TAG, "キーボード検出 (fw=0x%02X): 画面を 180 度反転する", kbd_hw::version());
        screen::set_flipped(true);
    }
    // ターミナル用途なので横向き固定。パネルはネイティブ縦 (720x1280) で来る。
    display.setRotation(screen::rotation());
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
        term->set_scrollback(sb, kScrollbackLines, renderer->cols());
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
    menu->set_profiles(&s_profiles);
    menu->set_wifi_nets(&s_wifi_nets);
    menu->set_wifi_scan(&s_wifi_scan_rows);
    refresh_wifi_nets();
    menu->set_action([](MenuUi::Action a, int index) {
        switch (a) {
            case MenuUi::Action::kConnectProfile:
                connect_profile(index);
                break;
            case MenuUi::Action::kReloadProfiles:
                load_profiles();
                menu->set_info(gather_menu_info(gather_status()));
                menu->refresh();
                break;
            case MenuUi::Action::kOpenSsh: {
                // 端末に移るのは set_menu_visible の仕事（画面キーボードの再表示と
                // 行数の張り直しがここにある）。直に set_visible すると隠れたままになる。
                set_menu_visible(false);
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
                // **set_menu_visible を通す。** menu->set_visible(false) を直に呼ぶと
                // 画面キーボードが隠れたまま・配分もメニューのまま・スワイプの
                // 掴み位置も残る（ステータスバーのタップとの食い違いになる）。
                set_menu_visible(false);
                break;
            case MenuUi::Action::kTsConnect:
            case MenuUi::Action::kWgUp:
                // ponytail: 接続先を NVS に持っていないので、まだ画面からは繋げない。
                // 保存できるようにしたら menu_ui 側の項目を enabled にする。
                break;
            case MenuUi::Action::kWifiConnect:
            case MenuUi::Action::kWifiDelete:
            case MenuUi::Action::kWifiScan:
            case MenuUi::Action::kWifiAddScanned:
            case MenuUi::Action::kWifiAddManual:
                wifi_menu_action(a, index);
                break;
        }
    });
    menu->set_info(gather_menu_info(gather_status()));
    // 操作説明をキーボードの有無で変える (#51)。挿さっていないのにキーを案内すると嘘になる。
    menu->set_has_keyboard(kbd_hw::present());
    menu->set_visible(true);
    // 描画はキーボードの表示が決まってから（下の apply_layout で行う）。

    keyboard->set_output(send_input);

    // 純正キーボードが挿さっていれば読み取りタスクを立てる（起動時に検出済み）。
    // **画面の向きでは判定しない**（`flip` で手で戻した後も読み続ける必要がある）。
    if (kbd_hw::present()) {
        // 打鍵ごとに描画経路（sprite への drawString と PPA 転送）を通るので 8KB 取る。
        if (xTaskCreate(&kbd_task, "kbd", 8192, nullptr, 4, &s_kbd_task) != pdPASS) {
            s_kbd_task = nullptr;
            ESP_LOGE(TAG, "キーボードの読み取りタスクを立てられなかった（`kbdhw` で再試行）");
        }
    }

    // 接続先は NVS から読む (#57)。**起動時に SD は触らない** — 取り込み済みなら
    // 挿していなくても繋がるのが要点。SD を見るのは `profiles import` のときだけ。
    load_profiles();
    menu->set_info(gather_menu_info(gather_status()));

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
                    refresh_wifi_nets();  // 繋がった / 切れたで `*` が動く (#56)
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
                if (s_touch_log) ESP_LOGI(TAG, "touch down (%d,%d)", tp.x, tp.y);
                touch_down_at(tp.x, tp.y);
            } else {
                touch_move_at(tp.x, tp.y);
                last_x = tp.x;
                last_y = tp.y;
            }
        } else if (touching && touch_guard.ok()) {
            touching = false;
            if (s_touch_log) ESP_LOGI(TAG, "touch up   (%d,%d)", last_x, last_y);
            touch_up_at(last_x, last_y);
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
