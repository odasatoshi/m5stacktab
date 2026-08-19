#pragma once
// 画面下部に 12 キーフリックキーボードと IME の未確定表示を描く。
// 入力結果は SSH へ送る文字列として返す。
#include <functional>
#include <string>

#include <M5GFX.h>

#include "flick.hpp"
#include "ime.hpp"
#include "term_render.hpp"

class KeyboardUi {
public:
    KeyboardUi(M5GFX& gfx, TermRenderer& renderer) : gfx_(gfx), renderer_(renderer) {}

    // 画面下部の height ピクセルをキーボードに使う。
    void begin(int height);
    void set_dict(const ime::SkkDict* dict) { ime_.set_dict(dict); }

    bool visible() const { return visible_; }
    void set_visible(bool v);
    int  height() const { return visible_ ? height_ : 0; }

    // タッチイベントを渡す。キーボードが処理したら true。
    bool touch_down(int x, int y);
    bool touch_move(int x, int y);
    bool touch_up(int x, int y);

    // リモートへ送る文字列が確定したときに呼ばれる。
    void set_output(std::function<void(const std::string&)> fn) { output_ = std::move(fn); }

    void draw();

private:
    void draw_key(int row, int col, bool pressed);
    void draw_status();
    void emit(const std::string& s);

    M5GFX&              gfx_;
    TermRenderer&       renderer_;
    ime::FlickKeyboard  kb_;
    ime::Ime            ime_;
    bool                visible_    = false;
    int                 height_     = 0;
    int                 status_h_   = 0;
    int                 pressed_key_ = -1;
    ime::Flick          preview_    = ime::Flick::kCenter;
    std::function<void(const std::string&)> output_;
};
