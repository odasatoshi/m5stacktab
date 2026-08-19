// Noise IK が使う暗号プリミティブを mbedTLS で実装する。
// ホストでも実機でも同じコードを使う（ホストでは Homebrew の mbedtls をリンクする）。
#include <cstdint>
#include <cstring>

#include <mbedtls/chachapoly.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>

#include "noise.hpp"

namespace wg {
namespace {

mbedtls_ctr_drbg_context& drbg();

// X25519 の秘密鍵は 32 バイトのスカラー。mbedTLS の ECP を使う。
bool x25519(uint8_t out[kKeyLen], const uint8_t priv[kKeyLen], const uint8_t pub[kKeyLen])
{
    mbedtls_ecp_group grp;
    mbedtls_mpi       d;
    mbedtls_ecp_point Q, R;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_point_init(&R);

    bool ok = false;
    do {
        if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519) != 0) break;

        // X25519 のスカラークランプ (RFC 7748 5)。mbedtls_ecp_mul はこれをやらない。
        uint8_t k[kKeyLen];
        std::memcpy(k, priv, kKeyLen);
        k[0] &= 248;
        k[31] &= 127;
        k[31] |= 64;

        // Curve25519 のスカラーと座標はリトルエンディアン。専用 API を使えば
        // 構造体の内部メンバ (MBEDTLS_PRIVATE) に触らずに済む。
        if (mbedtls_mpi_read_binary_le(&d, k, kKeyLen) != 0) break;
        if (mbedtls_ecp_point_read_binary(&grp, &Q, pub, kKeyLen) != 0) break;

        // Montgomery カーブでは f_rng が必須（座標ブラインディングに使う）。
        if (mbedtls_ecp_mul(&grp, &R, &d, &Q, mbedtls_ctr_drbg_random, &drbg()) != 0) break;

        size_t olen = 0;
        if (mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out,
                                           kKeyLen) != 0) {
            break;
        }
        if (olen != kKeyLen) break;

        // 全ゼロの共有鍵は不正なピア（低位数点）を意味するので弾く。
        uint8_t acc = 0;
        for (size_t i = 0; i < kKeyLen; ++i) acc |= out[i];
        ok = (acc != 0);
    } while (false);

    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_point_free(&Q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

bool x25519_pubkey(uint8_t out[kKeyLen], const uint8_t priv[kKeyLen])
{
    // 基点 9 に対するスカラー倍。
    uint8_t base[kKeyLen] = {9};
    return x25519(out, priv, base);
}

// nonce は 4 バイトのゼロ + 8 バイトのカウンタ (little endian)。WireGuard の定義。
void make_nonce(uint8_t nonce[12], uint64_t counter)
{
    std::memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

bool aead_encrypt(uint8_t* out, const uint8_t key[kKeyLen], uint64_t counter,
                  const uint8_t* plain, size_t plain_len, const uint8_t* ad, size_t ad_len)
{
    uint8_t nonce[12];
    make_nonce(nonce, counter);
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    bool ok = mbedtls_chachapoly_setkey(&ctx, key) == 0 &&
              mbedtls_chachapoly_encrypt_and_tag(&ctx, plain_len, nonce, ad, ad_len, plain, out,
                                                 out + plain_len) == 0;
    mbedtls_chachapoly_free(&ctx);
    return ok;
}

bool aead_decrypt(uint8_t* out, const uint8_t key[kKeyLen], uint64_t counter,
                  const uint8_t* cipher, size_t cipher_len, const uint8_t* ad, size_t ad_len)
{
    if (cipher_len < kTagLen) return false;
    const size_t plain_len = cipher_len - kTagLen;
    uint8_t      nonce[12];
    make_nonce(nonce, counter);
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    bool ok = mbedtls_chachapoly_setkey(&ctx, key) == 0 &&
              mbedtls_chachapoly_auth_decrypt(&ctx, plain_len, nonce, ad, ad_len,
                                              cipher + plain_len, cipher, out) == 0;
    mbedtls_chachapoly_free(&ctx);
    return ok;
}

mbedtls_ctr_drbg_context& drbg()
{
    static mbedtls_ctr_drbg_context ctr;
    static mbedtls_entropy_context  entropy;
    static bool                     inited = false;
    if (!inited) {
        mbedtls_ctr_drbg_init(&ctr);
        mbedtls_entropy_init(&entropy);
        static const char* pers = "wg-noise";
        // シードに失敗したら inited を立てない。立ててしまうと以後ずっと無言で
        // 使えない DRBG を返し続けることになる。
        if (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>(pers),
                                  std::strlen(pers)) == 0) {
            inited = true;
        }
    }
    return ctr;
}

bool random_bytes(uint8_t* out, size_t len)
{
    // 全ゼロを返して続行してはいけない。X25519 のクランプで有効なスカラーになってしまい、
    // 誰でも計算できる一時鍵で握手が成立する。
    if (mbedtls_ctr_drbg_random(&drbg(), out, len) != 0) {
        std::memset(out, 0, len);
        return false;
    }
    return true;
}

const Crypto kCrypto = {
    &x25519, &x25519_pubkey, &aead_encrypt, &aead_decrypt, &random_bytes,
};

}  // namespace

const Crypto& default_crypto() { return kCrypto; }

}  // namespace wg
