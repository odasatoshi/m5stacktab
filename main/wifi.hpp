#pragma once
#include <cstddef>

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

// ステータスバー用。接続していなければ false を返し、出力は触らない。
// esp_wifi のヘッダを main.cpp に持ち込まないためにここに置く。
bool wifi_status(char* ssid, size_t ssid_len, int* rssi, char* ip, size_t ip_len);

// --- 保存済みの接続先 (#56) ---
//
// **NVS に blob 1 つ**（`wifi/nets`）で持つ。1 件ごとにキーを切ると、消したときの
// 詰め直しで穴が空いた状態を作りかねない。全体を書き直すほうが小さくて確実。
// SD の profiles.json (#49) には混ぜない — SD を読む前に繋ぎたいし、
// SD は抜けば誰でも読めるのでパスワードの置き場として弱い。
constexpr size_t kMaxWifiNets = 5;

// i 番目の SSID。範囲外なら false。**パスワードは返さない**（画面に出す用途しかない）。
bool   wifi_net_ssid(size_t i, char* out, size_t len);
size_t wifi_net_count(void);
// 今つないでいる設定の index。繋いでいない／消したなら -1。
int    wifi_net_current(void);

// 足す。**同じ SSID があればパスワードを差し替える**（打ち直しで枠を食わない）。
// 満杯なら ESP_ERR_NO_MEM、SSID が空か長すぎれば ESP_ERR_INVALID_ARG。
esp_err_t wifi_net_add(const char* ssid, const char* pass);
// 消す。**今つながっている設定なら実際に切断する**（一覧と実態が食い違わないように）。
esp_err_t wifi_net_remove(size_t i);
// i 番目に繋ぎ直す。次回の起動でもこれを使う。
esp_err_t wifi_net_connect(size_t i);
// SSID から index を引く。無ければ -1（足した直後に繋ぐときに使う）。
int wifi_net_find(const char* ssid);

struct WifiScanEntry {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
};

// AP を探す。**数秒かかるうえ通信が一瞬止まる。専用タスクから呼ぶこと**
// （メインループや kbd タスクから呼ぶと画面が固まる）。
// 見つかった件数（SSID の重複は電波の強いほうだけ残す）、失敗なら負。
int wifi_scan(WifiScanEntry* out, int max);
