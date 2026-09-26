// DISCO レスポンダのホストテスト。ピアからの Ping に Pong を返すことを検証する。
#include <cstdint>

#include "disco_responder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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
    // 送信できたかは呼び出し側が教える契約。組み立てただけでは数えない
    // （送れていないのに「Pong を送った」と表示すると誤診断につながる）。
    CHECK(resp.pongs_sent() == 0);
    resp.note_send_result(true);
    CHECK(resp.pongs_sent() == 1);
    CHECK(resp.pongs_failed() == 0);
    resp.note_send_result(false);
    CHECK(resp.pongs_failed() == 1);
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
    CHECK(resp.pings_received() == 0);

    // DISCO ではないパケットは無視する（STUN の応答などが同じポートに来る）
    const uint8_t stun[20] = {0x01, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    CHECK(resp.handle(stun, sizeof(stun), 1, 2, pong, sizeof(pong)) == 0);

    // 鍵未設定では何もしない
    ts::DiscoResponder empty;
    CHECK(empty.handle(ping, plen, 1, 2, pong, sizeof(pong)) == 0);
    CHECK(!empty.add_peer(stranger_pub));
}

// レビュー指摘の回帰テスト。
void test_review_regressions()
{
    const auto& c = wg::default_crypto();

    // 鍵未設定のときは公開鍵を「鍵」として見せない（全ゼロを表示していた）。
    ts::DiscoResponder r;
    CHECK(!r.has_key());
    uint8_t priv[32];
    CHECK(c.random_bytes(priv, 32));
    CHECK(r.set_key(priv));
    CHECK(r.has_key());

    // PMTU 探索のためにパディングされた Ping にも応答する（以前は 256 バイト超で無言に落ちた）。
    uint8_t peer_priv[32], peer_pub[32], my_pub[32];
    CHECK(c.random_bytes(peer_priv, 32));
    CHECK(c.dh_pubkey(peer_pub, peer_priv));
    CHECK(c.dh_pubkey(my_pub, priv));
    CHECK(r.add_peer(peer_pub));

    uint8_t dh[32], shared[32];
    CHECK(c.dh(dh, peer_priv, my_pub));
    wg::box_beforenm(shared, dh);

    // 本体 (2 + 12) + パディング 800 バイトの Ping を手で組む
    std::vector<uint8_t> body(2 + ts::kDiscoTxIdLen + 800, 0);
    body[0] = static_cast<uint8_t>(ts::DiscoType::kPing);
    body[1] = 0;
    std::vector<uint8_t> pkt(ts::kDiscoHeaderLen + body.size() + wg::kBoxMacLen);
    const uint8_t        magic[6] = {0x54, 0x53, 0xf0, 0x9f, 0x92, 0xac};
    std::memcpy(pkt.data(), magic, sizeof(magic));
    std::memcpy(pkt.data() + 6, peer_pub, 32);
    uint8_t nonce[ts::kDiscoNonceLen] = {};
    std::memcpy(pkt.data() + 6 + 32, nonce, sizeof(nonce));
    CHECK(wg::secretbox_seal(pkt.data() + ts::kDiscoHeaderLen, body.data(), body.size(), nonce,
                             shared));

    uint8_t pong[256];
    CHECK(r.handle(pkt.data(), pkt.size(), 0x0100a8c0, 41641, pong, sizeof(pong)) > 0);
    CHECK(r.pings_received() == 1);
}

// 自分から Ping を打ち、返ってきた Pong の TxID を照合する (#98)。
void test_ping_and_pong()
{
    const auto& c = wg::default_crypto();
    uint8_t     me_priv[32], me_pub[32], peer_priv[32], peer_pub[32], node_key[32];
    CHECK(c.random_bytes(me_priv, 32));
    CHECK(c.random_bytes(peer_priv, 32));
    CHECK(c.random_bytes(node_key, 32));
    CHECK(c.dh_pubkey(me_pub, me_priv));
    CHECK(c.dh_pubkey(peer_pub, peer_priv));

    ts::DiscoResponder r;
    CHECK(r.set_key(me_priv));
    uint8_t ping[256];
    // 登録されていないピアには打てない（共有鍵が無い）
    CHECK(r.build_ping(peer_pub, node_key, ping, sizeof(ping)) == 0);
    CHECK(r.add_peer(peer_pub));
    const size_t plen = r.build_ping(peer_pub, node_key, ping, sizeof(ping));
    CHECK(plen > 0);

    // ピア側で開く: 送り主は自分の disco 鍵、NodeKey は自分の node 鍵
    uint8_t sender[32];
    CHECK(ts::disco_is_packet(ping, plen, sender));
    CHECK(std::memcmp(sender, me_pub, 32) == 0);
    uint8_t dh[32], peer_shared[32];
    CHECK(c.dh(dh, peer_priv, me_pub));
    wg::box_beforenm(peer_shared, dh);
    ts::DiscoType type{};
    ts::DiscoPing got{};
    CHECK(ts::disco_open(ping, plen, peer_shared, &type, &got));
    CHECK(type == ts::DiscoType::kPing);
    CHECK(got.has_node_key && std::memcmp(got.node_key, node_key, 32) == 0);

    auto pong_for = [&](const uint8_t* tx, uint8_t* out) {
        uint8_t src[16], nonce[ts::kDiscoNonceLen];
        ts::disco_v4_mapped(src, 0x1d00a8c0);
        CHECK(c.random_bytes(nonce, sizeof(nonce)));
        return ts::disco_build_pong(out, 256, peer_pub, peer_shared, tx, src, 41641, nonce);
    };
    uint8_t pong[256], reply[256];

    // TxID が違う Pong は数えない（こちらが打った Ping への返事ではない）
    uint8_t other[ts::kDiscoTxIdLen];
    CHECK(c.random_bytes(other, sizeof(other)));
    size_t n = pong_for(other, pong);
    CHECK(r.handle(pong, n, 0x6500a8c0, 41641, reply, sizeof(reply)) == 0);  // 返信はしない
    CHECK(r.pongs_received() == 0);

    // 一致すれば数える。同じ Pong が二度来ても一度だけ
    n = pong_for(got.tx_id, pong);
    CHECK(r.handle(pong, n, 0x6500a8c0, 41641, reply, sizeof(reply)) == 0);
    CHECK(r.pongs_received() == 1);
    CHECK(r.handle(pong, n, 0x6500a8c0, 41641, reply, sizeof(reply)) == 0);
    CHECK(r.pongs_received() == 1);
    CHECK(r.pings_received() == 0);  // Pong を Ping と数えない

    // 再送した後に、前の回の Ping への Pong が遅れて来ても受ける
    size_t len1 = r.build_ping(peer_pub, node_key, ping, sizeof(ping));
    CHECK(len1 > 0);
    CHECK(ts::disco_open(ping, len1, peer_shared, &type, &got));
    uint8_t first_tx[ts::kDiscoTxIdLen];
    std::memcpy(first_tx, got.tx_id, sizeof(first_tx));
    CHECK(r.build_ping(peer_pub, node_key, ping, sizeof(ping)) > 0);
    n = pong_for(first_tx, pong);
    CHECK(r.handle(pong, n, 0x6500a8c0, 41641, reply, sizeof(reply)) == 0);
    CHECK(r.pongs_received() == 2);

    // 鍵を替えたら前の鍵で閉じた Pong は復号できない
    const size_t n_old = pong_for(got.tx_id, pong);
    uint8_t      other_priv[32];
    CHECK(c.random_bytes(other_priv, 32));
    CHECK(r.set_key(other_priv));
    CHECK(r.add_peer(peer_pub));
    CHECK(r.handle(pong, n_old, 0x6500a8c0, 41641, reply, sizeof(reply)) == 0);
    CHECK(r.pongs_received() == 2);
}

}  // namespace

int main()
{
    test_responder();
    test_unknown_peer_rejected();
    test_review_regressions();
    test_ping_and_pong();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
