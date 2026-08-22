#pragma once
// Tab5 純正キーボード（Ext.Port1 の I2C @0x6D、SDA=G0 SCL=G1、INT=G50）。
// 文字列 (STRING) モードで使う。押されたキーの ASCII と修飾キーがそのまま降ってくるので、
// 行列座標や HID キーコードを自前で表に持たなくて済む。
#include <cstddef>
#include <cstdint>

namespace kbd_hw {

// I2C を張り、STRING モードに切り替える。キーボードが無ければ false。
bool begin();

// キーイベントを 1 つ取り出す。戻り値は out に入れた文字数（0 = イベント無し）。
// mod は修飾キーのビット（Ctrl/Alt など。中身はファームウェア依存なので生で返す）。
int poll(char* out, size_t cap, uint8_t* mod);

// 最後の begin() でキーボードが見つかったか。
bool present();

uint8_t version();

// Ext.Port1 の I2C を舐めて応答するアドレスを stdout に出す（配線の切り分け用）。
void scan();

}  // namespace kbd_hw
