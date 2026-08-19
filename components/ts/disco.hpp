#pragma once
// Tailscale の DISCO（ピア間の経路探索）。
//
// パケット形式:
//   magic "TS💬" (6B) | senderDiscoPub (32B) | nonce (24B) | crypto_box(...)
//   box の中身: msgType (1B) | msgVersion (1B, 0) | payload
//     Ping (0x01): TxID (12B) | NodeKey (32B, 省略可) | padding
//     Pong (0x02): TxID (12B) | src IP (16B, v4-mapped) | src port (2B BE)
//
// **DISCO を実装しないと直接パスが確立しない。** ピア側は DISCO の Pong を受け取って
// 初めて送信元アドレスを bestAddr に固定するので、応答を返さないと netmap 再構成のあとに
// DERP 経由へ落ちる（DERP を実装していない側は以後届かなくなる）。
//
// まずは「Ping に Pong を返す」だけを実装する（自分から Ping は打たない）。
#include <cstddef>
#include <cstdint>
#include <string>

namespace ts {

constexpr size_t kDiscoMagicLen = 6;
constexpr size_t kDiscoKeyLen   = 32;
constexpr size_t kDiscoNonceLen = 24;
constexpr size_t kDiscoTxIdLen  = 12;
// magic + sender pub + nonce + box(2 バイトのヘッダ + MAC)
constexpr size_t kDiscoHeaderLen = kDiscoMagicLen + kDiscoKeyLen + kDiscoNonceLen;

enum class DiscoType : uint8_t {
    kPing          = 0x01,
    kPong          = 0x02,
    kCallMeMaybe   = 0x03,
};

struct DiscoPing {
    uint8_t tx_id[kDiscoTxIdLen] = {};
    uint8_t node_key[32]         = {};
    bool    has_node_key         = false;
};

// 受信したパケットが DISCO かを見る（復号はしない）。
// 送信者の disco 公開鍵を sender_pub に返す。
bool disco_is_packet(const uint8_t* pkt, size_t len, uint8_t sender_pub[kDiscoKeyLen]);

// DISCO パケットを復号して種別と中身を取り出す。
// shared_key は crypto_box_beforenm(sender_pub, 自分の disco 秘密鍵) の結果。
bool disco_open(const uint8_t* pkt, size_t len, const uint8_t shared_key[32], DiscoType* type,
                DiscoPing* ping_out);

// Pong を作る。src_ip / src_port は「Ping を受け取った送信元」をそのまま返す
// （相手がこれを見て自分の外側アドレスを知る）。
// 戻り値は書き込んだバイト数。0 なら失敗。
size_t disco_build_pong(uint8_t* out, size_t cap, const uint8_t my_disco_pub[kDiscoKeyLen],
                        const uint8_t shared_key[32], const uint8_t tx_id[kDiscoTxIdLen],
                        const uint8_t src_ip[16], uint16_t src_port,
                        const uint8_t nonce[kDiscoNonceLen]);

// テストと送信側のために Ping も作れるようにしておく。
size_t disco_build_ping(uint8_t* out, size_t cap, const uint8_t my_disco_pub[kDiscoKeyLen],
                        const uint8_t shared_key[32], const uint8_t tx_id[kDiscoTxIdLen],
                        const uint8_t* node_key, const uint8_t nonce[kDiscoNonceLen]);

// IPv4 アドレスを v4-mapped IPv6（::ffff:a.b.c.d）にする。Pong の src IP はこの形。
void disco_v4_mapped(uint8_t out[16], uint32_t ipv4_be);

}  // namespace ts
