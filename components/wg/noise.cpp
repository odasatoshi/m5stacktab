#include "noise.hpp"

#include <cstring>

#include "blake2s.hpp"

namespace wg {
namespace {

// WireGuard 仕様の定数。
constexpr char kConstruction[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
constexpr char kIdentifier[]   = "WireGuard v1 zx2c4 Jason@zx2c4.com";

void mix_hash(uint8_t hash[kKeyLen], const uint8_t* data, size_t len)
{
    Blake2s h;
    h.init(kKeyLen);
    h.update(hash, kKeyLen);
    h.update(data, len);
    h.final(hash);
}

void mix_key(uint8_t ck[kKeyLen], const uint8_t* input, size_t input_len)
{
    kdf(ck, kKeyLen, input, input_len, 1, ck, nullptr, nullptr);
}

void store_le32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t load_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool is_zero(const uint8_t* p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) acc |= p[i];
    return acc == 0;
}

// MessageInitiation のレイアウト (RFC ではなく WireGuard 仕様):
//   u8 type, u8 reserved[3], u32 sender, u8 ephemeral[32],
//   u8 static[32+16], u8 timestamp[12+16], u8 mac1[16], u8 mac2[16]
constexpr size_t kInitEphemeral = 8;
constexpr size_t kInitStatic    = 40;
constexpr size_t kInitTimestamp = 88;
constexpr size_t kInitMac1      = 116;
// mac2 (offset 132) はクッキー応答を受けたときだけ埋める。今は未対応なので全ゼロのまま。
constexpr size_t kInitLen       = 148;

// MessageResponse:
//   u8 type, u8 reserved[3], u32 sender, u32 receiver, u8 ephemeral[32],
//   u8 empty[0+16], u8 mac1[16], u8 mac2[16]
constexpr size_t kRespEphemeral = 12;
constexpr size_t kRespEmpty     = 44;
constexpr size_t kRespMac1      = 60;
constexpr size_t kRespLen       = 92;

// mac1 = MAC(HASH(LABEL_MAC1 || peer_static_pub), msg[0..mac1))
void compute_mac1(uint8_t out[kTagLen], const uint8_t peer_static[kKeyLen], const uint8_t* msg,
                  size_t len)
{
    static const char kLabelMac1[] = "mac1----";
    uint8_t           key[kKeyLen];
    {
        Blake2s h;
        h.init(kKeyLen);
        h.update(reinterpret_cast<const uint8_t*>(kLabelMac1), sizeof(kLabelMac1) - 1);
        h.update(peer_static, kKeyLen);
        h.final(key);
    }
    blake2s(out, kTagLen, msg, len, key, kKeyLen);
}

}  // namespace

bool Handshake::set_keys(const uint8_t static_priv[kKeyLen], const uint8_t peer_static_pub[kKeyLen],
                         const uint8_t psk[kKeyLen])
{
    std::memcpy(static_priv_, static_priv, kKeyLen);
    std::memcpy(peer_static_, peer_static_pub, kKeyLen);
    if (psk) {
        std::memcpy(psk_, psk, kKeyLen);
    } else {
        std::memset(psk_, 0, kKeyLen);
    }
    return c_.dh_pubkey(static_pub_, static_priv_);
}

bool Handshake::create_initiation(uint8_t out[kInitLen], uint32_t local_index,
                                  const uint8_t timestamp[kTimestampLen])
{
    local_index_ = local_index;

    // ck = HASH(CONSTRUCTION), h = HASH(ck || IDENTIFIER || peer_static)
    blake2s(chaining_key_, kKeyLen, reinterpret_cast<const uint8_t*>(kConstruction),
            sizeof(kConstruction) - 1);
    std::memcpy(hash_, chaining_key_, kKeyLen);
    mix_hash(hash_, reinterpret_cast<const uint8_t*>(kIdentifier), sizeof(kIdentifier) - 1);
    mix_hash(hash_, peer_static_, kKeyLen);

    if (!c_.random_bytes(ephemeral_priv_, kKeyLen)) return false;
    if (!c_.dh_pubkey(ephemeral_pub_, ephemeral_priv_)) return false;

    std::memset(out, 0, kInitLen);
    out[0] = kMsgInitiation;
    store_le32(out + 4, local_index);
    std::memcpy(out + kInitEphemeral, ephemeral_pub_, kKeyLen);

    mix_key(chaining_key_, ephemeral_pub_, kKeyLen);
    mix_hash(hash_, ephemeral_pub_, kKeyLen);

    // 静的公開鍵を暗号化
    uint8_t dh[kKeyLen];
    if (!c_.dh(dh, ephemeral_priv_, peer_static_)) return false;
    uint8_t key[kKeyLen];
    kdf(chaining_key_, kKeyLen, dh, kKeyLen, 2, chaining_key_, key, nullptr);
    if (!c_.aead_encrypt(out + kInitStatic, key, 0, static_pub_, kKeyLen, hash_, kKeyLen)) {
        return false;
    }
    mix_hash(hash_, out + kInitStatic, kKeyLen + kTagLen);

    // タイムスタンプを暗号化（リプレイ防止）
    if (!c_.dh(dh, static_priv_, peer_static_)) return false;
    kdf(chaining_key_, kKeyLen, dh, kKeyLen, 2, chaining_key_, key, nullptr);
    if (!c_.aead_encrypt(out + kInitTimestamp, key, 0, timestamp, kTimestampLen, hash_, kKeyLen)) {
        return false;
    }
    mix_hash(hash_, out + kInitTimestamp, kTimestampLen + kTagLen);

    compute_mac1(out + kInitMac1, peer_static_, out, kInitMac1);
    // mac2 はクッキー応答を受けるまで全ゼロ。
    return true;
}

bool Handshake::consume_initiation(const uint8_t in[kInitLen], uint8_t peer_static_out[kKeyLen],
                                   uint8_t timestamp_out[kTimestampLen])
{
    if (in[0] != kMsgInitiation) return false;

    uint8_t mac1[kTagLen];
    compute_mac1(mac1, static_pub_, in, kInitMac1);
    if (std::memcmp(mac1, in + kInitMac1, kTagLen) != 0) return false;

    remote_index_ = load_le32(in + 4);

    blake2s(chaining_key_, kKeyLen, reinterpret_cast<const uint8_t*>(kConstruction),
            sizeof(kConstruction) - 1);
    std::memcpy(hash_, chaining_key_, kKeyLen);
    mix_hash(hash_, reinterpret_cast<const uint8_t*>(kIdentifier), sizeof(kIdentifier) - 1);
    mix_hash(hash_, static_pub_, kKeyLen);

    std::memcpy(peer_ephemeral_, in + kInitEphemeral, kKeyLen);
    mix_key(chaining_key_, peer_ephemeral_, kKeyLen);
    mix_hash(hash_, peer_ephemeral_, kKeyLen);

    uint8_t dh[kKeyLen], key[kKeyLen];
    if (!c_.dh(dh, static_priv_, peer_ephemeral_)) return false;
    kdf(chaining_key_, kKeyLen, dh, kKeyLen, 2, chaining_key_, key, nullptr);
    if (!c_.aead_decrypt(peer_static_out, key, 0, in + kInitStatic, kKeyLen + kTagLen, hash_,
                        kKeyLen)) {
        return false;
    }
    // set_keys で相手の静的公開鍵が指定されているなら照合する。mac1 は自分の公開鍵
    // (公開情報) で検証されるだけなので、ここを省くと誰でも握手を完了できてしまう。
    if (!is_zero(peer_static_, kKeyLen) &&
        std::memcmp(peer_static_, peer_static_out, kKeyLen) != 0) {
        return false;
    }
    std::memcpy(peer_static_, peer_static_out, kKeyLen);
    mix_hash(hash_, in + kInitStatic, kKeyLen + kTagLen);

    if (!c_.dh(dh, static_priv_, peer_static_)) return false;
    kdf(chaining_key_, kKeyLen, dh, kKeyLen, 2, chaining_key_, key, nullptr);
    if (!c_.aead_decrypt(timestamp_out, key, 0, in + kInitTimestamp, kTimestampLen + kTagLen, hash_,
                        kKeyLen)) {
        return false;
    }
    mix_hash(hash_, in + kInitTimestamp, kTimestampLen + kTagLen);
    return true;
}

bool Handshake::create_response(uint8_t out[kRespLen], uint32_t local_index, Keypair& keypair_out)
{
    local_index_ = local_index;

    if (!c_.random_bytes(ephemeral_priv_, kKeyLen)) return false;
    if (!c_.dh_pubkey(ephemeral_pub_, ephemeral_priv_)) return false;

    std::memset(out, 0, kRespLen);
    out[0] = kMsgResponse;
    store_le32(out + 4, local_index);
    store_le32(out + 8, remote_index_);
    std::memcpy(out + kRespEphemeral, ephemeral_pub_, kKeyLen);

    mix_key(chaining_key_, ephemeral_pub_, kKeyLen);
    mix_hash(hash_, ephemeral_pub_, kKeyLen);

    uint8_t dh[kKeyLen];
    if (!c_.dh(dh, ephemeral_priv_, peer_ephemeral_)) return false;
    mix_key(chaining_key_, dh, kKeyLen);
    if (!c_.dh(dh, ephemeral_priv_, peer_static_)) return false;
    mix_key(chaining_key_, dh, kKeyLen);

    // psk を混ぜる（未設定なら全ゼロ）
    uint8_t tau[kKeyLen], key[kKeyLen];
    kdf(chaining_key_, kKeyLen, psk_, kKeyLen, 3, chaining_key_, tau, key);
    mix_hash(hash_, tau, kKeyLen);

    if (!c_.aead_encrypt(out + kRespEmpty, key, 0, nullptr, 0, hash_, kKeyLen)) return false;
    mix_hash(hash_, out + kRespEmpty, kTagLen);

    compute_mac1(out + kRespMac1, peer_static_, out, kRespMac1);

    // トランスポート鍵。応答側は (recv, send) の順。
    kdf(chaining_key_, kKeyLen, nullptr, 0, 2, keypair_out.recv, keypair_out.send, nullptr);
    keypair_out.local_index  = local_index_;
    keypair_out.remote_index = remote_index_;
    keypair_out.initiator    = false;
    return true;
}

bool Handshake::consume_response(const uint8_t in[kRespLen], Keypair& out)
{
    if (in[0] != kMsgResponse) return false;
    if (load_le32(in + 8) != local_index_) return false;  // receiver index が自分宛でない

    uint8_t mac1[kTagLen];
    compute_mac1(mac1, static_pub_, in, kRespMac1);
    if (std::memcmp(mac1, in + kRespMac1, kTagLen) != 0) return false;

    remote_index_ = load_le32(in + 4);
    std::memcpy(peer_ephemeral_, in + kRespEphemeral, kKeyLen);

    mix_key(chaining_key_, peer_ephemeral_, kKeyLen);
    mix_hash(hash_, peer_ephemeral_, kKeyLen);

    uint8_t dh[kKeyLen];
    if (!c_.dh(dh, ephemeral_priv_, peer_ephemeral_)) return false;
    mix_key(chaining_key_, dh, kKeyLen);
    if (!c_.dh(dh, static_priv_, peer_ephemeral_)) return false;
    mix_key(chaining_key_, dh, kKeyLen);

    uint8_t tau[kKeyLen], key[kKeyLen];
    kdf(chaining_key_, kKeyLen, psk_, kKeyLen, 3, chaining_key_, tau, key);
    mix_hash(hash_, tau, kKeyLen);

    // empty の復号が通れば相手が正しい鍵を持っている証明になる。
    uint8_t dummy[1];
    if (!c_.aead_decrypt(dummy, key, 0, in + kRespEmpty, kTagLen, hash_, kKeyLen)) return false;
    mix_hash(hash_, in + kRespEmpty, kTagLen);

    // 開始側は (send, recv) の順。
    kdf(chaining_key_, kKeyLen, nullptr, 0, 2, out.send, out.recv, nullptr);
    out.local_index  = local_index_;
    out.remote_index = remote_index_;
    out.initiator    = true;
    if (is_zero(out.send, kKeyLen) || is_zero(out.recv, kKeyLen)) return false;
    return true;
}

}  // namespace wg
