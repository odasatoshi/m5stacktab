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

    // 横向きの左上 → ネイティブの左下側
    rot::landscape_to_native(p, 0, 0, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 1279);

    // 横向きの右上
    rot::landscape_to_native(p, 1279, 0, &nx, &ny);
    CHECK(nx == 0);
    CHECK(ny == 0);

    // 横向きの左下
    rot::landscape_to_native(p, 0, 719, &nx, &ny);
    CHECK(nx == 719);
    CHECK(ny == 1279);

    // 横向きの右下
    rot::landscape_to_native(p, 1279, 719, &nx, &ny);
    CHECK(nx == 719);
    CHECK(ny == 0);
}

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
    CHECK(nx == 0);
    CHECK(ny == 0);

    // 2 行目（ly = 24）は x が 24 ずれる
    rot::landscape_rect_to_native(p, 0, 24, 1280, 24, &nx, &ny, &nw, &nh);
    CHECK(nx == 24);
    CHECK(ny == 0);
    CHECK(nw == 24);
    CHECK(nh == 1280);

    // 行の一部（差分転送で使う範囲）
    rot::landscape_rect_to_native(p, 120, 48, 60, 24, &nx, &ny, &nw, &nh);
    CHECK(nw == 24);
    CHECK(nh == 60);
    CHECK(nx == 48);
    // 横向きの x=120..179 は、ネイティブでは y = 1280-1-179 .. 1280-1-120
    CHECK(ny == 1280 - 1 - 179);

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
    test_dimensions();
    test_corners();
    test_round_trip();
    test_rect();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
