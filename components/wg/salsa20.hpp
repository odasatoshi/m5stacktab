#pragma once
// NaCl の crypto_box（X25519 + XSalsa20-Poly1305）。Tailscale の DISCO が使う。
//
// mbedTLS には Salsa20 が無いので自前実装。Poly1305 は mbedTLS のものを使う。
// ESP-IDF に依存しないのでホストでテストできる（NaCl の公式テストベクタで検証）。
#include <cstddef>
#include <cstdint>

namespace wg {

constexpr size_t kBoxNonceLen = 24;
constexpr size_t kBoxMacLen   = 16;

// Salsa20 の 20 ラウンドコア。in は 16 バイト（nonce + カウンタ）、k は 32 バイト、
// c は 16 バイトの定数（sigma = "expand 32-byte k"）。
void salsa20_core(uint8_t out[64], const uint8_t in[16], const uint8_t k[32],
                  const uint8_t c[16]);

// HSalsa20。XSalsa20 の subkey 導出と crypto_box_beforenm に使う。
void hsalsa20(uint8_t out[32], const uint8_t in[16], const uint8_t k[32], const uint8_t c[16]);

// XSalsa20 のキーストリームを out に len バイト書く（nonce は 24 バイト）。
void xsalsa20_stream(uint8_t* out, size_t len, const uint8_t nonce[kBoxNonceLen],
                     const uint8_t key[32]);

// crypto_box の共有鍵。k = HSalsa20(X25519(sk, pk), 0^16, sigma)。
// X25519 は呼び出し側が計算して dh_output で渡す（mbedTLS を使うため）。
void box_beforenm(uint8_t out[32], const uint8_t dh_output[32]);

// crypto_secretbox（XSalsa20-Poly1305）。out は len + kBoxMacLen バイト必要。
// 出力は libsodium の easy 形式（MAC 16 バイト + 暗号文）。
bool secretbox_seal(uint8_t* out, const uint8_t* plain, size_t len,
                    const uint8_t nonce[kBoxNonceLen], const uint8_t key[32]);

// 復号。in は MAC 込みの長さ。out は in_len - kBoxMacLen バイト必要。
bool secretbox_open(uint8_t* out, const uint8_t* in, size_t in_len,
                    const uint8_t nonce[kBoxNonceLen], const uint8_t key[32]);

}  // namespace wg
