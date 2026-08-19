// BLAKE2s と KDF のホストテスト。RFC 7693 / WireGuard のテストベクタで検証する。
//
//   c++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=undefined
//       -o /tmp/test_wg components/wg/test_wg.cpp components/wg/blake2s.cpp && /tmp/test_wg
#include "blake2s.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

std::string hex(const uint8_t* p, size_t len)
{
    std::string s;
    char        buf[3];
    for (size_t i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        s += buf;
    }
    return s;
}

void expect(const std::string& got, const std::string& want, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d\n  got  %s\n  want %s\n", __FILE__, line, got.c_str(),
                     want.c_str());
        std::abort();
    }
}
#define EXPECT(got, want) expect((got), (want), __LINE__)

void test_blake2s_rfc7693()
{
    // RFC 7693 Appendix B: BLAKE2s-256 of "abc"
    const char* abc = "abc";
    uint8_t     out[32];
    wg::blake2s(out, 32, reinterpret_cast<const uint8_t*>(abc), 3);
    EXPECT(hex(out, 32), "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");

    // 空入力（既知値）
    wg::blake2s(out, 32, nullptr, 0);
    EXPECT(hex(out, 32), "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9");

    // 短い digest
    uint8_t out16[16];
    wg::blake2s(out16, 16, reinterpret_cast<const uint8_t*>(abc), 3);
    EXPECT(hex(out16, 16), "aa4938119b1dc7b87cbad0ffd200d0ae");
}

void test_blake2s_keyed()
{
    // BLAKE2s keyed (RFC 7693 のテストベクタセットより: key = 00..1f, input = 空)
    std::vector<uint8_t> key(32);
    for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i);
    uint8_t out[32];
    wg::blake2s(out, 32, nullptr, 0, key.data(), key.size());
    EXPECT(hex(out, 32), "48a8997da407876b3d79c0d92325ad3b89cbb754d86ab71aee047ad345fd2c49");

    // input = 0x00
    const uint8_t one[1] = {0x00};
    wg::blake2s(out, 32, one, 1, key.data(), key.size());
    EXPECT(hex(out, 32), "40d15fee7c328830166ac3f918650f807e7e01e177258cdc0a39b11f598066f1");
}

void test_blake2s_streaming()
{
    // 分割して update しても同じ結果になること（ブロック境界を跨ぐ長さで確認）
    std::vector<uint8_t> data(200);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);

    uint8_t whole[32];
    wg::blake2s(whole, 32, data.data(), data.size());

    for (size_t split : {size_t(1), size_t(63), size_t(64), size_t(65), size_t(128), size_t(199)}) {
        wg::Blake2s h;
        h.init(32);
        h.update(data.data(), split);
        h.update(data.data() + split, data.size() - split);
        uint8_t part[32];
        h.final(part);
        EXPECT(hex(part, 32), hex(whole, 32));
    }
    // ちょうど 64 バイト（1 ブロック）でも最終ブロック判定が壊れないこと
    uint8_t block_out[32];
    wg::blake2s(block_out, 32, data.data(), 64);
    wg::Blake2s h;
    h.init(32);
    for (int i = 0; i < 64; ++i) h.update(data.data() + i, 1);
    uint8_t byte_out[32];
    h.final(byte_out);
    EXPECT(hex(byte_out, 32), hex(block_out, 32));
}

void test_hmac_blake2s()
{
    // HMAC-BLAKE2s は WireGuard の KDF の土台。長い鍵はハッシュに畳まれる。
    const uint8_t key[4]  = {'k', 'e', 'y', '!'};
    const uint8_t data[5] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t       a[32], b[32];
    wg::hmac_blake2s(a, key, sizeof(key), data, sizeof(data));
    wg::hmac_blake2s(b, key, sizeof(key), data, sizeof(data));
    EXPECT(hex(a, 32), hex(b, 32));  // 決定的

    // 鍵が 1 ビット違えば結果が変わる
    uint8_t key2[4] = {'k', 'e', 'y', '"'};
    uint8_t c[32];
    wg::hmac_blake2s(c, key2, sizeof(key2), data, sizeof(data));
    ++g_checks;
    if (std::memcmp(a, c, 32) == 0) {
        std::fprintf(stderr, "FAIL %s:%d: hmac ignored the key\n", __FILE__, __LINE__);
        std::abort();
    }

    // 64 バイト超の鍵も扱える（内部でハッシュに畳む）
    std::vector<uint8_t> long_key(100, 0xAB);
    uint8_t              d[32];
    wg::hmac_blake2s(d, long_key.data(), long_key.size(), data, sizeof(data));
    ++g_checks;
    if (std::memcmp(d, a, 32) == 0) {
        std::fprintf(stderr, "FAIL %s:%d: long key collided\n", __FILE__, __LINE__);
        std::abort();
    }
}

void test_kdf()
{
    // WireGuard の初期 chaining key: Hash(CONSTRUCTION)
    const char* construction = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
    uint8_t     ck[32];
    wg::blake2s(ck, 32, reinterpret_cast<const uint8_t*>(construction), std::strlen(construction));
    // WireGuard 仕様の既知値
    EXPECT(hex(ck, 32), "60e26daef327efc02ec335e2a025d2d016eb4206f87277f52d38d1988b78cd36");

    // KDF は n に応じて出力が増えるだけで、先の出力は変わらない
    const uint8_t input[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t       a1[32], b1[32], b2[32], c1[32], c2[32], c3[32];
    wg::kdf(ck, 32, input, sizeof(input), 1, a1, nullptr, nullptr);
    wg::kdf(ck, 32, input, sizeof(input), 2, b1, b2, nullptr);
    wg::kdf(ck, 32, input, sizeof(input), 3, c1, c2, c3);
    EXPECT(hex(a1, 32), hex(b1, 32));
    EXPECT(hex(b1, 32), hex(c1, 32));
    EXPECT(hex(b2, 32), hex(c2, 32));
    ++g_checks;
    if (std::memcmp(c2, c3, 32) == 0) {
        std::fprintf(stderr, "FAIL %s:%d: kdf outputs identical\n", __FILE__, __LINE__);
        std::abort();
    }
}

}  // namespace

int main()
{
    test_blake2s_rfc7693();
    test_blake2s_keyed();
    test_blake2s_streaming();
    test_hmac_blake2s();
    test_kdf();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
