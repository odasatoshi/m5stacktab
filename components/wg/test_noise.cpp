// Noise IK ハンドシェイクのホストテスト。開始側と応答側を同じプロセス内で突き合わせる。
//
//   c++ -std=c++17 -Wall -Wextra -O1 -I/opt/homebrew/include -I components/wg
//       -o /tmp/test_noise components/wg/test_noise.cpp components/wg/noise.cpp
//       components/wg/blake2s.cpp components/wg/crypto_mbedtls.cpp
//       -L/opt/homebrew/lib -lmbedcrypto && /tmp/test_noise
#include <cstdint>
#include "noise.hpp"
#include "transport.hpp"

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

std::string hex(const uint8_t* p, size_t n)
{
    std::string s;
    char        b[3];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

void test_x25519_rfc7748()
{
    // RFC 7748 6.1 のテストベクタ。DH が正しくないとハンドシェイクは全部無意味になる。
    const auto& c = wg::default_crypto();
    uint8_t     alice_priv[32] = {0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
                                  0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
                                  0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
                                  0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
    uint8_t     bob_priv[32]   = {0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
                                  0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
                                  0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
                                  0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb};
    uint8_t alice_pub[32], bob_pub[32], shared_a[32], shared_b[32];
    CHECK(c.dh_pubkey(alice_pub, alice_priv));
    CHECK(c.dh_pubkey(bob_pub, bob_priv));
    CHECK(hex(alice_pub, 32) == "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK(hex(bob_pub, 32) == "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

    CHECK(c.dh(shared_a, alice_priv, bob_pub));
    CHECK(c.dh(shared_b, bob_priv, alice_pub));
    CHECK(hex(shared_a, 32) == "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(std::memcmp(shared_a, shared_b, 32) == 0);
}

void test_aead_roundtrip()
{
    const auto& c = wg::default_crypto();
    uint8_t     key[32];
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    const char* msg = "wireguard transport payload";
    const size_t len = std::strlen(msg);
    uint8_t      ad[32] = {0xAA};

    uint8_t cipher[128] = {};
    CHECK(c.aead_encrypt(cipher, key, 42, reinterpret_cast<const uint8_t*>(msg), len, ad, 32));

    uint8_t plain[128] = {};
    CHECK(c.aead_decrypt(plain, key, 42, cipher, len + wg::kTagLen, ad, 32));
    CHECK(std::memcmp(plain, msg, len) == 0);

    // カウンタが違えば復号できない（nonce に効いていることの確認）
    CHECK(!c.aead_decrypt(plain, key, 43, cipher, len + wg::kTagLen, ad, 32));
    // AD が違えば復号できない
    ad[0] = 0xBB;
    CHECK(!c.aead_decrypt(plain, key, 42, cipher, len + wg::kTagLen, ad, 32));
    // 1 ビット改竄で失敗する
    ad[0] = 0xAA;
    cipher[3] ^= 1;
    CHECK(!c.aead_decrypt(plain, key, 42, cipher, len + wg::kTagLen, ad, 32));
}

void test_handshake_roundtrip()
{
    const auto& c = wg::default_crypto();

    uint8_t init_priv[32], resp_priv[32];
    CHECK(c.random_bytes(init_priv, 32));
    CHECK(c.random_bytes(resp_priv, 32));
    uint8_t init_pub[32], resp_pub[32];
    CHECK(c.dh_pubkey(init_pub, init_priv));
    CHECK(c.dh_pubkey(resp_pub, resp_priv));

    wg::Handshake initiator(c), responder(c);
    CHECK(initiator.set_keys(init_priv, resp_pub));
    // 応答側は相手の静的鍵をハンドシェイクから学ぶが、set_keys では自分の鍵だけ確定させる。
    CHECK(responder.set_keys(resp_priv, init_pub));

    const uint8_t timestamp[12] = {0x40, 0, 0, 0, 0x67, 0x89, 0xab, 0xcd, 0, 0, 0, 1};

    uint8_t msg1[148];
    CHECK(initiator.create_initiation(msg1, 0x11223344, timestamp));
    CHECK(msg1[0] == wg::kMsgInitiation);

    uint8_t learned_static[32], learned_ts[12];
    CHECK(responder.consume_initiation(msg1, learned_static, learned_ts));
    // 応答側は開始側の静的公開鍵とタイムスタンプを正しく取り出せる
    CHECK(std::memcmp(learned_static, init_pub, 32) == 0);
    CHECK(std::memcmp(learned_ts, timestamp, 12) == 0);
    CHECK(responder.remote_index() == 0x11223344);

    wg::Keypair resp_keys, init_keys;
    uint8_t     msg2[92];
    CHECK(responder.create_response(msg2, 0x55667788, resp_keys));
    CHECK(msg2[0] == wg::kMsgResponse);
    CHECK(initiator.consume_response(msg2, init_keys));

    // 両者の鍵が交差して一致する（開始側の send = 応答側の recv）
    CHECK(std::memcmp(init_keys.send, resp_keys.recv, 32) == 0);
    CHECK(std::memcmp(init_keys.recv, resp_keys.send, 32) == 0);
    CHECK(init_keys.initiator);
    CHECK(!resp_keys.initiator);
    CHECK(init_keys.local_index == 0x11223344);
    CHECK(init_keys.remote_index == 0x55667788);
    CHECK(resp_keys.remote_index == 0x11223344);

    // 導出した鍵で実際に通信できる
    const char* payload = "hello over wireguard";
    uint8_t     ct[128] = {}, pt[128] = {};
    CHECK(c.aead_encrypt(ct, init_keys.send, 0, reinterpret_cast<const uint8_t*>(payload),
                         std::strlen(payload), nullptr, 0));
    CHECK(c.aead_decrypt(pt, resp_keys.recv, 0, ct, std::strlen(payload) + wg::kTagLen, nullptr, 0));
    CHECK(std::memcmp(pt, payload, std::strlen(payload)) == 0);
}

void test_handshake_rejects_tampering()
{
    const auto& c = wg::default_crypto();
    uint8_t     a_priv[32], b_priv[32], a_pub[32], b_pub[32];
    CHECK(c.random_bytes(a_priv, 32));
    CHECK(c.random_bytes(b_priv, 32));
    CHECK(c.dh_pubkey(a_pub, a_priv));
    CHECK(c.dh_pubkey(b_pub, b_priv));

    const uint8_t ts[12] = {0x40};

    // mac1 が合わない initiation は拒否する（別の相手宛のパケット）
    {
        uint8_t wrong_pub[32];
        uint8_t wrong_priv[32];
        CHECK(c.random_bytes(wrong_priv, 32));
        CHECK(c.dh_pubkey(wrong_pub, wrong_priv));

        wg::Handshake initiator(c), responder(c);
        CHECK(initiator.set_keys(a_priv, wrong_pub));  // 別人宛に作る
        CHECK(responder.set_keys(b_priv, a_pub));
        uint8_t msg1[148];
        CHECK(initiator.create_initiation(msg1, 1, ts));
        uint8_t s[32], t[12];
        CHECK(!responder.consume_initiation(msg1, s, t));
    }

    // 改竄された initiation は復号に失敗する
    {
        wg::Handshake initiator(c), responder(c);
        CHECK(initiator.set_keys(a_priv, b_pub));
        CHECK(responder.set_keys(b_priv, a_pub));
        uint8_t msg1[148];
        CHECK(initiator.create_initiation(msg1, 1, ts));
        msg1[50] ^= 0x01;  // 暗号化された静的鍵の中身をいじる
        // mac1 は msg1[0..116) を対象にするので、ここを壊すと mac1 も合わなくなる
        uint8_t s[32], t[12];
        CHECK(!responder.consume_initiation(msg1, s, t));
    }

    // receiver index が違う response は拒否する
    {
        wg::Handshake initiator(c), responder(c);
        CHECK(initiator.set_keys(a_priv, b_pub));
        CHECK(responder.set_keys(b_priv, a_pub));
        uint8_t msg1[148], msg2[92];
        CHECK(initiator.create_initiation(msg1, 0xAABBCCDD, ts));
        uint8_t s[32], t[12];
        CHECK(responder.consume_initiation(msg1, s, t));
        wg::Keypair rk, ik;
        CHECK(responder.create_response(msg2, 2, rk));
        msg2[8] ^= 0xFF;  // receiver index を壊す
        CHECK(!initiator.consume_response(msg2, ik));
    }
}

void test_psk()
{
    // psk を両者で共有していれば成立し、片方だけだと失敗する
    const auto& c = wg::default_crypto();
    uint8_t     a_priv[32], b_priv[32], a_pub[32], b_pub[32], psk[32];
    CHECK(c.random_bytes(a_priv, 32));
    CHECK(c.random_bytes(b_priv, 32));
    CHECK(c.random_bytes(psk, 32));
    CHECK(c.dh_pubkey(a_pub, a_priv));
    CHECK(c.dh_pubkey(b_pub, b_priv));
    const uint8_t ts[12] = {0x40};

    {
        wg::Handshake i(c), r(c);
        CHECK(i.set_keys(a_priv, b_pub, psk));
        CHECK(r.set_keys(b_priv, a_pub, psk));
        uint8_t m1[148], m2[92], s[32], t[12];
        wg::Keypair rk, ik;
        CHECK(i.create_initiation(m1, 1, ts));
        CHECK(r.consume_initiation(m1, s, t));
        CHECK(r.create_response(m2, 2, rk));
        CHECK(i.consume_response(m2, ik));
        CHECK(std::memcmp(ik.send, rk.recv, 32) == 0);
    }
    {
        wg::Handshake i(c), r(c);
        CHECK(i.set_keys(a_priv, b_pub, psk));
        CHECK(r.set_keys(b_priv, a_pub));  // psk なし
        uint8_t m1[148], m2[92], s[32], t[12];
        wg::Keypair rk, ik;
        CHECK(i.create_initiation(m1, 1, ts));
        CHECK(r.consume_initiation(m1, s, t));
        CHECK(r.create_response(m2, 2, rk));
        CHECK(!i.consume_response(m2, ik));  // empty の復号が通らない
    }
}

// ハンドシェイクで得た鍵で実際にパケットを往復させる。
void test_transport()
{
    const auto& c = wg::default_crypto();
    uint8_t     a_priv[32], b_priv[32], a_pub[32], b_pub[32];
    CHECK(c.random_bytes(a_priv, 32));
    CHECK(c.random_bytes(b_priv, 32));
    CHECK(c.dh_pubkey(a_pub, a_priv));
    CHECK(c.dh_pubkey(b_pub, b_priv));

    wg::Handshake i(c), r(c);
    CHECK(i.set_keys(a_priv, b_pub));
    CHECK(r.set_keys(b_priv, a_pub));
    const uint8_t ts[12] = {0x40};
    uint8_t       m1[148], m2[92], st[32], tsout[12];
    wg::Keypair   ik, rk;
    CHECK(i.create_initiation(m1, 0x1111, ts));
    CHECK(r.consume_initiation(m1, st, tsout));
    CHECK(r.create_response(m2, 0x2222, rk));
    CHECK(i.consume_response(m2, ik));

    wg::Transport ti(c), tr(c);
    ti.set_keypair(ik, 0);
    tr.set_keypair(rk, 0);
    CHECK(ti.ready());

    // 開始側 → 応答側
    const char* payload = "the quick brown fox";
    const size_t plen   = std::strlen(payload);
    uint8_t      pkt[256], got[256];
    const size_t n = ti.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>(payload), plen);
    CHECK(n == wg::kTransportHeader + plen + wg::kTagLen);
    CHECK(pkt[0] == wg::kMsgTransport);

    bool         valid = false;
    const size_t m     = tr.decrypt(got, sizeof(got), pkt, n, &valid);
    CHECK(valid);
    CHECK(m == plen);
    CHECK(std::memcmp(got, payload, plen) == 0);

    // 同じパケットをもう一度: リプレイとして捨てる
    CHECK(tr.decrypt(got, sizeof(got), pkt, n, &valid) == 0);
    CHECK(!valid);
    CHECK(tr.replay_drops() == 1);

    // 応答側 → 開始側（逆方向の鍵）
    const size_t n2 = tr.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("reply"), 5);
    CHECK(n2 > 0);
    CHECK(ti.decrypt(got, sizeof(got), pkt, n2, &valid) == 5);
    CHECK(valid);
    CHECK(std::memcmp(got, "reply", 5) == 0);

    // keepalive（平文長 0）は成功扱いで 0 バイト
    const size_t n3 = ti.encrypt(pkt, sizeof(pkt), nullptr, 0);
    CHECK(n3 == wg::kTransportHeader + wg::kTagLen);
    CHECK(tr.decrypt(got, sizeof(got), pkt, n3, &valid) == 0);
    CHECK(valid);

    // 改竄されたパケットは復号できず、リプレイウィンドウも汚さない
    const size_t before = tr.recv_max();
    const size_t n4 = ti.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("xyz"), 3);
    pkt[20] ^= 0x01;
    CHECK(tr.decrypt(got, sizeof(got), pkt, n4, &valid) == 0);
    CHECK(!valid);
    CHECK(tr.recv_max() == before);

    // 宛先インデックスが違うパケットは弾く
    const size_t n5 = ti.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("abc"), 3);
    pkt[4] ^= 0xFF;
    CHECK(tr.decrypt(got, sizeof(got), pkt, n5, &valid) == 0);
}

// 順序が入れ替わって届いてもウィンドウ内なら受け取る。
void test_replay_window()
{
    const auto& c = wg::default_crypto();
    wg::Keypair kp;
    for (int i = 0; i < 32; ++i) {
        kp.send[i] = static_cast<uint8_t>(i);
        kp.recv[i] = static_cast<uint8_t>(i);
    }
    kp.local_index  = 7;
    kp.remote_index = 7;

    wg::Transport tx(c), rx(c);
    tx.set_keypair(kp, 0);
    rx.set_keypair(kp, 0);

    // 10 個作って、順番を入れ替えて渡す
    std::vector<std::vector<uint8_t>> packets;
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> p(64);
        uint8_t              body[4] = {static_cast<uint8_t>(i), 0, 0, 0};
        const size_t         n = tx.encrypt(p.data(), p.size(), body, sizeof(body));
        CHECK(n > 0);
        p.resize(n);
        packets.push_back(std::move(p));
    }
    const int order[10] = {5, 0, 9, 2, 1, 8, 3, 7, 4, 6};
    uint8_t   got[64];
    bool      valid = false;
    for (int idx : order) {
        CHECK(rx.decrypt(got, sizeof(got), packets[idx].data(), packets[idx].size(), &valid) == 4);
        CHECK(valid);
        CHECK(got[0] == static_cast<uint8_t>(idx));
    }
    // 全部リプレイになる
    for (int idx : order) {
        CHECK(rx.decrypt(got, sizeof(got), packets[idx].data(), packets[idx].size(), &valid) == 0);
    }
    CHECK(rx.replay_drops() == 10);
    CHECK(rx.recv_max() == 9);
}

// transport パケットの宛先インデックス（どの鍵世代で送られたか）。
uint32_t recv_index(const uint8_t* pkt)
{
    return static_cast<uint32_t>(pkt[4]) | (static_cast<uint32_t>(pkt[5]) << 8) |
           (static_cast<uint32_t>(pkt[6]) << 16) | (static_cast<uint32_t>(pkt[7]) << 24);
}

// #29: rekey が交差したときに通信が切れないこと。
// 本家 wg と同じく「1 つ前の鍵を残す」「応答側は相手のデータを受けるまで
// 新しい鍵で送らない」の二点が満たされていれば、断は発生しない。
void test_rekey_crossover()
{
    const auto& c = wg::default_crypto();
    // 開始側の鍵を作る。send/recv を別の値にしておかないと、向きの間違いを見逃す。
    auto mk = [](uint8_t seed, uint32_t local, uint32_t remote) {
        wg::Keypair kp;
        for (int i = 0; i < 32; ++i) {
            kp.send[i] = static_cast<uint8_t>(seed + i);
            kp.recv[i] = static_cast<uint8_t>(seed + 0x80 + i);
        }
        kp.local_index  = local;
        kp.remote_index = remote;
        kp.initiator    = true;
        return kp;
    };
    // 同じ世代の応答側の鍵。send/recv とインデックスを入れ替えて initiator を落とす。
    auto peer_of = [](const wg::Keypair& k) {
        wg::Keypair o = k;
        std::memcpy(o.send, k.recv, 32);
        std::memcpy(o.recv, k.send, 32);
        o.local_index  = k.remote_index;
        o.remote_index = k.local_index;
        o.initiator    = !k.initiator;
        return o;
    };

    const wg::Keypair a1 = mk(0x10, 1001, 1002), b1 = peer_of(a1);
    const wg::Keypair a2 = mk(0x40, 2001, 2002), b2 = peer_of(a2);
    const wg::Keypair a3 = mk(0x70, 3001, 3002), b3 = peer_of(a3);

    // 応答側の鍵は initiator が落ちているので、確認前として扱われる。
    CHECK(a1.initiator);
    CHECK(!b1.initiator);

    wg::Transport a(c), b(c);
    a.set_keypair(a1, 0);
    b.set_keypair(b1, 0);

    uint8_t pkt[128], got[128];
    bool    valid = false;

    // 世代 1 で普通に通る。ここで b は世代 1 を確認済みに昇格させる。
    size_t n = a.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("old"), 3);
    CHECK(n > 0);
    CHECK(b.decrypt(got, sizeof(got), pkt, n, &valid) == 3);
    CHECK(valid);
    CHECK(b.current_confirmed());

    // a が世代 1 で送ったパケットが飛んでいる最中に rekey が起きる状況。
    uint8_t      inflight[128];
    const size_t inflight_len =
        a.encrypt(inflight, sizeof(inflight), reinterpret_cast<const uint8_t*>("fly"), 3);
    CHECK(inflight_len > 0);

    // b は応答側として世代 2 を受け取る（未確認）。
    b.set_keypair(b2, 0);
    CHECK(b.has_previous());
    CHECK(!b.current_confirmed());

    // 1 つ前を残しているので、飛んでいた世代 1 のパケットもまだ復号できる。
    // ここが #29 の本体で、以前は捨てていたので数秒間ぶんが落ちていた。
    valid = false;
    CHECK(b.decrypt(got, sizeof(got), inflight, inflight_len, &valid) == 3);
    CHECK(valid);
    CHECK(std::memcmp(got, "fly", 3) == 0);

    // 未確認のうちは b は**古い鍵で**送る。a はまだ世代 2 を知らないので、
    // 新しい鍵で送ってしまうと a 側で全部落ちる。
    n = b.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("rev"), 3);
    CHECK(n > 0);
    CHECK(recv_index(pkt) == b1.remote_index);
    valid = false;
    CHECK(a.decrypt(got, sizeof(got), pkt, n, &valid) == 3);
    CHECK(valid);

    // a が世代 2 に切り替えて（開始側なので確認済み）データを送ると、
    // b はそれを受けて世代 2 を確認済みに昇格させる。
    a.set_keypair(a2, 0);
    CHECK(a.current_confirmed());
    n = a.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("new"), 3);
    CHECK(n > 0);
    valid = false;
    CHECK(b.decrypt(got, sizeof(got), pkt, n, &valid) == 3);
    CHECK(valid);
    CHECK(b.current_confirmed());

    // 以後 b は世代 2 で送る。
    n = b.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("ok!"), 3);
    CHECK(n > 0);
    CHECK(recv_index(pkt) == b2.remote_index);
    valid = false;
    CHECK(a.decrypt(got, sizeof(got), pkt, n, &valid) == 3);
    CHECK(valid);

    // レビュー指摘（BLOCKER）: 未確認の世代を 2 連続で入れても、確認済みの世代を
    // 押し出さないこと。こちらの msg2 が落ちるとピアは msg1 を再送するので、
    // 未確認世代だけが並ぶ。無条件に降格すると「ピアが一度も持っていない世代」しか
    // 残らず、送信が全損したままピアの送信を待つしかなくなる。
    {
        wg::Transport e(c), peer(c);
        e.set_keypair(b1, 0);
        peer.set_keypair(a1, 0);
        n = peer.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("hi"), 2);
        valid = false;
        CHECK(e.decrypt(got, sizeof(got), pkt, n, &valid) == 2);
        CHECK(valid);
        CHECK(e.current_confirmed());

        e.set_keypair(b2, 0);  // msg1 を受けて応答（未確認）
        e.set_keypair(b3, 0);  // msg2 が落ちてピアが msg1 を再送（また未確認）
        CHECK(!e.current_confirmed());

        // 送信は世代 1 のまま。世代 2 で送るとピアには絶対に届かない。
        n = e.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("up"), 2);
        CHECK(n > 0);
        CHECK(recv_index(pkt) == b1.remote_index);
        valid = false;
        CHECK(peer.decrypt(got, sizeof(got), pkt, n, &valid) == 2);
        CHECK(valid);

        // 受信方向も世代 1 で通り続ける。
        n = peer.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("dn"), 2);
        valid = false;
        CHECK(e.decrypt(got, sizeof(got), pkt, n, &valid) == 2);
        CHECK(valid);
    }

    // 世代ごとにリプレイウィンドウが独立していること。
    // 共有していると、世代 2 のカウンタ 0 が世代 1 の受信済みと衝突して落ちる。
    wg::Transport d(c), s1(c), s2(c), s3(c);
    d.set_keypair(b1, 0);
    s1.set_keypair(a1, 0);
    s2.set_keypair(a2, 0);
    s3.set_keypair(a3, 0);
    for (int i = 0; i < 5; ++i) {
        n = s1.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("x"), 1);
        valid = false;
        CHECK(d.decrypt(got, sizeof(got), pkt, n, &valid) == 1);
        CHECK(valid);
    }
    d.set_keypair(b2, 0);
    for (int i = 0; i < 5; ++i) {
        n = s2.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("y"), 1);
        valid = false;
        CHECK(d.decrypt(got, sizeof(got), pkt, n, &valid) == 1);  // カウンタ 0..4 が再び通る
        CHECK(valid);
    }
    CHECK(d.replay_drops() == 0);

    // さらに世代 3 が来たら、世代 1 は落ちる（保持は 1 つ前まで）。
    // 世代 2 は確認済みになっているので、こちらは正しく押し出される。
    CHECK(d.current_confirmed());
    d.set_keypair(b3, 0);
    n = s1.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("z"), 1);
    valid = false;
    CHECK(d.decrypt(got, sizeof(got), pkt, n, &valid) == 0);
    CHECK(!valid);
    // 世代 3 は未確認なので、送信はまだ世代 2 のまま。
    n = d.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("w"), 1);
    CHECK(n > 0);
    CHECK(recv_index(pkt) == b2.remote_index);
    // 世代 3 でデータが来たら昇格して世代 3 で送る。
    n = s3.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("v"), 1);
    valid = false;
    CHECK(d.decrypt(got, sizeof(got), pkt, n, &valid) == 1);
    CHECK(valid);
    CHECK(d.current_confirmed());
    n = d.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("u"), 1);
    CHECK(recv_index(pkt) == b3.remote_index);
    valid = false;
    CHECK(s3.decrypt(got, sizeof(got), pkt, n, &valid) == 1);
    CHECK(valid);
}

// #30: 鍵世代に寿命を持たせる。相手が既に破棄した鍵で送り続けると、
// 送信は成功するのに届かず、ローカルには何の信号も出ない。
void test_previous_expires()
{
    const auto& c = wg::default_crypto();
    auto        mk = [](uint8_t seed, uint32_t local, uint32_t remote) {
        wg::Keypair kp;
        for (int i = 0; i < 32; ++i) {
            kp.send[i] = static_cast<uint8_t>(seed + i);
            kp.recv[i] = static_cast<uint8_t>(seed + 0x80 + i);
        }
        kp.local_index  = local;
        kp.remote_index = remote;
        kp.initiator    = true;
        return kp;
    };
    auto peer_of = [](const wg::Keypair& k) {
        wg::Keypair o = k;
        std::memcpy(o.send, k.recv, 32);
        std::memcpy(o.recv, k.send, 32);
        o.local_index  = k.remote_index;
        o.remote_index = k.local_index;
        o.initiator    = !k.initiator;
        return o;
    };
    const wg::Keypair a1 = mk(0x10, 1001, 1002), b1 = peer_of(a1);
    const wg::Keypair a2 = mk(0x40, 2001, 2002), b2 = peer_of(a2);

    constexpr int64_t kSec = 1000000LL;
    wg::Transport     b(c), a(c);
    a.set_keypair(a1, 0);
    b.set_keypair(b1, 0);

    uint8_t pkt[128], got[128];
    bool    valid = false;
    size_t  n     = a.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("hi"), 2);
    CHECK(b.decrypt(got, sizeof(got), pkt, n, &valid) == 2);
    CHECK(valid);  // b の世代 1 が確認済みになる

    // 応答側として世代 2 を受け取る（未確認）。世代 1 が「1 つ前」に降格。
    b.set_keypair(b2, 10 * kSec);
    CHECK(b.has_previous());
    CHECK(!b.current_confirmed());

    // 寿命内なら捨てない。送信も 1 つ前で続く。
    CHECK(!b.expire_previous(100 * kSec));
    CHECK(b.has_previous());
    n = b.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("x"), 1);
    CHECK(recv_index(pkt) == b1.remote_index);

    // REJECT_AFTER_TIME (180 秒) を越えたら捨てる。**基準は世代 1 が
    // 生まれた時刻**（0）なので、181 秒で切れる。
    CHECK(b.expire_previous(181 * kSec));
    CHECK(!b.has_previous());
    // 2 度目は false（もう無い）。
    CHECK(!b.expire_previous(200 * kSec));

    // 捨てたあとは、未確認でも現世代で送るしかない。
    n = b.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("y"), 1);
    CHECK(n > 0);
    CHECK(recv_index(pkt) == b2.remote_index);

    // 1 つ前が無くても現世代の復号は続く。
    wg::Transport a2t(c);
    a2t.set_keypair(a2, 181 * kSec);
    n     = a2t.encrypt(pkt, sizeof(pkt), reinterpret_cast<const uint8_t*>("z"), 1);
    valid = false;
    CHECK(b.decrypt(got, sizeof(got), pkt, n, &valid) == 1);
    CHECK(valid);
    CHECK(b.current_confirmed());

    // 1 つ前が無い状態で expire を呼んでも落ちない。
    wg::Transport fresh(c);
    CHECK(!fresh.expire_previous(0));
    fresh.set_keypair(a1, 0);
    CHECK(!fresh.expire_previous(1000 * kSec));  // prev_ が無いので何もしない
}

// レビュー指摘の回帰テスト。
void test_review_regressions()
{
    const auto& c = wg::default_crypto();

    // 乱数が取れないときは握手を作らせない（全ゼロの鍵で成立してはいけない）。
    // ダミーの Crypto で「乱数が失敗する」状況を作る。
    static wg::Crypto broken = c;
    broken.random_bytes = [](uint8_t* out, size_t len) -> bool {
        std::memset(out, 0, len);
        return false;
    };
    {
        uint8_t priv[32], pub[32];
        CHECK(c.random_bytes(priv, 32));
        CHECK(c.dh_pubkey(pub, priv));
        wg::Handshake h(broken);
        CHECK(h.set_keys(priv, pub));
        uint8_t       m1[148];
        const uint8_t ts[12] = {0x40};
        // 乱数が失敗したら initiation を作らない
        CHECK(!h.create_initiation(m1, 1, ts));
    }

    // 応答側は set_keys で指定した相手以外の握手を拒否する。
    {
        uint8_t a_priv[32], b_priv[32], c_priv[32];
        uint8_t a_pub[32], b_pub[32], c_pub[32];
        CHECK(c.random_bytes(a_priv, 32));
        CHECK(c.random_bytes(b_priv, 32));
        CHECK(c.random_bytes(c_priv, 32));
        CHECK(c.dh_pubkey(a_pub, a_priv));
        CHECK(c.dh_pubkey(b_pub, b_priv));
        CHECK(c.dh_pubkey(c_pub, c_priv));

        const uint8_t ts[12] = {0x40};
        uint8_t       m1[148], st[32], tsout[12];

        // C が B に握手を仕掛ける。B は A だけを相手と想定している。
        wg::Handshake attacker(c), responder(c);
        CHECK(attacker.set_keys(c_priv, b_pub));
        CHECK(responder.set_keys(b_priv, a_pub));
        CHECK(attacker.create_initiation(m1, 1, ts));
        CHECK(!responder.consume_initiation(m1, st, tsout));

        // 正しい相手 (A) なら通る
        wg::Handshake good(c), responder2(c);
        CHECK(good.set_keys(a_priv, b_pub));
        CHECK(responder2.set_keys(b_priv, a_pub));
        CHECK(good.create_initiation(m1, 1, ts));
        CHECK(responder2.consume_initiation(m1, st, tsout));

        // 相手を指定しない (全ゼロ) 応答側は誰でも受け入れる（サーバ役の用途）
        uint8_t zero[32] = {};
        wg::Handshake open_responder(c);
        CHECK(open_responder.set_keys(b_priv, zero));
        CHECK(attacker.create_initiation(m1, 2, ts));
        CHECK(open_responder.consume_initiation(m1, st, tsout));
        CHECK(std::memcmp(st, c_pub, 32) == 0);
    }
}

}  // namespace

int main()
{
    test_x25519_rfc7748();
    test_aead_roundtrip();
    test_handshake_roundtrip();
    test_handshake_rejects_tampering();
    test_psk();
    test_transport();
    test_replay_window();
    test_rekey_crossover();
    test_previous_expires();
    test_review_regressions();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
