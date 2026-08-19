#include "flick.hpp"

#include <cmath>
#include <cstdlib>

namespace ime {
namespace {

// [row][col][flick] = かな。flick は center, left, up, right, down の順。
// 濁点・半濁点・小文字は「小」キー（3 行 0 列）で直前の文字を変換する運用にする。
constexpr const char* kKana[4][3][5] = {
    {  // 1 行目
        {"あ", "い", "う", "え", "お"},
        {"か", "き", "く", "け", "こ"},
        {"さ", "し", "す", "せ", "そ"},
    },
    {  // 2 行目
        {"た", "ち", "つ", "て", "と"},
        {"な", "に", "ぬ", "ね", "の"},
        {"は", "ひ", "ふ", "へ", "ほ"},
    },
    {  // 3 行目
        {"ま", "み", "む", "め", "も"},
        {"や", "ゆ", "よ", "ゃ", "ゅ"},
        {"ら", "り", "る", "れ", "ろ"},
    },
    {  // 4 行目
        {"゛", "゜", "small", "", ""},  // 濁点・半濁点・小文字化（呼び出し側で処理する）
        {"わ", "を", "ん", "ー", "〜"},
        {"、", "。", "？", "！", "・"},
    },
};

constexpr const char* kLabel[4][4] = {
    {"あ", "か", "さ", "⌫"},
    {"た", "な", "は", "変換"},
    {"ま", "や", "ら", "確定"},
    {"゛小", "わ", "、", "abc"},
};

}  // namespace

const char* FlickKeyboard::key_label(int row, int col)
{
    if (row < 0 || row >= 4 || col < 0 || col >= 4) return "";
    return kLabel[row][col];
}

const char* FlickKeyboard::kana_for(int row, int col, Flick f)
{
    if (row < 0 || row >= 4 || col < 0 || col >= 3) return nullptr;
    const char* s = kKana[row][col][static_cast<int>(f)];
    return (s && s[0]) ? s : nullptr;
}

bool FlickKeyboard::touch_down(int px, int py)
{
    if (!layout_.contains(px, py) || layout_.key_w() <= 0 || layout_.key_h() <= 0) {
        pressed_ = false;
        return false;
    }
    const int col = (px - layout_.x) / layout_.key_w();
    const int row = (py - layout_.y) / layout_.key_h();
    pressed_      = true;
    press_key_    = row * layout_.cols + col;
    press_x_      = px;
    press_y_      = py;
    return true;
}

Flick FlickKeyboard::current_flick(int px, int py) const
{
    if (!pressed_) return Flick::kCenter;
    // キーサイズの 1/3 動かしたらフリックと見なす。指の太さを考えると 1/4 では誤検出する。
    const int threshold = std::max(12, layout_.key_w() / 3);
    const int dx        = px - press_x_;
    const int dy        = py - press_y_;
    if (std::abs(dx) < threshold && std::abs(dy) < threshold) return Flick::kCenter;
    if (std::abs(dx) > std::abs(dy)) return dx < 0 ? Flick::kLeft : Flick::kRight;
    return dy < 0 ? Flick::kUp : Flick::kDown;
}

FlickResult FlickKeyboard::touch_up(int px, int py)
{
    FlickResult r;
    if (!pressed_) return r;

    const Flick f = current_flick(px, py);
    const int   row = press_key_ / layout_.cols;
    const int   col = press_key_ % layout_.cols;
    pressed_        = false;

    r.valid     = true;
    r.key_index = press_key_;
    r.flick     = f;

    if (col == 3) {
        // 機能キー列。フリックは無視する。
        switch (row) {
            case 0: r.func = FuncKey::kBackspace; break;
            case 1: r.func = FuncKey::kConvert; break;
            case 2: r.func = FuncKey::kCommit; break;
            default: r.func = FuncKey::kMode; break;
        }
        return r;
    }
    if (const char* k = kana_for(row, col, f)) r.kana = k;
    return r;
}

}  // namespace ime
