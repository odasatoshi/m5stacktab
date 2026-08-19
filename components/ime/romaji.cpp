#include "romaji.hpp"

#include <algorithm>
#include <cstring>

namespace ime {
namespace {

struct Entry {
    const char* romaji;
    const char* kana;
};

// 長いものから順に照合する。同じ綴りの重複は置かない。
constexpr Entry kTable[] = {
    // 3〜4 文字（拗音・特殊）
    {"kya", "きゃ"}, {"kyi", "きぃ"}, {"kyu", "きゅ"}, {"kye", "きぇ"}, {"kyo", "きょ"},
    {"gya", "ぎゃ"}, {"gyi", "ぎぃ"}, {"gyu", "ぎゅ"}, {"gye", "ぎぇ"}, {"gyo", "ぎょ"},
    {"sha", "しゃ"}, {"shi", "し"},   {"shu", "しゅ"}, {"she", "しぇ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"ja",  "じゃ"}, {"ji",  "じ"},   {"ju",  "じゅ"}, {"je",  "じぇ"}, {"jo",  "じょ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"cha", "ちゃ"}, {"chi", "ち"},   {"chu", "ちゅ"}, {"che", "ちぇ"}, {"cho", "ちょ"},
    {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"},
    {"tsu", "つ"},   {"tsa", "つぁ"}, {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"fa",  "ふぁ"}, {"fi",  "ふぃ"}, {"fu",  "ふ"},   {"fe",  "ふぇ"}, {"fo",  "ふぉ"},
    {"va",  "ゔぁ"}, {"vi",  "ゔぃ"}, {"vu",  "ゔ"},   {"ve",  "ゔぇ"}, {"vo",  "ゔぉ"},
    {"dha", "でゃ"}, {"dhi", "でぃ"}, {"dhu", "でゅ"}, {"dho", "でょ"},
    {"tha", "てゃ"}, {"thi", "てぃ"}, {"thu", "てゅ"}, {"tho", "てょ"},
    {"wha", "うぁ"}, {"whi", "うぃ"}, {"whe", "うぇ"}, {"who", "うぉ"},
    {"xtu", "っ"},   {"ltu", "っ"},   {"xya", "ゃ"},   {"xyu", "ゅ"},   {"xyo", "ょ"},
    {"xa",  "ぁ"},   {"xi",  "ぃ"},   {"xu",  "ぅ"},   {"xe",  "ぇ"},   {"xo",  "ぉ"},
    // 2 文字
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"sa", "さ"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"za", "ざ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"ta", "た"}, {"ti", "ち"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"da", "だ"}, {"di", "ぢ"}, {"du", "づ"}, {"de", "で"}, {"do", "ど"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yi", "い"}, {"yu", "ゆ"}, {"ye", "いぇ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wi", "うぃ"}, {"wu", "う"}, {"we", "うぇ"}, {"wo", "を"},
    {"nn", "ん"},
    // 1 文字
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
    {"-", "ー"}, {",", "、"}, {".", "。"}, {"[", "「"}, {"]", "」"}, {"/", "・"},
};

bool is_vowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// pending が「どのエントリの先頭にもなり得ない」なら、もう待っても無駄。
bool could_extend(const std::string& s)
{
    for (const auto& e : kTable) {
        if (std::strncmp(e.romaji, s.c_str(), s.size()) == 0 && std::strlen(e.romaji) > s.size()) {
            return true;
        }
    }
    return false;
}

const Entry* exact_match(const std::string& s)
{
    for (const auto& e : kTable) {
        if (s == e.romaji) return &e;
    }
    return nullptr;
}

}  // namespace

void Romaji::input(char c, std::string& out)
{
    const char lc = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);

    // 促音: 同じ子音が続いたら「っ」を出して 1 文字ぶんを残す。
    if (!pending_.empty() && pending_.back() == lc && !is_vowel(lc) && lc != 'n') {
        std::string kana = "っ";
        out += (kana_ == Kana::kKatakana) ? to_katakana(kana) : kana;
        pending_.clear();
        pending_ += lc;
        return;
    }
    // 撥音: n の後に子音が来たら「ん」で確定させる ("nk" → ん + k)。
    if (pending_ == "n" && !is_vowel(lc) && lc != 'n' && lc != 'y') {
        std::string kana = "ん";
        out += (kana_ == Kana::kKatakana) ? to_katakana(kana) : kana;
        pending_.clear();
        input(lc, out);
        return;
    }

    pending_ += lc;

    if (const Entry* e = exact_match(pending_)) {
        // より長い綴りに伸びる可能性が残っているなら確定させない ("n" は "na" にも "nn" にもなる)。
        if (!could_extend(pending_)) {
            std::string kana = e->kana;
            out += (kana_ == Kana::kKatakana) ? to_katakana(kana) : kana;
            pending_.clear();
        }
        return;
    }
    if (could_extend(pending_)) return;

    // どれにも当たらない。先頭から最長一致を切り出して確定し、残りを入れ直す。
    // ここで pending_ を空にしてから再投入しないと無限に溜まる。
    for (size_t len = pending_.size(); len >= 1; --len) {
        if (const Entry* e = exact_match(pending_.substr(0, len))) {
            std::string kana = e->kana;
            out += (kana_ == Kana::kKatakana) ? to_katakana(kana) : kana;
            const std::string rest = pending_.substr(len);
            pending_.clear();
            for (char ch : rest) input(ch, out);
            return;
        }
    }
    // どの長さでも変換できない（"q" など）。1 文字そのまま出して残りを入れ直す。
    {
        const std::string head = pending_.substr(0, 1);
        const std::string rest = pending_.substr(1);
        out += head;
        pending_.clear();
        for (char ch : rest) input(ch, out);
    }
}

bool Romaji::backspace()
{
    if (pending_.empty()) return false;
    pending_.pop_back();
    return true;
}

void Romaji::flush(std::string& out)
{
    if (pending_ == "n") {
        std::string kana = "ん";
        out += (kana_ == Kana::kKatakana) ? to_katakana(kana) : kana;
    } else {
        out += pending_;
    }
    pending_.clear();
}

// ひらがな U+3041..U+3096 と カタカナ U+30A1..U+30F6 は 0x60 差。
namespace {

std::string shift_kana(const std::string& s, int delta, uint32_t lo, uint32_t hi)
{
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if ((b & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((b & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            if (cp >= lo && cp <= hi) cp += delta;
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            i += 3;
        } else {
            out += s[i++];
        }
    }
    return out;
}

}  // namespace

std::string to_katakana(const std::string& hiragana)
{
    return shift_kana(hiragana, 0x60, 0x3041, 0x3096);
}

std::string to_hiragana(const std::string& katakana)
{
    return shift_kana(katakana, -0x60, 0x30A1, 0x30F6);
}

}  // namespace ime
