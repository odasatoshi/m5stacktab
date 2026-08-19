// DISCO レスポンダのホストテスト。ピアからの Ping に Pong を返すことを検証する。
#include <cstdint>

#include "disco_responder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "disco.hpp"
#include "noise.hpp"
#include "salsa20.hpp"

namespace {

int g_checks = 0;

void check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

void test_responder()
{
    const auto& c = wg::default_crypto();

    // 自分（Tab5）とピア（Mac）の disco 鍵
    uint8_t me_priv[32], me_pub[32], peer_priv[32], peer_pub[32];
    CHECK(c.random_bytes(me_priv, 32));
    CHECK(c.random_bytes(peer_priv, 32));
    CHECK(c.dh_pubkey(me_pub, me_priv));
    CHECK(c.dh_pubkey(peer_pub, peer_priv));

    ts::DiscoResponder resp;
    CHECK(resp.set_key(me_priv));
    CHECK(std::memcmp(resp.public_key(), me_pub, 32) == 0);
    CHECK(resp.peer_count() == 0);
    CHECK(resp.add_peer(peer_pub));
    CHECK(resp.peer_count() == 1);
    // 同じピアを二度足しても増えない
    CHECK(resp.add_peer(peer_pub));
    CHECK(resp.peer_count() == 1);

    // ピア側で共有鍵を作って Ping を組む
    uint8_t dh[32], peer_shared[32];
    CHECK(c.dh(dh, peer_priv, me_pub));
    wg::box_beforenm(peer_shared, dh);

    uint8_t tx_id[ts::kDiscoTxIdLen], nonce[ts::kDiscoNonceLen];
    CHECK(c.random_bytes(tx_id, sizeof(tx_id)));
    CHECK(c.random_bytes(nonce, sizeof(nonce)));
    uint8_t      ping[256];
    const size_t plen = ts::disco_build_ping(ping, sizeof(ping), peer_pub, peer_shared, tx_id,
                                             nullptr, nonce);
    CHECK(plen > 0);

    // レスポンダに渡すと Pong が返る
    const uint32_t src_ip   = 0x0100a8c0;  // 192.168.0.1
    const uint16_t src_port = 41641;
    uint8_t        pong[256];
    const size_t   n = resp.handle(ping, plen, src_ip, src_port, pong, sizeof(pong));
    CHECK(n > 0);
    CHECK(resp.pings_received() == 1);
    CHECK(resp.pongs_sent() == 1);

    // ピア側で Pong を復号して中身を確認する
    ts::DiscoType type{};
    CHECK(ts::disco_open(pong, n, peer_shared, &type, nullptr));
    CHECK(type == ts::DiscoType::kPong);
    // 送信者は自分の disco 公開鍵
    uint8_t sender[32];
    CHECK(ts::disco_is_packet(pong, n, sender));
    CHECK(std::memcmp(sender, me_pub, 32) == 0);

    // Pong の中身（TxID と送信元アドレス）を直接確かめる
    {
        uint8_t plain[256];
        const size_t box_len = n - ts::kDiscoHeaderLen;
        CHECK(wg::secretbox_open(plain, pong + ts::kDiscoHeaderLen, box_len,
                                 pong + ts::kDiscoMagicLen + ts::kDiscoKeyLen, peer_shared));
        CHECK(plain[0] == static_cast<uint8_t>(ts::DiscoType::kPong));
        CHECK(plain[1] == 0);
        CHECK(std::memcmp(plain + 2, tx_id, sizeof(tx_id)) == 0);
        const uint8_t* ip = plain + 2 + ts::kDiscoTxIdLen;
        // v4-mapped で 192.168.0.1
        CHECK(ip[10] == 0xff && ip[11] == 0xff);
        CHECK(ip[12] == 192 && ip[13] == 168 && ip[14] == 0 && ip[15] == 1);
        const uint16_t port = static_cast<uint16_t>((ip[16] << 8) | ip[17]);
        CHECK(port == src_port);
    }
}

void test_unknown_peer_rejected()
{
    const auto& c = wg::default_crypto();
    uint8_t me_priv[32], me_pub[32], stranger_priv[32], stranger_pub[32];
    CHECK(c.random_bytes(me_priv, 32));
    CHECK(c.random_bytes(stranger_priv, 32));
    CHECK(c.dh_pubkey(me_pub, me_priv));
    CHECK(c.dh_pubkey(stranger_pub, stranger_priv));

    ts::DiscoResponder resp;
    CHECK(resp.set_key(me_priv));
    // ピアを登録していない

    uint8_t dh[32], shared[32];
    CHECK(c.dh(dh, stranger_priv, me_pub));
    wg::box_beforenm(shared, dh);
    uint8_t tx_id[ts::kDiscoTxIdLen] = {}, nonce[ts::kDiscoNonceLen] = {};
    uint8_t ping[256], pong[256];
    const size_t plen = ts::disco_build_ping(ping, sizeof(ping), stranger_pub, shared, tx_id,
                                             nullptr, nonce);
    // netmap に載っていない相手には応答しない
    CHECK(resp.handle(ping, plen, 1, 2, pong, sizeof(pong)) == 0);
    CHECK(resp.unknown_peers() == 1);
    CHECK(resp.pongs_sent() == 0);

    // DISCO ではないパケットは無視する（STUN の応答などが同じポートに来る）
    const uint8_t stun[20] = {0x01, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    CHECK(resp.handle(stun, sizeof(stun), 1, 2, pong, sizeof(pong)) == 0);

    // 鍵未設定では何もしない
    ts::DiscoResponder empty;
    CHECK(empty.handle(ping, plen, 1, 2, pong, sizeof(pong)) == 0);
    CHECK(!empty.add_peer(stranger_pub));
}

}  // namespace

int main()
{
    test_responder();
    test_unknown_peer_rejected();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
