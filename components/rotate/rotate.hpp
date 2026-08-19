#pragma once
// 横向き表示のための座標変換。
//
// Tab5 のパネルはネイティブ縦 (720x1280)。M5GFX の setRotation(1) に任せると
// 転送がピクセル単位の座標変換になって 3.3 倍遅くなるので、回転は PPA (ハードウェア) に
// 任せて、座標の対応だけ自分で持つ。
//
// この変換を間違えると描画とタッチがずれる（見た目では気づきにくく、押した場所と
// 反応する場所が違うだけになる）ので、純関数にしてテストで固定する。
#include <cstdint>

namespace rot {

// パネルのネイティブサイズ。
struct Panel {
    int native_w = 720;
    int native_h = 1280;

    // 横向きで見たときの画面サイズ。
    int landscape_w() const { return native_h; }
    int landscape_h() const { return native_w; }
};

// 横向きの座標 (lx, ly) をネイティブ座標 (nx, ny) に変換する。
// M5GFX の setRotation(1) と同じ向きにする（そうしないと今までの画面と上下左右が変わる）。
void landscape_to_native(const Panel& p, int lx, int ly, int* nx, int* ny);

// 逆変換。今のところ往復テストと、将来 rotation 0 で生のタッチを扱う場合のために置いてある
// （現状の実装は M5GFX の rotation 1 のまま getTouch を使うので、タッチは既に横向き座標で来る）。
void native_to_landscape(const Panel& p, int nx, int ny, int* lx, int* ly);

// 横向きの矩形 (lx, ly, lw, lh) を PPA の出力先ブロック (nx, ny, nw, nh) に変換する。
// 90 度回転するので幅と高さが入れ替わる。
void landscape_rect_to_native(const Panel& p, int lx, int ly, int lw, int lh, int* nx, int* ny,
                              int* nw, int* nh);

}  // namespace rot
