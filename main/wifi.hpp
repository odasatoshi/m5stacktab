#pragma once
#include <esp_err.h>

// NVS に保存済みの SSID / パスワードで STA 接続を開始する。
//
// **前提: display.init() (M5GFX の Tab5 初期化) を先に呼んでいること。**
// Tab5 の C6 電源は P4 の GPIO ではなく I2C の IO エクスパンダ (PI4IOE5V6408 @0x44) の
// pin0 にあり、M5GFX の Tab5 初期化がそこを出力 High にしている (実機で確認済み)。
// 電源が入る前に SDIO を叩くと列挙が失敗し、以後リセットもかからず永久に失敗する。
// 未設定なら接続せずに戻り、シリアルコンソールの `wifi` コマンドを待つ。
esp_err_t wifi_start(void);

// シリアルコンソールに `wifi <ssid> <password>` と `wifi-status` を登録して REPL を開始する。
// 画面から設定できるようになるまでの間の設定手段（#3 でキーボードが入ったら UI を足す）。
esp_err_t console_start(void);

bool wifi_is_connected(void);
