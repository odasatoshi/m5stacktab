#pragma once
#include <esp_err.h>

// ESP32-C6 の電源を入れる。Tab5 では C6 の電源が P4 の GPIO ではなく
// I2C の IO エクスパンダ (PI4IOE5V6408 @0x44) の pin0 に繋がっている。
// これを esp_wifi_init() より先にやらないと SDIO の列挙が必ず失敗する。
esp_err_t tab5_c6_power_on(void);

// NVS に保存済みの SSID / パスワードで STA 接続を開始する。
// 未設定なら接続せずに戻り、シリアルコンソールの `wifi` コマンドを待つ。
esp_err_t wifi_start(void);

// シリアルコンソールに `wifi <ssid> <password>` と `wifi-status` を登録して REPL を開始する。
// 画面から設定できるようになるまでの間の設定手段（#3 でキーボードが入ったら UI を足す）。
esp_err_t console_start(void);

bool wifi_is_connected(void);
