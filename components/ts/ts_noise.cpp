#include <cstdint>
#include "ts_noise.hpp"

#include <cstdio>
#include <cstring>

#include "blake2s.hpp"

namespace ts {
namespace {

constexpr char kProtocolName[] = "Noise_IK_25519_ChaChaPoly_BLAKE2s";
constexpr char kProloguePrefix[] = "Tailscale Control Protocol v";

void mix_hash(uint8_t hash[kKeyLen], const uint8_t* data, size_t len)
{
    wg::Blake2s h;
    h.init(kKeyLen);
    h.update(hash, kKeyLen);
    h.update(data, len);
    h.final(hash);
}

// ck, k = HKDF(ck, dh_output)
void mix_dh(uint8_t ck[kKeyLen], uint8_t key[kKeyLen], const uint8_t* dh, size_t dh_len)
{
    wg::kdf(ck, kKeyLen, dh, dh_len, 2, ck, key, nullptr);
}

void store_be16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

uint16_t load_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t load_be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool is_zero(const uint8_t* p, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; ++i) acc |= p[i];
    return acc == 0;
}

// ハンドシェイク中の AEAD はノンス全ゼロ、AD は現在のハッシュ。
bool encrypt_and_hash(const wg::Crypto& c, uint8_t* out, const uint8_t key[kKeyLen],
                      const uint8_t* plain, size_t len, uint8_t hash[kKeyLen])
{
    if (!c.aead_encrypt(out, key, 0, plain, len, hash, kKeyLen)) return false;
    mix_hash(hash, out, len + kTagLen);
    return true;
}

bool decrypt_and_hash(const wg::Crypto& c, uint8_t* out, const uint8_t key[kKeyLen],
                      const uint8_t* cipher, size_t cipher_len, uint8_t hash[kKeyLen])
{
    if (!c.aead_decrypt(out, key, 0, cipher, cipher_len, hash, kKeyLen)) return false;
    mix_hash(hash, cipher, cipher_len);
    return true;
}

// MessageInitiation のレイアウト:
//   [0..2)  uint16 BE version, [2] type, [3..5) uint16 BE payload len(96),
//   [5..37) client ephemeral pub, [37..85) machine pub + tag, [85..101) 空ペイロードの tag
constexpr size_t kInitEphemeral = 5;
constexpr size_t kInitMachine   = 37;
constexpr size_t kInitEmptyTag  = 85;

// MessageResponse:
//   [0] type, [1..3) uint16 BE payload len(48), [3..35) server ephemeral pub, [35..51) tag
constexpr size_t kRespEphemeral = 3;
constexpr size_t kRespTag       = 35;

}  // namespace

bool Handshake::init(const uint8_t machine_priv[kKeyLen], const uint8_t server_pub[kKeyLen],
                     uint16_t capability_version)
{
    std::memcpy(machine_priv_, machine_priv, kKeyLen);
    std::memcpy(server_pub_, server_pub, kKeyLen);
    version_ = capability_version;
    if (!c_.dh_pubkey(machine_pub_, machine_priv_)) return false;

    // h = BLAKE2s(protocol name), ck = h
    wg::blake2s(hash_, kKeyLen, reinterpret_cast<const uint8_t*>(kProtocolName),
                sizeof(kProtocolName) - 1);
    std::memcpy(chaining_key_, hash_, kKeyLen);

    // MixHash(prologue) — バージョン番号が入るのでサーバと一致していないと復号に失敗する
    char prologue[64];
    const int n = std::snprintf(prologue, sizeof(prologue), "%s%u", kProloguePrefix,
                                static_cast<unsigned>(capability_version));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(prologue)) return false;
    mix_hash(hash_, reinterpret_cast<const uint8_t*>(prologue), static_cast<size_t>(n));

    // MixHash(server static pub) — IK の pre-message
    mix_hash(hash_, server_pub_, kKeyLen);
    return true;
}

bool Handshake::create_initiation(uint8_t out[kInitiationLen])
{
    if (!c_.random_bytes(ephemeral_priv_, kKeyLen)) return false;
    if (!c_.dh_pubkey(ephemeral_pub_, ephemeral_priv_)) return false;

    std::memset(out, 0, kInitiationLen);
    // 先頭 5 バイトのヘッダはハッシュに混ぜない（バージョンは prologue で検証される）。
    store_be16(out, version_);
    out[2] = kMsgInitiation;
    store_be16(out + 3, static_cast<uint16_t>(kInitiationLen - 5));

    std::memcpy(out + kInitEphemeral, ephemeral_pub_, kKeyLen);
    mix_hash(hash_, ephemeral_pub_, kKeyLen);

    // es: 一時鍵 × サーバ静的鍵 で machine 公開鍵を暗号化
    uint8_t dh[kKeyLen], key[kKeyLen];
    if (!c_.dh(dh, ephemeral_priv_, server_pub_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    if (!encrypt_and_hash(c_, out + kInitMachine, key, machine_pub_, kKeyLen, hash_)) return false;

    // ss: machine 静的鍵 × サーバ静的鍵 で空ペイロードを暗号化（相互認証の証明）
    if (!c_.dh(dh, machine_priv_, server_pub_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    if (!encrypt_and_hash(c_, out + kInitEmptyTag, key, nullptr, 0, hash_)) return false;
    return true;
}

bool Handshake::consume_initiation(const uint8_t in[kInitiationLen],
                                   uint8_t peer_machine_out[kKeyLen])
{
    if (in[2] != kMsgInitiation) return false;
    if (load_be16(in) != version_) return false;
    if (load_be16(in + 3) != kInitiationLen - 5) return false;

    std::memcpy(peer_ephemeral_, in + kInitEphemeral, kKeyLen);
    mix_hash(hash_, peer_ephemeral_, kKeyLen);

    uint8_t dh[kKeyLen], key[kKeyLen];
    if (!c_.dh(dh, machine_priv_, peer_ephemeral_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    if (!decrypt_and_hash(c_, peer_machine_out, key, in + kInitMachine, kKeyLen + kTagLen, hash_)) {
        return false;
    }
    std::memcpy(peer_machine_, peer_machine_out, kKeyLen);

    if (!c_.dh(dh, machine_priv_, peer_machine_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    uint8_t dummy[1];
    if (!decrypt_and_hash(c_, dummy, key, in + kInitEmptyTag, kTagLen, hash_)) return false;
    return true;
}

bool Handshake::create_response(uint8_t out[kResponseLen], Session& out_session)
{
    if (!c_.random_bytes(ephemeral_priv_, kKeyLen)) return false;
    if (!c_.dh_pubkey(ephemeral_pub_, ephemeral_priv_)) return false;

    std::memset(out, 0, kResponseLen);
    out[0] = kMsgResponse;
    store_be16(out + 1, static_cast<uint16_t>(kResponseLen - 3));
    std::memcpy(out + kRespEphemeral, ephemeral_pub_, kKeyLen);
    mix_hash(hash_, ephemeral_pub_, kKeyLen);

    uint8_t dh[kKeyLen], key[kKeyLen];
    // ee
    if (!c_.dh(dh, ephemeral_priv_, peer_ephemeral_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    // se
    if (!c_.dh(dh, ephemeral_priv_, peer_machine_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);

    if (!encrypt_and_hash(c_, out + kRespTag, key, nullptr, 0, hash_)) return false;

    // Split: サーバ側は tx/rx がクライアントと逆。
    wg::kdf(chaining_key_, kKeyLen, nullptr, 0, 2, out_session.rx, out_session.tx, nullptr);
    out_session.tx_counter = 0;
    out_session.rx_counter = 0;
    out_session.valid      = !is_zero(out_session.tx, kKeyLen) && !is_zero(out_session.rx, kKeyLen);
    return out_session.valid;
}

bool Handshake::consume_response(const uint8_t in[kResponseLen], Session& out)
{
    if (in[0] != kMsgResponse) return false;
    if (load_be16(in + 1) != kResponseLen - 3) return false;

    std::memcpy(peer_ephemeral_, in + kRespEphemeral, kKeyLen);
    mix_hash(hash_, peer_ephemeral_, kKeyLen);

    uint8_t dh[kKeyLen], key[kKeyLen];
    // ee
    if (!c_.dh(dh, ephemeral_priv_, peer_ephemeral_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);
    // se
    if (!c_.dh(dh, machine_priv_, peer_ephemeral_)) return false;
    mix_dh(chaining_key_, key, dh, kKeyLen);

    uint8_t dummy[1];
    if (!decrypt_and_hash(c_, dummy, key, in + kRespTag, kTagLen, hash_)) return false;

    // クライアント側は tx = k1, rx = k2
    wg::kdf(chaining_key_, kKeyLen, nullptr, 0, 2, out.tx, out.rx, nullptr);
    out.tx_counter = 0;
    out.rx_counter = 0;
    out.valid      = !is_zero(out.tx, kKeyLen) && !is_zero(out.rx, kKeyLen);
    return out.valid;
}

size_t Record::seal(uint8_t* out, size_t out_cap, const uint8_t* plain, size_t len)
{
    if (!s_.valid || len > kMaxPlaintextLen) return 0;
    const size_t need = 3 + len + kTagLen;
    if (out_cap < need) return 0;
    if (s_.tx_counter == UINT64_MAX) return 0;

    out[0] = kMsgRecord;
    store_be16(out + 1, static_cast<uint16_t>(len + kTagLen));
    // ノンスはビッグエンディアン。WireGuard と逆なので間違えると復号だけ通らない。
    if (!c_.aead_encrypt_be(out + 3, s_.tx, s_.tx_counter, plain, len, nullptr, 0)) return 0;
    ++s_.tx_counter;
    return need;
}

size_t Record::open(uint8_t* out, size_t out_cap, const uint8_t* in, size_t len, size_t* consumed)
{
    if (consumed) *consumed = 0;
    if (!s_.valid) {
        if (consumed) *consumed = SIZE_MAX;
        return 0;
    }
    if (len < 3) return 0;  // ヘッダが揃っていない

    const uint8_t  type       = in[0];
    const uint16_t cipher_len = load_be16(in + 1);
    if (type != kMsgRecord || cipher_len < kTagLen || cipher_len > kMaxCiphertextLen) {
        if (consumed) *consumed = SIZE_MAX;
        return 0;
    }
    const size_t total = 3 + static_cast<size_t>(cipher_len);
    if (len < total) return 0;  // ペイロードが揃っていない

    const size_t plain_len = cipher_len - kTagLen;
    if (out_cap < plain_len) {
        if (consumed) *consumed = SIZE_MAX;
        return 0;
    }
    if (!c_.aead_decrypt_be(out, s_.rx, s_.rx_counter, in + 3, cipher_len, nullptr, 0)) {
        if (consumed) *consumed = SIZE_MAX;
        return 0;
    }
    ++s_.rx_counter;
    if (consumed) *consumed = total;
    return plain_len;  // 0 バイトのレコードも合法
}

bool parse_early_noise(const uint8_t* in, size_t len, std::string* json, size_t* consumed)
{
    if (consumed) *consumed = 0;
    // "\xff\xff\xffTS" + uint32 BE 長
    static const uint8_t kMagic[5] = {0xff, 0xff, 0xff, 'T', 'S'};
    if (len < sizeof(kMagic)) return false;
    if (std::memcmp(in, kMagic, sizeof(kMagic)) != 0) return false;  // HTTP/2 の開始
    if (len < 9) return false;  // 長さがまだ来ていない（呼び出し側がもっと読む）

    const uint32_t json_len = load_be32(in + 5);
    if (json_len > kMaxPlaintextLen) return false;
    if (len < 9 + json_len) return false;
    if (json) json->assign(reinterpret_cast<const char*>(in + 9), json_len);
    if (consumed) *consumed = 9 + json_len;
    return true;
}

}  // namespace ts
