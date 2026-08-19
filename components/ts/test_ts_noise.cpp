// ts2021 Noise 層のホストテスト。クライアントとサーバを同じプロセス内で突き合わせる。
//
//   M3=/opt/homebrew/opt/mbedtls@3
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -I$M3/include -I components/wg -I components/ts
//       -o /tmp/test_ts components/ts/test_ts_noise.cpp components/ts/ts_noise.cpp
//       components/wg/noise.cpp components/wg/blake2s.cpp components/wg/crypto_mbedtls.cpp
//       -L$M3/lib -lmbedcrypto && /tmp/test_ts
#include <cstdint>

#include "ts_noise.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

constexpr uint16_t kCapVer = 131;

struct Pair {
    uint8_t client_priv[32], client_pub[32];
    uint8_t server_priv[32], server_pub[32];
};

Pair make_keys()
{
    const auto& c = wg::default_crypto();
    Pair        p{};
    CHECK(c.random_bytes(p.client_priv, 32));
    CHECK(c.random_bytes(p.server_priv, 32));
    CHECK(c.dh_pubkey(p.client_pub, p.client_priv));
    CHECK(c.dh_pubkey(p.server_pub, p.server_priv));
    return p;
}

void test_message_shapes()
{
    // ts2021 のメッセージは WireGuard と長さもレイアウトも違う。
    const auto& c = wg::default_crypto();
    const Pair  k = make_keys();

    ts::Handshake client(c);
    CHECK(client.init(k.client_priv, k.server_pub, kCapVer));

    uint8_t msg1[ts::kInitiationLen];
    CHECK(client.create_initiation(msg1));
    CHECK(ts::kInitiationLen == 101);
    // [0..2) version BE, [2] type, [3..5) payload len BE
    CHECK(msg1[0] == 0 && msg1[1] == 131);
    CHECK(msg1[2] == ts::kMsgInitiation);
    CHECK(msg1[3] == 0 && msg1[4] == 96);
}

void test_handshake_roundtrip()
{
    const auto& c = wg::default_crypto();
    const Pair  k = make_keys();

    ts::Handshake client(c), server(c);
    CHECK(client.init(k.client_priv, k.server_pub, kCapVer));
    // サーバは自分の静的鍵で待ち受ける（相手の machine key はハンドシェイクで学ぶ）。
    CHECK(server.init(k.server_priv, k.server_pub, kCapVer));

    uint8_t msg1[ts::kInitiationLen];
    CHECK(client.create_initiation(msg1));

    uint8_t learned[32];
    CHECK(server.consume_initiation(msg1, learned));
    CHECK(std::memcmp(learned, k.client_pub, 32) == 0);

    uint8_t     msg2[ts::kResponseLen];
    ts::Session server_sess{}, client_sess{};
    CHECK(server.create_response(msg2, server_sess));
    CHECK(msg2[0] == ts::kMsgResponse);
    CHECK(msg2[1] == 0 && msg2[2] == 48);
    CHECK(client.consume_response(msg2, client_sess));

    // 鍵が交差して一致する
    CHECK(client_sess.valid && server_sess.valid);
    CHECK(std::memcmp(client_sess.tx, server_sess.rx, 32) == 0);
    CHECK(std::memcmp(client_sess.rx, server_sess.tx, 32) == 0);
}

void test_capability_version_mismatch()
{
    // prologue にバージョンが入るので、食い違うと復号に失敗する（黙って通ってはいけない）。
    const auto& c = wg::default_crypto();
    const Pair  k = make_keys();

    ts::Handshake client(c), server(c);
    CHECK(client.init(k.client_priv, k.server_pub, 131));
    CHECK(server.init(k.server_priv, k.server_pub, 145));

    uint8_t msg1[ts::kInitiationLen], learned[32];
    CHECK(client.create_initiation(msg1));
    CHECK(!server.consume_initiation(msg1, learned));
}

void test_records()
{
    const auto& c = wg::default_crypto();
    const Pair  k = make_keys();

    ts::Handshake client(c), server(c);
    CHECK(client.init(k.client_priv, k.server_pub, kCapVer));
    CHECK(server.init(k.server_priv, k.server_pub, kCapVer));
    uint8_t     msg1[ts::kInitiationLen], msg2[ts::kResponseLen], learned[32];
    ts::Session cs{}, ss{};
    CHECK(client.create_initiation(msg1));
    CHECK(server.consume_initiation(msg1, learned));
    CHECK(server.create_response(msg2, ss));
    CHECK(client.consume_response(msg2, cs));

    ts::Record c_rec(c), s_rec(c);
    c_rec.set_session(cs);
    s_rec.set_session(ss);

    // クライアント → サーバ
    const char*  msg = "GET /machine/map HTTP/2";
    const size_t len = std::strlen(msg);
    uint8_t      wire[256], plain[256];
    const size_t n = c_rec.seal(wire, sizeof(wire), reinterpret_cast<const uint8_t*>(msg), len);
    CHECK(n == 3 + len + 16);
    CHECK(wire[0] == ts::kMsgRecord);
    // 長さはビッグエンディアン
    CHECK(((wire[1] << 8) | wire[2]) == static_cast<int>(len + 16));

    size_t       consumed = 0;
    const size_t got      = s_rec.open(plain, sizeof(plain), wire, n, &consumed);
    CHECK(got == len);
    CHECK(consumed == n);
    CHECK(std::memcmp(plain, msg, len) == 0);

    // カウンタが進むので、同じレコードをもう一度渡すと復号できない
    CHECK(s_rec.open(plain, sizeof(plain), wire, n, &consumed) == 0);
    CHECK(consumed == SIZE_MAX);
}

void test_record_streaming()
{
    // レコード境界はバイトストリームの都合で割れる。細切れに渡しても組み立てられること。
    const auto& c = wg::default_crypto();
    const Pair  k = make_keys();
    ts::Handshake client(c), server(c);
    CHECK(client.init(k.client_priv, k.server_pub, kCapVer));
    CHECK(server.init(k.server_priv, k.server_pub, kCapVer));
    uint8_t     msg1[ts::kInitiationLen], msg2[ts::kResponseLen], learned[32];
    ts::Session cs{}, ss{};
    CHECK(client.create_initiation(msg1));
    CHECK(server.consume_initiation(msg1, learned));
    CHECK(server.create_response(msg2, ss));
    CHECK(client.consume_response(msg2, cs));

    ts::Record tx(c), rx(c);
    tx.set_session(cs);
    rx.set_session(ss);

    // 3 レコードを 1 本のバイト列にまとめる
    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        uint8_t      body[32];
        std::memset(body, static_cast<uint8_t>('a' + i), sizeof(body));
        uint8_t      rec[128];
        const size_t n = tx.seal(rec, sizeof(rec), body, sizeof(body));
        CHECK(n > 0);
        stream.insert(stream.end(), rec, rec + n);
    }

    // 1 バイトずつ足していって、揃ったところで 1 レコード取り出せること
    std::vector<uint8_t> buf;
    int                  decoded = 0;
    for (uint8_t b : stream) {
        buf.push_back(b);
        for (;;) {
            uint8_t      out[128];
            size_t       consumed = 0;
            const size_t got = rx.open(out, sizeof(out), buf.data(), buf.size(), &consumed);
            CHECK(consumed != SIZE_MAX);
            if (consumed == 0) break;  // まだ足りない
            CHECK(got == 32);
            CHECK(out[0] == static_cast<uint8_t>('a' + decoded));
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(consumed));
            ++decoded;
        }
    }
    CHECK(decoded == 3);
    CHECK(buf.empty());
}

void test_record_limits()
{
    const auto& c = wg::default_crypto();
    ts::Session s{};
    for (int i = 0; i < 32; ++i) {
        s.tx[i] = static_cast<uint8_t>(i);
        s.rx[i] = static_cast<uint8_t>(i);
    }
    s.valid = true;

    ts::Record r(c);
    r.set_session(s);

    // 平文の上限は 4077。超えたら送らない（サーバが切断する）。
    std::vector<uint8_t> big(ts::kMaxPlaintextLen + 1, 0x41);
    std::vector<uint8_t> out(ts::kMaxMessageSize + 64);
    CHECK(r.seal(out.data(), out.size(), big.data(), big.size()) == 0);
    CHECK(r.seal(out.data(), out.size(), big.data(), ts::kMaxPlaintextLen) > 0);

    // 0 バイトのレコードも合法
    ts::Record r2(c);
    r2.set_session(s);
    const size_t n = r2.seal(out.data(), out.size(), nullptr, 0);
    CHECK(n == 3 + 16);

    ts::Record r3(c);
    r3.set_session(s);
    uint8_t plain[16];
    size_t  consumed = 0;
    CHECK(r3.open(plain, sizeof(plain), out.data(), n, &consumed) == 0);
    CHECK(consumed == n);

    // 不正な type は接続を閉じるべき
    out[0] = 0x09;
    ts::Record r4(c);
    r4.set_session(s);
    r4.open(plain, sizeof(plain), out.data(), n, &consumed);
    CHECK(consumed == SIZE_MAX);
}

void test_nonce_endianness()
{
    // ts2021 はビッグエンディアン、WireGuard はリトルエンディアン。
    // counter=1 で結果が違うこと（同じなら片方の実装が間違っている）。
    const auto& c = wg::default_crypto();
    uint8_t     key[32] = {};
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    const uint8_t plain[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t       le[64] = {}, be[64] = {};
    CHECK(c.aead_encrypt(le, key, 1, plain, sizeof(plain), nullptr, 0));
    CHECK(c.aead_encrypt_be(be, key, 1, plain, sizeof(plain), nullptr, 0));
    CHECK(std::memcmp(le, be, sizeof(plain) + 16) != 0);

    // counter=0 では同じ（ノンスが全ゼロなので）
    uint8_t le0[64] = {}, be0[64] = {};
    CHECK(c.aead_encrypt(le0, key, 0, plain, sizeof(plain), nullptr, 0));
    CHECK(c.aead_encrypt_be(be0, key, 0, plain, sizeof(plain), nullptr, 0));
    CHECK(std::memcmp(le0, be0, sizeof(plain) + 16) == 0);

    // BE で暗号化したものは BE でしか復号できない
    uint8_t out[64];
    CHECK(c.aead_decrypt_be(out, key, 1, be, sizeof(plain) + 16, nullptr, 0));
    CHECK(!c.aead_decrypt(out, key, 1, be, sizeof(plain) + 16, nullptr, 0));
}

void test_early_noise()
{
    // "\xff\xff\xffTS" + uint32 BE 長 + JSON
    const std::string json = "{\"nodeKeyChallenge\":\"chalpub:abc\"}";
    std::vector<uint8_t> buf = {0xff, 0xff, 0xff, 'T', 'S'};
    const uint32_t       n   = static_cast<uint32_t>(json.size());
    buf.push_back(static_cast<uint8_t>(n >> 24));
    buf.push_back(static_cast<uint8_t>(n >> 16));
    buf.push_back(static_cast<uint8_t>(n >> 8));
    buf.push_back(static_cast<uint8_t>(n));
    buf.insert(buf.end(), json.begin(), json.end());

    std::string got;
    size_t      consumed = 0;
    CHECK(ts::parse_early_noise(buf.data(), buf.size(), &got, &consumed));
    CHECK(got == json);
    CHECK(consumed == buf.size());

    // 途中までしか来ていなければ false（もっと読む必要がある）
    CHECK(!ts::parse_early_noise(buf.data(), 7, &got, &consumed));
    CHECK(consumed == 0);

    // HTTP/2 の SETTINGS フレームは EarlyNoise ではない
    const uint8_t h2[9] = {0, 0, 0, 0x04, 0, 0, 0, 0, 0};
    CHECK(!ts::parse_early_noise(h2, sizeof(h2), &got, &consumed));
}

}  // namespace

int main()
{
    test_message_shapes();
    test_handshake_roundtrip();
    test_capability_version_mismatch();
    test_records();
    test_record_streaming();
    test_record_limits();
    test_nonce_endianness();
    test_early_noise();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
