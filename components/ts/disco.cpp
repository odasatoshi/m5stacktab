#include <cstdint>
#include "disco.hpp"

#include <cstring>

#include "salsa20.hpp"

namespace ts {
namespace {

// "TS💬" = 54 53 f0 9f 92 ac
constexpr uint8_t kMagic[kDiscoMagicLen] = {0x54, 0x53, 0xf0, 0x9f, 0x92, 0xac};

constexpr uint8_t kMsgVersion = 0;

}  // namespace

bool disco_is_packet(const uint8_t* pkt, size_t len, uint8_t sender_pub[kDiscoKeyLen])
{
    if (!pkt || len < kDiscoHeaderLen + wg::kBoxMacLen + 2) return false;
    if (std::memcmp(pkt, kMagic, kDiscoMagicLen) != 0) return false;
    if (sender_pub) std::memcpy(sender_pub, pkt + kDiscoMagicLen, kDiscoKeyLen);
    return true;
}

bool disco_open(const uint8_t* pkt, size_t len, const uint8_t shared_key[32], DiscoType* type,
                DiscoPing* ping_out)
{
    if (!disco_is_packet(pkt, len, nullptr)) return false;

    const uint8_t* nonce  = pkt + kDiscoMagicLen + kDiscoKeyLen;
    const uint8_t* box    = pkt + kDiscoHeaderLen;
    const size_t   box_len = len - kDiscoHeaderLen;
    if (box_len < wg::kBoxMacLen + 2) return false;

    uint8_t plain[256];
    const size_t plain_len = box_len - wg::kBoxMacLen;
    if (plain_len > sizeof(plain)) return false;
    if (!wg::secretbox_open(plain, box, box_len, nonce, shared_key)) return false;

    if (plain[1] != kMsgVersion) return false;  // 未知のバージョンは扱わない
    const auto t = static_cast<DiscoType>(plain[0]);
    if (type) *type = t;

    if (t == DiscoType::kPing) {
        if (plain_len < 2 + kDiscoTxIdLen) return false;
        if (ping_out) {
            std::memcpy(ping_out->tx_id, plain + 2, kDiscoTxIdLen);
            // NodeKey は省略されることがある。
            ping_out->has_node_key = (plain_len >= 2 + kDiscoTxIdLen + 32);
            if (ping_out->has_node_key) {
                std::memcpy(ping_out->node_key, plain + 2 + kDiscoTxIdLen, 32);
            }
        }
    }
    return true;
}

size_t disco_build_ping(uint8_t* out, size_t cap, const uint8_t my_disco_pub[kDiscoKeyLen],
                        const uint8_t shared_key[32], const uint8_t tx_id[kDiscoTxIdLen],
                        const uint8_t* node_key, const uint8_t nonce[kDiscoNonceLen])
{
    const size_t body_len = 2 + kDiscoTxIdLen + (node_key ? 32u : 0u);
    const size_t need     = kDiscoHeaderLen + body_len + wg::kBoxMacLen;
    if (!out || cap < need) return 0;

    uint8_t body[2 + kDiscoTxIdLen + 32];
    body[0] = static_cast<uint8_t>(DiscoType::kPing);
    body[1] = kMsgVersion;
    std::memcpy(body + 2, tx_id, kDiscoTxIdLen);
    if (node_key) std::memcpy(body + 2 + kDiscoTxIdLen, node_key, 32);

    std::memcpy(out, kMagic, kDiscoMagicLen);
    std::memcpy(out + kDiscoMagicLen, my_disco_pub, kDiscoKeyLen);
    std::memcpy(out + kDiscoMagicLen + kDiscoKeyLen, nonce, kDiscoNonceLen);
    if (!wg::secretbox_seal(out + kDiscoHeaderLen, body, body_len, nonce, shared_key)) return 0;
    return need;
}

size_t disco_build_pong(uint8_t* out, size_t cap, const uint8_t my_disco_pub[kDiscoKeyLen],
                        const uint8_t shared_key[32], const uint8_t tx_id[kDiscoTxIdLen],
                        const uint8_t src_ip[16], uint16_t src_port,
                        const uint8_t nonce[kDiscoNonceLen])
{
    constexpr size_t kBodyLen = 2 + kDiscoTxIdLen + 16 + 2;
    const size_t     need     = kDiscoHeaderLen + kBodyLen + wg::kBoxMacLen;
    if (!out || cap < need) return 0;

    uint8_t body[kBodyLen];
    body[0] = static_cast<uint8_t>(DiscoType::kPong);
    body[1] = kMsgVersion;
    std::memcpy(body + 2, tx_id, kDiscoTxIdLen);
    std::memcpy(body + 2 + kDiscoTxIdLen, src_ip, 16);
    // ポートはビッグエンディアン。
    body[2 + kDiscoTxIdLen + 16]     = static_cast<uint8_t>(src_port >> 8);
    body[2 + kDiscoTxIdLen + 16 + 1] = static_cast<uint8_t>(src_port);

    std::memcpy(out, kMagic, kDiscoMagicLen);
    std::memcpy(out + kDiscoMagicLen, my_disco_pub, kDiscoKeyLen);
    std::memcpy(out + kDiscoMagicLen + kDiscoKeyLen, nonce, kDiscoNonceLen);
    if (!wg::secretbox_seal(out + kDiscoHeaderLen, body, kBodyLen, nonce, shared_key)) return 0;
    return need;
}

void disco_v4_mapped(uint8_t out[16], uint32_t ipv4_be)
{
    std::memset(out, 0, 10);
    out[10] = 0xff;
    out[11] = 0xff;
    std::memcpy(out + 12, &ipv4_be, 4);
}

}  // namespace ts
