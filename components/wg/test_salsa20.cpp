// XSalsa20-Poly1305 (NaCl crypto_box) のホストテスト。
// テストベクタは NaCl 20110221 の tests/ から。実装順に並べてあるので、
// 落ちた位置でどの層が壊れているか分かる。
#include <cstdint>

#include "salsa20.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

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

std::vector<uint8_t> unhex(const std::string& s)
{
    std::vector<uint8_t> out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<uint8_t>((nib(s[i]) << 4) | nib(s[i + 1])));
    }
    return out;
}

void expect(const std::string& got, const std::string& want, const char* what, int line)
{
    ++g_checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:%d (%s)\n  got  %s\n  want %s\n", __FILE__, line, what,
                     got.c_str(), want.c_str());
        std::abort();
    }
}
#define EXPECT(got, want, what) expect((got), (want), (what), __LINE__)

void check(bool cond, const char* expr, int line)
{
    ++g_checks;
    if (!cond) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
        std::abort();
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

const char* kSigmaHex = "657870616e642033322d62797465206b";  // "expand 32-byte k"

// 1. Salsa20 コア（NaCl tests/core4）
void test_salsa20_core()
{
    const auto k     = unhex("0102030405060708090a0b0c0d0e0f10c9cacbcccdcecfd0d1d2d3d4d5d6d7d8");
    const auto in    = unhex("65666768696a6b6c6d6e6f7071727374");
    const auto sigma = unhex(kSigmaHex);
    uint8_t    out[64];
    wg::salsa20_core(out, in.data(), k.data(), sigma.data());
    EXPECT(hex(out, 64),
           "45254427290f6bc1ff8b7a06aae9d9625990b66a1533c841ef31de22d772287e"
           "68c507e1c5991f02664e4cb054f5f6b8b1a0858206489577c0c384ecea67f64a",
           "core4");

    // 全ゼロ入力は全ゼロ出力（Salsa20 仕様 §8 の例）
    const std::vector<uint8_t> zk(32, 0), zin(16, 0), zc(16, 0);
    wg::salsa20_core(out, zin.data(), zk.data(), zc.data());
    EXPECT(hex(out, 64), std::string(128, '0'), "all-zero state");
}

// 2. HSalsa20（NaCl tests/core1, core5）
void test_hsalsa20()
{
    const auto sigma = unhex(kSigmaHex);
    {
        // core1: crypto_box_beforenm の中身そのもの
        const auto k  = unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        const auto in = unhex("00000000000000000000000000000000");
        uint8_t    out[32];
        wg::hsalsa20(out, in.data(), k.data(), sigma.data());
        EXPECT(hex(out, 32), "1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389",
               "core1");
    }
    {
        // core2: XSalsa20 の subkey 導出
        const auto k  = unhex("1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389");
        const auto in = unhex("69696ee955b62b73cd62bda875fc73d6");
        uint8_t    out[32];
        wg::hsalsa20(out, in.data(), k.data(), sigma.data());
        EXPECT(hex(out, 32), "dc908dda0b9344a953629b733820778880f3ceb421bb61b91cbd4c3e66256ce4",
               "core2");
    }
    {
        // core5: 独立したベクタ
        const auto k  = unhex("ee304fca27008d8c126f90027901d80f7f1d8b8dc936cf3b9f819692827e5777");
        const auto in = unhex("81918ef2a5e0da9b3e9060521e4bb352");
        uint8_t    out[32];
        wg::hsalsa20(out, in.data(), k.data(), sigma.data());
        EXPECT(hex(out, 32), "bc1b30fc072cc14075e4baa731b5a845ea9b11e9a5191f94e18cba8fd821a7cd",
               "core5");
    }
}

// 3. XSalsa20 キーストリーム（NaCl tests/stream3 と 192 バイトの拡張）
void test_xsalsa20_stream()
{
    const auto key   = unhex("1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389");
    const auto nonce = unhex("69696ee955b62b73cd62bda875fc73d68219e0036b7a0b37");

    uint8_t ks[192];
    wg::xsalsa20_stream(ks, 32, nonce.data(), key.data());
    EXPECT(hex(ks, 32), "eea6a7251c1e72916d11c2cb214d3c252539121d8e234e652d651fa4c8cff880",
           "stream3 (first 32)");

    // 192 バイト = 3 ブロック。ブロックカウンタの進み方も含めて検証する。
    wg::xsalsa20_stream(ks, sizeof(ks), nonce.data(), key.data());
    EXPECT(hex(ks, 192),
           "eea6a7251c1e72916d11c2cb214d3c252539121d8e234e652d651fa4c8cff880"
           "309e645a74e9e0a60d8243acd9177ab51a1beb8d5a2f5d700c093c5e55855796"
           "25337bd3ab619d615760d8c5b224a85b1d0efe0eb8a7ee163abb0376529fcc09"
           "bab506c618e13ce777d82c3ae9d1a6f972d4160287cbfe60bf2130fc0a6ff604"
           "9d0a5c8a82f429231f008082e845d7e189d37f9ed2b464e6b919e6523a8c1210"
           "bd52a02a4c3fe406d3085f5068d1909eeeca6369abc981a42e87fe665583f0ab",
           "stream 192 bytes");

    // 分割して呼んでも同じ（長さ違いで先頭が変わらないこと）
    uint8_t partial[64];
    wg::xsalsa20_stream(partial, sizeof(partial), nonce.data(), key.data());
    CHECK(std::memcmp(partial, ks, sizeof(partial)) == 0);
}

// 4-5. secretbox（NaCl tests/secretbox）
void test_secretbox()
{
    const auto key   = unhex("1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389");
    const auto nonce = unhex("69696ee955b62b73cd62bda875fc73d68219e0036b7a0b37");
    const auto plain = unhex(
        "be075fc53c81f2d5cf141316ebeb0c7b5228c52a4c62cbd44b66849b64244ffc"
        "e5ecbaaf33bd751a1ac728d45e6c61296cdc3c01233561f41db66cce314adb31"
        "0e3be8250c46f06dceea3a7fa1348057e2f6556ad6b1318a024a838f21af1fde"
        "048977eb48f59ffd4924ca1c60902e52f0a089bc76897040e082f937763848645e0705");
    CHECK(plain.size() == 131);

    std::vector<uint8_t> out(plain.size() + wg::kBoxMacLen);
    CHECK(wg::secretbox_seal(out.data(), plain.data(), plain.size(), nonce.data(), key.data()));
    // MAC 16 バイト + 暗号文 131 バイト（libsodium の easy 形式）
    EXPECT(hex(out.data(), out.size()),
           "f3ffc7703f9400e52a7dfb4b3d3305d98e993b9f48681273c29650ba32fc76ce"
           "48332ea7164d96a4476fb8c531a1186ac0dfc17c98dce87b4da7f011ec48c972"
           "71d2c20f9b928fe2270d6fb863d51738b48eeee314a7cc8ab932164548e526ae"
           "90224368517acfeabd6bb3732bc0e9da99832b61ca01b6de56244a9e88d5f9b3"
           "7973f622a43d14a6599b1f654cb45a74e355a5",
           "secretbox");

    // 復号して元に戻る
    std::vector<uint8_t> back(plain.size());
    CHECK(wg::secretbox_open(back.data(), out.data(), out.size(), nonce.data(), key.data()));
    CHECK(std::memcmp(back.data(), plain.data(), plain.size()) == 0);

    // 1 ビット改竄で失敗する（MAC を先に検証している）
    out[20] ^= 1;
    CHECK(!wg::secretbox_open(back.data(), out.data(), out.size(), nonce.data(), key.data()));
    out[20] ^= 1;
    // MAC の改竄も検出
    out[0] ^= 1;
    CHECK(!wg::secretbox_open(back.data(), out.data(), out.size(), nonce.data(), key.data()));
    out[0] ^= 1;
    // nonce が違えば失敗
    auto other_nonce = nonce;
    other_nonce[0] ^= 1;
    CHECK(!wg::secretbox_open(back.data(), out.data(), out.size(), other_nonce.data(), key.data()));
    // 短すぎる入力
    CHECK(!wg::secretbox_open(back.data(), out.data(), 8, nonce.data(), key.data()));
}

// 短い平文（32 バイト境界の前後）も正しく扱えること
void test_secretbox_short()
{
    const auto key   = unhex("1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389");
    const auto nonce = unhex("69696ee955b62b73cd62bda875fc73d68219e0036b7a0b37");

    for (size_t len : {size_t(0), size_t(1), size_t(31), size_t(32), size_t(33), size_t(64),
                       size_t(200)}) {
        std::vector<uint8_t> plain(len);
        for (size_t i = 0; i < len; ++i) plain[i] = static_cast<uint8_t>(i * 3 + 1);
        std::vector<uint8_t> sealed(len + wg::kBoxMacLen);
        CHECK(wg::secretbox_seal(sealed.data(), plain.data(), len, nonce.data(), key.data()));
        std::vector<uint8_t> opened(len ? len : 1);
        CHECK(wg::secretbox_open(opened.data(), sealed.data(), sealed.size(), nonce.data(),
                                 key.data()));
        CHECK(len == 0 || std::memcmp(opened.data(), plain.data(), len) == 0);
    }
}

// 6. crypto_box_beforenm（NaCl tests/box と同じ鍵）
void test_box_beforenm()
{
    // X25519(alicesk, bobpk) の生出力（NaCl tests/scalarmult5）
    const auto dh = unhex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    uint8_t    k[32];
    wg::box_beforenm(k, dh.data());
    // crypto_box の共有鍵 = secretbox の firstkey
    EXPECT(hex(k, 32), "1b27556473e985d462cd51197a9a46c76009549eac6474f206c4ee0844f68389",
           "beforenm");
}

}  // namespace

int main()
{
    test_salsa20_core();
    test_hsalsa20();
    test_xsalsa20_stream();
    test_secretbox();
    test_secretbox_short();
    test_box_beforenm();
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
