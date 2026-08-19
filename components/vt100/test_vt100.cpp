// VT100 コアのホストテスト。実機は要らない。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined \
//       -o /tmp/test_vt100 components/vt100/test_vt100.cpp components/vt100/vt100.cpp && /tmp/test_vt100
//
// AddressSanitizer は macOS のこの環境で起動時に固まるため使っていない。Linux で回すなら
// -fsanitize=address,undefined を付けるとよい。
//
// フレームワークは使わない。落ちたら assert が場所を教える。
#include "vt100.hpp"

#include <cassert>
#include <cstdio>
#include <string>

using vt::Terminal;

namespace {

int g_checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::abort();                                                \
        }                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        ++g_checks;                                                             \
        auto va = (a);                                                          \
        auto vb = (b);                                                          \
        if (!(va == vb)) {                                                      \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

void check_eq_str(const std::string& got, const std::string& want, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, line,
                     got.c_str(), want.c_str());
        std::abort();
    }
}
#define CHECK_STR(got, want) check_eq_str((got), (want), __LINE__)

// セルのコードポイントと文字リテラルの比較 (符号違いの警告を避ける)
#define CHECK_CH(ch, lit) CHECK_EQ((ch), static_cast<uint32_t>(lit))

void test_plain_text()
{
    Terminal t(20, 5);
    t.write("hello");
    CHECK_STR(t.row_text(0), "hello");
    CHECK_EQ(t.cursor_x(), 5);
    CHECK_EQ(t.cursor_y(), 0);

    t.write("\r\nsecond");
    CHECK_STR(t.row_text(1), "second");
    CHECK_EQ(t.cursor_y(), 1);
}

void test_autowrap()
{
    Terminal t(5, 3);
    t.write("abcde");
    // 右端に置いた直後はまだ折り返していない (pending wrap)。
    CHECK_EQ(t.cursor_x(), 4);
    CHECK_EQ(t.cursor_y(), 0);
    t.write("f");
    CHECK_EQ(t.cursor_y(), 1);
    CHECK_EQ(t.cursor_x(), 1);
    CHECK_STR(t.row_text(0), "abcde");
    CHECK_STR(t.row_text(1), "f");

    // DECAWM off なら右端に留まって上書きし続ける。
    Terminal t2(5, 3);
    t2.write("\033[?7l");
    t2.write("abcdefg");
    CHECK_STR(t2.row_text(0), "abcdg");
    CHECK_EQ(t2.cursor_y(), 0);
}

void test_cursor_and_erase()
{
    Terminal t(10, 4);
    t.write("0123456789\r\nabcdefghij");
    t.write("\033[1;4H");  // 1 行 4 列 (1-origin)
    CHECK_EQ(t.cursor_x(), 3);
    CHECK_EQ(t.cursor_y(), 0);
    t.write("\033[K");  // カーソルから行末まで消去
    CHECK_STR(t.row_text(0), "012");
    CHECK_STR(t.row_text(1), "abcdefghij");

    t.write("\033[2;3H\033[1K");  // 行頭からカーソルまで消去
    CHECK_STR(t.row_text(1), "   defghij");

    t.write("\033[2J");
    CHECK_STR(t.row_text(0), "");
    CHECK_STR(t.row_text(1), "");

    // ED 0 は画面末尾まで、ED 1 は先頭からカーソルまで。
    Terminal t2(4, 3);
    t2.write("aaaa\r\nbbbb\r\ncccc");
    t2.write("\033[2;3H\033[J");
    CHECK_STR(t2.row_text(0), "aaaa");
    CHECK_STR(t2.row_text(1), "bb");
    CHECK_STR(t2.row_text(2), "");
}

void test_sgr()
{
    Terminal t(10, 2);
    t.write("\033[1;31mR\033[0mN");
    CHECK(t.cell(0, 0).attr.flags & vt::kBold);
    CHECK_EQ(t.cell(0, 0).attr.fg, 1);
    CHECK_EQ(t.cell(1, 0).attr.flags, 0);
    CHECK_EQ(t.cell(1, 0).attr.fg, vt::kDefaultFg);

    // 256 色
    t.write("\033[38;5;208mX");
    CHECK_EQ(t.cell(2, 0).attr.fg, 208);
    // 24bit 色は 256 色に丸める (捨てない)
    t.write("\033[38;2;255;0;0mY");
    CHECK_EQ(t.cell(3, 0).attr.fg, 196);
    // 明るい色
    t.write("\033[92mZ");
    CHECK_EQ(t.cell(4, 0).attr.fg, 10);
    // 背景色つきで消去すると背景が残る
    t.write("\033[41m\033[2K");
    CHECK_EQ(t.cell(0, 0).attr.bg, 1);
    CHECK_EQ(t.cell(0, 0).attr.flags, 0);
}

void test_scroll_region()
{
    Terminal t(4, 5);
    t.write("1\r\n2\r\n3\r\n4\r\n5");
    // 2〜4 行目をスクロール領域にして最下行で改行
    t.write("\033[2;4r");
    CHECK_EQ(t.cursor_y(), 0);  // DECSTBM はカーソルを原点に戻す
    t.write("\033[4;1H\n");
    CHECK_STR(t.row_text(0), "1");
    CHECK_STR(t.row_text(1), "3");
    CHECK_STR(t.row_text(2), "4");
    CHECK_STR(t.row_text(3), "");
    CHECK_STR(t.row_text(4), "5");

    // 領域外の行は影響を受けない (RI)
    t.write("\033[2;1H\033M");
    CHECK_STR(t.row_text(0), "1");
    CHECK_STR(t.row_text(1), "");
    CHECK_STR(t.row_text(2), "3");
}

void test_insert_delete()
{
    Terminal t(6, 2);
    t.write("abcdef\033[1;1H\033[2@");  // 先頭に 2 セル挿入
    CHECK_STR(t.row_text(0), "  abcd");
    t.write("\033[1;1H\033[2P");  // 2 セル削除
    CHECK_STR(t.row_text(0), "abcd");

    // IL / DL
    Terminal t2(3, 4);
    t2.write("a\r\nb\r\nc\r\nd");
    t2.write("\033[2;1H\033[L");
    CHECK_STR(t2.row_text(1), "");
    CHECK_STR(t2.row_text(2), "b");
    t2.write("\033[2;1H\033[M");
    CHECK_STR(t2.row_text(1), "b");
    CHECK_STR(t2.row_text(2), "c");
}

void test_alt_screen()
{
    Terminal t(6, 2);
    t.write("main\033[1;1H");
    t.write("\033[?1049h");  // vim などが最初に出すやつ
    CHECK(t.alt_screen());
    CHECK_STR(t.row_text(0), "");
    t.write("alt");
    CHECK_STR(t.row_text(0), "alt");
    t.write("\033[?1049l");
    CHECK(!t.alt_screen());
    CHECK_STR(t.row_text(0), "main");
}

void test_utf8_and_wide()
{
    Terminal t(10, 2);
    t.write("あ");
    CHECK_EQ(t.cell(0, 0).ch, 0x3042u);
    CHECK_EQ(t.cell(0, 0).width, 2);
    CHECK_EQ(t.cell(1, 0).width, 0);  // 右半分
    CHECK_EQ(t.cursor_x(), 2);
    CHECK_STR(t.row_text(0), "あ");

    // write() の境界がマルチバイトの途中で切れても壊れない
    Terminal t2(10, 2);
    const std::string s = "日本語";
    for (size_t i = 0; i < s.size(); ++i) {
        t2.write(s.substr(i, 1));
    }
    CHECK_STR(t2.row_text(0), "日本語");
    CHECK_EQ(t2.cursor_x(), 6);

    // 全角の右半分を上書きしたら左半分は空白化される
    Terminal t3(10, 2);
    t3.write("あい");
    t3.write("\033[1;2HX");
    CHECK_CH(t3.cell(0, 0).ch, ' ');
    CHECK_CH(t3.cell(1, 0).ch, 'X');

    // 全角が右端に収まらないときは折り返す (残り 1 列では入らない)
    Terminal t4(3, 2);
    t4.write("abあ");
    CHECK_STR(t4.row_text(0), "ab");
    CHECK_STR(t4.row_text(1), "あ");
    // 残り 2 列なら折り返さずに収まる
    Terminal t4b(3, 2);
    t4b.write("aあ");
    CHECK_STR(t4b.row_text(0), "aあ");
    CHECK_EQ(t4b.cursor_x(), 2);

    // 不正なバイト列は置換文字になり、後続は正常に処理される
    Terminal t5(10, 2);
    t5.write("\xC3\x28" "ok");  // 不正な 2 バイト
    CHECK_EQ(t5.cell(0, 0).ch, 0xFFFDu);
    CHECK_STR(t5.row_text(0).substr(3), "(ok");
    CHECK_EQ(vt::char_width(0x3042), 2);
    CHECK_EQ(vt::char_width('a'), 1);
    CHECK_EQ(vt::char_width(0xFF21), 2);  // 全角 A
}

void test_dirty_tracking()
{
    Terminal t(8, 4);
    t.clear_dirty();
    CHECK(!t.any_dirty());
    t.write("\033[3;1Hx");
    CHECK(t.is_dirty(2));
    CHECK(!t.is_dirty(0));
    t.clear_dirty();
    t.write("\033[2J");
    CHECK(t.is_dirty(0));
    CHECK(t.is_dirty(3));
}

void test_replies_and_title()
{
    Terminal t(10, 3);
    std::string reply;
    t.set_reply([&](const std::string& s) { reply += s; });
    t.write("\033[2;5H\033[6n");  // CPR
    CHECK_STR(reply, "\033[2;5R");
    reply.clear();
    t.write("\033[c");  // DA
    CHECK(!reply.empty());

    t.write("\033]0;my title\007");
    CHECK_STR(t.title(), "my title");
    // ST (ESC \) 終端も受ける
    t.write("\033]2;second\033\\");
    CHECK_STR(t.title(), "second");
    // OSC の直後の文字が失われない
    t.write("\033]0;t\033\\abc");
    CHECK_STR(t.row_text(1), "    abc");
}

void test_tabs_and_bs()
{
    Terminal t(20, 2);
    t.write("a\tb");
    CHECK_EQ(t.cursor_x(), 9);
    CHECK_CH(t.cell(8, 0).ch, 'b');
    t.write("\b\bZ");
    CHECK_CH(t.cell(7, 0).ch, 'Z');
}

void test_resize()
{
    Terminal t(10, 3);
    t.write("row0\r\nrow1\r\nrow2");
    t.resize(6, 2);
    // 下端を保つので row1/row2 が残る
    CHECK_STR(t.row_text(0), "row1");
    CHECK_STR(t.row_text(1), "row2");
    CHECK_EQ(t.cols(), 6);
    CHECK_EQ(t.rows(), 2);
    t.write("\033[1;1Hx");
    CHECK_CH(t.cell(0, 0).ch, 'x');
}

// 実アプリが出す並びをまとめて流し、画面が壊れないことを見る。
void test_real_sequences()
{
    // vim の起動シーケンス (代替画面 → クリア → ステータス行 → 終了)
    Terminal t(20, 5);
    t.write("\033[?1049h\033[?1h\033=\033[?25l\033[1;1H\033[2J");
    CHECK(t.alt_screen());
    CHECK(!t.cursor_visible());
    CHECK(t.app_cursor_keys());
    t.write("\033[5;1H\033[7m-- INSERT --\033[m");
    CHECK_STR(t.row_text(4), "-- INSERT --");
    CHECK(t.cell(0, 4).attr.flags & vt::kReverse);
    t.write("\033[?25h\033[?1049l\033[?1l\033>");
    CHECK(!t.alt_screen());
    CHECK(t.cursor_visible());

    // htop 風: スクロール領域 + 24bit 色 + カーソル移動の連打
    Terminal t2(30, 6);
    t2.write("\033[1;6r\033[H");
    for (int i = 0; i < 20; ++i) {
        t2.write("\033[38;2;0;255;0mline\r\n");
    }
    // 最後の "\r\n" でスクロールしているので、最下行は空でその上が最後の行。
    CHECK_EQ(t2.cursor_y(), 5);
    CHECK_STR(t2.row_text(4), "line");
    CHECK_STR(t2.row_text(5), "");

    // bracketed paste と DECSC/DECRC
    Terminal t3(10, 3);
    t3.write("\033[?2004h");
    CHECK(t3.bracketed_paste());
    t3.write("\0337\033[3;5Hx\0338y");
    CHECK_CH(t3.cell(4, 2).ch, 'x');
    CHECK_CH(t3.cell(0, 0).ch, 'y');

    // DCS (ソフトフォントなど) は ST まで読み捨てて画面を汚さない
    Terminal t4(10, 2);
    t4.write("A\033P1;2|junk\033\\B");
    CHECK_STR(t4.row_text(0), "AB");
}

}  // namespace

int main()
{
    test_plain_text();
    test_autowrap();
    test_cursor_and_erase();
    test_sgr();
    test_scroll_region();
    test_insert_delete();
    test_alt_screen();
    test_utf8_and_wide();
    test_dirty_tracking();
    test_replies_and_title();
    test_tabs_and_bs();
    test_resize();
    test_real_sequences();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
