// tap_gesture.hpp のホストテスト。**誤爆しないこと**が本体なので、
// 成立するケースより成立しないケースを厚く見る。
#include <cstdio>

#include "tap_gesture.hpp"

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const char* expr, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// キーボードを隠しているとき（帯が無い）の上端。
constexpr int kNoKeyboard = 720;
// 表示しているとき: 720 - 336。
constexpr int kKeyboardTop = 384;

// 1 回タップする。offset は動かさない。
bool tap(TapState* s, int x, int y, int64_t at, int keyboard_top = kNoKeyboard, int offset = 0)
{
    tap_down(s, x, y, at, offset);
    return tap_up(s, x, y, at + 30000, offset, keyboard_top);  // 30ms 触れた
}

void test_double_tap()
{
    TapState s;
    CHECK(!tap(&s, 400, 300, 1000000));            // 1 回目では成立しない
    CHECK(tap(&s, 400, 300, 1100000));             // 100ms 後の 2 回目で成立
    CHECK(!tap(&s, 400, 300, 1200000));            // 3 回目は 2 回目扱いしない
    CHECK(tap(&s, 400, 300, 1300000));             // 4 回目でまた成立
}

void test_too_slow()
{
    TapState s;
    CHECK(!tap(&s, 400, 300, 1000000));
    // ちょうど 400ms は成立しない（README の「400ms 以上あくと無効」と揃える）
    CHECK(!tap(&s, 400, 300, 1000000 + kDoubleTapUs));
    // **遅すぎた 2 回目は「新しい 1 回目」になる。** ここを捨てると、
    // 少し間があいただけで 3 回叩かないと切り替わらなくなる。
    CHECK(tap(&s, 400, 300, 1000000 + kDoubleTapUs + 50000));
}

void test_too_far()
{
    TapState s;
    CHECK(!tap(&s, 400, 300, 1000000));
    // ちょうど slop は成立しない
    CHECK(!tap(&s, 400 + kDoubleTapSlop, 300, 1100000));
    TapState s2;
    CHECK(!tap(&s2, 400, 300, 1000000));
    CHECK(!tap(&s2, 400, 300 + kDoubleTapSlop, 1100000));
    // 1px 内側なら成立する
    TapState s3;
    CHECK(!tap(&s3, 400, 300, 1000000));
    CHECK(tap(&s3, 400 + kDoubleTapSlop - 1, 300, 1100000));
}

// **長押しはタップではない。**
void test_long_press()
{
    TapState s;
    tap_down(&s, 400, 300, 1000000, 0);
    CHECK(!tap_up(&s, 400, 300, 1000000 + kTapMaxUs, 0, kNoKeyboard));
    CHECK(!tap(&s, 400, 300, 1500000));  // 直前が無効なので 1 回目に戻っている
}

// **スクロールバックのスワイプに食われない（指摘 1）。**
// cell_h = 24px なので、48px 引けば 2 行動く。距離だけで見ていると
// slop 60px の内側に収まってしまい、2 回引くとキーボードが消えていた。
void test_scroll_is_not_a_tap()
{
    TapState s;
    // 48px 引いて 2 行スクロールした（offset が変わる）
    tap_down(&s, 400, 300, 1000000, 0);
    CHECK(!tap_up(&s, 400, 348, 1030000, 2, kNoKeyboard));
    tap_down(&s, 400, 300, 1100000, 2);
    CHECK(!tap_up(&s, 400, 348, 1130000, 4, kNoKeyboard));  // ここで消えてはいけない

    // 距離が閾値の内側でも、offset が動いていればタップにしない
    TapState s2;
    tap_down(&s2, 400, 300, 1000000, 0);
    CHECK(!tap_up(&s2, 400, 310, 1030000, 1, kNoKeyboard));
    tap_down(&s2, 400, 300, 1100000, 1);
    CHECK(!tap_up(&s2, 400, 310, 1130000, 2, kNoKeyboard));
}

// **キーボードの帯は数えない（指摘 2）。**
// 帯の左右の余白と上端の候補表示は touch_down が false を返すので、
// 端末領域として流れてくる。ここを数えると入力中にキーボードが消える。
void test_keyboard_band_is_ignored()
{
    TapState s;
    // 候補帯 (y=400) を 2 回叩く
    CHECK(!tap(&s, 100, 400, 1000000, kKeyboardTop));
    CHECK(!tap(&s, 100, 400, 1100000, kKeyboardTop));
    // 左の余白 (x=100, y=600) も同じ
    CHECK(!tap(&s, 100, 600, 1200000, kKeyboardTop));
    CHECK(!tap(&s, 100, 600, 1300000, kKeyboardTop));

    // 帯より上（端末領域）なら成立する
    TapState s2;
    CHECK(!tap(&s2, 400, 300, 1000000, kKeyboardTop));
    CHECK(tap(&s2, 400, 300, 1100000, kKeyboardTop));

    // キーボードを隠していれば同じ座標でも成立する（帯が無いので）
    TapState s3;
    CHECK(!tap(&s3, 100, 600, 1000000, kNoKeyboard));
    CHECK(tap(&s3, 100, 600, 1100000, kNoKeyboard));

    // 押し始めが端末領域でも、離した先が帯なら数えない（下に払った指）
    TapState s4;
    CHECK(!tap(&s4, 400, 380, 1000000, kKeyboardTop));
    tap_down(&s4, 400, 380, 1100000, 0);
    CHECK(!tap_up(&s4, 400, 390, 1130000, 0, kKeyboardTop));
}

}  // namespace

int main()
{
    test_double_tap();
    test_too_slow();
    test_too_far();
    test_long_press();
    test_scroll_is_not_a_tap();
    test_keyboard_band_is_ignored();

    if (g_fails) {
        std::printf("FAILED: %d of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
