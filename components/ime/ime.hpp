#pragma once
// 日本語入力の状態機械。ESP-IDF にも描画にも依存しないのでホストでテストできる。
//
// 入力の流れ:
//   ローマ字/かな入力 → 未確定のかな (composing) → 変換 → 候補選択 → 確定 (commit)
//
// 変換候補は SkkDict から引き、辞書に無いときはカタカナ・ひらがなをそのまま候補にする。
#include <string>
#include <vector>

#include "romaji.hpp"
#include "skk_dict.hpp"

namespace ime {

enum class Mode {
    kDirect,    // 英数直接入力（変換しない）
    kKana,      // かな入力中（未変換）
    kSelect,    // 候補選択中
};

class Ime {
public:
    // 辞書は無くても動く（カタカナ・ひらがな変換だけになる）。
    void set_dict(const SkkDict* dict) { dict_ = dict; }

    Mode mode() const { return mode_; }
    void set_direct(bool direct);

    // --- 入力 ---
    // ローマ字 1 文字。確定した文字列が返る（通常は空）。
    std::string input_char(char c);
    // かなを直接 1 文字（フリック入力から）。
    std::string input_kana(const std::string& kana);

    // 直前のかなに濁点・半濁点・小書きを適用する（フリック入力の「゛小」キー）。
    enum class Modifier { kDakuten, kHandakuten, kSmall };
    bool modify_last(Modifier m);

    // 変換を開始する（スペースキー相当）。候補があれば kSelect に移る。
    void convert();
    // 次/前の候補へ。
    void next_candidate();
    void prev_candidate();

    // 確定する。確定した文字列を返す。
    std::string commit();
    // 未確定を取り消す。何か消したら true。
    bool cancel();
    // 1 文字削除。未確定が無ければ false（呼び出し側が端末に BS を送る）。
    bool backspace();

    // --- 表示用 ---
    // 未確定の表示文字列（下線付きで出す部分）。
    std::string composing() const;
    const std::vector<std::string>& candidates() const { return candidates_; }
    int candidate_index() const { return cand_index_; }
    // 未確定のローマ字（まだかなになっていない部分）。
    const std::string& pending_romaji() const { return romaji_.pending(); }

    bool empty() const { return kana_.empty() && romaji_.pending().empty(); }

private:
    void build_candidates();

    Romaji                   romaji_;
    std::string              kana_;        // 未確定のかな
    std::vector<std::string> candidates_;
    int                      cand_index_ = 0;
    Mode                     mode_       = Mode::kKana;
    const SkkDict*           dict_       = nullptr;
};

}  // namespace ime
