#pragma once
// Tailscale のコントロールプロトコル ts2021 の Noise 層。
//
// WireGuard の Noise IK (components/wg) と似ているが、以下が決定的に違う:
//   - パターンは Noise_IK_25519_ChaChaPoly_BLAKE2s（psk なし。WireGuard は IKpsk2）
//   - prologue は "Tailscale Control Protocol v<capability version>"
//   - MessageInitiation は 101 バイト（148 ではない）。sender index / timestamp / mac1 / mac2 が無い
//   - MessageResponse は 51 バイト（92 ではない）
//   - **トランスポートのノンスがビッグエンディアン**（WireGuard はリトルエンディアン）
//   - トランスポートは TCP 上の長さプレフィックス方式で、平文は 4077 バイトごとに分割する
//
// ESP-IDF に依存しないのでホストでテストできる。
#include <cstddef>
#include <cstdint>
#include <string>

#include "noise.hpp"  // wg::Crypto / wg::kKeyLen などを共用する

namespace ts {

using wg::kKeyLen;
using wg::kTagLen;

constexpr size_t kInitiationLen = 101;
constexpr size_t kResponseLen   = 51;

// レコードの上限（control/controlbase/conn.go）。
constexpr size_t kMaxMessageSize   = 4096;
constexpr size_t kMaxCiphertextLen = 4093;
constexpr size_t kMaxPlaintextLen  = 4077;

constexpr uint8_t kMsgInitiation = 0x01;
constexpr uint8_t kMsgResponse   = 0x02;
constexpr uint8_t kMsgError      = 0x03;
constexpr uint8_t kMsgRecord     = 0x04;

// 送受信それぞれの鍵とカウンタ。
struct Session {
    uint8_t  tx[kKeyLen] = {};
    uint8_t  rx[kKeyLen] = {};
    uint64_t tx_counter  = 0;
    uint64_t rx_counter  = 0;
    bool     valid       = false;
};

class Handshake {
public:
    Handshake(const wg::Crypto& crypto) : c_(crypto) {}

    // machine key（Noise 用の自分の静的鍵）とサーバの Noise 公開鍵。
    // capability version は prologue に入るので、サーバと一致していないと復号に失敗する。
    bool init(const uint8_t machine_priv[kKeyLen], const uint8_t server_pub[kKeyLen],
              uint16_t capability_version);

    // MessageInitiation を作る（101 バイト）。HTTP の X-Tailscale-Handshake に base64 で載せる。
    bool create_initiation(uint8_t out[kInitiationLen]);

    // MessageResponse を処理してセッション鍵を得る（51 バイト）。
    bool consume_response(const uint8_t in[kResponseLen], Session& out);

    // 応答側（テストとローカル検証用）。
    bool consume_initiation(const uint8_t in[kInitiationLen], uint8_t peer_machine_out[kKeyLen]);
    bool create_response(uint8_t out[kResponseLen], Session& out_session);

private:
    const wg::Crypto& c_;
    uint8_t  machine_priv_[kKeyLen] = {};
    uint8_t  machine_pub_[kKeyLen]  = {};
    uint8_t  server_pub_[kKeyLen]   = {};
    uint8_t  ephemeral_priv_[kKeyLen] = {};
    uint8_t  ephemeral_pub_[kKeyLen]  = {};
    uint8_t  peer_ephemeral_[kKeyLen] = {};
    uint8_t  peer_machine_[kKeyLen]   = {};
    uint8_t  chaining_key_[kKeyLen]   = {};
    uint8_t  hash_[kKeyLen]           = {};
    uint16_t version_ = 0;
};

// Noise 確立後のレコード I/O。バイトストリームとして扱う（フレーム境界に意味はない）。
class Record {
public:
    Record(const wg::Crypto& crypto) : c_(crypto) {}

    void set_session(const Session& s) { s_ = s; }
    bool ready() const { return s_.valid; }

    // 平文を 1 レコードに包む。len は kMaxPlaintextLen 以下。返り値は出力長（0 = 失敗）。
    size_t seal(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len);

    // 受信バッファから 1 レコードを取り出す。
    // 戻り値: 平文長。consumed に消費したバイト数を返す。
    // レコードが未完成なら consumed = 0 を返す（もっと読む必要がある）。
    // 失敗時は consumed = SIZE_MAX を返す（接続を閉じるべき）。
    size_t open(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len, size_t* consumed);

    uint64_t tx_counter() const { return s_.tx_counter; }
    uint64_t rx_counter() const { return s_.rx_counter; }

private:
    const wg::Crypto& c_;
    Session           s_{};
};

// EarlyNoise（Noise 確立直後、HTTP/2 の前に来ることがある）。
// 先頭が "\xff\xff\xffTS" + uint32 BE 長 なら EarlyNoise、そうでなければ HTTP/2 の開始。
//
// 「EarlyNoise ではない」と「EarlyNoise だがまだ届いていない」を区別する。
// 混同すると、分割して届いたときに HTTP/2 を先に送ってしまい、
// \xff\xff\xff... が HTTP/2 のフレーム長として読まれて永久にストールする。
enum class EarlyNoiseResult {
    kNotPresent,   // HTTP/2 が始まっている
    kIncomplete,   // マジックは一致したが本体がまだ来ていない
    kFound,
};
EarlyNoiseResult parse_early_noise(const uint8_t* in, size_t len, std::string* json,
                                   size_t* consumed);

}  // namespace ts
