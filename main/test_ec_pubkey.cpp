// ec_pubkey.hpp のホストテスト (#84)。
//
// 期待値は macOS の `ssh-keygen -y` で出した行をそのまま焼いてある。
// **自分で自分の組み立てを確かめているテストだと、ワイヤ形式ごと間違えても
// 通ってしまう**ので、基準は OpenSSH 側から取った。
//
//   openssl ecparam -name prime256v1 -genkey -noout -out key.pem
//   ssh-keygen -y -f key.pem
#include <cstdio>
#include <cstring>
#include <string>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include "ec_pubkey.hpp"

namespace {

// 上の手順で作った鍵と、その `ssh-keygen -y` の出力。
const char* kP256 =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIAM7Za5CRZKQw9pm0jE94w86zwTXXFKGij4nlvk93PROoAoGCCqGSM49\n"
    "AwEHoUQDQgAEOc6LnoLybbT4dwGF6ohV+sdam/tQfH23RXjYeQuEx1AT5MVM0V2J\n"
    "B5wCI72dlKIx6pt/89eVNxkSpJNDfnGpMQ==\n"
    "-----END EC PRIVATE KEY-----\n";

// 同じ鍵を `openssl pkcs8 -topk8 -nocrypt` で入れ直したもの。
const char* kP256Pkcs8 =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgAztlrkJFkpDD2mbS\n"
    "MT3jDzrPBNdcUoaKPieW+T3c9E6hRANCAAQ5zouegvJttPh3AYXqiFX6x1qb+1B8\n"
    "fbdFeNh5C4THUBPkxUzRXYkHnAIjvZ2UojHqm3/z15U3GRKkk0N+cakx\n"
    "-----END PRIVATE KEY-----\n";

const char* kWantP256 =
    "ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTY"
    "AAABBBDnOi56C8m20+HcBheqIVfrHWpv7UHx9t0V42HkLhMdQE+TFTNFdiQecAiO9nZ"
    "SiMeqbf/PXlTcZEqSTQ35xqTE=";

// `openssl ecparam -name secp384r1 -genkey`
const char* kP384 =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MIGkAgEBBDDALTEsX+q4tGbcIQn/w29Ii8422gsJNikHDkLYuP4tGRxT1HGuRSL1\n"
    "BF7xIgOVhoSgBwYFK4EEACKhZANiAATi3IcAWUDrP4ebRI4a+aoU4adm+Rml5KWz\n"
    "VsydkgwwMob8semfIGpGm7fSSCri5IMSKA/tgbI6L/0C2bFvTNqJlCvfM1HdBlMn\n"
    "jCXqiiUgskLOZ0sw1MrEZlq27bIGfsw=\n"
    "-----END EC PRIVATE KEY-----\n";

// `openssl genrsa -traditional 1024`
const char* kRsa =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIICWwIBAAKBgQC3E/vTm17B2ep57A0BYGmoaXIgIVKVVBhHsW6q702XNlJVFMMT\n"
    "-----END RSA PRIVATE KEY-----\n";

// 同じ RSA を `openssl pkcs8 -topk8 -nocrypt` で入れ直したもの。**先読み
// (`BEGIN PRIVATE KEY`) を通る**ので、型を見て弾かれることを確かめる。
const char* kRsaPkcs8 =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIICdQIBADANBgkqhkiG9w0BAQEFAASCAl8wggJbAgEAAoGBALcT+9ObXsHZ6nns\n"
    "DQFgaahpciAhUpVUGEexbqrvTZc2UlUUwxNFwIKKACxBHoIaCY9P9IcH/ubFACn3\n"
    "LPutrghQXX4MrXPXmLVXb7kDcVVQrMKmjfvpwlmcK14c/m5+tUzp/UCjuR6ezeTc\n"
    "pKNuvHUmBArLw2r+iq/ye0TlvqJjAgMBAAECgYA4fYQQ0To12oXNRRbmO9eeTZsX\n"
    "avDOfvpW9NrPB7QLWomhExz2T2mNXgObpCRDxMD6ZZNwLvlqP5NDX6+ToQ23oMEh\n"
    "WF7cqY3cbRZ0a7Vo3UAYnxQWyDOPiV/o8f7SkRlb5vUvo1Z3B73kcjjm/pQ5exRn\n"
    "LtJynT92D4IWlYUVQQJBAO8i4DksWYB71Lnj0aK0tNxbcxZSQU3oLx1BhGQZYked\n"
    "HpNWf4B6WX6p2tFB9wlW5pBvR/YQDXETk7L6hPc6vzECQQDD/RcVwutK2g+Ya8FF\n"
    "UYuvOQGXajdggA8jsj75VZvTSb4r5jI7GCdpQBCgPm003sIRCa5CoKL0qhYO1od5\n"
    "sp3TAkA9JOi6Fano3UC+Kw8uEBByi3t4yJ1kAysQyvDD+22SrAzmVWaSfjYl2d5W\n"
    "RQyaObsIUTvQIbieIghQ6hdXc5DxAkA/dV9YKHjPD7QlAh7eNv0niym8wSOVF2HP\n"
    "iRNi4BlIXIQ66poxEC0Soy++8vehOs9TfPLzl9erqLbjrwrGmNMJAkAukfo401OU\n"
    "vzjbnCx2OfoAjH/GKsW4KM4Xe0TAFGmNf3jJBBad7bXiuqNA81U/rY1olJQEChY9\n"
    "2EmnHM7IcRxc\n"
    "-----END PRIVATE KEY-----\n";

// kP256 を `openssl ec -aes128 -passout pass:tab5pass` で暗号化したもの。
// **接続時は NVS のパスワードがパスフレーズとして届く**ので、その経路を固定する。
const char* kP256Enc =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "Proc-Type: 4,ENCRYPTED\n"
    "DEK-Info: AES-128-CBC,8945D3B24CEEEF8C193728719E1954E4\n"
    "\n"
    "8T9P66R338hwJ3YkX9zs4ezNz4w4wWKaLklum1d0vIqMwpgsAhegbzONv1is3Lkw\n"
    "iBlxe5sHMP7tHatmeL6r+GmgtCwiNocBTV46dEPs6WHqLfiIZYvvucmBA7liU9Mv\n"
    "f9x4gV9MIKIwsdw558xRyuUl/2JIN59r22EwErmGc8E=\n"
    "-----END EC PRIVATE KEY-----\n";

int g_fail = 0;

void expect(EcPubKeyStatus got, EcPubKeyStatus want, const char* what)
{
    if (got == want) return;
    std::printf("NG %s: got %s want %s\n", what, ec_pubkey_status_name(got),
                ec_pubkey_status_name(want));
    ++g_fail;
}

void expectb(bool got, bool want, const char* what)
{
    if (got == want) return;
    std::printf("NG %s: got %d want %d\n", what, (int)got, (int)want);
    ++g_fail;
}

void expect_line(const std::string& got, const char* want, const char* what)
{
    if (got == want) return;
    std::printf("NG %s:\n  got  %s\n  want %s\n", what, got.c_str(), want);
    ++g_fail;
}

mbedtls_ctr_drbg_context* drbg()
{
    static mbedtls_ctr_drbg_context ctr;
    static mbedtls_entropy_context  ent;
    static bool                     ready = false;
    if (!ready) {
        mbedtls_ctr_drbg_init(&ctr);
        mbedtls_entropy_init(&ent);
        static const char* pers = "test_ec_pubkey";
        ready = mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent,
                                      reinterpret_cast<const unsigned char*>(pers),
                                      std::strlen(pers)) == 0;
    }
    return ready ? &ctr : nullptr;
}

}  // namespace

int main()
{
    // 1) SEC1 (named curve) の P-256 -> OpenSSH が読む行と完全一致。
    std::string line;
    expect(ec_ssh_pubkey(kP256, "", drbg(), &line), EcPubKeyStatus::kOk, "P-256 SEC1");
    expect_line(line, kWantP256, "P-256 SEC1 の行");

    // 2) PKCS#8 に入っても同じ行になる（`ssh-keygen -t ecdsa -m PEM` 系は別として、
    //    手元で入れ直した鍵が動くことを確認する）。
    line.clear();
    expect(ec_ssh_pubkey(kP256Pkcs8, "", drbg(), &line), EcPubKeyStatus::kOk, "P-256 PKCS#8");
    expect_line(line, kWantP256, "P-256 PKCS#8 の行");

    // 3) P-256 以外は名前も点の長さも違うので組まない。
    line.clear();
    expect(ec_ssh_pubkey(kP384, "", drbg(), &line), EcPubKeyStatus::kNotP256, "P-384");
    expect_line(line, "", "P-384 は out を触らない");

    // 4) RSA は導出させない（呼び出し側が nullptr のまま渡す経路）。
    expect(ec_ssh_pubkey(kRsa, "", drbg(), &line), EcPubKeyStatus::kNotEc, "RSA (traditional)");

    // 4b) RSA の PKCS#8。**`BEGIN PRIVATE KEY` は先読みを通るので、型を見て
    //     弾かれる方**に依存している。RSA の接続は libssh2 の導出に任せる (#75)。
    expect(ec_ssh_pubkey(kRsaPkcs8, "", drbg(), &line), EcPubKeyStatus::kNotEc,
           "RSA (PKCS#8、先読みを通る)");

    // 4c) パスフレーズ付きの EC。**接続時は NVS のパスワードがそのままパスフレーズに
    //     なる**ので、正しいものは組めて、違うものは組めないことを固定する。
    line.clear();
    expect(ec_ssh_pubkey(kP256Enc, "tab5pass", drbg(), &line), EcPubKeyStatus::kOk,
           "暗号化 EC + 正しいパスフレーズ");
    expect_line(line, kWantP256, "暗号化 EC の行");
    expect(ec_ssh_pubkey(kP256Enc, "wrong", drbg(), &line), EcPubKeyStatus::kParse,
           "暗号化 EC + 間違ったパスフレーズ");

    // 5) 壊れた本体は kParse。先読みの `EC PRIVATE KEY` は通るが展開できない。
    expect(ec_ssh_pubkey("-----BEGIN EC PRIVATE KEY-----\nnot base64\n"
                         "-----END EC PRIVATE KEY-----\n",
                         "", drbg(), &line),
           EcPubKeyStatus::kParse, "壊れた PEM");

    // 6) 乱数源が無いときは作らない（mbedTLS は EC の展開に f_rng を要求する）。
    expect(ec_ssh_pubkey(kP256, "", nullptr, &line), EcPubKeyStatus::kNoRng, "drbg なし");

    // 組み上がった行の形を固定しておく。**libssh2 の `memory_read_publickey` は
    // 最初の空白の次から次の空白までを base64 として読む**ので、型名の後に
    // 空白が 1 つある形が壊れていると鍵認証ごと通らない。
    std::string got;
    if (ec_ssh_pubkey(kP256, "", drbg(), &got) == EcPubKeyStatus::kOk) {
        const size_t sp = got.find(' ');
        expectb(sp != std::string::npos && got.compare(0, sp, "ecdsa-sha2-nistp256") == 0, true,
                "型名で始まる");
        expectb(got.find('\n') == std::string::npos, true, "1 行");
    }

    if (g_fail) {
        std::printf("%d checks failed\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
