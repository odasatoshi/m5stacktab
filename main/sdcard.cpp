#include "sdcard.hpp"

#include <cstdio>

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>

namespace {

const char* TAG = "sd";

// M5Stack Tab5 の microSD（上流の BSP m5stack_tab5.h の値）。
// スロットは 0、4 線。カード検出 (CD) と書き込み禁止 (WP) の線は出ていない。
constexpr int kPinClk = 43;
constexpr int kPinCmd = 44;
constexpr int kPinD0  = 39;
constexpr int kPinD1  = 40;
constexpr int kPinD2  = 41;
constexpr int kPinD3  = 42;

// **ESP32-P4 の SDMMC の IO 電源は on-chip LDO の VO4**。これを開けないとカードは
// 一切応答しない（初期化が 0x107 などで落ちる）。Tab5 の BSP も同じことをしている。
constexpr int kLdoChanSd = 4;

const char* kMountPoint = "/sdcard";

sdmmc_card_t*        s_card = nullptr;
sd_pwr_ctrl_handle_t s_pwr  = nullptr;

}  // namespace

bool sd_mounted() { return s_card != nullptr; }

esp_err_t sd_mount()
{
    if (s_card) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;  // 40MHz

    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {};
        ldo.ldo_chan_id              = kLdoChanSd;
        if (esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr); err != ESP_OK) {
            ESP_LOGE(TAG, "on-chip LDO (VO4) の確保に失敗: %s", esp_err_to_name(err));
            return err;
        }
    }
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width               = 4;
    slot.clk                 = static_cast<gpio_num_t>(kPinClk);
    slot.cmd                 = static_cast<gpio_num_t>(kPinCmd);
    slot.d0                  = static_cast<gpio_num_t>(kPinD0);
    slot.d1                  = static_cast<gpio_num_t>(kPinD1);
    slot.d2                  = static_cast<gpio_num_t>(kPinD2);
    slot.d3                  = static_cast<gpio_num_t>(kPinD3);

    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    // **format_if_mount_failed は false のまま。** true にすると、読めなかった
    // (= FAT32 ではない / 配線を間違えた) だけでユーザーのカードを消してしまう。
    mount.format_if_mount_failed = false;
    mount.max_files              = 4;  // profiles.json と鍵を 1 つずつ開ければ足りる
    mount.allocation_unit_size   = 16 * 1024;

    const int64_t t0  = esp_timer_get_time();
    esp_err_t     err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount 失敗: %s (カードが入っているか、FAT32/MBR かを確認する)",
                 esp_err_to_name(err));
        s_card = nullptr;
        return err;
    }
    ESP_LOGI(TAG, "mounted %s in %lldms: %s %lluMB", kMountPoint,
             (esp_timer_get_time() - t0) / 1000, s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

void sd_unmount()
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(kMountPoint, s_card);
    s_card = nullptr;
}

esp_err_t sd_read_file(const char* path, size_t max_bytes, std::string* out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    out->clear();
    if (!s_card) return ESP_ERR_INVALID_STATE;

    struct stat st;
    if (::stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    // **開く前に大きさで弾く。** 読みながら数えると、巨大なファイルで
    // ヒープを食い潰してから気づくことになる。
    if (st.st_size < 0 || static_cast<size_t>(st.st_size) > max_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE* f = std::fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    out->resize(static_cast<size_t>(st.st_size));
    const size_t n = out->empty() ? 0 : std::fread(&(*out)[0], 1, out->size(), f);
    std::fclose(f);
    if (n != out->size()) {
        out->clear();
        return ESP_FAIL;
    }
    return ESP_OK;
}
