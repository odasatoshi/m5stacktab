// term_glyphs のホストテスト (#100)。ビットマップに描かせて、隣のセルとの継ぎ目を確かめる。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -I main
//       -o /tmp/test_term_glyphs main/test_term_glyphs.cpp main/term_glyphs.cpp && /tmp/test_term_glyphs
#include "term_glyphs.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_checks = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            std::abort();                                                             \
        }                                                                             \
    } while (0)

// 実機のセル (efontJA_24 の半角) と同じ寸法。
constexpr int kW = 12;
constexpr int kH = 24;

struct Bitmap {
    std::vector<uint8_t> px = std::vector<uint8_t>(kW * kH, 0);
    uint8_t at(int x, int y) const { return px[y * kW + x]; }
    int     count() const
    {
        int n = 0;
        for (auto v : px) n += v != 0;
        return n;
    }
};

Bitmap render(uint32_t cp)
{
    Bitmap b;
    glyph::draw(cp, kW, kH, [&](int x, int y, int w, int h, uint8_t a) {
        // セルの外に塗らない (隣のセルを壊す)。
        CHECK(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= kW && y + h <= kH);
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) b.px[j * kW + i] = a;
    });
    return b;
}

// 端の 1 列 (x 固定) / 1 行 (y 固定) の塗り方。
std::vector<uint8_t> column(const Bitmap& b, int x)
{
    std::vector<uint8_t> v;
    for (int y = 0; y < kH; ++y) v.push_back(b.at(x, y));
    return v;
}
std::vector<uint8_t> row(const Bitmap& b, int y)
{
    std::vector<uint8_t> v;
    for (int x = 0; x < kW; ++x) v.push_back(b.at(x, y));
    return v;
}

// 罫線の継ぎ目: 右に腕がある字の右端は、同じ太さの横線 (─ ━ ═) の左端と一致する。
// 下に腕がある字の下端は、同じ太さの縦線 (│ ┃ ║) の上端と一致する。左・上も同様。
// 円弧 ╭╮╯╰ も細線の腕として扱う。破線は周期で途切れるので対象外。
void test_box_joins()
{
    const Bitmap h[4] = {{}, render(0x2500), render(0x2501), render(0x2550)};
    const Bitmap v[4] = {{}, render(0x2502), render(0x2503), render(0x2551)};
    struct Arms {
        int up, right, down, left;
    };
    auto arms_of = [](uint32_t cp) -> Arms {
        switch (cp) {
            case 0x256D: return {0, 1, 1, 0};
            case 0x256E: return {0, 0, 1, 1};
            case 0x256F: return {1, 0, 0, 1};
            case 0x2570: return {1, 1, 0, 0};
            default: break;
        }
        // 円弧以外は端に描かれたものを見る。表の値は下で既知の字を使って確かめる。
        return {-1, -1, -1, -1};
    };
    int joins = 0;
    for (uint32_t cp = 0x2500; cp <= 0x2570; ++cp) {
        if ((cp >= 0x2504 && cp <= 0x250B) || (cp >= 0x254C && cp <= 0x254F)) continue;  // 破線
        const Bitmap b = render(cp);
        CHECK(b.count() > 0);
        Arms a = arms_of(cp);
        if (a.up < 0) {
            // 端に何か描かれていれば腕がある。その太さは基準の線と一致しなければならない。
            auto match = [&](const std::vector<uint8_t>& edge, const Bitmap* ref, bool vertical_ref,
                             int ref_index) {
                bool any = false;
                for (auto p : edge) any |= p != 0;
                if (!any) return;
                bool ok = false;
                for (int w = 1; w <= 3; ++w) {
                    const auto r = vertical_ref ? row(ref[w], ref_index) : column(ref[w], ref_index);
                    ok |= (r == edge);
                }
                CHECK(ok);
                ++joins;
            };
            match(column(b, kW - 1), h, false, 0);  // 右端 ↔ 横線の左端
            match(column(b, 0), h, false, kW - 1);  // 左端 ↔ 横線の右端
            match(row(b, kH - 1), v, true, 0);      // 下端 ↔ 縦線の上端
            match(row(b, 0), v, true, kH - 1);      // 上端 ↔ 縦線の下端
        } else {
            if (a.right) CHECK(column(b, kW - 1) == column(h[1], 0));
            if (a.left) CHECK(column(b, 0) == column(h[1], kW - 1));
            if (a.down) CHECK(row(b, kH - 1) == row(v[1], 0));
            if (a.up) CHECK(row(b, 0) == row(v[1], kH - 1));
            if (!a.right) CHECK(column(b, kW - 1) == std::vector<uint8_t>(kH, 0));
            if (!a.up) CHECK(row(b, 0) == std::vector<uint8_t>(kW, 0));
            joins += 2;
        }
    }
    CHECK(joins > 200);

    // 表の値を既知の字で確かめる (腕の有無と太さ)。
    auto has_right = [](const Bitmap& b) { return column(b, kW - 1) != std::vector<uint8_t>(kH, 0); };
    auto has_up    = [](const Bitmap& b) { return row(b, 0) != std::vector<uint8_t>(kW, 0); };
    CHECK(has_right(render(0x250C)) && !has_up(render(0x250C)));  // ┌
    CHECK(!has_right(render(0x2518)) && has_up(render(0x2518)));  // ┘
    CHECK(column(render(0x2547), kW - 1) == column(h[2], 0));      // ╇ の右は太線
    CHECK(row(render(0x2547), kH - 1) == row(v[1], 0));            // ╇ の下は細線
    CHECK(column(render(0x255E), kW - 1) == column(h[3], 0));      // ╞ の右は二重線
}

void test_blocks_and_braille()
{
    CHECK(render(0x2588).count() == kW * kH);              // █
    CHECK(render(0x2580).count() == kW * kH / 2);          // ▀
    CHECK(render(0x2580).at(0, 0) && !render(0x2580).at(0, kH - 1));
    CHECK(render(0x2584).count() == kW * kH / 2);          // ▄
    CHECK(render(0x258C).count() == kW * kH / 2);          // ▌
    CHECK(render(0x2590).count() == kW * kH / 2);          // ▐
    CHECK(render(0x2591).at(5, 5) == 64);                  // ░ は薄く塗る
    CHECK(render(0x259B).count() == kW * kH * 3 / 4);      // ▛
    CHECK(render(0x2800).count() == 0);                    // 空の点字
    const Bitmap all = render(0x28FF);
    CHECK(all.count() > 0);
    CHECK(render(0x2801).count() * 8 == all.count());  // 8 点が重ならない
}

void test_substitute()
{
    CHECK(glyph::substitute(0x23FA) == 0x25CF);  // ⏺ → ●
    CHECK(glyph::substitute(0x276F) == '>');     // ❯
    CHECK(glyph::substitute(0x23BF) == 0x2514);  // ⎿ → └ (自前で描く字)
    CHECK(glyph::is_drawn(glyph::substitute(0x23BF)));
    CHECK(glyph::substitute('A') == 0);
    CHECK(glyph::substitute(0x3042) == 0);
}

}  // namespace

int main()
{
    test_box_joins();
    test_blocks_and_braille();
    test_substitute();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
