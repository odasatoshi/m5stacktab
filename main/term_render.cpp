#include "term_render.hpp"

#include <algorithm>

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
    cols_ = gfx_.width() / cell_w_;
    rows_ = gfx_.height() / cell_h_;

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
    if (!row_.createSprite(gfx_.width(), cell_h_)) {
        ESP_LOGE(TAG, "row sprite alloc failed (%dx%d)", (int)gfx_.width(), cell_h_);
        return false;
    }
    ESP_LOGI(TAG, "grid=%dx%d cell=%dx%d sprite=%d bytes", cols_, rows_, cell_w_, cell_h_,
             (int)gfx_.width() * cell_h_ * 2);

    return true;
}

void TermRenderer::draw_row(vt::Terminal& term, int y)
{
    const int64_t t_draw = esp_timer_get_time();
    row_.fillSprite(pal_[vt::kDefaultBg]);

    const bool cursor_here = term.cursor_visible() && y == term.cursor_y();
    const int  cursor_x    = term.cursor_x();

    for (int x = 0; x < cols_; ++x) {
        const vt::Cell& c = term.cell(x, y);
        if (c.width == 0) continue;  // 全角の右半分は左半分が描いている

        uint16_t fg = pal_[c.attr.fg];
        uint16_t bg = pal_[c.attr.bg];

        // 太字は明色化する (専用のボールドフォントは無い)。xterm 系と同じ扱い。
        if ((c.attr.flags & vt::kBold) && c.attr.fg < 8) fg = pal_[c.attr.fg + 8];

        const bool reverse = (c.attr.flags & vt::kReverse) != 0;
        // カーソルは反転ブロック。反転属性と重なったら二重反転で元に戻る。
        const bool on_cursor = cursor_here && (x == cursor_x || (c.width == 2 && x + 1 == cursor_x));
        if (reverse != on_cursor) std::swap(fg, bg);

        const int px = x * cell_w_;

        // 既定背景のまま・空白・装飾なしなら fillSprite の結果で足りる。
        const bool plain_blank = (c.ch == ' ' || c.ch == 0) &&
                                 bg == pal_[vt::kDefaultBg] &&
                                 (c.attr.flags & (vt::kUnderline | vt::kStrike)) == 0;
        if (plain_blank) continue;

        row_.fillRect(px, 0, cell_w_ * c.width, cell_h_, bg);

        if ((c.attr.flags & vt::kInvisible) == 0 && c.ch != ' ' && c.ch != 0) {
            row_.setTextColor(fg, bg);
            // drawChar は BMP (16bit) までなので、それ以外は下駄にする。
            uint16_t glyph = (c.ch <= 0xFFFF) ? static_cast<uint16_t>(c.ch) : 0x3013;
            row_.drawChar(glyph, px, 0);
        }
        if (c.attr.flags & vt::kUnderline) {
            row_.drawFastHLine(px, cell_h_ - 2, cell_w_ * c.width, fg);
        }
        if (c.attr.flags & vt::kStrike) {
            row_.drawFastHLine(px, cell_h_ / 2, cell_w_ * c.width, fg);
        }
    }
    const int64_t t_push = esp_timer_get_time();
    last_draw_us_ += static_cast<uint32_t>(t_push - t_draw);
    row_.pushSprite(0, y * cell_h_);
    last_push_us_ += static_cast<uint32_t>(esp_timer_get_time() - t_push);
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
    // 転送の準備を 1 回にまとめる。行ごとに pushSprite するとその都度セットアップが走る。
    gfx_.startWrite();
    for (int y = 0; y < rows_; ++y) {
        // カーソルが動いたら、消す側と描く側の 2 行も描き直す。
        const bool need = force || term.is_dirty(y) || (cursor_moved && (y == cur_y_ || y == cy));
        if (!need) continue;
        draw_row(term, y);
        ++drawn;
    }
    gfx_.endWrite();
    cur_x_ = cx;
    cur_y_ = cy;
    term.clear_dirty();

    last_us_   = static_cast<uint32_t>(esp_timer_get_time() - t0);
    last_rows_ = drawn;
}
