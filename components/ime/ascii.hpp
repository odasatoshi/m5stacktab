#pragma once
// 画面キーボードの ASCII 面。座標とキーの表だけを持ち、描画にも端末にも依存しない
// （ホストでテストできる）。
//
// **押されたキーは「名前」で返す。** 純正キーボード (A164) と同じ名前
// ("a" / "enter" / "up" ...) にしてあるので、端末へ送るバイト列は
// `main/kbd_keys.hpp` の `kbd_key_to_bytes` が両方の面倒を見る
// （DECCKM も Ctrl の 0x1F マスクも、片方だけ直すということが起きない）。
//
// 配列（12 列 x 5 行）:
//   1 2 3 4 5 6 7 8 9 0 - =
//   q w e r t y u i o p [ ]
//   a s d f g h j k l ; ' BS
//   Shift z x c v b n m , . / Enter
//   Ctrl Esc Tab [   space   ] ← ↑ ↓ → かな
//
// **面の文字は efontJA に有る字だけを使う。** ⌫ (U+232B) や ⏎ (U+23CE) は
// 豆腐 (□) になる（実機で確認）。矢印 (U+2190..2193) は JIS X 0208 にあるので出る。
#include <cstdint>

#include "flick.hpp"  // FlickLayout（矩形と key_w/key_h。かな面と共用する）

namespace ime {

// 修飾キー。**指は押しっぱなしにできない**ので、次の 1 打だけ効くラッチにする。
// kKana は面の切り替え（かな面の "abc" と対）。
enum class AsciiMod : uint8_t { kNone, kShift, kCtrl, kKana };

struct AsciiKey {
    const char* label = "";       // 面の表示
    const char* name  = "";       // 送るキー名（kbd_key_to_bytes が解釈する）
    // Shift ラッチ中の表示と名前。nullptr なら素のまま（BS や矢印など）。
    const char* shift = nullptr;
    AsciiMod    mod   = AsciiMod::kNone;  // kNone 以外は送らずにラッチを操作する
    uint8_t     span  = 1;                // 横に占める列数（space だけ 4）
};

class AsciiKeyboard {
public:
    static constexpr int kRows = 5;
    static constexpr int kCols = 12;

    void               set_layout(const FlickLayout& l) { layout_ = l; }
    const FlickLayout& layout() const { return layout_; }

    // 行 row の index 番目のキー。行の終端を越えたら nullptr。
    static const AsciiKey* key_at(int row, int index);
    // 行 row の index 番目のキーの左端の列（0..kCols-1）。範囲外なら -1。
    static int col_of(int row, int index);

    // 座標 -> (row, index)。キーボードの外なら false。
    bool hit(int px, int py, int* row, int* index) const;

private:
    FlickLayout layout_;
};

}  // namespace ime
