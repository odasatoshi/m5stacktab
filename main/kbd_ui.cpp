#include "kbd_ui.hpp"

#include <algorithm>
#include <cstdio>

#include <esp_log.h>

namespace {

const char* TAG = "kbd";

constexpr uint16_t kBg       = 0x2104;  // 暗いグレー
constexpr uint16_t kKeyBg    = 0x4208;
constexpr uint16_t kKeyDown  = 0x05BF;  // 押下中は青
constexpr uint16_t kKeyLine  = 0x8410;
constexpr uint16_t kText     = 0xFFFF;
constexpr uint16_t kStatusBg = 0x0000;
constexpr uint16_t kComposeC = 0xFFE0;  // 未確定は黄色

}  // namespace

void KeyboardUi::begin(int height)
{
    height_   = height;
    status_h_ = 32;  // 未確定と候補を出す帯

    // 画面幅いっぱいに 4 列だと 1 キー 320px で間延びする。親指で押せる幅に収めて中央に置き、
    // 空いた右側は候補表示に使う。
    constexpr int kMaxKeyW = 160;
    ime::FlickLayout l;
    l.width  = std::min(gfx_.width(), kMaxKeyW * 4);
    l.x      = (gfx_.width() - l.width) / 2;
    l.y      = gfx_.height() - height_ + status_h_;
    l.height = height_ - status_h_;
    l.cols   = 4;
    l.rows   = 4;
    kb_.set_layout(l);
    ESP_LOGI(TAG, "keyboard %dx%d at y=%d (key %dx%d)", l.width, l.height, l.y, l.key_w(),
             l.key_h());
}

void KeyboardUi::set_visible(bool v)
{
    if (visible_ == v) return;
    visible_ = v;
    if (visible_) {
        draw();
    } else {
        // 端末側を描き直させる。
        gfx_.fillRect(0, gfx_.height() - height_, gfx_.width(), height_, TFT_BLACK);
    }
}

void KeyboardUi::draw_key(int row, int col, bool pressed)
{
    const auto& l  = kb_.layout();
    const int   kw = l.key_w();
    const int   kh = l.key_h();
    const int   x  = l.x + col * kw;
    const int   y  = l.y + row * kh;

    gfx_.fillRect(x + 2, y + 2, kw - 4, kh - 4, pressed ? kKeyDown : kKeyBg);
    gfx_.drawRect(x + 2, y + 2, kw - 4, kh - 4, kKeyLine);

    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextColor(kText, pressed ? kKeyDown : kKeyBg);
    const char* label = ime::FlickKeyboard::key_label(row, col);
    const int   tw    = gfx_.textWidth(label);
    gfx_.drawString(label, x + (kw - tw) / 2, y + (kh - 24) / 2);

    // 押下中はフリック先を四方に薄く出す（どこに飛ぶか分かるように）。
    if (pressed && col < 3) {
        gfx_.setFont(&fonts::efontJA_16);
        struct { ime::Flick f; int dx; int dy; } dirs[] = {
            {ime::Flick::kLeft, -kw / 3, 0},
            {ime::Flick::kUp, 0, -kh / 3},
            {ime::Flick::kRight, kw / 3, 0},
            {ime::Flick::kDown, 0, kh / 3},
        };
        for (const auto& d : dirs) {
            const char* k = ime::FlickKeyboard::kana_for(row, col, d.f);
            if (!k) continue;
            const bool sel = (preview_ == d.f);
            gfx_.setTextColor(sel ? kComposeC : kKeyLine, kKeyDown);
            gfx_.drawString(k, x + kw / 2 - 8 + d.dx, y + kh / 2 - 8 + d.dy);
        }
    }
}

void KeyboardUi::draw_status()
{
    const int y = gfx_.height() - height_;
    gfx_.fillRect(0, y, gfx_.width(), status_h_, kStatusBg);
    gfx_.setFont(&fonts::efontJA_24);

    std::string line;
    if (ime_.mode() == ime::Mode::kDirect) {
        line = "[abc] ";
    } else {
        line = "[かな] ";
    }
    const std::string composing = ime_.composing() + ime_.pending_romaji();
    if (!composing.empty()) {
        line += composing;
        if (ime_.mode() == ime::Mode::kSelect) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "  (%d/%d)", ime_.candidate_index() + 1,
                          (int)ime_.candidates().size());
            line += buf;
        }
    }
    gfx_.setTextColor(composing.empty() ? kKeyLine : kComposeC, kStatusBg);
    gfx_.drawString(line.c_str(), 8, y + 4);

    // 候補を横に並べる（選択中のものを強調）。
    if (ime_.mode() == ime::Mode::kSelect) {
        int cx = 8 + gfx_.textWidth(line.c_str()) + 24;
        for (size_t i = 0; i < ime_.candidates().size() && cx < gfx_.width() - 60; ++i) {
            const bool sel = (static_cast<int>(i) == ime_.candidate_index());
            gfx_.setTextColor(sel ? kStatusBg : kText, sel ? kComposeC : kStatusBg);
            const std::string& c = ime_.candidates()[i];
            gfx_.drawString(c.c_str(), cx, y + 4);
            cx += gfx_.textWidth(c.c_str()) + 16;
        }
    }
}

void KeyboardUi::draw()
{
    if (!visible_) return;
    const auto& l = kb_.layout();
    gfx_.fillRect(0, l.y, gfx_.width(), l.height, kBg);
    // キーボードの左右に余白ができるので、そこは背景色で塗るだけにする。
    for (int row = 0; row < l.rows; ++row) {
        for (int col = 0; col < l.cols; ++col) {
            draw_key(row, col, pressed_key_ == row * l.cols + col);
        }
    }
    draw_status();
}

void KeyboardUi::emit(const std::string& s)
{
    if (!s.empty() && output_) output_(s);
}

bool KeyboardUi::touch_down(int x, int y)
{
    if (!visible_) return false;
    if (!kb_.touch_down(x, y)) return false;
    pressed_key_ = kb_.pressed_key();
    preview_     = ime::Flick::kCenter;
    const auto& l = kb_.layout();
    draw_key(pressed_key_ / l.cols, pressed_key_ % l.cols, true);
    return true;
}

bool KeyboardUi::touch_move(int x, int y)
{
    if (!visible_ || !kb_.is_pressed()) return false;
    const ime::Flick f = kb_.current_flick(x, y);
    if (f == preview_) return true;
    preview_ = f;
    const auto& l = kb_.layout();
    draw_key(pressed_key_ / l.cols, pressed_key_ % l.cols, true);
    return true;
}

bool KeyboardUi::touch_up(int x, int y)
{
    if (!visible_ || !kb_.is_pressed()) return false;

    const int  released = pressed_key_;
    const auto r        = kb_.touch_up(x, y);
    pressed_key_        = -1;
    preview_            = ime::Flick::kCenter;

    const auto& l = kb_.layout();
    if (released >= 0) draw_key(released / l.cols, released % l.cols, false);

    if (!r.valid) return true;

    if (!r.kana.empty()) {
        // 「゛」「゜」「small」は文字ではなく直前のかなへの修飾。
        if (r.kana == "゛") {
            ime_.modify_last(ime::Ime::Modifier::kDakuten);
        } else if (r.kana == "゜") {
            ime_.modify_last(ime::Ime::Modifier::kHandakuten);
        } else if (r.kana == "small") {
            ime_.modify_last(ime::Ime::Modifier::kSmall);
        } else {
            emit(ime_.input_kana(r.kana));
        }
    } else {
        switch (r.func) {
            case ime::FuncKey::kBackspace:
                // 未確定が無ければ端末に BS を送る。
                if (!ime_.backspace()) emit("\x7f");
                break;
            case ime::FuncKey::kConvert:
                ime_.convert();
                break;
            case ime::FuncKey::kCommit:
                if (ime_.empty()) {
                    emit("\r");  // 何も入力中でなければ Enter
                } else {
                    emit(ime_.commit());
                }
                break;
            case ime::FuncKey::kMode:
                ime_.set_direct(ime_.mode() == ime::Mode::kDirect ? false : true);
                break;
            case ime::FuncKey::kNone:
                break;
        }
    }
    draw_status();
    return true;
}
