#include <cstdint>
#include "term_render.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <driver/ppa.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>

#include "rotate.hpp"

namespace {

const char* TAG = "render";

// xterm の標準 16 色。
constexpr uint8_t kBase16[16][3] = {
    {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
    {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
    {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
    {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
};

constexpr uint8_t kCubeLevels[6] = {0, 95, 135, 175, 215, 255};

}  // namespace

bool TermRenderer::begin()
{
    // 日本語等幅で一番大きいもの。1280x720 で 106x30 になる。
    gfx_.setFont(&fonts::efontJA_24);
    // fontWidth() は efont では実際の送り幅と一致しない (24px 高で 6 を返す) ので、
    // 半角 1 文字を実測する。全角はこの 2 倍で描かれる。
    cell_w_ = gfx_.textWidth("A");
    cell_h_ = gfx_.fontHeight();
    ESP_LOGI(TAG, "font: fontWidth=%d textWidth(A)=%d fontHeight=%d", (int)gfx_.fontWidth(),
             cell_w_, cell_h_);
    if (cell_w_ <= 0 || cell_h_ <= 0) {
        ESP_LOGE(TAG, "font metrics unavailable (w=%d h=%d)", cell_w_, cell_h_);
        return false;
    }
    cols_      = gfx_.width() / cell_w_;
    rows_      = gfx_.height() / cell_h_;
    full_rows_ = rows_;

    for (int i = 0; i < 16; ++i) {
        pal_[i] = gfx_.color565(kBase16[i][0], kBase16[i][1], kBase16[i][2]);
    }
    for (int i = 0; i < 216; ++i) {
        pal_[16 + i] = gfx_.color565(kCubeLevels[i / 36], kCubeLevels[(i / 6) % 6],
                                     kCubeLevels[i % 6]);
    }
    for (int i = 0; i < 24; ++i) {
        uint8_t v    = static_cast<uint8_t>(8 + i * 10);
        pal_[232 + i] = gfx_.color565(v, v, v);
    }
    pal_[vt::kDefaultFg] = pal_[7];
    pal_[vt::kDefaultBg] = pal_[0];

    // 行スプライトは内蔵 RAM に置く (1280x24x2 = 60KB)。PSRAM に置くと転送が遅くなる。
    row_.setPsram(false);
    // **rgb565_nonswapped を明示する。** 16 を渡すと M5GFX では swap565
    // (メモリ上 byte0 = RRRRRGGG) になり、PPA に memcpy で渡すと色が入れ替わる
    // （赤 0xF800 が暗い青に見える。白と黒はスワップ不変なので気づきにくい）。
    // パネルも PPA もネイティブのリトルエンディアン RGB565 を期待している。
    row_.setColorDepth(lgfx::v1::color_depth_t::rgb565_nonswapped);
    row_.setFont(&fonts::efontJA_24);
    row_.setTextDatum(textdatum_t::top_left);
    if (!row_.createSprite(gfx_.width(), cell_h_)) {
        ESP_LOGE(TAG, "row sprite alloc failed (%dx%d)", (int)gfx_.width(), cell_h_);
        return false;
    }
    ESP_LOGI(TAG, "grid=%dx%d cell=%dx%d sprite=%d bytes", cols_, rows_, cell_w_, cell_h_,
             (int)gfx_.width() * cell_h_ * 2);

    return true;
}

namespace {

void append_utf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        // efont は BMP までなので下駄記号にする。
        append_utf8(out, 0x3013);
    }
}

}  // namespace

bool TermRenderer::enable_ppa()
{
    if (ppa_) return true;

    auto* panel = static_cast<lgfx::Panel_DSI*>(gfx_.getPanel());
    if (!panel) return false;
    const auto& cfg = panel->config_detail();
    if (!cfg.buffer) return false;

    // フレームバッファはネイティブ向きで、M5GFX の rotation とは無関係に物理的な並びで置かれる。
    // サイズは決め打ちにせずパネル定義から取る（buffer_length は 0 なので使えない）。
    // 決め打ちだと、パネル定義が変わったときに PPA が FB の外へ書く。
    const auto& pcfg = panel->config();
    fb_   = static_cast<uint16_t*>(cfg.buffer);
    fb_w_ = pcfg.panel_width;
    fb_h_ = pcfg.panel_height;
    if (fb_w_ <= 0 || fb_h_ <= 0) {
        ESP_LOGW(TAG, "ppa: panel size unavailable (%dx%d)", fb_w_, fb_h_);
        fb_ = nullptr;
        return false;
    }

    // PPA の入力は DMA するので 64B 境界の内蔵 RAM に置く。
    row_dma_ = static_cast<uint16_t*>(heap_caps_aligned_alloc(
        64, static_cast<size_t>(gfx_.width()) * cell_h_ * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!row_dma_) {
        ESP_LOGW(TAG, "ppa: row buffer alloc failed");
        return false;
    }

    ppa_client_config_t pc = {};
    pc.oper_type           = PPA_OPERATION_SRM;
    ppa_client_handle_t client = nullptr;
    if (ppa_register_client(&pc, &client) != ESP_OK) {
        heap_caps_free(row_dma_);
        row_dma_ = nullptr;
        ESP_LOGW(TAG, "ppa: register_client failed");
        return false;
    }
    ppa_ = client;
    ESP_LOGI(TAG, "ppa enabled: fb=%p %dx%d", fb_, fb_w_, fb_h_);
    return true;
}

// 行スプライトの一部を PPA で 90 度回転してフレームバッファへ直接書く。
bool TermRenderer::push_row_ppa(int y, int x_from, int x_to)
{
    const int px  = x_from * cell_w_;
    const int pw  = (x_to - x_from + 1) * cell_w_;
    const int py  = y * cell_h_;

    // スプライトの該当矩形を DMA バッファに詰める（行ごとにストライドが違うのでコピーが必要）。
    const auto* src = static_cast<const uint16_t*>(row_.getBuffer());
    if (!src) return false;
    // ストライドはスプライト自身の幅から取る。gfx_.width() を使うと、
    // 誰かが rotation を変えた瞬間にストライドがずれて行が崩れる。
    const int sprite_w = row_.width();
    for (int r = 0; r < cell_h_; ++r) {
        std::memcpy(row_dma_ + static_cast<size_t>(r) * pw, src + static_cast<size_t>(r) * sprite_w + px,
                    static_cast<size_t>(pw) * 2);
    }

    rot::Panel rp;
    rp.native_w = fb_w_;
    rp.native_h = fb_h_;
    int nx = 0, ny = 0, nw = 0, nh = 0;
    rot::landscape_rect_to_native(rp, px, py, pw, cell_h_, &nx, &ny, &nw, &nh);

    ppa_srm_oper_config_t op = {};
    op.in.buffer           = row_dma_;
    op.in.pic_w            = pw;
    op.in.pic_h            = cell_h_;
    op.in.block_w          = pw;
    op.in.block_h          = cell_h_;
    op.in.srm_cm           = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer          = fb_;
    // M5GFX は buffer_length を埋めていないので自分で計算する（0 を渡すと引数エラー）。
    op.out.buffer_size     = static_cast<size_t>(fb_w_) * fb_h_ * 2;
    op.out.pic_w           = fb_w_;
    op.out.pic_h           = fb_h_;
    op.out.block_offset_x  = nx;
    op.out.block_offset_y  = ny;
    op.out.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle      = PPA_SRM_ROTATION_ANGLE_90;
    op.scale_x             = 1.0f;
    op.scale_y             = 1.0f;
    op.mode                = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_scale_rotate_mirror(static_cast<ppa_client_handle_t>(ppa_), &op) == ESP_OK;
}

void TermRenderer::draw_row(vt::Terminal& term, int y, int x_from, int x_to)
{
    const int64_t t_draw = esp_timer_get_time();

    // 範囲の端が全角セルを割っていると、片方だけ塗って相棒を描き直さないので
    // グリフが半分消える。端を全角の境界まで広げる。
    if (x_from > 0 && term.view_cell(x_from, y).width == 0) --x_from;
    if (x_to < cols_ - 1 && term.view_cell(x_to, y).width == 2) ++x_to;
    // 転送する範囲だけ塗る。行全体を毎回 1280px 送るのが最大の無駄だった。
    row_.fillRect(x_from * cell_w_, 0, (x_to - x_from + 1) * cell_w_, cell_h_,
                  pal_[vt::kDefaultBg]);

    // 履歴を見ている間はカーソルを描かない（過去の行の上に出ると紛らわしい）。
    const bool cursor_here =
        term.cursor_visible() && term.view_offset() == 0 && y == term.cursor_y();
    const int cursor_x = term.cursor_x();

    // 同じ見た目が続く区間をまとめて 1 回の drawString で描く。
    // drawChar は指定した座標に描いてくれない (advance は正しいが位置が効かない) ので使わない。
    int x = x_from;
    while (x <= x_to) {
        const vt::Cell& head = term.view_cell(x, y);
        if (head.width == 0) {  // 孤立した右半分（通常ここには来ない）
            ++x;
            continue;
        }

        auto effective = [&](int cx, const vt::Cell& c, uint16_t& fg, uint16_t& bg) {
            fg = pal_[c.attr.fg];
            bg = pal_[c.attr.bg];
            // 太字は明色化する (専用のボールドフォントは無い)。xterm 系と同じ扱い。
            if ((c.attr.flags & vt::kBold) && c.attr.fg < 8) fg = pal_[c.attr.fg + 8];
            const bool reverse = (c.attr.flags & vt::kReverse) != 0;
            // カーソルは反転ブロック。反転属性と重なったら二重反転で元に戻る。
            const bool on_cursor =
                cursor_here && (cx == cursor_x || (c.width == 2 && cx + 1 == cursor_x));
            if (reverse != on_cursor) std::swap(fg, bg);
        };

        uint16_t fg = 0, bg = 0;
        effective(x, head, fg, bg);
        const uint16_t flags = head.attr.flags & (vt::kUnderline | vt::kStrike | vt::kInvisible);

        const int start_x = x;
        std::string run;
        while (x <= x_to) {
            const vt::Cell& c = term.view_cell(x, y);
            if (c.width == 0) {
                ++x;
                continue;
            }
            uint16_t cfg = 0, cbg = 0;
            effective(x, c, cfg, cbg);
            if (cfg != fg || cbg != bg ||
                (c.attr.flags & (vt::kUnderline | vt::kStrike | vt::kInvisible)) != flags) {
                break;
            }
            append_utf8(run, (c.ch == 0) ? ' ' : c.ch);
            x += c.width;
        }
        if (run.empty()) continue;

        const int px    = start_x * cell_w_;
        const int width = (x - start_x) * cell_w_;

        // 既定背景のまま・空白だけ・装飾なしなら fillSprite の結果で足りる。
        const bool all_blank = run.find_first_not_of(' ') == std::string::npos;
        if (all_blank && bg == pal_[vt::kDefaultBg] && flags == 0) continue;

        row_.fillRect(px, 0, width, cell_h_, bg);
        if (!all_blank && (flags & vt::kInvisible) == 0) {
            row_.setTextColor(fg, bg);
            row_.drawString(run.c_str(), px, 0);
        }
        if (flags & vt::kUnderline) row_.drawFastHLine(px, cell_h_ - 2, width, fg);
        if (flags & vt::kStrike) row_.drawFastHLine(px, cell_h_ / 2, width, fg);
    }

    const int64_t t_push = esp_timer_get_time();
    last_draw_us_ += static_cast<uint32_t>(t_push - t_draw);

    // PPA が使えるならハードウェアで回転してフレームバッファへ直接書く。
    // M5GFX の pushSprite は setRotation(1) の座標変換をソフトでやるので 4.6 倍遅い。
    if (!ppa_ || !push_row_ppa(y, x_from, x_to)) {
        // pushSprite に部分矩形版が無いので、転送先のクリップ矩形で範囲を絞る。
        const int px_from = x_from * cell_w_;
        const int px_w    = (x_to - x_from + 1) * cell_w_;
        gfx_.setClipRect(px_from, y * cell_h_, px_w, cell_h_);
        row_.pushSprite(&gfx_, 0, y * cell_h_);
        gfx_.clearClipRect();
    }
    last_push_us_ += static_cast<uint32_t>(esp_timer_get_time() - t_push);
    last_px_ += static_cast<uint32_t>((x_to - x_from + 1) * cell_w_ * cell_h_);
}

void TermRenderer::render(vt::Terminal& term, bool force)
{
    const int64_t t0 = esp_timer_get_time();

    const int  cx           = term.cursor_x();
    const int  cy           = term.cursor_y();
    const bool cursor_moved = (cx != cur_x_ || cy != cur_y_);

    int drawn     = 0;
    last_draw_us_ = 0;
    last_push_us_ = 0;
    last_px_      = 0;
    // 転送の準備を 1 回にまとめる。行ごとに pushSprite するとその都度セットアップが走る。
    gfx_.startWrite();
    for (int y = 0; y < rows_; ++y) {
        // カーソルが動いたら、消す側と描く側の 2 行も描き直す。
        const bool cursor_row = cursor_moved && (y == cur_y_ || y == cy);
        const bool need       = force || term.is_dirty(y) || cursor_row;
        if (!need) continue;

        int x_from = 0;
        int x_to   = cols_ - 1;
        if (!force && !cursor_row && term.is_dirty(y)) {
            x_from = term.dirty_min_x(y);
            x_to   = term.dirty_max_x(y);
        } else if (!force && cursor_row && term.is_dirty(y)) {
            // カーソルが絡む行は、変更範囲とカーソル位置の両方を含める。
            x_from = std::min(term.dirty_min_x(y), std::min(cur_x_, cx));
            x_to   = std::max(term.dirty_max_x(y), std::max(cur_x_, cx));
        } else if (!force && cursor_row) {
            x_from = std::max(0, std::min(cur_x_, cx) - 1);
            x_to   = std::min(cols_ - 1, std::max(cur_x_, cx) + 1);
        }
        if (x_to < x_from) continue;
        draw_row(term, y, x_from, x_to);
        ++drawn;
    }
    gfx_.endWrite();
    cur_x_ = cx;
    cur_y_ = cy;
    term.clear_dirty();

    last_us_   = static_cast<uint32_t>(esp_timer_get_time() - t0);
    last_rows_ = drawn;
}
