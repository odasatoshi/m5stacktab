#include "ascii.hpp"

#include <algorithm>

namespace ime {
namespace {

// **数字段の記号は US 配列そのまま。** 端末の向こう側 (stty / シェル) は US 前提で
// 動いているので、ここだけ JIS にすると打った記号と出る記号が食い違う。
constexpr AsciiKey kRow0[] = {
    {"1", "1", "!"}, {"2", "2", "@"}, {"3", "3", "#"}, {"4", "4", "$"},
    {"5", "5", "%"}, {"6", "6", "^"}, {"7", "7", "&"}, {"8", "8", "*"},
    {"9", "9", "("}, {"0", "0", ")"}, {"-", "-", "_"}, {"=", "=", "+"},
};
constexpr AsciiKey kRow1[] = {
    {"q", "q", "Q"}, {"w", "w", "W"}, {"e", "e", "E"}, {"r", "r", "R"},
    {"t", "t", "T"}, {"y", "y", "Y"}, {"u", "u", "U"}, {"i", "i", "I"},
    {"o", "o", "O"}, {"p", "p", "P"}, {"[", "[", "{"}, {"]", "]", "}"},
};
constexpr AsciiKey kRow2[] = {
    {"a", "a", "A"}, {"s", "s", "S"}, {"d", "d", "D"}, {"f", "f", "F"},
    {"g", "g", "G"}, {"h", "h", "H"}, {"j", "j", "J"}, {"k", "k", "K"},
    {"l", "l", "L"}, {";", ";", ":"}, {"'", "'", "\""},
    {"BS", "backspace"},
};
constexpr AsciiKey kRow3[] = {
    {"Shift", "", nullptr, AsciiMod::kShift},
    {"z", "z", "Z"}, {"x", "x", "X"}, {"c", "c", "C"}, {"v", "v", "V"},
    {"b", "b", "B"}, {"n", "n", "N"}, {"m", "m", "M"},
    {",", ",", "<"}, {".", ".", ">"}, {"/", "/", "?"},
    {"Enter", "enter"},
};
constexpr AsciiKey kRow4[] = {
    {"Ctrl", "", nullptr, AsciiMod::kCtrl},
    {"Esc", "esc"},
    {"Tab", "tab"},
    {"space", " ", nullptr, AsciiMod::kNone, 4},
    {"←", "left"}, {"↑", "up"}, {"↓", "down"}, {"→", "right"},
    {"かな", "", nullptr, AsciiMod::kKana},
};

struct Row {
    const AsciiKey* keys;
    int             n;
};
constexpr Row kTable[AsciiKeyboard::kRows] = {
    {kRow0, (int)(sizeof(kRow0) / sizeof(kRow0[0]))},
    {kRow1, (int)(sizeof(kRow1) / sizeof(kRow1[0]))},
    {kRow2, (int)(sizeof(kRow2) / sizeof(kRow2[0]))},
    {kRow3, (int)(sizeof(kRow3) / sizeof(kRow3[0]))},
    {kRow4, (int)(sizeof(kRow4) / sizeof(kRow4[0]))},
};

}  // namespace

const AsciiKey* AsciiKeyboard::key_at(int row, int index)
{
    if (row < 0 || row >= kRows) return nullptr;
    if (index < 0 || index >= kTable[row].n) return nullptr;
    return &kTable[row].keys[index];
}

int AsciiKeyboard::col_of(int row, int index)
{
    if (!key_at(row, index)) return -1;
    int col = 0;
    for (int i = 0; i < index; ++i) col += kTable[row].keys[i].span;
    return col;
}

bool AsciiKeyboard::hit(int px, int py, int* row, int* index) const
{
    if (!layout_.contains(px, py)) return false;
    const int kw = layout_.key_w();
    const int kh = layout_.key_h();
    if (kw <= 0 || kh <= 0) return false;
    // 端数で col == kCols になると次の行の 0 列として解釈されるのでクランプする
    // （かな面の touch_down と同じ理由）。
    const int r = std::clamp((py - layout_.y) / kh, 0, kRows - 1);
    const int c = std::clamp((px - layout_.x) / kw, 0, kCols - 1);
    for (int i = 0, col = 0; i < kTable[r].n; ++i) {
        col += kTable[r].keys[i].span;
        if (c < col) {
            *row   = r;
            *index = i;
            return true;
        }
    }
    return false;  // 行の span の合計が kCols に足りていない（表の書き間違い）
}

}  // namespace ime
