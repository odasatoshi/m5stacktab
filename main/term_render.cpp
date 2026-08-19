#include "term_render.hpp"

#include <algorithm>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>

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
    row_.setColorDepth(16);
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
    // pushSprite に部分矩形版が無いので、転送先のクリップ矩形で範囲を絞る。
    // これで 1 行 1280px を毎回送らずに済む（タイプ入力なら数セル分だけ）。
    const int px_from = x_from * cell_w_;
    const int px_w    = (x_to - x_from + 1) * cell_w_;
    gfx_.setClipRect(px_from, y * cell_h_, px_w, cell_h_);
    row_.pushSprite(&gfx_, 0, y * cell_h_);
    gfx_.clearClipRect();
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
