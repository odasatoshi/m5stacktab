#pragma once
// ts2021 の Noise の内側で話す HTTP/2 (h2c) の最小クライアント。
//
// nghttp2 は要らない。制御プレーンとのやり取りに必要なのは
// 「POST してボディを送り、DATA を読み続ける」だけなので、フレームの組み立てと
// 最小限のパースで足りる。
//
// ソケット I/O は持たない（バイト列を作る・読むだけ）のでホストでテストできる。
#include <cstddef>
#include <cstdint>

namespace ts {

// フレーム種別（必要なものだけ）。
enum class H2Type : uint8_t {
    kData         = 0x00,
    kHeaders      = 0x01,
    kRstStream    = 0x03,
    kSettings     = 0x04,
    kPing         = 0x06,
    kGoaway       = 0x07,
    kWindowUpdate = 0x08,
};

constexpr size_t kH2FrameHeader = 9;
// クライアントのコネクションプリフェイス。
constexpr char kH2Preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr size_t kH2PrefaceLen = sizeof(kH2Preface) - 1;

// フラグ
constexpr uint8_t kFlagEndStream  = 0x01;
constexpr uint8_t kFlagAck        = 0x01;
constexpr uint8_t kFlagEndHeaders = 0x04;

struct H2Frame {
    H2Type         type{};
    uint8_t        flags = 0;
    uint32_t       stream_id = 0;
    const uint8_t* payload = nullptr;
    size_t         payload_len = 0;
};

// プリフェイス + 初期 SETTINGS を書く。long-poll のために受信ウィンドウを大きく取る。
size_t h2_build_preface(uint8_t* out, size_t cap);

// 受け取った SETTINGS への ACK。
size_t h2_build_settings_ack(uint8_t* out, size_t cap);

// PING への ACK（サーバが生存確認してくる）。
size_t h2_build_ping_ack(uint8_t* out, size_t cap, const uint8_t opaque[8]);

// ウィンドウ更新。これを送らないと 64KB で止まる（long-poll では致命的）。
size_t h2_build_window_update(uint8_t* out, size_t cap, uint32_t stream_id, uint32_t increment);

// POST リクエスト（HEADERS + DATA）。body_len が 0 なら HEADERS だけで END_STREAM を立てる。
// HPACK は静的テーブルのインデックスとリテラル（Huffman なし）だけを使う。
size_t h2_build_post(uint8_t* out, size_t cap, uint32_t stream_id, const char* authority,
                     const char* path, const uint8_t* body, size_t body_len);

// 受信バッファから 1 フレーム取り出す。
// 戻り値 true でフレームを取得、consumed に消費バイト数。
// フレームが未完成なら false を返して consumed = 0（もっと読む必要がある）。
bool h2_parse_frame(const uint8_t* in, size_t len, H2Frame* out, size_t* consumed);

// HEADERS ペイロードの先頭が「:status 200」を表す indexed field (0x88) かを見る。
// HPACK のフルデコードはしない（必要なのは成否の判定だけ）。
bool h2_headers_is_status_200(const uint8_t* payload, size_t len);

}  // namespace ts
