#pragma once
// 12 キーフリック入力。タッチ座標だけを扱い、描画には依存しない（ホストでテストできる）。
//
// キー配置（3 列 x 4 行 + 機能キー列）:
//   あ か さ
//   た な は
//   ま や ら
//   小 わ 、
// 各キーは中心タップで「あ」、上下左右のフリックで「い う え お」を出す。
#include <cstdint>
#include <string>

namespace ime {

// フリック方向。
enum class Flick {
    kCenter,
    kLeft,
    kUp,
    kRight,
    kDown,
};

// キーボードの矩形レイアウト。画面下部に置く想定。
struct FlickLayout {
    int x      = 0;
    int y      = 0;
    int width  = 0;
    int height = 0;
    int cols   = 4;  // 3 列のかなキー + 1 列の機能キー
    int rows   = 4;

    int key_w() const { return cols > 0 ? width / cols : 0; }
    int key_h() const { return rows > 0 ? height / rows : 0; }
    bool contains(int px, int py) const
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// 機能キー（右端の列）。
enum class FuncKey {
    kNone,
    kBackspace,
    kConvert,   // 変換 / 次候補
    kCommit,    // 確定 / 改行
    kMode,      // かな / 英数 切り替え
};

// 1 回のタッチ操作の結果。
struct FlickResult {
    bool        valid    = false;
    std::string kana;                    // 出力するかな（機能キーなら空）
    FuncKey     func     = FuncKey::kNone;
    int         key_index = -1;          // デバッグ用
    Flick       flick    = Flick::kCenter;
};

class FlickKeyboard {
public:
    void set_layout(const FlickLayout& l) { layout_ = l; }
    const FlickLayout& layout() const { return layout_; }

    // 指を置いた。キーボード内なら true。
    bool touch_down(int px, int py);
    // 指を離した。押していたキーとフリック方向から結果を返す。
    FlickResult touch_up(int px, int py);
    // 押しっぱなしのまま面が切り替わったときに捨てる（そのまま離すと、
    // 選んでいないかなが出る）。
    void cancel() { pressed_ = false; }
    bool is_pressed() const { return pressed_; }
    int  pressed_key() const { return press_key_; }

    // 指の移動をフリック方向に落とす（プレビュー表示用）。
    Flick current_flick(int px, int py) const;

    // キーに割り当てられた文字（表示用）。row/col は 0 起点。
    static const char* key_label(int row, int col);
    // キーとフリック方向に対応するかな。機能キー列では nullptr。
    static const char* kana_for(int row, int col, Flick f);

private:
    FlickLayout layout_;
    bool        pressed_   = false;
    int         press_key_ = -1;
    int         press_x_   = 0;
    int         press_y_   = 0;
};

}  // namespace ime
