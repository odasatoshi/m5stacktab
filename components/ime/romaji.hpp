#pragma once
// ローマ字→かな変換。ESP-IDF に依存しないのでホストでテストできる。
//
// 入力は 1 文字ずつ。確定したかなは commit 側に出て、まだ確定できないローマ字は pending に残る。
// 促音 (kka → っか)、撥音 (n + 子音 → ん)、拗音 (kya → きゃ) を扱う。
#include <string>

namespace ime {

enum class Kana {
    kHiragana,
    kKatakana,
};

class Romaji {
public:
    // c を入力する。確定したかな (UTF-8) を out に追加する。
    void input(char c, std::string& out);

    // 未確定のローマ字（画面に下線付きで出す部分）。
    const std::string& pending() const { return pending_; }

    // 未確定があれば 1 文字消して true。無ければ false（呼び出し側が確定文字を消す）。
    bool backspace();

    // 未確定を強制的に吐き出す（確定操作や入力終了時）。
    void flush(std::string& out);

    void clear() { pending_.clear(); }

    void set_kana(Kana k) { kana_ = k; }
    Kana kana() const { return kana_; }

private:
    std::string pending_;
    Kana        kana_ = Kana::kHiragana;
};

// ひらがな↔カタカナ変換（UTF-8）。
std::string to_katakana(const std::string& hiragana);
std::string to_hiragana(const std::string& katakana);

}  // namespace ime
