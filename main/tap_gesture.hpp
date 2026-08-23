#pragma once
// 端末領域のダブルタップ判定 (#54)。画面キーボードの表示を切り替える。
//
// **同じ領域に縦スワイプ（スクロールバック）が既に居る**ので、判定は
// 「タップだったか」を厳しく見る側に倒してある。取りこぼしても指をもう一度
// 動かせば済むが、誤爆すると打っている最中にキーボードが消える。
//
// ESP-IDF に依存させないのでホストでテストできる（実機のグローバルを触らない）。
#include <cstdint>
#include <cstdlib>

// 動いてよい距離 (px)。**セルの高さ (24px) より小さくない値は使えない** —
// 大きくすると「2 行スクロールしたのに動いていない扱い」になり、
// 2 回引いただけでキーボードが消える。距離だけに頼らず view_offset も見る。
constexpr int kDoubleTapSlop = 24;
// **テストはこの定数を基準に書いてあるので、値を変えてもテストは緑のまま通る。**
// （実際に 60 に戻して 29 チェック全部通ることを確認済み。）
// レビューが捕まえた回帰をここで止める。
static_assert(kDoubleTapSlop <= 24,
              "セルの高さ (24px) より大きいと、2 行スクロールしただけで"
              "「動いていない」扱いになり、2 回引くとキーボードが消える");
// これ以上長く触っていたらタップではない（長押し・ドラッグ）。
constexpr int64_t kTapMaxUs = 400000;
// 1 回目から 2 回目までの猶予。これ以上あいたら別のタップ。
constexpr int64_t kDoubleTapUs = 400000;

struct TapState {
    int64_t down_us     = 0;
    int     down_x      = 0;
    int     down_y      = 0;
    int     down_offset = 0;  // 押した時点のスクロールバック位置
    int64_t last_tap_us = 0;  // 0 = 直前のタップは無い
    int     last_tap_x  = 0;
    int     last_tap_y  = 0;
};

inline void tap_down(TapState* s, int x, int y, int64_t now_us, int scroll_offset)
{
    s->down_us     = now_us;
    s->down_x      = x;
    s->down_y      = y;
    s->down_offset = scroll_offset;
}

// 離した。ダブルタップが成立したら true。
//
// keyboard_top: 画面キーボードの帯の上端 y（隠しているなら画面の高さ）。
//   **ここより下は数えない。** キーボードの帯にはキーの無い隙間（左右の余白と
//   上端の候補表示）があり、そこは touch_down が false を返して端末領域として
//   流れてくる。数えると、変換候補のあたりを 2 回叩いただけでキーボードが消える。
inline bool tap_up(TapState* s, int x, int y, int64_t now_us, int scroll_offset, int keyboard_top)
{
    const bool in_keyboard = (s->down_y >= keyboard_top || y >= keyboard_top);
    const bool moved       = (std::abs(y - s->down_y) >= kDoubleTapSlop ||
                        std::abs(x - s->down_x) >= kDoubleTapSlop);
    // **距離ではなく結果も見る。** スクロールは cell 単位で量子化されるので、
    // 閾値の取り方次第で「動いたのに動いていない扱い」が残る。
    const bool scrolled = (scroll_offset != s->down_offset);
    const bool slow     = (now_us - s->down_us >= kTapMaxUs);

    if (in_keyboard || moved || scrolled || slow) {
        s->last_tap_us = 0;  // スワイプを挟んだら 1 回目は無かったことにする
        return false;
    }
    if (s->last_tap_us != 0 && (now_us - s->last_tap_us) < kDoubleTapUs &&
        std::abs(x - s->last_tap_x) < kDoubleTapSlop &&
        std::abs(y - s->last_tap_y) < kDoubleTapSlop) {
        s->last_tap_us = 0;  // 3 回目を 2 回目として扱わない
        return true;
    }
    s->last_tap_us = now_us;
    s->last_tap_x  = x;
    s->last_tap_y  = y;
    return false;
}
