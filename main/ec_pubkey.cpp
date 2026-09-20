// CI のホスト側 mbedTLS は 2.28 で、3.6 で増えた keypair のアクセサ
// (`mbedtls_ecp_keypair_get_group_id` / `mbedtls_ecp_write_public_key`) が無い。
// フィールドを素で見るための脱出口（メンバーのリネームだけでレイアウトは同じ）。
// ESP-IDF は 3.6 なので、ここでは両方で通る書き方にする。
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include "ec_pubkey.hpp"

#include <cstddef>
#include <cstdint>

#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/version.h>

namespace {

// SSH の公開鍵 blob。`string` は 4 バイトのビッグエンディアン長 + 中身。
void put_string(std::string* blob, const void* p, size_t n)
{
    const uint32_t be = static_cast<uint32_t>(n);
    blob->push_back(static_cast<char>((be >> 24) & 0xFF));
    blob->push_back(static_cast<char>((be >> 16) & 0xFF));
    blob->push_back(static_cast<char>((be >> 8) & 0xFF));
    blob->push_back(static_cast<char>(be & 0xFF));
    blob->append(static_cast<const char*>(p), n);
}

// 秘密鍵を展開する。**mbedTLS 3.0 で `f_rng` が追加された**ので、ホストの 2.28
// (CI の libmbedtls-dev) と ESP-IDF の 3.6 で呼び出し形が違う。libssh2 自身も
// 同じガードをしている (`libssh2/src/mbedtls.c:862`)。
// EC の展開に乱数源が要るのは座標ブラインディングのためで、2.x では不要。
int parse_key(mbedtls_pk_context* pk, const std::string& pem, const std::string& passphrase,
              mbedtls_ctr_drbg_context* drbg)
{
    const unsigned char* key = reinterpret_cast<const unsigned char*>(pem.data());
    const unsigned char* pwd = passphrase.empty()
                                   ? nullptr
                                   : reinterpret_cast<const unsigned char*>(passphrase.data());
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_pk_parse_key(pk, key, pem.size() + 1, pwd, passphrase.size(),
                                mbedtls_ctr_drbg_random, drbg);
#else
    (void)drbg;
    return mbedtls_pk_parse_key(pk, key, pem.size() + 1, pwd, passphrase.size());
#endif
}

}  // namespace

const char* ec_pubkey_status_name(EcPubKeyStatus st)
{
    switch (st) {
        case EcPubKeyStatus::kOk:
            return "ok";
        case EcPubKeyStatus::kNotEc:
            return "EC ではない (RSA は libssh2 側で導出できる)";
        // **「EC が壊れている」と決めつけない。** PKCS#8 (`BEGIN PRIVATE KEY`) は
        // RSA でもここを通るので、型を見る前に落ちた場合は中立的な言い方にする。
        case EcPubKeyStatus::kParse:
            return "PEM を読めない (named curve でない / パスフレーズが違う / 未対応の形式)";
        case EcPubKeyStatus::kNotP256:
            return "P-256 以外の曲線 (型名も点の長さも変わる)";
        case EcPubKeyStatus::kNoPoint:
            return "公開鍵の点 (Q) を組み立てられない";
        case EcPubKeyStatus::kNoRng:
            return "乱数源が無い";
    }
    return "不明";
}

EcPubKeyStatus ec_ssh_pubkey(const std::string& pem, const std::string& passphrase,
                             mbedtls_ctr_drbg_context* drbg, std::string* out)
{
    // **RSA を毎回展開しないための安い先読み。** `ec_pem_to_der` と同じ型。
    // PKCS#8 (`BEGIN PRIVATE KEY`) は RSA でも来るので、それは下で型を見て弾く。
    if (pem.find("EC PRIVATE KEY") == std::string::npos &&
        pem.find("BEGIN PRIVATE KEY") == std::string::npos) {
        return EcPubKeyStatus::kNotEc;
    }
    if (!drbg) return EcPubKeyStatus::kNoRng;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    // mbedTLS は鍵データの末尾が NUL であることを要求する。std::string の data() は
    // data()[size()] が '\0' なので、長さに +1 して渡す。
    const int prc = parse_key(&pk, pem, passphrase, drbg);
    if (prc != 0) {
        mbedtls_pk_free(&pk);
        return EcPubKeyStatus::kParse;
    }
    if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY) {
        mbedtls_pk_free(&pk);
        return EcPubKeyStatus::kNotEc;
    }
    const mbedtls_ecp_keypair* ec = mbedtls_pk_ec(pk);
    // **P-256 だけを通す。** 他の曲線は型名 (`ecdsa-sha2-nistp384` など) も点の長さも
    // 変わる。使っていないものを想像で通すと、実機で繋いでみるまで気づけない。
    if (!ec || ec->grp.id != MBEDTLS_ECP_DP_SECP256R1) {
        mbedtls_pk_free(&pk);
        return EcPubKeyStatus::kNotP256;
    }
    // 非圧縮の点 = 0x04 + X(32) + Y(32) = 65 バイト。
    unsigned char q[133];
    size_t        qlen = 0;
    const int     wrc  = mbedtls_ecp_point_write_binary(&ec->grp, &ec->Q,
                                                        MBEDTLS_ECP_PF_UNCOMPRESSED, &qlen, q,
                                                        sizeof(q));
    mbedtls_pk_free(&pk);
    if (wrc != 0 || qlen != 65 || q[0] != 0x04) return EcPubKeyStatus::kNoPoint;

    static const char kType[]  = "ecdsa-sha2-nistp256";
    static const char kCurve[] = "nistp256";
    std::string       blob;
    put_string(&blob, kType, sizeof(kType) - 1);
    put_string(&blob, kCurve, sizeof(kCurve) - 1);
    put_string(&blob, q, qlen);

    // base64 の長さは (n + 2) / 3 * 4。104 バイト -> 140 文字。
    unsigned char b64[256];
    size_t        b64len = 0;
    // ponytail: P-256 固定なので溢れようがない（140 < 256）。溢れるとしたら
    // 曲線を増やしたときで、そのときは kNoPoint ではなく専用のもので出したい。
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64len,
                              reinterpret_cast<const unsigned char*>(blob.data()),
                              blob.size()) != 0) {
        return EcPubKeyStatus::kNoPoint;
    }
    out->assign(kType);
    out->push_back(' ');
    out->append(reinterpret_cast<const char*>(b64), b64len);
    return EcPubKeyStatus::kOk;
}
