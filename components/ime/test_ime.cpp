// IME コアのホストテスト。実機は要らない。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined \
//       -o /tmp/test_ime components/ime/test_ime.cpp components/ime/romaji.cpp \
//       components/ime/skk_dict.cpp && /tmp/test_ime
#include "romaji.hpp"
#include "skk_dict.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

void check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

void check_str(const std::string& got, const std::string& want, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d: got \"%s\" want \"%s\"\n", __FILE__, line, got.c_str(),
                     want.c_str());
        std::abort();
    }
}
#define CHECK_STR(got, want) check_str((got), (want), __LINE__)

// ローマ字列を流し込んで、確定したかな + 未確定を返す。
std::string type(const char* romaji, bool flush = true, ime::Kana kana = ime::Kana::kHiragana)
{
    ime::Romaji r;
    r.set_kana(kana);
    std::string out;
    for (const char* p = romaji; *p; ++p) r.input(*p, out);
    if (flush) r.flush(out);
    return out;
}

void test_romaji_basic()
{
    CHECK_STR(type("aiueo"), "あいうえお");
    CHECK_STR(type("kana"), "かな");
    CHECK_STR(type("nihongo"), "にほんご");
    CHECK_STR(type("konnnichiha"), "こんにちは");
    CHECK_STR(type("watasi"), "わたし");
    CHECK_STR(type("gakkou"), "がっこう");
}

void test_romaji_special()
{
    CHECK_STR(type("kya"), "きゃ");
    CHECK_STR(type("shi"), "し");
    CHECK_STR(type("chi"), "ち");
    CHECK_STR(type("tsu"), "つ");
    CHECK_STR(type("ji"), "じ");
    CHECK_STR(type("fu"), "ふ");
    CHECK_STR(type("kka"), "っか");   // 促音
    CHECK_STR(type("tta"), "った");
    CHECK_STR(type("sinbun"), "しんぶん");  // n + 子音 → ん
    CHECK_STR(type("nn"), "ん");
    CHECK_STR(type("n"), "ん");        // flush で ん になる
    CHECK_STR(type("kyou"), "きょう");
    CHECK_STR(type("ryokou"), "りょこう");
    CHECK_STR(type("xtu"), "っ");
    CHECK_STR(type("-"), "ー");
    CHECK_STR(type("."), "。");
}

void test_romaji_pending()
{
    ime::Romaji r;
    std::string out;
    r.input('k', out);
    CHECK_STR(out, "");
    CHECK_STR(r.pending(), "k");
    r.input('y', out);
    CHECK_STR(r.pending(), "ky");
    r.input('o', out);
    CHECK_STR(out, "きょ");
    CHECK_STR(r.pending(), "");

    // backspace は未確定を先に削る
    out.clear();
    r.input('k', out);
    CHECK(r.backspace());
    CHECK_STR(r.pending(), "");
    CHECK(!r.backspace());  // 未確定が無ければ false（呼び出し側が確定文字を削る）
}

void test_katakana()
{
    CHECK_STR(type("katakana", true, ime::Kana::kKatakana), "カタカナ");
    CHECK_STR(ime::to_katakana("にほんご"), "ニホンゴ");
    CHECK_STR(ime::to_hiragana("ニホンゴ"), "にほんご");
    // 変換対象外の文字はそのまま
    CHECK_STR(ime::to_katakana("あa、"), "アa、");
}

void test_romaji_passthrough()
{
    // 変換できない綴りは落とさずそのまま出す
    CHECK_STR(type("q"), "q");
    CHECK_STR(type("abc"), "あbc");
}

// テスト用に小さな辞書バイナリを組む（tools/build_dict.py と同じ形式）。
std::string make_dict(const std::vector<std::pair<std::string, std::string>>& sorted_entries)
{
    const uint32_t count       = static_cast<uint32_t>(sorted_entries.size());
    const uint32_t header_size = 16;
    const uint32_t strings_off = header_size + count * 4;

    std::string index, strings;
    auto        put_u32 = [](std::string& s, uint32_t v) {
        s += static_cast<char>(v & 0xFF);
        s += static_cast<char>((v >> 8) & 0xFF);
        s += static_cast<char>((v >> 16) & 0xFF);
        s += static_cast<char>((v >> 24) & 0xFF);
    };
    for (const auto& [key, val] : sorted_entries) {
        put_u32(index, strings_off + static_cast<uint32_t>(strings.size()));
        strings += key;
        strings += '\0';
        strings += val;
        strings += '\0';
    }
    std::string out = "SKKD";
    put_u32(out, 1);
    put_u32(out, count);
    put_u32(out, strings_off);
    out += index;
    out += strings;
    return out;
}

void test_skk_dict()
{
    // キーは UTF-8 のバイト順に並べる必要がある
    std::vector<std::pair<std::string, std::string>> entries = {
        {"かんじ", "漢字/幹事/監事"},
        {"にほん", "日本/二本;counter/日本"},
        {"にほんご", "日本語"},
    };
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::string blob = make_dict(entries);

    ime::SkkDict dict;
    CHECK(dict.open(blob.data(), blob.size()));
    CHECK(dict.count() == 3);

    std::vector<std::string> out;
    CHECK(dict.lookup("にほんご", out));
    CHECK(out.size() == 1);
    CHECK_STR(out[0], "日本語");

    CHECK(dict.lookup("かんじ", out));
    CHECK(out.size() == 3);
    CHECK_STR(out[0], "漢字");
    CHECK_STR(out[2], "監事");

    // 注釈 (";" 以降) は落とす
    CHECK(dict.lookup("にほん", out));
    CHECK(out.size() == 3);
    CHECK_STR(out[1], "二本");

    CHECK(!dict.lookup("そんなよみはない", out));
    CHECK(out.empty());
    CHECK(!dict.lookup("", out));
}

void test_skk_dict_broken()
{
    ime::SkkDict dict;
    CHECK(!dict.open(nullptr, 0));
    CHECK(!dict.open("SKKD", 4));                 // 短すぎる
    CHECK(!dict.open("XXXX\0\0\0\0", 16));        // マジックが違う
    // count に対してインデックスが収まらない辞書は拒否する
    std::string bad = "SKKD";
    auto        put_u32 = [&](uint32_t v) {
        bad += static_cast<char>(v & 0xFF);
        bad += static_cast<char>((v >> 8) & 0xFF);
        bad += static_cast<char>((v >> 16) & 0xFF);
        bad += static_cast<char>((v >> 24) & 0xFF);
    };
    put_u32(1);
    put_u32(1000000);  // count
    put_u32(20);       // strings offset がインデックスと重なる
    bad.resize(64, 'x');
    CHECK(!dict.open(bad.data(), bad.size()));
}

}  // namespace

int main()
{
    test_romaji_basic();
    test_romaji_special();
    test_romaji_pending();
    test_katakana();
    test_romaji_passthrough();
    test_skk_dict();
    test_skk_dict_broken();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
