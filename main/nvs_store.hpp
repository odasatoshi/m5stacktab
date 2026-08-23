#pragma once
// 接続先と鍵の保存先 (#57)。**正式な置き場は NVS**で、SD は取り込み元でしかない。
//
// 保存の形は「`profiles.json` の本文をそのまま blob 1 つ」。項目ごとにキーを切ると
// 構造をパーサと二重に持つことになるので、取り込みはファイルのコピー、読み出しは
// 既存の `components/profiles` のパーサに通すだけにする。
//
// 実測 (`nvsstat`): 空きは 707 エントリ ≈ 22.6KB。RSA の PEM 1.7KB が約 53 エントリ
// なので、鍵 5 本 + JSON でも空きの半分以下に収まる。パーティションは広げない。
#include <cstddef>
#include <string>
#include <vector>

#include <esp_err.h>

// NVS のキー名は 15 文字まで。"k_" を前置するので、鍵のファイル名はこの長さまで。
constexpr size_t kNvsKeyNameMax = 13;

// profiles.json の本文。無ければ ESP_ERR_NVS_NOT_FOUND。
esp_err_t nvs_profiles_load(std::string* json);
esp_err_t nvs_profiles_store(const std::string& json);

// 鍵 / authkey。name は keys/ 配下のファイル名（ディレクトリを含まない）。
esp_err_t nvs_key_load(const std::string& name, std::string* out);
esp_err_t nvs_key_store(const std::string& name, const std::string& data);

// 取り込み済みの鍵の名前を並べる（`profiles` の表示と、消すときの確認用）。
std::vector<std::string> nvs_key_names();

// 接続先と鍵を全部消す。
esp_err_t nvs_profiles_clear();

// 空きエントリ数。**消す前に「入るか」を見る**ために使う。
size_t nvs_free_entries();

// size バイトの blob がおおよそ何エントリ要るか。1 エントリ 32 バイト、
// blob ごとにヘッダぶんの余裕を足す。
size_t nvs_entries_for(size_t size);
