#pragma once
// フォントに頼らずに描く字と、フォントに無い字の置き換え (#100)。
//
// efontJA_24 は罫線が 32/128 字、ブロック要素と点字は 1 字も無い。無い字は M5GFX が
// 2 セル幅の空箱で描くので、行のそれ以降が 1 セルずつずれる（TUI が崩れる本当の原因）。
// 罫線・ブロック・点字は自分で描く（kitty / WezTerm と同じ方式）。フォントの罫線は
// 線の位置がセルの中心と合っていないので、収録されている字も自前のほうを使う。
//
// M5GFX に依存しない。塗る手段は呼び出し側が渡すので、ホストでビットマップに描かせて
// テストできる (test_term_glyphs.cpp)。
#include <cstdint>
#include <functional>

namespace glyph {

// (x, y, w, h) を前景色で塗る。alpha は前景の濃さ (255 = 前景そのもの、シェード ░▒▓ は 64-192)。
using Fill = std::function<void(int x, int y, int w, int h, uint8_t alpha)>;

// 自前で描く字か (罫線 U+2500-257F / ブロック U+2580-259F / 点字 U+2800-28FF)。
bool is_drawn(uint32_t cp);

// w x h のセルに cp を描く。is_drawn(cp) が false なら何もしない。
void draw(uint32_t cp, int w, int h, const Fill& fill);

// フォントに無い字の代わり。代わりが無ければ 0。代わりの字も is_drawn か
// フォントに有る字のどちらか（呼び出し側がもう一度引き直す）。
uint32_t substitute(uint32_t cp);

}  // namespace glyph
