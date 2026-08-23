// 座標変換のホストテスト。ここがずれると「押した場所と反応する場所が違う」形で壊れる。
#include <cstdint>

#include "rotate.hpp"

#include <cstdio>
#include <cstdlib>

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

// 180 度反転 (#15)。**normal に 180 度回した座標を通したものと一致すること**を
// 不変量にする (両方を手で書くと片方だけ直して 180 度ずれる)。
void test_flip()
{
    rot::Panel n;  // normal
    rot::Panel f;
    f.flipped = true;

    for (int ly = 0; ly < n.landscape_h(); ly += 37) {
        for (int lx = 0; lx < n.landscape_w(); lx += 53) {
            int fx = 0, fy = 0, ex = 0, ey = 0;
            rot::landscape_to_native(f, lx, ly, &fx, &fy);
            // 期待値: normal に 180 度回した座標を通す
            rot::landscape_to_native(n, n.landscape_w() - 1 - lx, n.landscape_h() - 1 - ly, &ex,
                                     &ey);
            CHECK(fx == ex);
            CHECK(fy == ey);
            // 範囲に収まる
            CHECK(fx >= 0 && fx < n.native_w);
            CHECK(fy >= 0 && fy < n.native_h);
            // 往復する
            int rx = 0, ry = 0;
            rot::native_to_landscape(f, fx, fy, &rx, &ry);
            CHECK(rx == lx);
            CHECK(ry == ly);
        }
    }
    // 四隅: 反転すると横向きの左上はネイティブの左下側へ行く
    int nx = 0, ny = 0;
    rot::landscape_to_native(f, 0, 0, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 1279);
    rot::landscape_to_native(f, 1279, 719, &nx, &ny);
    CHECK(nx == 719);
    CHECK(ny == 0);
}

// **矩形の変換も反転で確かめる。** present() が実際に呼ぶのはこれで、符号を間違えると
// PPA の出力先ブロックがフレームバッファの外に出る (転送が ESP_ERR_INVALID_ARG で
// 落ちて何も映らないが、点の変換のテストは緑のまま通る)。
// 点と同じ不変量: 反転の矩形 == normal の「180 度回した矩形」。
void test_flip_rect()
{
    rot::Panel n;
    rot::Panel f;
    f.flipped = true;

    struct Rect {
        int x, y, w, h;
    };
    // 全画面、中央寄せ (既定の 960x700)、**キーボードを出したときの非対称な位置**
    const Rect rects[] = {{0, 0, 1280, 720}, {160, 10, 960, 700}, {320, 0, 640, 400},
                          {0, 0, 1, 1},      {1279, 719, 1, 1},   {320, 160, 640, 400}};
    for (const Rect& r : rects) {
        int fx = 0, fy = 0, fw = 0, fh = 0;
        rot::landscape_rect_to_native(f, r.x, r.y, r.w, r.h, &fx, &fy, &fw, &fh);
        // 期待値: normal に 180 度回した矩形を通す
        int ex = 0, ey = 0, ew = 0, eh = 0;
        rot::landscape_rect_to_native(n, n.landscape_w() - r.x - r.w, n.landscape_h() - r.y - r.h,
                                      r.w, r.h, &ex, &ey, &ew, &eh);
        CHECK(fx == ex);
        CHECK(fy == ey);
        CHECK(fw == ew);
        CHECK(fh == eh);
        // **フレームバッファの外に出ない。** ここが本命 (PPA が弾く条件)
        CHECK(fx >= 0);
        CHECK(fy >= 0);
        CHECK(fw > 0 && fh > 0);
        CHECK(fx + fw <= n.native_w);
        CHECK(fy + fh <= n.native_h);
        // 90 度回るので幅と高さが入れ替わる
        CHECK(fw == r.h);
        CHECK(fh == r.w);
    }
}

void test_dimensions()
{
    rot::Panel p;  // 720x1280
    CHECK(p.landscape_w() == 1280);
    CHECK(p.landscape_h() == 720);
}

void test_corners()
{
    rot::Panel p;
    int nx = 0, ny = 0;

    // 横向きの左上 → ネイティブの右上側
    rot::landscape_to_native(p, 0, 0, &nx, &ny);
    CHECK(nx == 719);
    CHECK(ny == 0);

    // 横向きの右上
    rot::landscape_to_native(p, 1279, 0, &nx, &ny);
    CHECK(nx == 719);
    CHECK(ny == 1279);

    // 横向きの左下
    rot::landscape_to_native(p, 0, 719, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 0);

    // 横向きの右下
    rot::landscape_to_native(p, 1279, 719, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 1279);
}

// 720/1280 のリテラルだけで固めていると、native_w と landscape_h() の
// 取り違えを検出できない（Tab5 では値が同じ）。非正方で 1 本固定する。
void test_non_square_panel()
{
    rot::Panel p{100, 300};  // native 100x300 -> landscape 300x100
    CHECK(p.landscape_w() == 300);
    CHECK(p.landscape_h() == 100);

    int nx = 0, ny = 0;
    rot::landscape_to_native(p, 0, 0, &nx, &ny);
    CHECK(nx == 99);  // landscape_h()-1。native_w を使っても同じなので下で分ける
    CHECK(ny == 0);

    rot::landscape_to_native(p, 299, 99, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 299);

    // landscape_w() (=300) を取り違えて使うと nx が範囲外 (299) になる
    rot::landscape_to_native(p, 0, 99, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 0);

    int bx = 0, by = 0;
    rot::native_to_landscape(p, nx, ny, &bx, &by);
    CHECK(bx == 0);
    CHECK(by == 99);
}

// 往復して戻ることだけを見る。**向きは見ていない**（任意の全単射で通るので、
// 180 度ずれていてもここは通る)。向きの固定は test_corners と test_rect の仕事。
void test_round_trip()
{
    rot::Panel p;
    // 全体を粗くスキャンして、往復して元に戻ることを確かめる
    for (int lx = 0; lx < p.landscape_w(); lx += 7) {
        for (int ly = 0; ly < p.landscape_h(); ly += 11) {
            int nx = 0, ny = 0, bx = 0, by = 0;
            rot::landscape_to_native(p, lx, ly, &nx, &ny);
            // ネイティブの範囲に収まっていること
            CHECK(nx >= 0 && nx < p.native_w);
            CHECK(ny >= 0 && ny < p.native_h);
            rot::native_to_landscape(p, nx, ny, &bx, &by);
            CHECK(bx == lx);
            CHECK(by == ly);
        }
    }
}

void test_rect()
{
    rot::Panel p;
    int nx = 0, ny = 0, nw = 0, nh = 0;

    // 端末の 1 行（横 1280 x 高さ 24）は、ネイティブでは幅 24 の縦帯になる
    rot::landscape_rect_to_native(p, 0, 0, 1280, 24, &nx, &ny, &nw, &nh);
    CHECK(nw == 24);
    CHECK(nh == 1280);
    // 1 行目は右端の縦帯（native x = 720-24 .. 719）
    CHECK(nx == 720 - 24);
    CHECK(ny == 0);

    // 2 行目（ly = 24）は x が 24 手前にずれる
    rot::landscape_rect_to_native(p, 0, 24, 1280, 24, &nx, &ny, &nw, &nh);
    CHECK(nx == 720 - 48);
    CHECK(ny == 0);
    CHECK(nw == 24);
    CHECK(nh == 1280);

    // 行の一部（差分転送で使う範囲）
    rot::landscape_rect_to_native(p, 120, 48, 60, 24, &nx, &ny, &nw, &nh);
    CHECK(nw == 24);
    CHECK(nh == 60);
    // 横向きの y=48..71 は、ネイティブでは x = 720-1-71 .. 720-1-48
    CHECK(nx == 720 - 1 - 71);
    CHECK(ny == 120);

    // 変換した矩形がネイティブの範囲に収まること
    CHECK(nx >= 0 && nx + nw <= p.native_w);
    CHECK(ny >= 0 && ny + nh <= p.native_h);

    // 画面いっぱいの矩形
    rot::landscape_rect_to_native(p, 0, 0, 1280, 720, &nx, &ny, &nw, &nh);
    CHECK(nx == 0 && ny == 0 && nw == 720 && nh == 1280);
}

}  // namespace

int main()
{
    test_flip();
    test_flip_rect();
    test_dimensions();
    test_corners();
    test_non_square_panel();
    test_round_trip();
    test_rect();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
