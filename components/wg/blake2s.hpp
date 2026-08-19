#pragma once
// BLAKE2s (RFC 7693)。mbedTLS には入っていないので自前で持つ。
// WireGuard の Noise IK がハッシュと HMAC/HKDF に使う。
// ESP-IDF に依存しないのでホストでテストできる。
#include <cstddef>
#include <cstdint>

namespace wg {

class Blake2s {
public:
    static constexpr size_t kMaxDigest = 32;
    static constexpr size_t kBlockSize = 64;

    // digest_len は 1..32。key を渡すと keyed BLAKE2s (HMAC 相当) になる。
    void init(size_t digest_len = 32, const uint8_t* key = nullptr, size_t key_len = 0);
    void update(const uint8_t* data, size_t len);
    void final(uint8_t* out);

    size_t digest_len() const { return digest_len_; }

private:
    void compress(const uint8_t block[kBlockSize], bool last);

    uint32_t h_[8]      = {};
    uint8_t  buf_[kBlockSize] = {};
    size_t   buf_len_   = 0;
    uint64_t counter_   = 0;
    size_t   digest_len_ = 32;
};

// 一発で計算する版。
void blake2s(uint8_t* out, size_t out_len, const uint8_t* data, size_t len,
             const uint8_t* key = nullptr, size_t key_len = 0);

// HMAC-BLAKE2s（WireGuard の HKDF が使うのは keyed hash ではなく通常の HMAC 構成）。
void hmac_blake2s(uint8_t out[32], const uint8_t* key, size_t key_len, const uint8_t* data,
                  size_t len);

// WireGuard の KDF (HKDF-BLAKE2s)。n は 1..3。
void kdf(const uint8_t* key, size_t key_len, const uint8_t* input, size_t input_len, int n,
         uint8_t* out1, uint8_t* out2, uint8_t* out3);

}  // namespace wg
