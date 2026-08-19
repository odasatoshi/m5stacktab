// DISCO メッセージのホストテスト。Ping を作って Pong を返す往復を検証する。
#include <cstdint>

#include "disco.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

struct Party {
    uint8_t priv[32], pub[32], shared[32];
};

// 2 者の disco 鍵を作り、共有鍵（beforenm）を計算する。
void setup(Party& a, Party& b)
{
    const auto& c = wg::default_crypto();
    CHECK(c.random_bytes(a.priv, 32));
    CHECK(c.random_bytes(b.priv, 32));
    CHECK(c.dh_pubkey(a.pub, a.priv));
    CHECK(c.dh_pubkey(b.pub, b.priv));

    uint8_t dh_a[32], dh_b[32];
    CHECK(c.dh(dh_a, a.priv, b.pub));
    CHECK(c.dh(dh_b, b.priv, a.pub));
    CHECK(std::memcmp(dh_a, dh_b, 32) == 0);
    wg::box_beforenm(a.shared, dh_a);
    wg::box_beforenm(b.shared, dh_b);
    CHECK(std::memcmp(a.shared, b.shared, 32) == 0);
}

void test_ping_pong()
{
    Party a{}, b{};
    setup(a, b);
    const auto& c = wg::default_crypto();

    uint8_t tx_id[ts::kDiscoTxIdLen];
    uint8_t nonce[ts::kDiscoNonceLen];
    uint8_t node_key[32];
    CHECK(c.random_bytes(tx_id, sizeof(tx_id)));
    CHECK(c.random_bytes(nonce, sizeof(nonce)));
    CHECK(c.random_bytes(node_key, sizeof(node_key)));

    // A が Ping を送る
    uint8_t      pkt[256];
    const size_t n = ts::disco_build_ping(pkt, sizeof(pkt), a.pub, a.shared, tx_id, node_key,
                                          nonce);
    CHECK(n > 0);
    // magic は "TS💬"
    CHECK(pkt[0] == 0x54 && pkt[1] == 0x53 && pkt[2] == 0xf0 && pkt[3] == 0x9f && pkt[4] == 0x92 &&
          pkt[5] == 0xac);

    // B が受け取る
    uint8_t sender[32];
    CHECK(ts::disco_is_packet(pkt, n, sender));
    CHECK(std::memcmp(sender, a.pub, 32) == 0);

    ts::DiscoType type{};
    ts::DiscoPing ping{};
    CHECK(ts::disco_open(pkt, n, b.shared, &type, &ping));
    CHECK(type == ts::DiscoType::kPing);
    CHECK(std::memcmp(ping.tx_id, tx_id, sizeof(tx_id)) == 0);
    CHECK(ping.has_node_key);
    CHECK(std::memcmp(ping.node_key, node_key, 32) == 0);

    // B が Pong を返す（受信元アドレスをそのまま入れる）
    uint8_t src_ip[16];
    ts::disco_v4_mapped(src_ip, 0x0100a8c0);  // 192.168.0.1 (network byte order)
    CHECK(src_ip[10] == 0xff && src_ip[11] == 0xff);
    CHECK(src_ip[12] == 192 && src_ip[13] == 168 && src_ip[14] == 0 && src_ip[15] == 1);

    uint8_t pong_nonce[ts::kDiscoNonceLen];
    CHECK(c.random_bytes(pong_nonce, sizeof(pong_nonce)));
    uint8_t      pong[256];
    const size_t m = ts::disco_build_pong(pong, sizeof(pong), b.pub, b.shared, ping.tx_id, src_ip,
                                          41641, pong_nonce);
    CHECK(m > 0);

    // A が Pong を受け取る
    ts::DiscoType ptype{};
    CHECK(ts::disco_open(pong, m, a.shared, &ptype, nullptr));
    CHECK(ptype == ts::DiscoType::kPong);
}

void test_ping_without_node_key()
{
    Party a{}, b{};
    setup(a, b);
    uint8_t tx_id[ts::kDiscoTxIdLen] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    uint8_t nonce[ts::kDiscoNonceLen] = {};

    uint8_t      pkt[256];
    const size_t n = ts::disco_build_ping(pkt, sizeof(pkt), a.pub, a.shared, tx_id, nullptr, nonce);
    CHECK(n > 0);

    ts::DiscoType type{};
    ts::DiscoPing ping{};
    CHECK(ts::disco_open(pkt, n, b.shared, &type, &ping));
    CHECK(type == ts::DiscoType::kPing);
    CHECK(!ping.has_node_key);
    CHECK(std::memcmp(ping.tx_id, tx_id, sizeof(tx_id)) == 0);
}

void test_rejects()
{
    Party a{}, b{}, evil{};
    setup(a, b);
    setup(evil, evil);  // 無関係な鍵

    uint8_t tx_id[ts::kDiscoTxIdLen] = {};
    uint8_t nonce[ts::kDiscoNonceLen] = {};
    uint8_t pkt[256];
    const size_t n = ts::disco_build_ping(pkt, sizeof(pkt), a.pub, a.shared, tx_id, nullptr, nonce);
    CHECK(n > 0);

    ts::DiscoType type{};
    // 別の共有鍵では復号できない
    CHECK(!ts::disco_open(pkt, n, evil.shared, &type, nullptr));

    // magic が違えば DISCO ではない
    uint8_t other[256];
    std::memcpy(other, pkt, n);
    other[0] ^= 1;
    CHECK(!ts::disco_is_packet(other, n, nullptr));
    CHECK(!ts::disco_open(other, n, b.shared, &type, nullptr));

    // 改竄されていれば復号に失敗する
    std::memcpy(other, pkt, n);
    other[n - 1] ^= 1;
    CHECK(!ts::disco_open(other, n, b.shared, &type, nullptr));

    // 短すぎるパケット
    CHECK(!ts::disco_is_packet(pkt, 10, nullptr));
    CHECK(!ts::disco_open(pkt, 10, b.shared, &type, nullptr));
    CHECK(!ts::disco_is_packet(nullptr, 0, nullptr));

    // 未知のバージョンは扱わない（body[1] を書き換えると MAC が合わなくなるので、
    // 正しい鍵で作り直してから検証する）
    uint8_t body[2 + ts::kDiscoTxIdLen] = {static_cast<uint8_t>(ts::DiscoType::kPing), 0x09};
    uint8_t crafted[256];
    std::memcpy(crafted, pkt, ts::kDiscoHeaderLen);
    CHECK(wg::secretbox_seal(crafted + ts::kDiscoHeaderLen, body, sizeof(body), nonce, a.shared));
    CHECK(!ts::disco_open(crafted, ts::kDiscoHeaderLen + sizeof(body) + wg::kBoxMacLen, b.shared,
                          &type, nullptr));

    // バッファ不足では 0
    CHECK(ts::disco_build_ping(pkt, 20, a.pub, a.shared, tx_id, nullptr, nonce) == 0);
    uint8_t src_ip[16] = {};
    CHECK(ts::disco_build_pong(pkt, 20, a.pub, a.shared, tx_id, src_ip, 1, nonce) == 0);
}

}  // namespace

int main()
{
    test_ping_pong();
    test_ping_without_node_key();
    test_rejects();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
