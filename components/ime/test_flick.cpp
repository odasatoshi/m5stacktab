// IME 状態機械 + 12 キーフリック入力のホストテスト。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined -o /tmp/test_flick
//       components/ime/test_flick.cpp components/ime/ime.cpp components/ime/flick.cpp
//       components/ime/romaji.cpp components/ime/skk_dict.cpp && /tmp/test_flick
#include <cstdint>
#include "flick.hpp"
#include "ime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

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

void check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// テスト用の小さな辞書（tools/build_dict.py と同じ形式）
std::string make_dict(std::vector<std::pair<std::string, std::string>> entries)
{
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const uint32_t count = static_cast<uint32_t>(entries.size());
    const uint32_t strings_off = 16 + count * 4;
    auto put_u32 = [](std::string& s, uint32_t v) {
        s += static_cast<char>(v & 0xFF);
        s += static_cast<char>((v >> 8) & 0xFF);
        s += static_cast<char>((v >> 16) & 0xFF);
        s += static_cast<char>((v >> 24) & 0xFF);
    };
    std::string index, strings;
    for (const auto& [k, v] : entries) {
        put_u32(index, strings_off + static_cast<uint32_t>(strings.size()));
        strings += k;
        strings += '\0';
        strings += v;
        strings += '\0';
    }
    std::string out = "SKKD";
    put_u32(out, 1);
    put_u32(out, count);
    put_u32(out, strings_off);
    return out + index + strings;
}

void test_ime_romaji_flow()
{
    const std::string blob = make_dict({{"にほん", "日本/二本"}, {"にほんご", "日本語"}});
    ime::SkkDict dict;
    CHECK(dict.open(blob.data(), blob.size()));

    ime::Ime ime;
    ime.set_dict(&dict);

    for (char c : std::string("nihongo")) ime.input_char(c);
    EXPECT(ime.composing(), "にほんご");
    CHECK(ime.mode() == ime::Mode::kKana);

    ime.convert();
    CHECK(ime.mode() == ime::Mode::kSelect);
    EXPECT(ime.composing(), "日本語");
    // 辞書の候補の後にカタカナ・ひらがなが続く
    CHECK(ime.candidates().size() >= 3);
    EXPECT(ime.candidates()[0], "日本語");

    ime.next_candidate();
    EXPECT(ime.composing(), "ニホンゴ");
    ime.next_candidate();
    EXPECT(ime.composing(), "にほんご");
    ime.prev_candidate();
    EXPECT(ime.composing(), "ニホンゴ");

    EXPECT(ime.commit(), "ニホンゴ");
    CHECK(ime.empty());
    CHECK(ime.mode() == ime::Mode::kKana);
}

void test_ime_no_dict()
{
    // 辞書が無くてもカタカナ・ひらがなには変換できる
    ime::Ime ime;
    for (char c : std::string("purogaramu")) ime.input_char(c);
    EXPECT(ime.composing(), "ぷろがらむ");
    ime.convert();
    CHECK(ime.mode() == ime::Mode::kSelect);
    EXPECT(ime.composing(), "プロガラム");
    EXPECT(ime.commit(), "プロガラム");
}

void test_ime_editing()
{
    ime::Ime ime;
    for (char c : std::string("kana")) ime.input_char(c);
    EXPECT(ime.composing(), "かな");
    CHECK(ime.backspace());
    EXPECT(ime.composing(), "か");
    CHECK(ime.backspace());
    CHECK(ime.empty());
    CHECK(!ime.backspace());  // 何も無ければ false（端末に BS を送る）

    // 未確定のローマ字を先に消す
    ime.input_char('k');
    ime.input_char('y');
    EXPECT(ime.pending_romaji(), "ky");
    CHECK(ime.backspace());
    EXPECT(ime.pending_romaji(), "k");

    // 候補選択中の backspace は選択を抜けてかなに戻る
    ime.cancel();
    for (char c : std::string("aiu")) ime.input_char(c);
    ime.convert();
    CHECK(ime.mode() == ime::Mode::kSelect);
    CHECK(ime.backspace());
    CHECK(ime.mode() == ime::Mode::kKana);
    EXPECT(ime.composing(), "あいう");
}

void test_modifiers()
{
    ime::Ime ime;
    ime.input_kana("か");
    CHECK(ime.modify_last(ime::Ime::Modifier::kDakuten));
    EXPECT(ime.composing(), "が");
    // もう一度押すと戻る（トグル）
    CHECK(ime.modify_last(ime::Ime::Modifier::kDakuten));
    EXPECT(ime.composing(), "か");

    ime.cancel();
    ime.input_kana("は");
    CHECK(ime.modify_last(ime::Ime::Modifier::kHandakuten));
    EXPECT(ime.composing(), "ぱ");

    ime.cancel();
    ime.input_kana("つ");
    CHECK(ime.modify_last(ime::Ime::Modifier::kSmall));
    EXPECT(ime.composing(), "っ");

    // 適用できない文字では false
    ime.cancel();
    ime.input_kana("ん");
    CHECK(!ime.modify_last(ime::Ime::Modifier::kDakuten));
    EXPECT(ime.composing(), "ん");

    // 何も無ければ false
    ime.cancel();
    CHECK(!ime.modify_last(ime::Ime::Modifier::kDakuten));
}

void test_flick_keys()
{
    ime::FlickKeyboard kb;
    ime::FlickLayout   l;
    l.x = 0;
    l.y = 400;
    l.width = 480;   // 4 列 x 120px
    l.height = 320;  // 4 行 x 80px
    kb.set_layout(l);

    // キーボード外は反応しない
    CHECK(!kb.touch_down(10, 100));
    CHECK(!kb.is_pressed());

    // 「あ」キー（0,0）を中心タップ
    CHECK(kb.touch_down(60, 440));
    CHECK(kb.is_pressed());
    auto r = kb.touch_up(60, 440);
    CHECK(r.valid);
    EXPECT(r.kana, "あ");
    CHECK(r.flick == ime::Flick::kCenter);
    CHECK(!kb.is_pressed());

    // 右フリックで「え」
    kb.touch_down(60, 440);
    r = kb.touch_up(60 + 60, 440);
    EXPECT(r.kana, "え");
    CHECK(r.flick == ime::Flick::kRight);

    // 上フリックで「う」
    kb.touch_down(60, 440);
    r = kb.touch_up(60, 440 - 40);
    EXPECT(r.kana, "う");

    // 左フリックで「い」、下フリックで「お」
    kb.touch_down(60, 440);
    EXPECT(kb.touch_up(60 - 60, 440).kana, "い");
    kb.touch_down(60, 440);
    EXPECT(kb.touch_up(60, 440 + 40).kana, "お");

    // 「か」キー（0,1）
    kb.touch_down(180, 440);
    EXPECT(kb.touch_up(180, 440).kana, "か");
    kb.touch_down(180, 440);
    EXPECT(kb.touch_up(180, 440 - 40).kana, "く");

    // 閾値未満の移動は中心扱い
    kb.touch_down(60, 440);
    r = kb.touch_up(60 + 5, 440 + 5);
    EXPECT(r.kana, "あ");
    CHECK(r.flick == ime::Flick::kCenter);

    // 機能キー列（col 3）
    kb.touch_down(420, 440);
    r = kb.touch_up(420, 440);
    CHECK(r.func == ime::FuncKey::kBackspace);
    CHECK(r.kana.empty());
    kb.touch_down(420, 520);
    CHECK(kb.touch_up(420, 520).func == ime::FuncKey::kConvert);
    kb.touch_down(420, 600);
    CHECK(kb.touch_up(420, 600).func == ime::FuncKey::kCommit);
    kb.touch_down(420, 680);
    CHECK(kb.touch_up(420, 680).func == ime::FuncKey::kMode);

    // touch_down なしの touch_up は無効
    r = kb.touch_up(60, 440);
    CHECK(!r.valid);

    // ラベルは全キーに存在する
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            CHECK(ime::FlickKeyboard::key_label(row, col)[0] != '\0');
        }
    }
}

void test_flick_to_ime()
{
    // フリック入力から IME へ流し込む（実機の操作に相当）
    ime::FlickKeyboard kb;
    ime::FlickLayout   l;
    l.x = 0; l.y = 0; l.width = 480; l.height = 320;
    kb.set_layout(l);
    ime::Ime ime;

    // 「か」→濁点で「が」、「つ」→小書きで「っ」
    kb.touch_down(180, 40);
    ime.input_kana(kb.touch_up(180, 40).kana);
    EXPECT(ime.composing(), "か");
    ime.modify_last(ime::Ime::Modifier::kDakuten);
    EXPECT(ime.composing(), "が");

    // 「た」キー上フリックで「つ」
    kb.touch_down(60, 120);
    ime.input_kana(kb.touch_up(60, 120 - 40).kana);
    EXPECT(ime.composing(), "がつ");
    ime.modify_last(ime::Ime::Modifier::kSmall);
    EXPECT(ime.composing(), "がっ");

    EXPECT(ime.commit(), "がっ");
}

// レビュー指摘の回帰テスト。
// 押しっぱなしのまま面が切り替わったときに捨てられること (#65)。
// **捨てないと、離したときに選んでいないかなが出る** — 画面キーボードの段は
// 指を置いたままでも変わる（純正キーボードの Ctrl+Alt+M でメニューが開く）。
void test_flick_cancel()
{
    ime::FlickKeyboard kb;
    ime::FlickLayout   l;
    l.x      = 0;
    l.y      = 400;
    l.width  = 480;
    l.height = 320;
    kb.set_layout(l);

    CHECK(kb.touch_down(60, 440));  // 「あ」キー
    CHECK(kb.is_pressed());
    kb.cancel();
    CHECK(!kb.is_pressed());
    const auto r = kb.touch_up(60, 440);
    CHECK(!r.valid);
    EXPECT(r.kana, "");
}

void test_review_regressions()
{
    // composing() は未確定のローマ字を含む。UI 側で pending_romaji を足すと二重になるので、
    // ここで「composing だけで足りる」ことを固定する。
    ime::Ime ime;
    ime.input_char('k');
    EXPECT(ime.composing(), "k");
    EXPECT(ime.pending_romaji(), "k");
    ime.input_char('y');
    EXPECT(ime.composing(), "ky");
    ime.input_char('o');
    EXPECT(ime.composing(), "きょ");
    EXPECT(ime.pending_romaji(), "");

    // キー幅が割り切れないレイアウトでも、右端・下端で隣のキーに化けない。
    ime::FlickKeyboard kb;
    ime::FlickLayout   l;
    l.x = 0; l.y = 0; l.width = 483; l.height = 322;  // 4 で割り切れない
    kb.set_layout(l);
    // 右端 1px と下端 1px
    CHECK(kb.touch_down(l.width - 1, l.height - 1));
    const auto r = kb.touch_up(l.width - 1, l.height - 1);
    CHECK(r.valid);
    // 右下は機能キー列の最下段 = モード切替。行が繰り上がって別キーになっていないこと。
    CHECK(r.func == ime::FuncKey::kMode);
}

}  // namespace

int main()
{
    test_ime_romaji_flow();
    test_ime_no_dict();
    test_ime_editing();
    test_modifiers();
    test_flick_keys();
    test_flick_to_ime();
    test_flick_cancel();
    test_review_regressions();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
