#pragma once
// EC (ECDSA P-256) の秘密鍵から SSH の公開鍵の行を組み立てる (#84)。
//
// **libssh2 に導出させられない。** mbedTLS バックエンドの
// `_libssh2_mbedtls_pub_priv_key` は RSA 決め打ちで、ECDSA は "Key type not
// supported" で弾かれる (#75)。公開鍵を鍵の後ろに続けて書けばその経路を飛ばせるが、
// 連結し忘れると繋がるまで理由が見えない。秘密鍵があれば点は出せるので、
// 無いときだけ自分で組む。
//
// 形式は `ecdsa-sha2-nistp256 <base64>`。base64 の中身は SSH のワイヤ形式:
//   string("ecdsa-sha2-nistp256") || string("nistp256") || string(Q)
// `string` は 4 バイトのビッグエンディアン長 + 中身、`Q` は非圧縮の点 (0x04 || X || Y)。
//
// mbedTLS 以外の依存が無いのでホストでテストできる (`main/test_ec_pubkey.cpp`)。
// 鍵の展開に使う乱数源は呼び出し側から渡す（実機は `ssh_drbg()`）。
#include <string>

#include <mbedtls/ctr_drbg.h>

enum class EcPubKeyStatus {
    kOk,       // out に公開鍵の行が入った
    kNotEc,    // RSA など。libssh2 側の導出が通るので呼び出し側は何もしない
    kParse,    // PEM が読めない（named curve でない / パスフレーズが違う）
    kNotP256,  // 曲線が違う。型名も点の長さも変わるので組み立てない
    kNoPoint,  // 点が出ない、または想定した長さではない
    kNoRng,    // 乱数源が無い（座標ブラインディングができません）
};

// 判定理由をそのままログに出すための名前。
const char* ec_pubkey_status_name(EcPubKeyStatus st);

// 秘密鍵 PEM から SSH 公開鍵の行を組み立てる。kOk 以外なら out は触らない。
EcPubKeyStatus ec_ssh_pubkey(const std::string& pem, const std::string& passphrase,
                             mbedtls_ctr_drbg_context* drbg, std::string* out);
