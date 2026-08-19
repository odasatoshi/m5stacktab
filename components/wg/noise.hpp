#pragma once
// WireGuard の Noise IK ハンドシェイク。ESP-IDF に依存せず、暗号は差し替え可能な形にする
// （ホストでは mbedTLS、実機でも mbedTLS。X25519 と ChaCha20-Poly1305 は外から注入する）。
//
// 対応するのは WireGuard プロトコルのハンドシェイク部分:
//   - MessageInitiation  (type 1, 148 bytes)
//   - MessageResponse    (type 2, 92 bytes)
//   - トランスポート鍵の導出
// データパケット (type 4) の暗号化は transport.hpp。
#include <cstddef>
#include <cstdint>

namespace wg {

constexpr size_t kKeyLen   = 32;
constexpr size_t kTagLen   = 16;
constexpr size_t kTimestampLen = 12;  // TAI64N

constexpr uint8_t kMsgInitiation = 1;
constexpr uint8_t kMsgResponse   = 2;
constexpr uint8_t kMsgCookie     = 3;
constexpr uint8_t kMsgTransport  = 4;

// 暗号プリミティブの注入。実機とホストで同じ実装を使うが、テストで差し替えられるようにする。
struct Crypto {
    // X25519. 失敗時 false（全ゼロ共有鍵など）。
    bool (*dh)(uint8_t out[kKeyLen], const uint8_t priv[kKeyLen], const uint8_t pub[kKeyLen]);
    // 公開鍵の導出。
    bool (*dh_pubkey)(uint8_t out[kKeyLen], const uint8_t priv[kKeyLen]);
    // ChaCha20-Poly1305 AEAD (nonce は 12 バイト、counter を little endian で下位に置く)。
    bool (*aead_encrypt)(uint8_t* out, const uint8_t key[kKeyLen], uint64_t counter,
                         const uint8_t* plain, size_t plain_len, const uint8_t* ad, size_t ad_len);
    bool (*aead_decrypt)(uint8_t* out, const uint8_t key[kKeyLen], uint64_t counter,
                         const uint8_t* cipher, size_t cipher_len, const uint8_t* ad,
                         size_t ad_len);
    // 乱数。失敗したら false。全ゼロを返して続行してはいけない（予測可能な鍵になる）。
    bool (*random_bytes)(uint8_t* out, size_t len);
};

struct Keypair {
    uint8_t send[kKeyLen]    = {};
    uint8_t recv[kKeyLen]    = {};
    uint32_t local_index     = 0;
    uint32_t remote_index    = 0;
    bool     initiator       = false;
};

// ハンドシェイクの途中状態。
class Handshake {
public:
    Handshake(const Crypto& crypto) : c_(crypto) {}

    // 自分の静的鍵と相手の静的公開鍵を設定する。psk は省略可（全ゼロ扱い）。
    // 公開鍵の導出に失敗したら false（そのまま進むと mac1 と初期ハッシュが壊れる）。
    bool set_keys(const uint8_t static_priv[kKeyLen], const uint8_t peer_static_pub[kKeyLen],
                  const uint8_t psk[kKeyLen] = nullptr);

    // MessageInitiation を作る。out は 148 バイト。
    bool create_initiation(uint8_t out[148], uint32_t local_index, const uint8_t timestamp[kTimestampLen]);

    // 相手からの MessageResponse を処理してトランスポート鍵を得る。
    bool consume_response(const uint8_t in[92], Keypair& out);

    // 受信側の処理（サーバ役。テストで自分同士を繋ぐのに使う）。
    bool consume_initiation(const uint8_t in[148], uint8_t peer_static_out[kKeyLen],
                            uint8_t timestamp_out[kTimestampLen]);
    bool create_response(uint8_t out[92], uint32_t local_index, Keypair& keypair_out);

    uint32_t local_index() const { return local_index_; }
    uint32_t remote_index() const { return remote_index_; }

private:
    const Crypto& c_;

    uint8_t static_priv_[kKeyLen]  = {};
    uint8_t static_pub_[kKeyLen]   = {};
    uint8_t peer_static_[kKeyLen]  = {};
    uint8_t psk_[kKeyLen]          = {};

    uint8_t ephemeral_priv_[kKeyLen] = {};
    uint8_t ephemeral_pub_[kKeyLen]  = {};
    uint8_t peer_ephemeral_[kKeyLen] = {};

    uint8_t  chaining_key_[kKeyLen] = {};
    uint8_t  hash_[kKeyLen]         = {};
    uint32_t local_index_  = 0;
    uint32_t remote_index_ = 0;
};

// mbedTLS を使う既定の Crypto 実装を返す（ホストでも実機でも同じ）。
const Crypto& default_crypto();

}  // namespace wg
