#include "term_glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace glyph {

namespace {

// 罫線 U+2500-257F の 4 本の腕。2 ビットずつ 上 / 右 / 下 / 左 の順 (bit 0-1 が上)。
// 値は 0 = なし、1 = 細、2 = 太、3 = 二重。Unicode の文字名から機械的に作った:
//   "DOWN LIGHT AND RIGHT HEAVY" → " AND " で区切り、各部の太さ語を方向語に掛ける
//   （太さ語の無い部は前の部の太さを引き継ぐ）。
// 破線は腕だけ持ち、破線の数は dashes() が返す。円弧と斜線は 0 にして別に描く。
constexpr uint8_t kBoxArms[128] = {
    0x44, 0x88, 0x11, 0x22, 0x44, 0x88, 0x11, 0x22,  // U+2500
    0x44, 0x88, 0x11, 0x22, 0x14, 0x18, 0x24, 0x28,  // U+2508
    0x50, 0x90, 0x60, 0xA0, 0x05, 0x09, 0x06, 0x0A,  // U+2510
    0x41, 0x81, 0x42, 0x82, 0x15, 0x19, 0x16, 0x25,  // U+2518
    0x26, 0x1A, 0x29, 0x2A, 0x51, 0x91, 0x52, 0x61,  // U+2520
    0x62, 0x92, 0xA1, 0xA2, 0x54, 0x94, 0x58, 0x98,  // U+2528
    0x64, 0xA4, 0x68, 0xA8, 0x45, 0x85, 0x49, 0x89,  // U+2530
    0x46, 0x86, 0x4A, 0x8A, 0x55, 0x95, 0x59, 0x99,  // U+2538
    0x56, 0x65, 0x66, 0x96, 0x5A, 0xA5, 0x69, 0x9A,  // U+2540
    0xA9, 0xA6, 0x6A, 0xAA, 0x44, 0x88, 0x11, 0x22,  // U+2548
    0xCC, 0x33, 0x1C, 0x34, 0x3C, 0xD0, 0x70, 0xF0,  // U+2550
    0x0D, 0x07, 0x0F, 0xC1, 0x43, 0xC3, 0x1D, 0x37,  // U+2558
    0x3F, 0xD1, 0x73, 0xF3, 0xDC, 0x74, 0xFC, 0xCD,  // U+2560
    0x47, 0xCF, 0xDD, 0x77, 0xFF, 0x00, 0x00, 0x00,  // U+2568
    0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0x04, 0x10,  // U+2570
    0x80, 0x02, 0x08, 0x20, 0x48, 0x21, 0x84, 0x12,  // U+2578
};

int dashes(uint32_t cp)
{
    if (cp >= 0x2504 && cp <= 0x2507) return 3;
    if (cp >= 0x2508 && cp <= 0x250B) return 4;
    if (cp >= 0x254C && cp <= 0x254F) return 2;
    return 0;
}

struct Sub {
    uint32_t from;
    uint32_t to;
};

// フォントに無い字の代わり (from で昇順)。4 つの TUI のキャプチャに出てきた字が中心。
// 代わりの字は efontJA_24 に有ること、または自前で描く字であること。
constexpr Sub kSubs[] = {
    {0x00A0, ' '},                                                     // NBSP
    {0x2039, '<'},    {0x203A, '>'},                                   // ‹ › (codex のプロンプト)
    {0x2219, 0x00B7}, {0x22EF, 0x2026},                                // ∙ ⋯
    {0x23BA, 0x2594}, {0x23BB, 0x2500}, {0x23BC, 0x2500}, {0x23BD, 0x2581},  // ⎺⎻⎼⎽
    {0x23BF, 0x2514},                                                  // ⎿ (Claude Code の結果行)
    {0x23F4, 0x25C0}, {0x23F5, 0x25B6},                                // ⏴ ⏵
    {0x23FA, 0x25CF},                                                  // ⏺ (Claude Code の行頭)
    {0x25AA, 0x25A0}, {0x25AB, 0x25A1}, {0x25B8, 0x25B6}, {0x25B9, 0x25B7},
    {0x25BA, 0x25B6}, {0x25C2, 0x25C0}, {0x25C4, 0x25C0},
    {0x25FB, 0x25A1}, {0x25FC, 0x25A0},
    {0x26A0, '!'},
    {0x2713, 0x221A}, {0x2714, 0x221A}, {0x2715, 0x00D7}, {0x2716, 0x00D7},
    {0x2717, 0x00D7}, {0x2718, 0x00D7},
    {0x2722, '*'},    {0x2723, '*'},    {0x2724, '*'},    {0x2725, '*'},   // ✢ (スピナー)
    {0x2731, '*'},    {0x2732, '*'},    {0x2733, '*'},    {0x2734, '*'},   // ✳
    {0x2735, '*'},    {0x2736, '*'},    {0x2737, '*'},    {0x2738, '*'},   // ✶
    {0x2739, '*'},    {0x273A, '*'},    {0x273B, '*'},    {0x273C, '*'},   // ✻
    {0x273D, '*'},                                                          // ✽
    {0x276E, '<'},    {0x276F, '>'},                                   // ❮ ❯ (Claude Code のプロンプト)
    {0x2B1D, 0x00B7}, {0x2B24, 0x25CF},                                // ⬝ ⬤
    {0x2E3A, 0x2500}, {0x2E3B, 0x2500},                                // ⸺ ⸻
};

}  // namespace

bool is_drawn(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF);
}

uint32_t substitute(uint32_t cp)
{
    auto it = std::lower_bound(std::begin(kSubs), std::end(kSubs), cp,
                               [](const Sub& s, uint32_t v) { return s.from < v; });
    return (it != std::end(kSubs) && it->from == cp) ? it->to : 0;
}

namespace {

// 線の太さ。細線 lw、太線 2lw、二重線は lw の線 2 本 (間も lw)。
// 隣のセルと継ぎ目が揃うよう、位置は (セル幅 - 太さ) / 2 で決める（どの字でも同じ式）。
void draw_box(uint32_t cp, int w, int h, const Fill& fill)
{
    const int lw = std::max(1, w / 6);
    auto thick = [&](int weight) { return weight == 2 ? 2 * lw : weight == 3 ? 3 * lw : lw; };

    // 円弧 ╭╮╯╰。╭ を基準に描き、左右・上下を反転して残りを作る。
    if (cp >= 0x256D && cp <= 0x2570) {
        const bool fx = (cp == 0x256E || cp == 0x256F);
        const bool fy = (cp == 0x256F || cp == 0x2570);
        const int  vx = (w - lw) / 2;
        const int  hy = (h - lw) / 2;
        const int  r  = std::max(1, std::min(w - vx, h - hy) - lw);
        auto put = [&](int x, int y, int pw, int ph) {
            fill(fx ? w - x - pw : x, fy ? h - y - ph : y, pw, ph, 255);
        };
        put(vx, hy + r, lw, h - hy - r);  // 下へ
        put(vx + r, hy, w - vx - r, lw);  // 右へ
        const int steps = 4 * r;
        for (int i = 0; i <= steps; ++i) {
            const double t = (M_PI / 2) * i / steps;
            const int    x = vx + r - static_cast<int>(std::lround(r * std::cos(t)));
            const int    y = hy + r - static_cast<int>(std::lround(r * std::sin(t)));
            put(x, y, lw, lw);
        }
        return;
    }
    // 斜線 ╱╲╳。
    if (cp >= 0x2571 && cp <= 0x2573) {
        const int steps = std::max(w, h) * 2;
        for (int i = 0; i <= steps; ++i) {
            const int x = (w - lw) * i / steps;
            const int y = (h - lw) * i / steps;
            if (cp != 0x2572) fill(w - lw - x, y, lw, lw, 255);  // ╱
            if (cp != 0x2571) fill(x, y, lw, lw, 255);           // ╲
        }
        return;
    }

    const uint8_t arms = kBoxArms[cp - 0x2500];
    const int up = arms & 3, right = (arms >> 2) & 3, down = (arms >> 4) & 3, left = (arms >> 6) & 3;
    // 中心で重ねる幅。腕の中で一番太いものに合わせると、細線と太線の継ぎ目に隙間ができない。
    const int tv = std::max(thick(up), thick(down));     // 縦の腕が中心で占める幅
    const int th = std::max(thick(left), thick(right));  // 横の腕が中心で占める高さ
    const int n  = dashes(cp);

    // 横の腕。x0-x1 の範囲に weight の線を引く。
    auto hline = [&](int x0, int x1, int weight) {
        const int t = thick(weight);
        const int y = (h - t) / 2;
        auto seg = [&](int a, int b) {
            if (weight == 3) {
                fill(a, y, b - a, lw, 255);
                fill(a, y + 2 * lw, b - a, lw, 255);
            } else {
                fill(a, y, b - a, t, 255);
            }
        };
        if (n == 0) {
            seg(x0, x1);
            return;
        }
        // 破線: セルを n 等分し、各区間の前半を引く（隣のセルとも同じ周期になる）。
        for (int i = 0; i < n; ++i) {
            const int a = w * i / n;
            const int b = a + std::max(1, w / (2 * n));
            seg(a, b);
        }
    };
    auto vline = [&](int y0, int y1, int weight) {
        const int t = thick(weight);
        const int x = (w - t) / 2;
        auto seg = [&](int a, int b) {
            if (weight == 3) {
                fill(x, a, lw, b - a, 255);
                fill(x + 2 * lw, a, lw, b - a, 255);
            } else {
                fill(x, a, t, b - a, 255);
            }
        };
        if (n == 0) {
            seg(y0, y1);
            return;
        }
        for (int i = 0; i < n; ++i) {
            const int a = h * i / n;
            const int b = a + std::max(1, h / (2 * n));
            seg(a, b);
        }
    };
    // 破線は腕が 1 方向 (横か縦) だけなので、セル全体に 1 本引けば足りる。
    if (n != 0) {
        if (left) hline(0, w, left);
        if (up) vline(0, h, up);
        return;
    }
    // ponytail: 二重線の交点は 2 本ずつ重ねるだけで、角を内側・外側で繋ぎ分けない (#100)。
    //           ╔ が井桁に見える。4 つの TUI は二重線を使っていないので、使うものが出たら直す。
    if (left) hline(0, (w + tv) / 2, left);
    if (right) hline((w - tv) / 2, w, right);
    if (up) vline(0, (h + th) / 2, up);
    if (down) vline((h - th) / 2, h, down);
}

// ブロック要素 U+2580-259F。
void draw_block(uint32_t cp, int w, int h, const Fill& fill)
{
    const int hw = w / 2, hh = h / 2;
    auto quads = [&](bool ul, bool ur, bool ll, bool lr) {
        if (ul) fill(0, 0, hw, hh, 255);
        if (ur) fill(hw, 0, w - hw, hh, 255);
        if (ll) fill(0, hh, hw, h - hh, 255);
        if (lr) fill(hw, hh, w - hw, h - hh, 255);
    };
    if (cp == 0x2580) {
        fill(0, 0, w, hh, 255);  // ▀
    } else if (cp >= 0x2581 && cp <= 0x2588) {
        const int t = h * static_cast<int>(cp - 0x2580) / 8;  // ▁ - █ (下から n/8)
        fill(0, h - t, w, t, 255);
    } else if (cp >= 0x2589 && cp <= 0x258F) {
        const int t = std::max(1, w * static_cast<int>(0x2590 - cp) / 8);  // ▉ - ▏ (左から n/8)
        fill(0, 0, t, h, 255);
    } else if (cp == 0x2590) {
        fill(hw, 0, w - hw, h, 255);  // ▐
    } else if (cp >= 0x2591 && cp <= 0x2593) {
        fill(0, 0, w, h, static_cast<uint8_t>(64 * (cp - 0x2590)));  // ░ ▒ ▓
    } else if (cp == 0x2594) {
        fill(0, 0, w, std::max(1, h / 8), 255);  // ▔
    } else if (cp == 0x2595) {
        const int t = std::max(1, w / 8);
        fill(w - t, 0, t, h, 255);  // ▕
    } else {
        switch (cp) {
            case 0x2596: quads(false, false, true, false); break;  // ▖
            case 0x2597: quads(false, false, false, true); break;  // ▗
            case 0x2598: quads(true, false, false, false); break;  // ▘
            case 0x2599: quads(true, false, true, true); break;    // ▙
            case 0x259A: quads(true, false, false, true); break;   // ▚
            case 0x259B: quads(true, true, true, false); break;    // ▛
            case 0x259C: quads(true, true, false, true); break;    // ▜
            case 0x259D: quads(false, true, false, false); break;  // ▝
            case 0x259E: quads(false, true, true, false); break;   // ▞
            case 0x259F: quads(false, true, true, true); break;    // ▟
            default: break;
        }
    }
}

// 点字 U+2800-28FF。2 列 x 4 段の点。ビットと位置の対応は Unicode の順 (1-2-3-7 が左列)。
void draw_braille(uint32_t cp, int w, int h, const Fill& fill)
{
    constexpr int kCol[8] = {0, 0, 0, 1, 1, 1, 0, 1};
    constexpr int kRow[8] = {0, 1, 2, 0, 1, 2, 3, 3};
    const int     s       = std::max(1, w / 4);
    const unsigned bits   = cp - 0x2800;
    for (int i = 0; i < 8; ++i) {
        if (!(bits & (1u << i))) continue;
        const int cx = w * (2 * kCol[i] + 1) / 4;
        const int cy = h * (2 * kRow[i] + 1) / 8;
        fill(cx - s / 2, cy - s / 2, s, s, 255);
    }
}

}  // namespace

void draw(uint32_t cp, int w, int h, const Fill& fill)
{
    if (cp >= 0x2500 && cp <= 0x257F) draw_box(cp, w, h, fill);
    else if (cp >= 0x2580 && cp <= 0x259F) draw_block(cp, w, h, fill);
    else if (cp >= 0x2800 && cp <= 0x28FF) draw_braille(cp, w, h, fill);
}

}  // namespace glyph
