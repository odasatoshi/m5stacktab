#pragma once
// VT100 コアのスクリーンバッファを M5GFX で描く。
//
// 1280x720 を毎フレーム全面書き換えると間に合わないので、行単位のスプライトを使って
// dirty 行だけ転送する。フォントは M5GFX 内蔵の日本語等幅 (efont) を使い、
// 全角は 2 セル分の幅で描く。
#include <M5GFX.h>

#include "vt100.hpp"

class TermRenderer {
public:
    explicit TermRenderer(M5GFX& gfx) : gfx_(gfx), row_(&gfx) {}

    bool begin();

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    // 画面下部を別用途（キーボード）に使うとき、端末が使う行数を狭める。
    // full_rows() まで戻せる（キーボードを隠したときに画面全体を使うため）。
    void set_rows(int rows)
    {
        if (rows > 0 && rows <= full_rows_) rows_ = rows;
    }
    int full_rows() const { return full_rows_; }
    int cell_w() const { return cell_w_; }
    int cell_h() const { return cell_h_; }

    // dirty 行 (とカーソルが動いた行) だけ描画する。force で全面。
    void render(vt::Terminal& term, bool force = false);

    uint32_t last_render_us() const { return last_us_; }
    int      last_rows_drawn() const { return last_rows_; }
    // 内訳 (最適化の判断用): スプライトへの描画と、パネルへの転送。
    uint32_t last_draw_us() const { return last_draw_us_; }
    uint32_t last_push_us() const { return last_push_us_; }
    // 実際にパネルへ送ったピクセル数（差分転送の効き目を見る）。
    uint32_t last_pixels() const { return last_px_; }

private:
    // [x_from, x_to] のセル範囲だけ描いて転送する。
    void draw_row(vt::Terminal& term, int y, int x_from, int x_to);

    M5GFX&    gfx_;
    M5Canvas  row_;
    int       cell_w_ = 0;
    int       cell_h_ = 0;
    int       cols_      = 0;
    int       rows_      = 0;
    int       full_rows_ = 0;  // 画面全体を使ったときの行数
    uint32_t  last_us_      = 0;
    uint32_t  last_draw_us_ = 0;
    uint32_t  last_push_us_ = 0;
    uint32_t  last_px_      = 0;
    int       last_rows_ = 0;
    int       cur_x_ = -1;
    int       cur_y_ = -1;
    // 0-255 が xterm パレット、256 が既定前景、257 が既定背景 (vt::kDefaultFg/Bg に対応)。
    uint16_t  pal_[258] = {};
};
