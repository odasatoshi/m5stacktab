#include "blake2s.hpp"

#include <algorithm>
#include <cstring>

namespace wg {
namespace {

constexpr uint32_t kIV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                             0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

constexpr uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

uint32_t load32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void store32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

void Blake2s::compress(const uint8_t block[kBlockSize], bool last)
{
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = load32(block + i * 4);

    uint32_t v[16];
    for (int i = 0; i < 8; ++i) v[i] = h_[i];
    for (int i = 0; i < 8; ++i) v[8 + i] = kIV[i];
    v[12] ^= static_cast<uint32_t>(counter_);
    v[13] ^= static_cast<uint32_t>(counter_ >> 32);
    if (last) v[14] = ~v[14];

    auto mix = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 12);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr(v[d] ^ v[a], 8);
        v[c] = v[c] + v[d];
        v[b] = rotr(v[b] ^ v[c], 7);
    };

    for (int r = 0; r < 10; ++r) {
        const uint8_t* s = kSigma[r];
        mix(0, 4, 8, 12, m[s[0]], m[s[1]]);
        mix(1, 5, 9, 13, m[s[2]], m[s[3]]);
        mix(2, 6, 10, 14, m[s[4]], m[s[5]]);
        mix(3, 7, 11, 15, m[s[6]], m[s[7]]);
        mix(0, 5, 10, 15, m[s[8]], m[s[9]]);
        mix(1, 6, 11, 12, m[s[10]], m[s[11]]);
        mix(2, 7, 8, 13, m[s[12]], m[s[13]]);
        mix(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; ++i) h_[i] ^= v[i] ^ v[8 + i];
}

void Blake2s::init(size_t digest_len, const uint8_t* key, size_t key_len)
{
    if (digest_len == 0 || digest_len > kMaxDigest) digest_len = kMaxDigest;
    if (key_len > 32) key_len = 32;

    digest_len_ = digest_len;
    counter_    = 0;
    buf_len_    = 0;
    std::memset(buf_, 0, sizeof(buf_));
    for (int i = 0; i < 8; ++i) h_[i] = kIV[i];
    // パラメータブロック: digest_len | key_len | fanout(1) | depth(1)
    h_[0] ^= static_cast<uint32_t>(digest_len) | (static_cast<uint32_t>(key_len) << 8) |
             (1u << 16) | (1u << 24);

    if (key_len > 0) {
        // 鍵は 64 バイトに 0 詰めした 1 ブロックとして先に流す。
        uint8_t block[kBlockSize] = {};
        std::memcpy(block, key, key_len);
        update(block, kBlockSize);
    }
}

void Blake2s::update(const uint8_t* data, size_t len)
{
    while (len > 0) {
        if (buf_len_ == kBlockSize) {
            // 最終ブロックかどうかは次の入力があるか分かってからでないと決まらないので、
            // バッファが満杯のときは「次がある」と分かった時点で流す。
            counter_ += kBlockSize;
            compress(buf_, false);
            buf_len_ = 0;
        }
        const size_t take = std::min(kBlockSize - buf_len_, len);
        std::memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
    }
}

void Blake2s::final(uint8_t* out)
{
    counter_ += buf_len_;
    std::memset(buf_ + buf_len_, 0, kBlockSize - buf_len_);
    compress(buf_, true);

    uint8_t full[kMaxDigest];
    for (int i = 0; i < 8; ++i) store32(full + i * 4, h_[i]);
    std::memcpy(out, full, digest_len_);
}

void blake2s(uint8_t* out, size_t out_len, const uint8_t* data, size_t len, const uint8_t* key,
             size_t key_len)
{
    Blake2s h;
    h.init(out_len, key, key_len);
    h.update(data, len);
    h.final(out);
}

void hmac_blake2s(uint8_t out[32], const uint8_t* key, size_t key_len, const uint8_t* data,
                  size_t len)
{
    uint8_t k[Blake2s::kBlockSize] = {};
    if (key_len > Blake2s::kBlockSize) {
        blake2s(k, 32, key, key_len);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[Blake2s::kBlockSize], opad[Blake2s::kBlockSize];
    for (size_t i = 0; i < Blake2s::kBlockSize; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    uint8_t inner[32];
    {
        Blake2s h;
        h.init(32);
        h.update(ipad, sizeof(ipad));
        h.update(data, len);
        h.final(inner);
    }
    Blake2s h;
    h.init(32);
    h.update(opad, sizeof(opad));
    h.update(inner, sizeof(inner));
    h.final(out);
}

void kdf(const uint8_t* key, size_t key_len, const uint8_t* input, size_t input_len, int n,
         uint8_t* out1, uint8_t* out2, uint8_t* out3)
{
    // WireGuard の KDF は HKDF-BLAKE2s。extract してから 1 バイトのカウンタで expand する。
    uint8_t prk[32];
    hmac_blake2s(prk, key, key_len, input, input_len);

    uint8_t t[32];
    uint8_t one = 0x01;
    hmac_blake2s(t, prk, sizeof(prk), &one, 1);
    if (out1) std::memcpy(out1, t, 32);
    if (n < 2) return;

    uint8_t msg[33];
    std::memcpy(msg, t, 32);
    msg[32] = 0x02;
    hmac_blake2s(t, prk, sizeof(prk), msg, sizeof(msg));
    if (out2) std::memcpy(out2, t, 32);
    if (n < 3) return;

    std::memcpy(msg, t, 32);
    msg[32] = 0x03;
    hmac_blake2s(t, prk, sizeof(prk), msg, sizeof(msg));
    if (out3) std::memcpy(out3, t, 32);
}

}  // namespace wg
