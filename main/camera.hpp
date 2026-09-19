#pragma once
// MIPI-CSI カメラ (SC202CS / SC2356) (#77)。QR で接続先を取り込むための土台。
//
// **`display.init()` の後に呼ぶこと。** カメラの電源は P4 の GPIO ではなく
// IO エクスパンダ 0x43 の pin6 (CAM_EN) にあり、**M5GFX の Tab5 初期化が
// OUT_SET に `0b01110110` を書いて High にしている**（C6 の電源が 0x44 pin0 なのと
// 同じ構図）。自分で叩く必要は無いが、先に display.init() を呼ばないと電源が来ない。
#include <cstddef>
#include <cstdint>
#include <string>

#include <esp_err.h>

namespace cam {

struct Info {
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    uint32_t    pixelformat = 0;  // V4L2_PIX_FMT_*
    uint32_t    bytesperline = 0;
    uint32_t    sizeimage   = 0;
    std::string driver;           // VIDIOC_QUERYCAP の driver
    std::string card;
};

// カメラを立ち上げる。**2 回目以降は何もせず ESP_OK**（`camtest` を繰り返し叩ける）。
//
// **SCCB は M5GFX と同じ I2C を使う。** センサは G31/G32 = M5GFX が `lgfx::i2c` で
// 握っている I2C_NUM_1 にぶら下がっている。ここは画面・タッチと同じロックの下で
// 呼ぶこと（CLAUDE.md の「画面に触る経路は全部同じロックで守る」と同じ理由）。
esp_err_t init();

// 立ち上げ済みか。
bool ready();

// 今の設定を読む（VIDIOC_QUERYCAP / G_FMT）。init() が済んでいること。
esp_err_t probe(Info* out);

// 1 枚取り込んだ結果。**「本当に画が来たか」を数字で見るためのもの。**
// 目視できない環境で「全部 0 のバッファを掴んで成功と言う」のを防ぐ。
struct Frame {
    size_t   bytes = 0;
    uint32_t us    = 0;  // DQBUF までにかかった時間
    uint8_t  min   = 0;  // 輝度の最小 / 最大 / 平均（RGB565 の G を 8bit に伸ばす）
    uint8_t  max   = 0;
    uint8_t  mean  = 0;
};

// ストリームを開始して 1 枚取り、統計を返す。init() が済んでいること。
// **毎回開始と停止をする。** 常時回すと SCCB は無通信でも ISP と DMA が動き続け、
// 画面の描画と PSRAM の帯域を取り合う。QR は「シャッターを切る」使い方で足りる。
esp_err_t capture(Frame* out);

// V4L2 の 4 文字コードを "RGB565" のような表示用の文字列にする。
std::string fourcc(uint32_t v);

}  // namespace cam
