#include <cstdint>
#include "salsa20.hpp"

#include <cstring>
#include <new>

#include <mbedtls/poly1305.h>

namespace wg {
namespace {

// sigma = "expand 32-byte k"
constexpr uint8_t kSigma[16] = {'e', 'x', 'p', 'a', 'n', 'd', ' ', '3',
                               '2', '-', 'b', 'y', 't', 'e', ' ', 'k'};

uint32_t load_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void store_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// 20 ラウンド（doubleround を 10 回）。out に加算前の状態を残せるように分離してある。
void rounds(uint32_t x[16])
{
    for (int i = 0; i < 10; ++i) {
        // 列
        x[4] ^= rotl(x[0] + x[12], 7);
        x[8] ^= rotl(x[4] + x[0], 9);
        x[12] ^= rotl(x[8] + x[4], 13);
        x[0] ^= rotl(x[12] + x[8], 18);
        x[9] ^= rotl(x[5] + x[1], 7);
        x[13] ^= rotl(x[9] + x[5], 9);
        x[1] ^= rotl(x[13] + x[9], 13);
        x[5] ^= rotl(x[1] + x[13], 18);
        x[14] ^= rotl(x[10] + x[6], 7);
        x[2] ^= rotl(x[14] + x[10], 9);
        x[6] ^= rotl(x[2] + x[14], 13);
        x[10] ^= rotl(x[6] + x[2], 18);
        x[3] ^= rotl(x[15] + x[11], 7);
        x[7] ^= rotl(x[3] + x[15], 9);
        x[11] ^= rotl(x[7] + x[3], 13);
        x[15] ^= rotl(x[11] + x[7], 18);
        // 行
        x[1] ^= rotl(x[0] + x[3], 7);
        x[2] ^= rotl(x[1] + x[0], 9);
        x[3] ^= rotl(x[2] + x[1], 13);
        x[0] ^= rotl(x[3] + x[2], 18);
        x[6] ^= rotl(x[5] + x[4], 7);
        x[7] ^= rotl(x[6] + x[5], 9);
        x[4] ^= rotl(x[7] + x[6], 13);
        x[5] ^= rotl(x[4] + x[7], 18);
        x[11] ^= rotl(x[10] + x[9], 7);
        x[8] ^= rotl(x[11] + x[10], 9);
        x[9] ^= rotl(x[8] + x[11], 13);
        x[10] ^= rotl(x[9] + x[8], 18);
        x[12] ^= rotl(x[15] + x[14], 7);
        x[13] ^= rotl(x[12] + x[15], 9);
        x[14] ^= rotl(x[13] + x[12], 13);
        x[15] ^= rotl(x[14] + x[13], 18);
    }
}

// NaCl の crypto_core_salsa20 と同じ状態レイアウト。
void build_state(uint32_t x[16], const uint8_t in[16], const uint8_t k[32], const uint8_t c[16])
{
    x[0]  = load_le32(c + 0);
    x[1]  = load_le32(k + 0);
    x[2]  = load_le32(k + 4);
    x[3]  = load_le32(k + 8);
    x[4]  = load_le32(k + 12);
    x[5]  = load_le32(c + 4);
    x[6]  = load_le32(in + 0);
    x[7]  = load_le32(in + 4);
    x[8]  = load_le32(in + 8);
    x[9]  = load_le32(in + 12);
    x[10] = load_le32(c + 8);
    x[11] = load_le32(k + 16);
    x[12] = load_le32(k + 20);
    x[13] = load_le32(k + 24);
    x[14] = load_le32(k + 28);
    x[15] = load_le32(c + 12);
}

}  // namespace

void salsa20_core(uint8_t out[64], const uint8_t in[16], const uint8_t k[32], const uint8_t c[16])
{
    uint32_t x[16], j[16];
    build_state(x, in, k, c);
    std::memcpy(j, x, sizeof(j));
    rounds(x);
    // feedforward（元の状態を足す）
    for (int i = 0; i < 16; ++i) store_le32(out + i * 4, x[i] + j[i]);
}

void hsalsa20(uint8_t out[32], const uint8_t in[16], const uint8_t k[32], const uint8_t c[16])
{
    uint32_t x[16];
    build_state(x, in, k, c);
    rounds(x);
    // HSalsa20 は feedforward 後に定数と nonce を引くのと等価なので、
    // feedforward しない中間値の word 0,5,10,15,6,7,8,9 を取り出す。
    store_le32(out + 0, x[0]);
    store_le32(out + 4, x[5]);
    store_le32(out + 8, x[10]);
    store_le32(out + 12, x[15]);
    store_le32(out + 16, x[6]);
    store_le32(out + 20, x[7]);
    store_le32(out + 24, x[8]);
    store_le32(out + 28, x[9]);
}

void xsalsa20_stream(uint8_t* out, size_t len, const uint8_t nonce[kBoxNonceLen],
                     const uint8_t key[32])
{
    // subkey = HSalsa20(key, nonce[0..15])、以降は素の Salsa20 に nonce[16..23] を使う。
    uint8_t subkey[32];
    hsalsa20(subkey, nonce, key, kSigma);

    uint8_t in[16] = {};
    std::memcpy(in, nonce + 16, 8);  // 残り 8 バイトが nonce、in[8..15] がブロックカウンタ

    uint64_t counter = 0;
    while (len > 0) {
        for (int i = 0; i < 8; ++i) in[8 + i] = static_cast<uint8_t>(counter >> (8 * i));
        uint8_t block[64];
        salsa20_core(block, in, subkey, kSigma);
        const size_t take = (len < sizeof(block)) ? len : sizeof(block);
        std::memcpy(out, block, take);
        out += take;
        len -= take;
        ++counter;
    }
}

void box_beforenm(uint8_t out[32], const uint8_t dh_output[32])
{
    const uint8_t zero[16] = {};
    hsalsa20(out, zero, dh_output, kSigma);
}

bool secretbox_seal(uint8_t* out, const uint8_t* plain, size_t len,
                    const uint8_t nonce[kBoxNonceLen], const uint8_t key[32])
{
    // キーストリームの先頭 32 バイトが Poly1305 の鍵になる（捨てるのではなく転用する）。
    // 平文の暗号化はバイト 32 から始まる。
    const size_t total = 32 + len;
    // キーストリームを必要なぶんだけ作る。
    uint8_t  poly_key[32];
    uint8_t* cipher = out + kBoxMacLen;
    {
        // 先頭 64 バイトぶんを一度に作って、前半を鍵、後半を暗号化に使う。
        uint8_t block0[64];
        xsalsa20_stream(block0, sizeof(block0), nonce, key);
        std::memcpy(poly_key, block0, 32);
        const size_t first = (len < 32) ? len : 32;
        for (size_t i = 0; i < first; ++i) cipher[i] = plain[i] ^ block0[32 + i];
        if (len > 32) {
            // 残りはブロック 1 以降。ストリーム全体を作り直して 64 バイト目以降を使う。
            const size_t rest = len - 32;
            uint8_t*     ks   = new (std::nothrow) uint8_t[total];
            if (!ks) return false;
            xsalsa20_stream(ks, total, nonce, key);
            for (size_t i = 0; i < rest; ++i) cipher[32 + i] = plain[32 + i] ^ ks[64 + i];
            delete[] ks;
        }
    }
    mbedtls_poly1305_context ctx;
    mbedtls_poly1305_init(&ctx);
    const bool ok = mbedtls_poly1305_starts(&ctx, poly_key) == 0 &&
                    mbedtls_poly1305_update(&ctx, cipher, len) == 0 &&
                    mbedtls_poly1305_finish(&ctx, out) == 0;
    mbedtls_poly1305_free(&ctx);
    return ok;
}

bool secretbox_open(uint8_t* out, const uint8_t* in, size_t in_len,
                    const uint8_t nonce[kBoxNonceLen], const uint8_t key[32])
{
    if (in_len < kBoxMacLen) return false;
    const size_t len    = in_len - kBoxMacLen;
    const size_t total  = 32 + len;
    const uint8_t* cipher = in + kBoxMacLen;

    uint8_t poly_key[32];
    uint8_t block0[64];
    xsalsa20_stream(block0, sizeof(block0), nonce, key);
    std::memcpy(poly_key, block0, 32);

    // タグを検証してから復号する。
    uint8_t                  tag[kBoxMacLen];
    mbedtls_poly1305_context ctx;
    mbedtls_poly1305_init(&ctx);
    const bool mac_ok = mbedtls_poly1305_starts(&ctx, poly_key) == 0 &&
                        mbedtls_poly1305_update(&ctx, cipher, len) == 0 &&
                        mbedtls_poly1305_finish(&ctx, tag) == 0;
    mbedtls_poly1305_free(&ctx);
    if (!mac_ok) return false;

    uint8_t diff = 0;
    for (size_t i = 0; i < kBoxMacLen; ++i) diff |= static_cast<uint8_t>(tag[i] ^ in[i]);
    if (diff != 0) return false;

    const size_t first = (len < 32) ? len : 32;
    for (size_t i = 0; i < first; ++i) out[i] = cipher[i] ^ block0[32 + i];
    if (len > 32) {
        const size_t rest = len - 32;
        uint8_t*     ks   = new (std::nothrow) uint8_t[total];
        if (!ks) return false;
        xsalsa20_stream(ks, total, nonce, key);
        for (size_t i = 0; i < rest; ++i) out[32 + i] = cipher[32 + i] ^ ks[64 + i];
        delete[] ks;
    }
    return true;
}

}  // namespace wg
