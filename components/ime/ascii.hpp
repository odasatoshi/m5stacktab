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
    // 送るときに必ず付ける修飾ビット（PAD の `^C` のように、キー 1 つで
    // Ctrl 付きを送るため）。**HID と同じ並び**で、kbd_key_to_bytes に渡す。
    uint8_t     send_mod = 0;
};

// PAD 面（端末に薄く重ねる小さなパッド）。**行数を削らない**ので、出力を読みながら
// 矢印で辿るときに使う。span は全部 1 なので当たり判定は割り算だけ（描画側でやる）。
//
//   Esc  ↑  Tab  ^C
//   ←    ↓  →    Enter
//
// **Ctrl 単体は置かない。** PAD に文字キーが無いので、ラッチしても掛ける先が無い
// （`kbd_key_to_bytes` の Ctrl は 1 文字のキー名にしか効かない）。端末で本当に要るのは
// `Ctrl-C` なので、それをキー 1 つにしてある。
// Ctrl の修飾ビット。`main/kbd_keys.hpp` の kKbdModCtrl と同じ値でなければならない
// （ここは main に依存させたくないので持ち直している。一致はホストテストで見る）。
constexpr uint8_t kCtrlBit = 0x01;

constexpr int kPadCols = 4;
constexpr int kPadRows = 2;
// 範囲外なら nullptr。
const AsciiKey* pad_key(int row, int col);

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
