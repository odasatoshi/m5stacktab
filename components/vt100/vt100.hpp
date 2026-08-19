// VT100/ANSI 端末エミュレータのコア。
//
// ESP-IDF にも描画ライブラリにも依存しない。PTY から来たバイト列を食わせるとスクリーン
// バッファが更新され、UI 側はセルを読んで描画する。ホスト (macOS/Linux) でもそのまま
// ビルドできるので、テストは実機を使わずに回す。
//
// 対応範囲: カーソル移動、消去、SGR (256色 + 24bit色は近似)、スクロール領域、
//           代替画面バッファ、DECAWM、挿入削除、タブ、UTF-8、East Asian Width。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vt {

enum AttrFlag : uint16_t {
    kBold      = 1 << 0,
    kDim       = 1 << 1,
    kItalic    = 1 << 2,
    kUnderline = 1 << 3,
    kBlink     = 1 << 4,
    kReverse   = 1 << 5,
    kInvisible = 1 << 6,
    kStrike    = 1 << 7,
};

// 色は 256 色パレットの番号。既定色は kDefaultFg / kDefaultBg で表す。
// 24bit 色 (SGR 38;2;r;g;b) は 256 色パレットの最近似に丸める。
constexpr uint8_t kDefaultFg = 255;  // パレット 255 は実質使わないので既定色の印にする
constexpr uint8_t kDefaultBg = 254;

struct Attr {
    uint8_t  fg    = kDefaultFg;
    uint8_t  bg    = kDefaultBg;
    uint16_t flags = 0;

    bool operator==(const Attr& o) const
    {
        return fg == o.fg && bg == o.bg && flags == o.flags;
    }
    bool operator!=(const Attr& o) const { return !(*this == o); }
};

struct Cell {
    uint32_t ch    = ' ';  // Unicode コードポイント
    Attr     attr  = {};
    uint8_t  width = 1;    // 1 = 半角, 2 = 全角, 0 = 全角の右半分 (continuation)

    bool operator==(const Cell& o) const
    {
        return ch == o.ch && attr == o.attr && width == o.width;
    }
};

// 1 コードポイントの表示セル幅。East Asian Wide / Fullwidth を 2 とする。
int char_width(uint32_t cp);

class Terminal {
public:
    Terminal(int cols, int rows);

    // PTY / SSH channel から来た生バイト列。境界がマルチバイト文字の途中でも良い。
    void write(const uint8_t* data, size_t len);
    void write(const std::string& s);

    void resize(int cols, int rows);

    int cols() const { return cols_; }
    int rows() const { return rows_; }

    // 表示中の画面 (代替画面が有効ならそちら) のセル。
    const Cell& cell(int x, int y) const;

    int  cursor_x() const { return cur_.x; }
    int  cursor_y() const { return cur_.y; }
    bool cursor_visible() const { return cursor_visible_; }

    // 差分描画用。write() で内容が変わった行だけ true。
    bool is_dirty(int y) const { return y >= 0 && y < rows_ && dirty_[y]; }
    void clear_dirty();
    bool any_dirty() const;

    // 端末がホストへ返す応答 (DA, DSR/CPR など)。UI 側が SSH に書き戻す。
    void set_reply(std::function<void(const std::string&)> fn) { reply_ = std::move(fn); }

    // OSC 0/2 で設定されたウィンドウタイトル。
    const std::string& title() const { return title_; }

    // ホストがベルを鳴らした回数 (UI 側で振動や表示に使う)。
    uint32_t bell_count() const { return bell_count_; }

    bool alt_screen() const { return alt_active_; }
    bool bracketed_paste() const { return bracketed_paste_; }
    bool app_cursor_keys() const { return app_cursor_keys_; }

    // デバッグ・テスト用: 1 行を UTF-8 文字列にする (末尾の空白は落とす)。
    std::string row_text(int y) const;

private:
    struct Cursor {
        int  x = 0;
        int  y = 0;
        Attr attr{};
        bool pending_wrap = false;  // DECAWM: 右端に文字を置いた直後の状態
    };

    enum class State {
        kGround,
        kEsc,
        kEscIntermediate,  // ESC ( ) * + # の次の 1 バイトを読み捨てる
        kCsiParam,
        kOsc,
        kOscEsc,  // OSC 文字列中に ESC が来た (ST = ESC \ の待ち)
        kDcs,     // DCS/SOS/PM/APC。ST まで読み捨てる
        kDcsEsc,
    };

    std::vector<Cell>& screen() { return alt_active_ ? alt_ : main_; }
    const std::vector<Cell>& screen() const { return alt_active_ ? alt_ : main_; }
    Cell& at(int x, int y) { return screen()[static_cast<size_t>(y) * cols_ + x]; }

    void put_char(uint32_t cp);
    void exec_c0(uint8_t b);
    void exec_esc(uint8_t b);
    void exec_csi(uint8_t final_byte);
    void exec_sgr();
    void set_mode(bool enable);

    void mark_dirty(int y);
    void mark_all_dirty();
    void erase_cells(int x, int y, int count);
    void clear_region(int from_index, int to_index);
    void scroll_up(int top, int bottom, int n);
    void scroll_down(int top, int bottom, int n);
    void index();      // カーソルを 1 行下げる (必要ならスクロール)
    void reverse_index();
    void carriage_return();
    void tab_forward();
    void reset();
    void save_cursor();
    void restore_cursor();

    int  param(size_t i, int def) const;
    void clamp_cursor();
    // 全角の右半分を踏んだときに左半分を空白化して整合を保つ。
    void split_wide_at(int x, int y);

    int cols_ = 0;
    int rows_ = 0;

    std::vector<Cell> main_;
    std::vector<Cell> alt_;
    std::vector<bool> dirty_;

    Cursor cur_{};
    Cursor saved_main_{};
    Cursor saved_alt_{};

    int scroll_top_    = 0;  // スクロール領域 (0-origin, inclusive)
    int scroll_bottom_ = 0;

    bool alt_active_      = false;
    bool cursor_visible_  = true;
    bool autowrap_        = true;
    bool origin_mode_     = false;
    bool insert_mode_     = false;
    bool reverse_video_   = false;
    bool bracketed_paste_ = false;
    bool app_cursor_keys_ = false;

    std::vector<bool> tab_stops_;

    State                state_ = State::kGround;
    std::vector<int>     params_;
    bool                 param_seen_  = false;
    uint8_t              csi_private_ = 0;
    uint8_t              csi_inter_   = 0;
    std::string          osc_;

    // UTF-8 デコーダの持ち越し状態 (write() 境界で分断されるため)
    uint32_t utf8_cp_        = 0;
    int      utf8_remaining_ = 0;
    uint32_t utf8_min_       = 0;

    std::string title_;
    uint32_t    bell_count_ = 0;

    std::function<void(const std::string&)> reply_;
};

}  // namespace vt
