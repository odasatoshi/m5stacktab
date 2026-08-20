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

// 色は 256 色パレットの番号 (0-255)。24bit 色 (SGR 38;2;r;g;b) は最近似に丸める。
// 既定色はパレット外の値で表す。パレット内の番号を流用すると ESC[38;5;255m や
// 24bit の明るいグレーが「既定色」に潰れる。
constexpr uint16_t kDefaultFg = 256;
constexpr uint16_t kDefaultBg = 257;

struct Attr {
    uint16_t fg    = kDefaultFg;
    uint16_t bg    = kDefaultBg;
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
    // その行で変わった列の範囲 [min, max]。1280px を毎回転送しないために使う。
    // is_dirty(y) が false のときの値は無意味。
    int dirty_min_x(int y) const { return (y >= 0 && y < rows_) ? dirty_min_[y] : 0; }
    int dirty_max_x(int y) const { return (y >= 0 && y < rows_) ? dirty_max_[y] : -1; }

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
    // **ライブ画面**を読む。スクロールバックを見ている間は画面に出ているものと違う。
    // 今のところ呼び出し元はテストだけ（production は view 側に統一されている）。
    std::string row_text(int y) const;

    // **今画面に見えているもの**を読む（view_offset を反映する）。
    // これが無いと履歴の中身をテストで検証できず、行数しか固められない
    // （追い出した行の代わりに空行を積んでもテストが通ってしまう）。
    std::string view_row_text(int y) const;
    // 1 セル単位で見たいとき。view_offset を反映する。
    const Cell& view_cell(int x, int y) const;

    // --- スクロールバック ---
    //
    // バッファは呼び出し側が用意する (cols * max_lines 個の Cell)。ESP-IDF 側は PSRAM から
    // 確保して渡す。コア側で確保するとアロケータを選べないため。
    // **resize() の扱い**: 桁数が変わったときだけ履歴を破棄する（履歴は cols 単位で
    // 詰めてあるので使い回せない）。行数だけの変更では履歴は残り、行が減って
    // 画面から追い出される分はここに積まれる。view_offset はどちらでも 0 に戻る。
    // buffer は cols * max_lines 個。**cols は必須。** push_scrollback は現在の
    // cols_ でストライドするので、確保時と食い違うと呼び出し側のバッファを
    // 踏み越える（今は cols が変わらないので到達しない）。既定値を持たせると
    // 「呼び出し側の意図」ではなく「その瞬間の端末の桁数」を推測することになり、
    // 引数を足した動機と矛盾する。
    void set_scrollback(Cell* buffer, int max_lines, int cols);
    int  scrollback_lines() const { return sb_count_; }
    int  view_offset() const { return view_offset_; }
    // 表示位置を動かす (正 = 過去へ)。実際に動いた行数を返す。
    int  scroll_view(int delta);

    // 履歴が「確保時と桁数が違うので積めない」状態か。到達しない前提だが、
    // 到達したときに履歴が空のまま何の痕跡も残らないのは困るので観測できるようにする。
    bool scrollback_stalled() const { return sb_buf_ != nullptr && sb_cols_ != cols_; }

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
    // 列範囲つき。x2 < 0 なら行全体。
    void mark_dirty_range(int y, int x1, int x2);
    void mark_all_dirty();
    void erase_cells(int x, int y, int count);
    // 現在の背景色で埋めるだけ。全角の整合は取らないので repair_row と併用する。
    void fill_blank(int x, int y, int count);
    // 行内の全角セルの整合を直す (孤立した右半分、相棒を失った左半分を空白に戻す)。
    // セルをずらす操作 (ICH/DCH/IRM) や範囲消去の後に呼ぶ。
    void repair_row(int y);
    void clear_region(int from_index, int to_index);
    void switch_alt(bool enable, bool clear, bool save_restore_cursor);
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
    std::vector<int>  dirty_min_;
    std::vector<int>  dirty_max_;

    Cursor cur_{};
    Cursor saved_main_{};  // ESC 7 / CSI s の退避先 (主画面)
    Cursor saved_alt_{};   // ESC 7 / CSI s の退避先 (代替画面)
    Cursor alt_entry_{};   // 代替画面に入るときの退避先。DECSC とは別スロットにする

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

    // スクロールバック (呼び出し側所有のリングバッファ)。
    Cell* sb_buf_      = nullptr;
    int   sb_max_      = 0;
    int   sb_count_    = 0;   // 保持している行数
    int   sb_head_     = 0;   // 次に書く位置
    int   view_offset_ = 0;   // 0 = 最新
    int   sb_cols_     = 0;   // 履歴バッファを確保したときの桁数
    const Cell* sb_line(int lines_back) const;
    void  push_scrollback(const Cell* row);
    std::string row_text_impl(int y, bool use_view) const;

    // CSI パラメータの個数上限。無制限だと ";" の連打で際限なくヒープを食う。
    static constexpr size_t kMaxParams = 32;

    State                state_ = State::kGround;
    std::vector<int>     params_;
    bool                 param_seen_     = false;
    bool                 param_overflow_ = false;
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
