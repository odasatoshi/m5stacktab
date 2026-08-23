#pragma once
// microSD (SDMMC 4 線)。接続先の設定と鍵をここから読む (#49)。
//
// **P4 の SDMMC の IO 電源は on-chip LDO の VO4**。開けないとカードは一切応答しない。
#include <cstddef>
#include <string>

#include <esp_err.h>

// マウント済みなら ESP_OK を返して何もしない。
esp_err_t sd_mount();
void      sd_unmount();
bool      sd_mounted();

// ファイルを丸ごと読む。max_bytes を超えるファイルは ESP_ERR_INVALID_SIZE。
// **SD の中身は信用しない**ので、上限は呼び出し側が必ず渡す。
esp_err_t sd_read_file(const char* path, size_t max_bytes, std::string* out);
