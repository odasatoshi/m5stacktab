#include "nvs_store.hpp"

#include <cstring>

#include <esp_log.h>
#include <nvs.h>

namespace {

const char* TAG = "nvsprof";

// **`stacktab98`（隣のプロジェクト）と同じ NVS パーティションを使っている**
// （`nvsstat` に出る）。名前空間を分けたまま、他人のキーには触らない。
const char* kNamespace = "prof";
const char* kJsonKey   = "json";
const char* kKeyPrefix = "k_";

// 鍵の NVS キー名を作る。呼ぶ前に長さを検査していること。
std::string key_name(const std::string& name) { return std::string(kKeyPrefix) + name; }

esp_err_t load_blob(const char* key, std::string* out)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(kNamespace, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = 0;
    err        = nvs_get_blob(h, key, nullptr, &len);
    if (err == ESP_OK) {
        out->resize(len);
        err = len ? nvs_get_blob(h, key, &(*out)[0], &len) : ESP_OK;
    }
    nvs_close(h);
    if (err != ESP_OK) out->clear();
    return err;
}

esp_err_t store_blob(const char* key, const std::string& data)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data.data(), data.size());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

}  // namespace

esp_err_t nvs_profiles_load(std::string* json)
{
    if (!json) return ESP_ERR_INVALID_ARG;
    return load_blob(kJsonKey, json);
}

esp_err_t nvs_profiles_store(const std::string& json)
{
    const esp_err_t err = store_blob(kJsonKey, json);
    if (err == ESP_OK) ESP_LOGI(TAG, "stored profiles.json (%d bytes)", (int)json.size());
    return err;
}

esp_err_t nvs_key_load(const std::string& name, std::string* out)
{
    if (!out || name.empty() || name.size() > kNvsKeyNameMax) return ESP_ERR_INVALID_ARG;
    return load_blob(key_name(name).c_str(), out);
}

esp_err_t nvs_key_store(const std::string& name, const std::string& data)
{
    // **NVS のキー名は 15 文字まで。** 長い名前を黙って切り詰めると、別の鍵と
    // 同じキーになって取り違える。呼び出し側で弾けるように理由を返す。
    if (name.empty() || name.size() > kNvsKeyNameMax) return ESP_ERR_INVALID_ARG;
    const esp_err_t err = store_blob(key_name(name).c_str(), data);
    if (err == ESP_OK) ESP_LOGI(TAG, "stored key \"%s\" (%d bytes)", name.c_str(), (int)data.size());
    return err;
}

std::vector<std::string> nvs_key_names()
{
    std::vector<std::string> names;
    nvs_iterator_t           it  = nullptr;
    esp_err_t                err = nvs_entry_find(NVS_DEFAULT_PART_NAME, kNamespace,
                                                  NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info{};
        nvs_entry_info(it, &info);
        const size_t plen = std::strlen(kKeyPrefix);
        if (std::strncmp(info.key, kKeyPrefix, plen) == 0) names.emplace_back(info.key + plen);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return names;
}

size_t nvs_free_entries()
{
    nvs_stats_t st{};
    if (nvs_get_stats(nullptr, &st) != ESP_OK) return 0;
    return st.free_entries;
}

size_t nvs_entries_for(size_t size)
{
    // データは 32 バイト刻み。blob 自体のヘッダとチャンクの索引で数エントリ増えるので、
    // **多めに見積もる**（足りないと判断して断る方が、書いている途中で落ちるよりよい）。
    return (size + 31) / 32 + 4;
}

esp_err_t nvs_profiles_clear()
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // **名前空間ごと消す。** 鍵を 1 本ずつ消すと、消し漏れが「取り込んだはずの鍵が
    // 残っている」という形で後から出る。
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "cleared profiles and keys");
    return err;
}
