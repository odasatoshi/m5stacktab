#include <cstdint>
#include "rotate.hpp"

namespace rot {

// M5GFX の rotation 1 は「ネイティブを反時計回りに 90 度回して見る」向き。
// 横向きの (lx, ly) は、ネイティブでは x = ly、y = (landscape_w - 1 - lx) に対応する。
void landscape_to_native(const Panel& p, int lx, int ly, int* nx, int* ny)
{
    if (nx) *nx = ly;
    if (ny) *ny = p.landscape_w() - 1 - lx;
}

void native_to_landscape(const Panel& p, int nx, int ny, int* lx, int* ly)
{
    if (lx) *lx = p.landscape_w() - 1 - ny;
    if (ly) *ly = nx;
}

void landscape_rect_to_native(const Panel& p, int lx, int ly, int lw, int lh, int* nx, int* ny,
                              int* nw, int* nh)
{
    // 矩形の左上 (lx, ly) と右下 (lx+lw-1, ly+lh-1) を変換して、
    // ネイティブでの左上と大きさを求める。90 度回るので幅と高さが入れ替わる。
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    landscape_to_native(p, lx, ly, &x0, &y0);
    landscape_to_native(p, lx + lw - 1, ly + lh - 1, &x1, &y1);
    if (nx) *nx = (x0 < x1) ? x0 : x1;
    if (ny) *ny = (y0 < y1) ? y0 : y1;
    if (nw) *nw = lh;
    if (nh) *nh = lw;
}

}  // namespace rot
