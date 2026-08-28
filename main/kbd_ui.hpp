#pragma once
// 画面下部のソフトキーボード。段 (Mode) を切り替えて、ASCII 面とかな面を出し分ける。
//
// 段は**ステータスバーのボタンで巡回する** (#65)。ASCII 面のキーは
// 純正キーボード (A164) と同じ「名前」で返すので、端末へ送るバイト列は
// `kbd_keys.hpp` の kbd_key_to_bytes が両方の面倒を見る（片方だけ直せない）。
#include <cstdint>
#include <functional>
#include <string>

#include <M5GFX.h>

#include "ascii.hpp"
#include "flick.hpp"
#include "ime.hpp"
#include "term_render.hpp"

class KeyboardUi {
public:
    KeyboardUi(M5GFX& gfx, TermRenderer& renderer)
        : gfx_(gfx), renderer_(renderer), pad_(&gfx)
    {
    }

    // 段。**switch に default を置かない** — 段を足したときに、
    // 巡回とラベルの両方がコンパイルエラーで見つかるようにする。
    //
    // **kPad だけは端末に重なる。** 行数を削らないので height() は 0 を返す。
    enum class Mode : uint8_t { kOff, kAscii, kKana, kPad };

    // 巡回の順序とラベルはここにしか置かない（複製すると片方が取り残される）。
    static Mode        next_mode(Mode m);
    static const char* mode_label(Mode m);

    // 画面下部の height ピクセルをキーボードに使う。
    void begin(int height);
    void set_dict(const ime::SkkDict* dict) { ime_.set_dict(dict); }

    Mode mode() const { return mode_; }
    void set_mode(Mode m);
    bool visible() const { return mode_ != Mode::kOff; }
    // **画面下部から奪う高さ。** PAD は端末に重ねるので 0（行数を削らない）。
    int  height() const
    {
        return (mode_ == Mode::kAscii || mode_ == Mode::kKana) ? height_ : 0;
    }

    // 端末を描いた後に呼ぶ。**PAD は端末の差分描画で欠ける**ので、重ね直す。
    // PAD 以外の段では何もしない。
    void draw_overlay();
    // 直近の重ね直しにかかった時間 (us)。端末の描画に毎回上乗せされるので測れるようにする。
    uint32_t last_overlay_us() const { return last_overlay_us_; }

    // タッチイベントを渡す。キーボードが処理したら true。
    bool touch_down(int x, int y);
    bool touch_move(int x, int y);
    bool touch_up(int x, int y);

    // リモートへ送る文字列が確定したときに呼ばれる（かな面）。
    void set_output(std::function<void(const std::string&)> fn) { output_ = std::move(fn); }
    // ASCII 面のキー。名前と修飾ビットで返すので、純正キーボードと同じ経路に載せられる。
    void set_key_output(std::function<void(const std::string&, uint8_t)> fn)
    {
        key_output_ = std::move(fn);
    }
    // 面の切り替え要求（かな面の "abc" / ASCII 面の "かな"）。**自分では set_mode しない** —
    // 段はステータスバーの表示と端末の行数と対になっているので、ここで直に変えると
    // 画面だけ変わって状態が食い違う（実機で踏んだ: 面は ABC なのに `kbd` は かな に戻る）。
    void set_mode_request(std::function<void(Mode)> fn) { mode_request_ = std::move(fn); }

    void draw();

private:
    void draw_key(int row, int col, bool pressed);
    void build_pad();
    void draw_pad_key(int row, int col, bool pressed);
    bool pad_hit(int x, int y, int* row, int* col) const;
    bool pad_touch_up();
    void draw_ascii_key(int row, int index, bool pressed);
    void draw_ascii();
    void draw_status();
    void emit(const std::string& s);
    bool ascii_touch_up();

    M5GFX&              gfx_;
    TermRenderer&       renderer_;
    ime::FlickKeyboard  kb_;
    ime::AsciiKeyboard  ascii_;
    // PAD は市松に間引いたスプライトを透過色つきで重ねる（作り方の理由は .cpp）。
    LGFX_Sprite         pad_;
    int                 pad_x_ = 0;
    int                 pad_y_ = 0;
    uint32_t            last_overlay_us_ = 0;
    ime::Ime            ime_;
    Mode                mode_       = Mode::kOff;
    int                 height_     = 0;
    int                 status_h_   = 0;
    int                 pressed_key_ = -1;
    // ASCII 面で押しているキー（行 / 行内の番号）。-1 は押していない。
    int                 press_row_  = -1;
    int                 press_idx_  = -1;
    // **次の 1 打だけ効くラッチ。** 指は押しっぱなしにできない。
    bool                shift_      = false;
    bool                ctrl_       = false;
    ime::Flick          preview_    = ime::Flick::kCenter;
    std::function<void(const std::string&)> output_;
    std::function<void(const std::string&, uint8_t)> key_output_;
    std::function<void(Mode)>                        mode_request_;
};
