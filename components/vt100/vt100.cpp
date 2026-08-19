#include "vt100.hpp"

#include <algorithm>
#include <cstdio>

namespace vt {

namespace {

// East Asian Wide / Fullwidth の範囲。Unicode 15 の EastAsianWidth.txt から W/F のみ抜粋。
// 結合文字 (幅 0) は扱わない: 端末で結合文字を使う場面が限られるうえ、幅 0 を導入すると
// カーソル位置の整合を取る箇所が増える。
// ponytail: 結合文字は幅 1 として扱う。濁点付き仮名などが崩れたら幅 0 を追加する。
struct Range {
    uint32_t lo;
    uint32_t hi;
};

constexpr Range kWide[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6DF},
    {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF},
    {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

// 24bit 色を xterm 256 色パレットの最近似に丸める。
uint16_t rgb_to_256(int r, int g, int b)
{
    // グレースケール軸のほうが近ければそちらを使う。
    if (std::abs(r - g) < 8 && std::abs(g - b) < 8) {
        if (r < 8) return 16;
        if (r >= 248) return 231;  // > だと r==248 で 256 になり uint8_t で 0 (黒) に化ける
        return static_cast<uint16_t>(232 + (r - 8) * 24 / 240);
    }
    auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    return static_cast<uint16_t>(16 + 36 * q(r) + 6 * q(g) + q(b));
}

}  // namespace

int char_width(uint32_t cp)
{
    if (cp < 0x1100) return 1;
    for (const auto& r : kWide) {
        if (cp < r.lo) break;
        if (cp <= r.hi) return 2;
    }
    return 1;
}

Terminal::Terminal(int cols, int rows)
{
    // 全角文字は 2 セル使うので 1 桁の端末は許さない (put_char が右隣を触る)。
    cols_ = std::max(2, cols);
    rows_ = std::max(1, rows);
    main_.assign(static_cast<size_t>(cols_) * rows_, Cell{});
    alt_ = main_;
    dirty_.assign(rows_, true);
    dirty_min_.assign(rows_, 0);
    dirty_max_.assign(rows_, cols_ - 1);
    tab_stops_.assign(cols_, false);
    for (int x = 8; x < cols_; x += 8) tab_stops_[x] = true;
    scroll_top_    = 0;
    scroll_bottom_ = rows_ - 1;
    params_.reserve(16);
}

void Terminal::mark_dirty(int y)
{
    mark_dirty_range(y, 0, cols_ - 1);
}

void Terminal::mark_dirty_range(int y, int x1, int x2)
{
    if (y < 0 || y >= rows_) return;
    if (x2 < 0) x2 = cols_ - 1;
    x1 = std::clamp(x1, 0, cols_ - 1);
    x2 = std::clamp(x2, 0, cols_ - 1);
    if (!dirty_[y]) {
        dirty_[y]     = true;
        dirty_min_[y] = x1;
        dirty_max_[y] = x2;
    } else {
        dirty_min_[y] = std::min(dirty_min_[y], x1);
        dirty_max_[y] = std::max(dirty_max_[y], x2);
    }
}

void Terminal::mark_all_dirty()
{
    for (int y = 0; y < rows_; ++y) {
        dirty_[y]     = true;
        dirty_min_[y] = 0;
        dirty_max_[y] = cols_ - 1;
    }
}

void Terminal::clear_dirty()
{
    std::fill(dirty_.begin(), dirty_.end(), false);
    std::fill(dirty_min_.begin(), dirty_min_.end(), 0);
    std::fill(dirty_max_.begin(), dirty_max_.end(), -1);
}

bool Terminal::any_dirty() const
{
    return std::any_of(dirty_.begin(), dirty_.end(), [](bool d) { return d; });
}

const Cell& Terminal::cell(int x, int y) const
{
    static const Cell blank{};
    if (x < 0 || x >= cols_ || y < 0 || y >= rows_) return blank;
    return screen()[static_cast<size_t>(y) * cols_ + x];
}

std::string Terminal::row_text(int y) const
{
    std::string out;
    if (y < 0 || y >= rows_) return out;
    int last = -1;
    for (int x = 0; x < cols_; ++x) {
        if (cell(x, y).ch != ' ' || cell(x, y).width == 0) last = x;
    }
    for (int x = 0; x <= last; ++x) {
        const Cell& c = cell(x, y);
        if (c.width == 0) continue;  // 全角の右半分は出力しない
        uint32_t cp = c.ch;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

namespace {
// resize / スクロール後は行全体を描き直す必要があるので、範囲は行全体にする。
}  // namespace

void Terminal::resize(int cols, int rows)
{
    cols = std::max(2, cols);
    rows = std::max(1, rows);
    if (cols == cols_ && rows == rows_) return;

    auto regrow = [&](std::vector<Cell>& src) {
        std::vector<Cell> dst(static_cast<size_t>(cols) * rows, Cell{});
        // 下端を保ったまま詰め替える。行が減る場合は上を捨てる (シェルの挙動に近い)。
        int copy_rows = std::min(rows, rows_);
        int copy_cols = std::min(cols, cols_);
        int src_off   = rows_ - copy_rows;
        for (int y = 0; y < copy_rows; ++y) {
            for (int x = 0; x < copy_cols; ++x) {
                dst[static_cast<size_t>(y) * cols + x] =
                    src[static_cast<size_t>(y + src_off) * cols_ + x];
            }
        }
        src.swap(dst);
    };
    int old_rows = rows_;
    regrow(main_);
    regrow(alt_);

    cur_.y -= std::max(0, old_rows - rows);
    cols_ = cols;
    rows_ = rows;
    dirty_.assign(rows_, true);
    dirty_min_.assign(rows_, 0);
    dirty_max_.assign(rows_, cols_ - 1);
    tab_stops_.assign(cols_, false);
    for (int x = 8; x < cols_; x += 8) tab_stops_[x] = true;
    scroll_top_    = 0;
    scroll_bottom_ = rows_ - 1;
    cur_.pending_wrap = false;
    // 履歴は cols_ 単位で詰めてあるので、桁数が変わったら使い回せない。
    sb_count_    = 0;
    sb_head_     = 0;
    view_offset_ = 0;
    clamp_cursor();
}

void Terminal::clamp_cursor()
{
    cur_.x = std::clamp(cur_.x, 0, cols_ - 1);
    cur_.y = std::clamp(cur_.y, 0, rows_ - 1);
}

void Terminal::split_wide_at(int x, int y)
{
    // 全角セルの片方だけを上書きすると表示が壊れるので、相棒を空白に戻す。
    Cell& c = at(x, y);
    if (c.width == 2 && x + 1 < cols_) {
        Cell& r = at(x + 1, y);
        r.ch = ' ';
        r.width = 1;
    } else if (c.width == 0 && x > 0) {
        Cell& l = at(x - 1, y);
        l.ch = ' ';
        l.width = 1;
    }
}

void Terminal::fill_blank(int x, int y, int count)
{
    Cell blank{};
    blank.attr       = cur_.attr;
    blank.attr.flags = 0;  // 消去したセルに下線などを残さない
    for (int i = 0; i < count && x + i < cols_; ++i) {
        at(x + i, y) = blank;
    }
    mark_dirty_range(y, x - 1, x + count);
}

void Terminal::repair_row(int y)
{
    if (y < 0 || y >= rows_) return;
    for (int x = 0; x < cols_; ++x) {
        Cell& c = at(x, y);
        if (c.width == 2) {
            // 右半分が失われた左半分
            if (x + 1 >= cols_ || at(x + 1, y).width != 0) {
                c.ch    = ' ';
                c.width = 1;
            }
        } else if (c.width == 0) {
            // 左半分が失われた孤立した右半分
            if (x == 0 || at(x - 1, y).width != 2) {
                c.ch    = ' ';
                c.width = 1;
            }
        }
    }
    mark_dirty(y);
}

void Terminal::erase_cells(int x, int y, int count)
{
    fill_blank(x, y, count);
    repair_row(y);
}

void Terminal::scroll_up(int top, int bottom, int n)
{
    if (n <= 0 || top > bottom) return;
    n = std::min(n, bottom - top + 1);
    auto& s = screen();
    // 画面外へ押し出される行を履歴に残す。代替画面 (vim など) の中身は残さない。
    // スクロール領域が画面上端から始まっているときだけが「流れて消えた行」。
    if (!alt_active_ && sb_buf_ && top == 0) {
        for (int i = 0; i < n; ++i) {
            push_scrollback(&s[static_cast<size_t>(top + i) * cols_]);
        }
    }
    for (int y = top; y <= bottom - n; ++y) {
        std::copy(s.begin() + static_cast<size_t>(y + n) * cols_,
                  s.begin() + static_cast<size_t>(y + n + 1) * cols_,
                  s.begin() + static_cast<size_t>(y) * cols_);
    }
    Cell blank{};
    blank.attr = cur_.attr;
    blank.attr.flags = 0;
    for (int y = bottom - n + 1; y <= bottom; ++y) {
        std::fill(s.begin() + static_cast<size_t>(y) * cols_,
                  s.begin() + static_cast<size_t>(y + 1) * cols_, blank);
    }
    for (int y = top; y <= bottom; ++y) mark_dirty(y);
}

void Terminal::scroll_down(int top, int bottom, int n)
{
    if (n <= 0 || top > bottom) return;
    n = std::min(n, bottom - top + 1);
    auto& s = screen();
    for (int y = bottom; y >= top + n; --y) {
        std::copy(s.begin() + static_cast<size_t>(y - n) * cols_,
                  s.begin() + static_cast<size_t>(y - n + 1) * cols_,
                  s.begin() + static_cast<size_t>(y) * cols_);
    }
    Cell blank{};
    blank.attr = cur_.attr;
    blank.attr.flags = 0;
    for (int y = top; y < top + n; ++y) {
        std::fill(s.begin() + static_cast<size_t>(y) * cols_,
                  s.begin() + static_cast<size_t>(y + 1) * cols_, blank);
    }
    for (int y = top; y <= bottom; ++y) mark_dirty(y);
}

void Terminal::index()
{
    if (cur_.y == scroll_bottom_) {
        scroll_up(scroll_top_, scroll_bottom_, 1);
    } else if (cur_.y < rows_ - 1) {
        ++cur_.y;
    }
}

void Terminal::reverse_index()
{
    if (cur_.y == scroll_top_) {
        scroll_down(scroll_top_, scroll_bottom_, 1);
    } else if (cur_.y > 0) {
        --cur_.y;
    }
}

void Terminal::carriage_return()
{
    cur_.x            = 0;
    cur_.pending_wrap = false;
}

void Terminal::tab_forward()
{
    cur_.pending_wrap = false;
    for (int x = cur_.x + 1; x < cols_; ++x) {
        if (tab_stops_[x]) {
            cur_.x = x;
            return;
        }
    }
    cur_.x = cols_ - 1;
}

void Terminal::switch_alt(bool enable, bool clear, bool save_restore_cursor)
{
    if (enable == alt_active_) return;
    if (enable) {
        // DECSC (ESC 7) の退避先とは別スロットを使う。共用すると
        // 「ESC 7 → ページャ起動 → ESC 8」で復元位置がすり替わる。
        if (save_restore_cursor) alt_entry_ = cur_;
        if (clear) std::fill(alt_.begin(), alt_.end(), Cell{});
        alt_active_ = true;
        if (save_restore_cursor) cur_ = Cursor{};
    } else {
        alt_active_ = false;
        if (save_restore_cursor) cur_ = alt_entry_;
    }
    scroll_top_    = 0;
    scroll_bottom_ = rows_ - 1;
    cur_.pending_wrap = false;
    clamp_cursor();
    mark_all_dirty();
}

void Terminal::save_cursor()
{
    (alt_active_ ? saved_alt_ : saved_main_) = cur_;
}

void Terminal::restore_cursor()
{
    cur_ = alt_active_ ? saved_alt_ : saved_main_;
    clamp_cursor();
}

void Terminal::reset()
{
    Cell blank{};
    std::fill(main_.begin(), main_.end(), blank);
    std::fill(alt_.begin(), alt_.end(), blank);
    cur_             = Cursor{};
    saved_main_      = Cursor{};
    saved_alt_       = Cursor{};
    alt_active_      = false;
    cursor_visible_  = true;
    autowrap_        = true;
    origin_mode_     = false;
    insert_mode_     = false;
    reverse_video_   = false;
    bracketed_paste_ = false;
    app_cursor_keys_ = false;
    scroll_top_      = 0;
    scroll_bottom_   = rows_ - 1;
    std::fill(tab_stops_.begin(), tab_stops_.end(), false);
    for (int x = 8; x < cols_; x += 8) tab_stops_[x] = true;
    mark_all_dirty();
}

void Terminal::put_char(uint32_t cp)
{
    int w = char_width(cp);

    if (cur_.pending_wrap && autowrap_) {
        carriage_return();
        index();
    }
    // 全角が右端に収まらないなら折り返す。
    if (w == 2 && cur_.x == cols_ - 1) {
        if (autowrap_) {
            erase_cells(cur_.x, cur_.y, 1);
            carriage_return();
            index();
        } else {
            return;
        }
    }

    if (insert_mode_) {
        auto& s = screen();
        auto  row = s.begin() + static_cast<size_t>(cur_.y) * cols_;
        std::copy_backward(row + cur_.x, row + cols_ - w, row + cols_);
        repair_row(cur_.y);  // ずらして割れた全角を先に直す (この後で書き込む)
    }

    split_wide_at(cur_.x, cur_.y);
    Cell& c = at(cur_.x, cur_.y);
    c.ch    = cp;
    c.attr  = cur_.attr;
    c.width = static_cast<uint8_t>(w);
    if (w == 2) {
        split_wide_at(cur_.x + 1, cur_.y);
        Cell& r = at(cur_.x + 1, cur_.y);
        r.ch    = 0;
        r.attr  = cur_.attr;
        r.width = 0;
    }
    // 全角の相棒を空白化した可能性があるので 1 セル余裕を持たせる。
    mark_dirty_range(cur_.y, cur_.x - 1, cur_.x + w);

    if (cur_.x + w >= cols_) {
        cur_.x            = cols_ - 1;
        cur_.pending_wrap = true;
    } else {
        cur_.x += w;
    }
}

void Terminal::exec_c0(uint8_t b)
{
    switch (b) {
        case 0x07: ++bell_count_; break;
        case 0x08:  // BS
            if (cur_.pending_wrap) {
                cur_.pending_wrap = false;
            } else if (cur_.x > 0) {
                --cur_.x;
            }
            break;
        case 0x09: tab_forward(); break;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            cur_.pending_wrap = false;
            index();
            break;
        case 0x0D: carriage_return(); break;
        default: break;  // SO/SI や未対応の C0 は無視
    }
}

int Terminal::param(size_t i, int def) const
{
    if (i >= params_.size() || params_[i] < 0) return def;
    return params_[i];
}

void Terminal::exec_esc(uint8_t b)
{
    switch (b) {
        case '7': save_cursor(); break;
        case '8': restore_cursor(); break;
        case 'D': cur_.pending_wrap = false; index(); break;
        case 'M': cur_.pending_wrap = false; reverse_index(); break;
        case 'E': carriage_return(); index(); break;
        case 'c': reset(); break;
        case 'H': if (cur_.x < cols_) tab_stops_[cur_.x] = true; break;
        default: break;  // = > などのキーパッドモードは無視
    }
}

void Terminal::set_mode(bool enable)
{
    for (size_t i = 0; i < params_.size(); ++i) {
        int p = param(i, 0);
        if (csi_private_ == '?') {
            switch (p) {
                case 1: app_cursor_keys_ = enable; break;
                case 6:
                    origin_mode_ = enable;
                    cur_.x = 0;
                    cur_.y = enable ? scroll_top_ : 0;
                    break;
                case 7: autowrap_ = enable; break;
                case 25: cursor_visible_ = enable; break;
                case 47:
                case 1047:
                    // 旧 vim などの smcup。バッファだけ切り替え、カーソルは保存しない。
                    switch_alt(enable, /*clear=*/enable, /*save_restore_cursor=*/false);
                    break;
                case 1048:
                    if (enable) save_cursor(); else restore_cursor();
                    break;
                case 1049:
                    switch_alt(enable, /*clear=*/enable, /*save_restore_cursor=*/true);
                    break;
                case 2004: bracketed_paste_ = enable; break;
                default: break;  // マウス報告 (1000 系) などは未対応
            }
        } else {
            switch (p) {
                case 4: insert_mode_ = enable; break;
                case 20: break;  // LNM は未対応
                default: break;
            }
        }
    }
}

void Terminal::exec_sgr()
{
    if (params_.empty()) params_.push_back(0);
    for (size_t i = 0; i < params_.size(); ++i) {
        int p = param(i, 0);
        switch (p) {
            case 0: cur_.attr = Attr{}; break;
            case 1: cur_.attr.flags |= kBold; break;
            case 2: cur_.attr.flags |= kDim; break;
            case 3: cur_.attr.flags |= kItalic; break;
            case 4: cur_.attr.flags |= kUnderline; break;
            case 5: cur_.attr.flags |= kBlink; break;
            case 7: cur_.attr.flags |= kReverse; break;
            case 8: cur_.attr.flags |= kInvisible; break;
            case 9: cur_.attr.flags |= kStrike; break;
            case 21:
            case 22: cur_.attr.flags &= ~(kBold | kDim); break;
            case 23: cur_.attr.flags &= ~kItalic; break;
            case 24: cur_.attr.flags &= ~kUnderline; break;
            case 25: cur_.attr.flags &= ~kBlink; break;
            case 27: cur_.attr.flags &= ~kReverse; break;
            case 28: cur_.attr.flags &= ~kInvisible; break;
            case 29: cur_.attr.flags &= ~kStrike; break;
            case 39: cur_.attr.fg = kDefaultFg; break;
            case 49: cur_.attr.bg = kDefaultBg; break;
            case 38:
            case 48: {
                bool fg  = (p == 38);
                int  kind = param(i + 1, 0);
                if (kind == 5) {
                    uint16_t idx = static_cast<uint16_t>(std::clamp(param(i + 2, 0), 0, 255));
                    if (fg) cur_.attr.fg = idx; else cur_.attr.bg = idx;
                    i += 2;
                } else if (kind == 2) {
                    // コロン形式の標準は 38:2:<colorspace>:r:g:b で colorspace 欄が空。
                    // 空欄 (-1) を r として食うと色が壊れるので 1 つ読み飛ばす。
                    size_t j = i + 2;
                    if (j < params_.size() && params_[j] < 0) ++j;
                    uint16_t idx = rgb_to_256(std::clamp(param(j, 0), 0, 255),
                                              std::clamp(param(j + 1, 0), 0, 255),
                                              std::clamp(param(j + 2, 0), 0, 255));
                    if (fg) cur_.attr.fg = idx; else cur_.attr.bg = idx;
                    i = j + 2;
                }
                break;
            }
            default:
                if (p >= 30 && p <= 37) {
                    cur_.attr.fg = static_cast<uint16_t>(p - 30);
                } else if (p >= 40 && p <= 47) {
                    cur_.attr.bg = static_cast<uint16_t>(p - 40);
                } else if (p >= 90 && p <= 97) {
                    cur_.attr.fg = static_cast<uint16_t>(p - 90 + 8);
                } else if (p >= 100 && p <= 107) {
                    cur_.attr.bg = static_cast<uint16_t>(p - 100 + 8);
                }
                break;
        }
    }
}

void Terminal::exec_csi(uint8_t f)
{
    // 原点モードではスクロール領域が座標の基準になる。
    const int y_origin = origin_mode_ ? scroll_top_ : 0;
    // カーソルがスクロール領域内にいるなら、上下移動はマージンで止まる (VT100 仕様)。
    // 領域外にいるときだけ画面端まで動ける。
    const int move_top    = (cur_.y >= scroll_top_) ? scroll_top_ : 0;
    const int move_bottom = (cur_.y <= scroll_bottom_) ? scroll_bottom_ : rows_ - 1;
    // 絶対位置指定 (CUP/VPA) のクランプ先。原点モードなら領域の下端で止める。
    const int abs_limit = origin_mode_ ? scroll_bottom_ : rows_ - 1;

    switch (f) {
        case 'A': cur_.y = std::max(move_top, cur_.y - param(0, 1)); cur_.pending_wrap = false; break;
        case 'B': cur_.y = std::min(move_bottom, cur_.y + param(0, 1)); cur_.pending_wrap = false; break;
        case 'C': cur_.x = std::min(cols_ - 1, cur_.x + param(0, 1)); cur_.pending_wrap = false; break;
        case 'D': cur_.x = std::max(0, cur_.x - param(0, 1)); cur_.pending_wrap = false; break;
        case 'E': cur_.y = std::min(move_bottom, cur_.y + param(0, 1)); carriage_return(); break;
        case 'F': cur_.y = std::max(move_top, cur_.y - param(0, 1)); carriage_return(); break;
        case 'G':
        case '`': cur_.x = std::clamp(param(0, 1) - 1, 0, cols_ - 1); cur_.pending_wrap = false; break;
        case 'd': cur_.y = std::clamp(y_origin + param(0, 1) - 1, 0, abs_limit); cur_.pending_wrap = false; break;
        case 'H':
        case 'f':
            cur_.y            = std::clamp(y_origin + param(0, 1) - 1, 0, abs_limit);
            cur_.x            = std::clamp(param(1, 1) - 1, 0, cols_ - 1);
            cur_.pending_wrap = false;
            break;
        case 'J': {
            int mode = param(0, 0);
            int here = cur_.y * cols_ + cur_.x;
            if (mode == 0) {
                clear_region(here, rows_ * cols_);
            } else if (mode == 1) {
                clear_region(0, here + 1);
            } else if (mode == 2 || mode == 3) {
                clear_region(0, rows_ * cols_);
            }
            break;
        }
        case 'K': {
            int mode = param(0, 0);
            if (mode == 0) {
                erase_cells(cur_.x, cur_.y, cols_ - cur_.x);
            } else if (mode == 1) {
                erase_cells(0, cur_.y, cur_.x + 1);
            } else {
                erase_cells(0, cur_.y, cols_);
            }
            break;
        }
        case 'L':
            if (cur_.y >= scroll_top_ && cur_.y <= scroll_bottom_) {
                scroll_down(cur_.y, scroll_bottom_, param(0, 1));
            }
            break;
        case 'M':
            if (cur_.y >= scroll_top_ && cur_.y <= scroll_bottom_) {
                scroll_up(cur_.y, scroll_bottom_, param(0, 1));
            }
            break;
        case '@': {  // ICH
            int n = std::clamp(param(0, 1), 1, cols_ - cur_.x);
            auto& s = screen();
            auto row = s.begin() + static_cast<size_t>(cur_.y) * cols_;
            std::copy_backward(row + cur_.x, row + cols_ - n, row + cols_);
            // ずらした後の空き領域は素の blank で埋める。ここで split_wide_at を通すと
            // 「ずらす前の残骸」を見て正しいセルを消してしまう。
            fill_blank(cur_.x, cur_.y, n);
            repair_row(cur_.y);
            break;
        }
        case 'P': {  // DCH
            int n = std::clamp(param(0, 1), 1, cols_ - cur_.x);
            auto& s = screen();
            auto row = s.begin() + static_cast<size_t>(cur_.y) * cols_;
            std::copy(row + cur_.x + n, row + cols_, row + cur_.x);
            fill_blank(cols_ - n, cur_.y, n);
            repair_row(cur_.y);
            break;
        }
        case 'X': erase_cells(cur_.x, cur_.y, std::max(1, param(0, 1))); break;
        case 'S': scroll_up(scroll_top_, scroll_bottom_, param(0, 1)); break;
        case 'T': scroll_down(scroll_top_, scroll_bottom_, param(0, 1)); break;
        case 'm': exec_sgr(); break;
        case 'r': {
            int top    = std::clamp(param(0, 1) - 1, 0, rows_ - 1);
            int bottom = std::clamp(param(1, rows_) - 1, 0, rows_ - 1);
            if (top < bottom) {
                scroll_top_    = top;
                scroll_bottom_ = bottom;
            } else {
                scroll_top_    = 0;
                scroll_bottom_ = rows_ - 1;
            }
            cur_.x = 0;
            cur_.y = origin_mode_ ? scroll_top_ : 0;
            break;
        }
        case 'h': set_mode(true); break;
        case 'l': set_mode(false); break;
        case 'n':
            if (reply_ && param(0, 0) == 6) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "\033[%d;%dR",
                              cur_.y - (origin_mode_ ? scroll_top_ : 0) + 1, cur_.x + 1);
                reply_(buf);
            } else if (reply_ && param(0, 0) == 5) {
                reply_("\033[0n");
            }
            break;
        case 'c':
            // VT102 相当を主張する。
            if (reply_) reply_("\033[?6c");
            break;
        case 's': save_cursor(); break;
        case 'u': restore_cursor(); break;
        case 'g':
            if (param(0, 0) == 3) {
                std::fill(tab_stops_.begin(), tab_stops_.end(), false);
            } else if (cur_.x < cols_) {
                tab_stops_[cur_.x] = false;
            }
            break;
        default: break;  // 未対応のシーケンスは捨てる (画面を壊さないことを優先)
    }
}

void Terminal::clear_region(int from_index, int to_index)
{
    auto& s = screen();
    Cell blank{};
    blank.attr = cur_.attr;
    blank.attr.flags = 0;
    from_index = std::clamp(from_index, 0, static_cast<int>(s.size()));
    to_index   = std::clamp(to_index, 0, static_cast<int>(s.size()));
    if (from_index >= to_index) return;
    std::fill(s.begin() + from_index, s.begin() + to_index, blank);
    for (int y = from_index / cols_; y <= (to_index - 1) / cols_; ++y) mark_dirty(y);
    // 範囲の端で全角が割れている可能性があるのは最初と最後の行だけ。
    repair_row(from_index / cols_);
    repair_row((to_index - 1) / cols_);
}

void Terminal::write(const std::string& s)
{
    write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void Terminal::write(const uint8_t* data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];

        switch (state_) {
            case State::kGround:
                if (utf8_remaining_ > 0) {
                    if ((b & 0xC0) == 0x80) {
                        utf8_cp_ = (utf8_cp_ << 6) | (b & 0x3F);
                        if (--utf8_remaining_ == 0) {
                            // オーバーロング・サロゲート・範囲外は置換文字にする。
                            uint32_t cp = utf8_cp_;
                            if (cp < utf8_min_ || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                                cp = 0xFFFD;
                            }
                            put_char(cp);
                        }
                        continue;
                    }
                    // 不正な継続バイト: 置換文字を置いて、このバイトを最初から解釈し直す。
                    utf8_remaining_ = 0;
                    put_char(0xFFFD);
                }
                if (b == 0x1B) {
                    state_ = State::kEsc;
                } else if (b < 0x20) {
                    exec_c0(b);
                } else if (b == 0x7F) {
                    // DEL は無視
                } else if (b < 0x80) {
                    put_char(b);
                } else if ((b & 0xE0) == 0xC0) {
                    utf8_cp_        = b & 0x1F;
                    utf8_remaining_ = 1;
                    utf8_min_       = 0x80;
                } else if ((b & 0xF0) == 0xE0) {
                    utf8_cp_        = b & 0x0F;
                    utf8_remaining_ = 2;
                    utf8_min_       = 0x800;
                } else if ((b & 0xF8) == 0xF0) {
                    utf8_cp_        = b & 0x07;
                    utf8_remaining_ = 3;
                    utf8_min_       = 0x10000;
                } else {
                    put_char(0xFFFD);
                }
                break;

            case State::kEsc:
                if (b == '[') {
                    params_.clear();
                    param_seen_     = false;
                    param_overflow_ = false;
                    csi_private_ = 0;
                    csi_inter_   = 0;
                    state_       = State::kCsiParam;
                } else if (b == ']') {
                    osc_.clear();
                    state_ = State::kOsc;
                } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                    state_ = State::kDcs;
                } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#' || b == ' ') {
                    state_ = State::kEscIntermediate;
                } else if (b == 0x1B) {
                    // ESC ESC: 後ろの ESC から解釈し直す
                } else {
                    exec_esc(b);
                    state_ = State::kGround;
                }
                break;

            case State::kEscIntermediate:
                state_ = State::kGround;  // 文字集合指定などは読み捨てる
                break;

            case State::kCsiParam:
                if (b >= '0' && b <= '9') {
                    if (!param_seen_) {
                        param_seen_ = true;
                        if (params_.size() < kMaxParams) {
                            params_.push_back(0);
                        } else {
                            param_overflow_ = true;  // 以降このシーケンスの数字は捨てる
                        }
                    }
                    if (!param_overflow_ && params_.back() < 100000) {
                        params_.back() = params_.back() * 10 + (b - '0');
                    }
                } else if (b == ';' || b == ':') {
                    // ':' は SGR のサブパラメータ区切りだが、ここでは ';' と同じ扱いにする。
                    // 数字が来ないまま区切られたら -1 を積む。param() がそれを既定値に読み替える。
                    if (!param_seen_ && params_.size() < kMaxParams) params_.push_back(-1);
                    param_seen_ = false;
                } else if (b >= '<' && b <= '?') {
                    csi_private_ = b;
                } else if (b >= ' ' && b <= '/') {
                    csi_inter_ = b;
                } else if (b >= '@' && b <= '~') {
                    exec_csi(b);
                    state_ = State::kGround;
                } else if (b == 0x1B) {
                    state_ = State::kEsc;
                } else if (b < 0x20) {
                    exec_c0(b);  // シーケンス途中の C0 は即実行 (xterm 互換)
                } else {
                    state_ = State::kGround;
                }
                break;

            case State::kOsc:
                if (b == 0x07) {
                    // OSC 0/2 はウィンドウタイトル
                    if (osc_.size() >= 2 && (osc_[0] == '0' || osc_[0] == '2') && osc_[1] == ';') {
                        title_ = osc_.substr(2);
                    }
                    state_ = State::kGround;
                } else if (b == 0x1B) {
                    state_ = State::kOscEsc;
                } else if (osc_.size() < 512) {
                    osc_ += static_cast<char>(b);
                }
                break;

            case State::kOscEsc:
                if (osc_.size() >= 2 && (osc_[0] == '0' || osc_[0] == '2') && osc_[1] == ';') {
                    title_ = osc_.substr(2);
                }
                state_ = State::kGround;
                if (b != '\\') --i;  // ST 以外なら読み直す
                break;

            case State::kDcs:
                if (b == 0x1B) state_ = State::kDcsEsc;
                break;

            case State::kDcsEsc:
                state_ = (b == '\\') ? State::kGround : State::kDcs;
                break;
        }
    }
}


void Terminal::set_scrollback(Cell* buffer, int max_lines)
{
    sb_buf_      = (max_lines > 0) ? buffer : nullptr;
    sb_max_      = (buffer && max_lines > 0) ? max_lines : 0;
    sb_count_    = 0;
    sb_head_     = 0;
    view_offset_ = 0;
}

void Terminal::push_scrollback(const Cell* row)
{
    if (!sb_buf_ || sb_max_ <= 0) return;
    std::copy(row, row + cols_, sb_buf_ + static_cast<size_t>(sb_head_) * cols_);
    sb_head_ = (sb_head_ + 1) % sb_max_;
    if (sb_count_ < sb_max_) ++sb_count_;
    // 履歴を見ている最中に新しい行が来たら、見ている位置を保つ (画面が勝手に動かない)。
    if (view_offset_ > 0 && view_offset_ < sb_count_) ++view_offset_;
}

const Cell* Terminal::sb_line(int lines_back) const
{
    // lines_back = 1 が最新の履歴行。
    if (!sb_buf_ || lines_back <= 0 || lines_back > sb_count_) return nullptr;
    const int idx = (sb_head_ - lines_back + sb_max_ * 2) % sb_max_;
    return sb_buf_ + static_cast<size_t>(idx) * cols_;
}

int Terminal::scroll_view(int delta)
{
    const int before = view_offset_;
    // 代替画面ではスクロールバックを見せない (vim の画面が壊れて見えるだけ)。
    const int limit = alt_active_ ? 0 : sb_count_;
    view_offset_    = std::clamp(view_offset_ + delta, 0, limit);
    if (view_offset_ != before) mark_all_dirty();
    return view_offset_ - before;
}

const Cell& Terminal::view_cell(int x, int y) const
{
    static const Cell blank{};
    if (x < 0 || x >= cols_ || y < 0 || y >= rows_) return blank;
    if (y < view_offset_) {
        // 画面上部には履歴の古い方から順に並べる。
        const Cell* line = sb_line(view_offset_ - y);
        return line ? line[x] : blank;
    }
    return screen()[static_cast<size_t>(y - view_offset_) * cols_ + x];
}

}  // namespace vt
