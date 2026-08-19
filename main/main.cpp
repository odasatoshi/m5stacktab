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
#include "disco_responder.hpp"
#include "netmap.hpp"
#include <driver/ppa.h>
#include <esp_memory_utils.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>

#include "rotate.hpp"
#include "ts_control.hpp"
#include "wg_netif.hpp"
#include "blake2s.hpp"
#include "kbd_ui.hpp"
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

M5GFX                          display;
std::unique_ptr<TermRenderer>  renderer;
std::unique_ptr<vt::Terminal>  term;
std::unique_ptr<KeyboardUi>    keyboard;

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
    TermGuard guard;
    if (!guard.ok()) {
        std::printf("busy\n");
        return 1;
    }
    term->write(s);
    renderer->render(*term);
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
    {
        TermGuard guard;
        term->write(line);
        renderer->render(*term);
    }
    return 0;
}

// スクロールバックの確認用（タッチのスワイプは #7 の UI で繋ぐ）。
int cmd_scroll(int argc, char** argv)
{
    const int delta = (argc > 1) ? atoi(argv[1]) : 3;
    TermGuard guard;
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

    {
        TermGuard guard;
        term->write("\r\n\033[33mconnecting to ");
        term->write(cfg.user + "@" + cfg.host + "...\033[m\r\n");
        renderer->render(*term);
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
    // 計測中に別タスクが描画すると値が混ざるので、区間全体を押さえる。
    TermGuard guard;
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

int cmd_kbd(int argc, char** argv)
{
    const bool show = (argc < 2) || (std::string(argv[1]) != "off");
    TermGuard guard;
    keyboard->set_visible(show);
    // 隠したら画面全体を端末に使う。端末側の行数と PTY サイズも合わせる。
    const int rows = show ? (display.height() - keyboard->height()) / renderer->cell_h()
                          : renderer->full_rows();
    renderer->set_rows(rows);
    term->resize(renderer->cols(), rows);
    if (ssh_is_connected()) ssh_resize(renderer->cols(), rows);
    renderer->render(*term, /*force=*/true);
    std::printf("keyboard %s, terminal %dx%d\n", show ? "shown" : "hidden", renderer->cols(), rows);
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
int cmd_ts(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: ts <host> <authkey> [port] [capver]\n");
        return 1;
    }
    // machine / node / disco の 3 つは別の鍵にする（役割ごとに分離する）。
    static uint8_t machine_priv[32], node_priv[32], disco_priv[32];
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

    static ts::Client client;
    ts::ClientConfig  cfg;
    cfg.host     = argv[1];
    cfg.auth_key = argv[2];
    cfg.port     = (argc > 3) ? static_cast<uint16_t>(atoi(argv[3])) : 80;
    cfg.capability_version = (argc > 4) ? static_cast<uint16_t>(atoi(argv[4])) : 131;
    cfg.hostname = "m5stack-tab5";
    // 自分のエンドポイント（STUN を実装していないので LAN アドレスのみ申告する）
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
        return 1;
    }
    client.set_config(cfg);
    // netmap が来たらピアの disco 公開鍵を登録する。これが DISCO の前提。
    client.set_map_handler([](const std::string& json) {
        ts::NetMap map;
        if (!ts::parse_netmap(json, &map)) return;
        if (map.keepalive) return;
        register_disco_peers(map);
    });

    std::printf("connecting to %s:%u (capver %u)...\n", cfg.host.c_str(), cfg.port,
                cfg.capability_version);
    const int64_t t0 = esp_timer_get_time();
    const bool    ok = client.run_once();
    const auto&   st = client.status();
    std::printf("result: %s (%lld ms)\n", ok ? "ok" : "failed", (esp_timer_get_time() - t0) / 1000);
    // REPL タスクのスタックがどこまで減ったか（X25519 と HTTP/2 のバッファが重なる経路）。
    std::printf("  console task stack headroom: %u bytes\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    std::printf("  state=%d registered=%d map_messages=%u keepalives=%u\n", (int)st.state,
                st.registered ? 1 : 0, (unsigned)st.map_messages, (unsigned)st.keepalives);
    if (!st.assigned_address.empty()) {
        std::printf("  >>> assigned address: %s\n", st.assigned_address.c_str());
        std::string line = "\r\n\033[32mtailscale: " + st.assigned_address + "\033[m\r\n";
        TermGuard guard;
        term->write(line);
        renderer->render(*term);
    }
    if (!st.error.empty()) std::printf("  error: %s\n", st.error.c_str());
    return ok ? 0 : 1;
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
    sendto(sa, buf, wlen, 0, reinterpret_cast<sockaddr*>(&addr_b), sizeof(addr_b));
    n = recvfrom(sb, buf, sizeof(buf), 0, nullptr, nullptr);
    static uint8_t out[2048];
    bool           valid = false;
    const size_t   got = (n > 0) ? tb.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid)
                                 : 0;
    const int64_t rt_us = esp_timer_get_time() - t1;
    std::printf("data over udp loopback: %s (%lld us round trip)\n",
                (valid && got == plen && std::memcmp(out, payload, plen) == 0) ? "ok" : "FAILED",
                rt_us);

    // 逆方向も確認する（応答側の鍵で送れること）
    const size_t rlen = tb.encrypt(buf, sizeof(buf), reinterpret_cast<const uint8_t*>("reply"), 5);
    sendto(sb, buf, rlen, 0, reinterpret_cast<sockaddr*>(&addr_a), sizeof(addr_a));
    n = recvfrom(sa, buf, sizeof(buf), 0, nullptr, nullptr);
    const size_t got2 =
        (n > 0) ? ta.decrypt(out, sizeof(out), buf, static_cast<size_t>(n), &valid) : 0;
    std::printf("reverse direction: %s\n",
                (valid && got2 == 5 && std::memcmp(out, "reply", 5) == 0) ? "ok" : "FAILED");

    close(sa);
    close(sb);
    return keys_match ? 0 : 1;
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

// WireGuard のトンネル netif を上げる。ピア指定は任意（無ければ netif だけ作る）。
int cmd_wg(int argc, char** argv)
{
    auto& nif = wg::netif_instance();

    if (argc >= 2 && std::string(argv[1]) == "down") {
        nif.down();
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
    {
        uint8_t      dkey[32];
        nvs_handle_t h;
        bool         ok = false;
        if (nvs_open("ts", NVS_READONLY, &h) == ESP_OK) {
            size_t len = 32;
            ok = (nvs_get_blob(h, "dkey", dkey, &len) == ESP_OK && len == 32);
            nvs_close(h);
        }
        if (ok && s_disco.set_key(dkey)) {
            nif.set_foreign_handler(&on_foreign_packet);
        } else {
            std::printf("warning: no disco key yet (run `ts` first) - DISCO disabled\n");
        }
    }

    if (!nif.is_up()) {
        const esp_err_t err = nif.up(priv, addr, mask);
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
    op.rotation_angle       = PPA_SRM_ROTATION_ANGLE_90;
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
    return err == ESP_OK ? 0 : 1;
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

    // まず横向き (rotation 1) で四隅に印を描く
    display.setRotation(1);
    display.fillScreen(TFT_BLACK);
    for (const auto& pt : pts) display.fillCircle(pt.lx, pt.ly, 18, pt.color);
    display.setFont(&fonts::efontJA_24);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("rotation 1: big circles", 300, 300);

    // 次に rotation 0 に切り替えて、変換した座標に小さい印を描く
    display.setRotation(0);
    for (const auto& pt : pts) {
        int nx = 0, ny = 0;
        rot::landscape_to_native(panel, pt.lx, pt.ly, &nx, &ny);
        display.fillCircle(nx, ny, 7, TFT_WHITE);
        std::printf("  %-13s landscape(%4d,%3d) -> native(%3d,%4d)\n", pt.name, pt.lx, pt.ly, nx,
                    ny);
    }
    display.setRotation(1);
    display.drawString("white dots inside circles = ok", 300, 340);
    std::printf("check the screen: white dots must be inside the colored circles\n");
    std::printf("run `termtest` or type to restore the terminal\n");
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
        {"kbd", "画面キーボードの表示切り替え", "[off]", &cmd_kbd, nullptr, nullptr, nullptr},
        {"wgtest", "WireGuard の暗号とハンドシェイクを実機で検証", nullptr, &cmd_wgtest, nullptr,
         nullptr, nullptr},
        {"wgloop", "UDP ループバックでトンネルを往復させる（相手機不要）", nullptr, &cmd_wgloop,
         nullptr, nullptr, nullptr},
        {"keytest", "sshkey パーティションの鍵を mbedTLS で直接パースする", nullptr, &cmd_keytest,
         nullptr, nullptr, nullptr},
        {"rottest", "座標変換が setRotation(1) と一致するか実機で照合", nullptr, &cmd_rottest,
         nullptr, nullptr, nullptr},
        {"ppatest", "PPA でフレームバッファに直接回転転送してみる", nullptr, &cmd_ppatest, nullptr,
         nullptr, nullptr},
        {"ts", "Tailscale/Headscale の制御プレーンに接続", "<host> <authkey> [port] [capver]",
         &cmd_ts, nullptr, nullptr, nullptr},
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
    // 画面下部をキーボードに使うので、端末の行数はその分減らす。
    keyboard = std::make_unique<KeyboardUi>(display, *renderer);
    // キーボードの上端は端末のセル境界に合わせる。合わせないと誰も描かない帯が残る。
    constexpr int kWantKeyboardH = 320;  // 4 行 x 72px + ステータス帯 32px 程度
    const int term_rows = (display.height() - kWantKeyboardH) / renderer->cell_h();
    const int keyboard_h = display.height() - term_rows * renderer->cell_h();
    keyboard->begin(keyboard_h);
    renderer->set_rows(term_rows);
    ESP_LOGI(TAG, "terminal %d rows, keyboard %d px (no gap)", term_rows, keyboard_h);

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
    keyboard->set_output([](const std::string& s) {
        if (ssh_is_connected()) {
            ssh_send(s.data(), s.size());
        } else {
            TermGuard guard;
            term->write(s);
            renderer->render(*term);
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
    keyboard->set_visible(true);

    // 描画とタッチのポーリング。キーボードが来るまではタッチが唯一の直接入力手段。
    int  last_log_s = 0;
    bool touching   = false;
    int  last_x = 0, last_y = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        // リモートからの出力を端末に流す。コンソールタスクも term を触るのでロックする。
        {
            TermGuard guard;
            if (guard.ok()) {
                uint8_t rx[1024];
                for (;;) {
                    size_t n = ssh_receive(rx, sizeof(rx));
                    if (n == 0) break;
                    term->write(rx, n);
                }
                if (term->any_dirty()) renderer->render(*term);
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
                if (!keyboard->touch_down(tp.x, tp.y)) {
                    // キーボード外 = 端末領域。縦スワイプでスクロールバックを見る。
                    ESP_LOGI(TAG, "touch down x=%d y=%d (cell %d,%d)", tp.x, tp.y,
                             tp.x / renderer->cell_w(), tp.y / renderer->cell_h());
                }
            } else {
                keyboard->touch_move(tp.x, tp.y);
                last_x = tp.x;
                last_y = tp.y;
            }
        } else if (touching && touch_guard.ok()) {
            touching = false;
            if (!keyboard->touch_up(last_x, last_y)) {
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
