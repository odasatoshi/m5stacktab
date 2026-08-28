// 画面キーボードの ASCII 面のホストテスト。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -I main -I components/ime -o /tmp/test_ascii
//       components/ime/test_ascii.cpp components/ime/ascii.cpp components/ime/flick.cpp
//
// **一番効くのは「表のキー名が端末に届くか」**。名前を打ち間違えても画面は
// 正しく出るので、実機では「そのキーだけ黙って効かない」という形でしか出ない。
// kbd_key_to_bytes を通して空でないことをここで見る。
#include "ascii.hpp"
#include "kbd_keys.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_checks = 0;

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, what);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

void expect(const std::string& got, const std::string& want, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, line, got.c_str(),
                     want.c_str());
        std::abort();
    }
}
#define EXPECT(got, want) expect((got), (want), __LINE__)

ime::FlickLayout layout()
{
    ime::FlickLayout l;
    l.x      = 4;
    l.y      = 400;
    l.width  = ime::AsciiKeyboard::kCols * 106;
    l.height = ime::AsciiKeyboard::kRows * 70;
    l.cols   = ime::AsciiKeyboard::kCols;
    l.rows   = ime::AsciiKeyboard::kRows;
    return l;
}

// 行の span の合計はちょうど 12 列。足りないと hit() が末尾で false を返し、
// 多いと右端のキーが押せなくなる。
void test_table()
{
    for (int row = 0; row < ime::AsciiKeyboard::kRows; ++row) {
        int span = 0;
        for (int i = 0;; ++i) {
            const ime::AsciiKey* k = ime::AsciiKeyboard::key_at(row, i);
            if (!k) break;
            CHECK(ime::AsciiKeyboard::col_of(row, i) == span);
            span += k->span;
        }
        CHECK(span == ime::AsciiKeyboard::kCols);
    }
}

// 表のキー名は全部 kbd_key_to_bytes が解釈できること（修飾キーを除く）。
void test_names_reach_terminal()
{
    for (int row = 0; row < ime::AsciiKeyboard::kRows; ++row) {
        for (int i = 0;; ++i) {
            const ime::AsciiKey* k = ime::AsciiKeyboard::key_at(row, i);
            if (!k) break;
            if (k->mod != ime::AsciiMod::kNone) {
                CHECK(k->name[0] == '\0');  // 修飾キーは送らない
                continue;
            }
            CHECK(!kbd_key_to_bytes(k->name, 0, false).empty());
            if (k->shift) CHECK(!kbd_key_to_bytes(k->shift, 0, false).empty());
        }
    }
}

// PAD 面も同じ不変条件（名前が端末に届く）を満たすこと。
void test_pad_table()
{
    for (int row = 0; row < ime::kPadRows; ++row) {
        for (int col = 0; col < ime::kPadCols; ++col) {
            const ime::AsciiKey* k = ime::pad_key(row, col);
            CHECK(k != nullptr);
            CHECK(k->span == 1);
            if (k->mod != ime::AsciiMod::kNone) {
                CHECK(k->name[0] == '\0');
                continue;
            }
            CHECK(!kbd_key_to_bytes(k->name, k->send_mod, false).empty());
            // **修飾を付けるキーは、付けない場合と違うバイトになること。**
            // 同じなら、そのキーは修飾を付けているつもりで素のまま送っている。
            if (k->send_mod) {
                CHECK(kbd_key_to_bytes(k->name, k->send_mod, false) !=
                      kbd_key_to_bytes(k->name, 0, false));
            }
        }
    }
    CHECK(ime::pad_key(-1, 0) == nullptr);
    CHECK(ime::pad_key(0, ime::kPadCols) == nullptr);
    CHECK(ime::pad_key(ime::kPadRows, 0) == nullptr);
    // **表を 2 か所に持っている。** ずれると PAD の `^C` が黙って `c` になる。
    CHECK(ime::kCtrlBit == kKbdModCtrl);
}

// 各キーの中心を押したらそのキーが返ること。span 付きの space も含む。
void test_hit()
{
    ime::AsciiKeyboard kb;
    kb.set_layout(layout());
    const ime::FlickLayout& l = kb.layout();

    for (int row = 0; row < ime::AsciiKeyboard::kRows; ++row) {
        for (int i = 0;; ++i) {
            const ime::AsciiKey* k = ime::AsciiKeyboard::key_at(row, i);
            if (!k) break;
            const int col = ime::AsciiKeyboard::col_of(row, i);
            const int px  = l.x + col * l.key_w() + k->span * l.key_w() / 2;
            const int py  = l.y + row * l.key_h() + l.key_h() / 2;
            int       hr = -1, hi = -1;
            CHECK(kb.hit(px, py, &hr, &hi));
            CHECK(hr == row && hi == i);
        }
    }

    int r = -1, i = -1;
    CHECK(!kb.hit(l.x - 1, l.y + 1, &r, &i));
    CHECK(!kb.hit(l.x + 1, l.y - 1, &r, &i));
    CHECK(!kb.hit(l.x + 1, l.y + l.height + 1, &r, &i));
    // 右下の隅（端数の切り上がりで行がはみ出さないこと）
    CHECK(kb.hit(l.x + l.width - 1, l.y + l.height - 1, &r, &i));
    CHECK(r == ime::AsciiKeyboard::kRows - 1);
}

// ラッチした修飾が実際に効くこと（面ではなく送るバイトで見る）。
void test_modifiers()
{
    EXPECT(kbd_key_to_bytes("c", kKbdModCtrl, false), std::string(1, '\x03'));
    EXPECT(kbd_key_to_bytes("C", kKbdModCtrl, false), std::string(1, '\x03'));
    EXPECT(kbd_key_to_bytes("A", 0, false), "A");
    EXPECT(kbd_key_to_bytes(" ", 0, false), " ");
    EXPECT(kbd_key_to_bytes("up", 0, false), "\033[A");
    EXPECT(kbd_key_to_bytes("up", 0, true), "\033OA");
    EXPECT(kbd_key_to_bytes("backspace", 0, false), "\177");
}

}  // namespace

int main()
{
    test_table();
    test_names_reach_terminal();
    test_pad_table();
    test_hit();
    test_modifiers();
    std::printf("ok (%d checks)\n", g_checks);
    return 0;
}
