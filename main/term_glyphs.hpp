#pragma once
// フォントに頼らずに描く字 (#100) と、efont に無い記号 (#102)。
//
// efontJA_24 は罫線が 32/128 字、ブロック要素と点字は 1 字も無い。無い字は M5GFX が
// 2 セル幅の空箱で描くので、行のそれ以降が 1 セルずつずれる（TUI が崩れる本当の原因）。
// 罫線・ブロック・点字は自分で描く（kitty / WezTerm と同じ方式）。フォントの罫線は
// 線の位置がセルの中心と合っていないので、収録されている字も自前のほうを使う。
// それ以外の記号 (⏺ ✻ ✓ ❯ ⚠ など) は、Noto から焼いた記号フォント (symbols.bin,
// tools/gen_symbol_font.py が作る) で描く。
//
// M5GFX に依存しない。塗る手段は呼び出し側が渡すので、ホストでビットマップに描かせて
// テストできる (test_term_glyphs.cpp)。
#include <cstddef>
#include <cstdint>
#include <functional>

namespace glyph {

// (x, y, w, h) を前景色で塗る。alpha は前景の濃さ (255 = 前景そのもの、シェード ░▒▓ は 64-192)。
using Fill = std::function<void(int x, int y, int w, int h, uint8_t alpha)>;

// 自前で描く字か (罫線 U+2500-257F / ブロック U+2580-259F / 点字 U+2800-28FF)。
bool is_drawn(uint32_t cp);

// w x h のセルに cp を描く。is_drawn(cp) が false なら何もしない。
void draw(uint32_t cp, int w, int h, const Fill& fill);

// 記号フォント (symbols.bin) の 1 字。セル (12x24) の中の外接矩形と、1 画素 4 bit の濃さ。
// w == 0 は「何も描かない字」(各種の空白)。
struct Symbol {
    uint8_t        x, y, w, h;
    const uint8_t* data;  // 行ごとに (w + 1) / 2 バイト、上位 4 bit が左の画素
};

// blob (symbols.bin の中身) から cp を引く。無ければ false。out は nullptr でもよい (有無だけ見る)。
// 形式が壊れていたら (magic 違い・範囲外) 何も見つからないものとして扱う。
bool find_symbol(const uint8_t* blob, size_t size, uint32_t cp, Symbol* out);

// 記号を w x h のセルの中央に描く (記号は 12x24 で焼いてあり、全角のセルなら左右に余白)。
// 同じ濃さが横に続く区間はまとめて fill を呼ぶ。
void draw_symbol(const Symbol& s, int w, int h, const Fill& fill);

}  // namespace glyph
