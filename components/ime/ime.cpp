#include "ime.hpp"

#include <algorithm>

namespace ime {

std::string Ime::input_char(char c)
{
    // 候選択中に文字が来たら、今の候補を確定してから続ける（一般的な IME の挙動）。
    std::string out;
    if (mode_ == Mode::kSelect) out = commit();

    romaji_.input(c, kana_);
    return out;
}

std::string Ime::input_kana(const std::string& kana)
{
    std::string out;
    if (mode_ == Mode::kSelect) out = commit();
    // フリック入力はローマ字を経由しないので、未確定のローマ字があれば先に吐き出す。
    romaji_.flush(kana_);
    kana_ += kana;
    return out;
}

void Ime::build_candidates()
{
    candidates_.clear();
    cand_index_ = 0;
    if (kana_.empty()) return;

    if (dict_ && dict_->is_open()) {
        std::vector<std::string> found;
        if (dict_->lookup(kana_, found)) {
            candidates_ = std::move(found);
        }
    }
    // 辞書に無い読みでもカタカナ・ひらがなは常に選べるようにする
    // （SKK 辞書はカタカナ語のエントリが薄いので、これが無いと「ぷろぐらむ」が変換できない）。
    const std::string katakana = to_katakana(kana_);
    if (std::find(candidates_.begin(), candidates_.end(), katakana) == candidates_.end()) {
        candidates_.push_back(katakana);
    }
    if (std::find(candidates_.begin(), candidates_.end(), kana_) == candidates_.end()) {
        candidates_.push_back(kana_);
    }
}

namespace {

struct KanaMap {
    const char* from;
    const char* to;
};

// 濁点。う→ゔ も入れておく（外来語で使う）。
constexpr KanaMap kDakuten[] = {
    {"か", "が"}, {"き", "ぎ"}, {"く", "ぐ"}, {"け", "げ"}, {"こ", "ご"},
    {"さ", "ざ"}, {"し", "じ"}, {"す", "ず"}, {"せ", "ぜ"}, {"そ", "ぞ"},
    {"た", "だ"}, {"ち", "ぢ"}, {"つ", "づ"}, {"て", "で"}, {"と", "ど"},
    {"は", "ば"}, {"ひ", "び"}, {"ふ", "ぶ"}, {"へ", "べ"}, {"ほ", "ぼ"},
    {"う", "ゔ"},
};

constexpr KanaMap kHandakuten[] = {
    {"は", "ぱ"}, {"ひ", "ぴ"}, {"ふ", "ぷ"}, {"へ", "ぺ"}, {"ほ", "ぽ"},
    {"ば", "ぱ"}, {"び", "ぴ"}, {"ぶ", "ぷ"}, {"べ", "ぺ"}, {"ぼ", "ぽ"},
};

// 小書き。濁点付きから戻せるように濁音も入れる。
constexpr KanaMap kSmall[] = {
    {"あ", "ぁ"}, {"い", "ぃ"}, {"う", "ぅ"}, {"え", "ぇ"}, {"お", "ぉ"},
    {"つ", "っ"}, {"や", "ゃ"}, {"ゆ", "ゅ"}, {"よ", "ょ"}, {"わ", "ゎ"},
};

// 逆変換（もう一度押したら元に戻す）。
bool lookup_map(const KanaMap* map, size_t n, const std::string& c, std::string& out)
{
    for (size_t i = 0; i < n; ++i) {
        if (c == map[i].from) {
            out = map[i].to;
            return true;
        }
    }
    // すでに変換済みなら戻す（トグル）
    for (size_t i = 0; i < n; ++i) {
        if (c == map[i].to) {
            out = map[i].from;
            return true;
        }
    }
    return false;
}

}  // namespace

bool Ime::modify_last(Modifier m)
{
    if (mode_ == Mode::kSelect) cancel();
    romaji_.flush(kana_);
    if (kana_.empty()) return false;

    // 末尾の 1 文字（UTF-8）を取り出す
    size_t i = kana_.size();
    while (i > 0 && (static_cast<unsigned char>(kana_[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0) --i;
    const std::string last = kana_.substr(i);

    std::string replaced;
    bool        ok = false;
    switch (m) {
        case Modifier::kDakuten:
            ok = lookup_map(kDakuten, sizeof(kDakuten) / sizeof(kDakuten[0]), last, replaced);
            break;
        case Modifier::kHandakuten:
            ok = lookup_map(kHandakuten, sizeof(kHandakuten) / sizeof(kHandakuten[0]), last,
                            replaced);
            break;
        case Modifier::kSmall:
            ok = lookup_map(kSmall, sizeof(kSmall) / sizeof(kSmall[0]), last, replaced);
            break;
    }
    if (!ok) return false;
    kana_.resize(i);
    kana_ += replaced;
    return true;
}

void Ime::convert()
{
    if (mode_ == Mode::kSelect) {
        next_candidate();
        return;
    }
    romaji_.flush(kana_);  // "n" だけ残っている状態を「ん」にする
    if (kana_.empty()) return;
    build_candidates();
    if (!candidates_.empty()) mode_ = Mode::kSelect;
}

void Ime::next_candidate()
{
    if (mode_ != Mode::kSelect || candidates_.empty()) return;
    cand_index_ = (cand_index_ + 1) % static_cast<int>(candidates_.size());
}

void Ime::prev_candidate()
{
    if (mode_ != Mode::kSelect || candidates_.empty()) return;
    cand_index_ =
        (cand_index_ - 1 + static_cast<int>(candidates_.size())) % static_cast<int>(candidates_.size());
}

std::string Ime::commit()
{
    std::string out;
    if (mode_ == Mode::kSelect && !candidates_.empty()) {
        out = candidates_[cand_index_];
    } else {
        romaji_.flush(kana_);
        out = kana_;
    }
    kana_.clear();
    candidates_.clear();
    cand_index_ = 0;
    romaji_.clear();
    if (mode_ == Mode::kSelect) mode_ = Mode::kKana;
    return out;
}

bool Ime::cancel()
{
    if (mode_ == Mode::kSelect) {
        // 候補選択を抜けてかな入力に戻る（かなは残す）。
        candidates_.clear();
        cand_index_ = 0;
        mode_       = Mode::kKana;
        return true;
    }
    if (kana_.empty() && romaji_.pending().empty()) return false;
    kana_.clear();
    romaji_.clear();
    return true;
}

bool Ime::backspace()
{
    if (mode_ == Mode::kSelect) {
        cancel();
        return true;
    }
    if (romaji_.backspace()) return true;
    if (kana_.empty()) return false;
    // UTF-8 の 1 文字ぶん削る。
    size_t i = kana_.size();
    while (i > 0 && (static_cast<unsigned char>(kana_[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0) --i;
    kana_.resize(i);
    return true;
}

std::string Ime::composing() const
{
    if (mode_ == Mode::kSelect && !candidates_.empty()) {
        return candidates_[cand_index_];
    }
    return kana_ + romaji_.pending();
}

}  // namespace ime
