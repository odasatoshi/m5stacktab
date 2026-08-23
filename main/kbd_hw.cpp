#include "kbd_hw.hpp"

#include <cstdio>

#include <M5GFX.h>

namespace kbd_hw {
namespace {

// Ext.Port1。M5GFX は I2C_NUM_1 (G31/G32) を握っているので、こちらは 0 を使う。
constexpr int      kPort = I2C_NUM_0;
constexpr int      kSda  = 0;
constexpr int      kScl  = 1;
constexpr int      kAddr = 0x6D;
constexpr uint32_t kFreq = 100000;

// レジスタ（M5Tab5-Keyboard-UserDemo の m5_tab5_keyboard.h より）
constexpr uint8_t kRegIntSta       = 0x01;
constexpr uint8_t kRegEventNum     = 0x02;
constexpr uint8_t kRegMode         = 0x10;
constexpr uint8_t kRegCharLen      = 0x40;
constexpr uint8_t kRegCharBase     = 0x50;
constexpr uint8_t kRegVersion      = 0xFE;
constexpr uint8_t kModeString      = 2;
constexpr uint8_t kMaxCharEventLen = 15;

uint8_t s_version = 0;
bool    s_present = false;

}  // namespace

bool begin()
{
    if (lgfx::i2c::init(kPort, kSda, kScl).has_error()) {
        std::printf("kbd: i2c init failed (sda=%d scl=%d)\n", kSda, kScl);
        return false;
    }
    auto ver = lgfx::i2c::readRegister8(kPort, kAddr, kRegVersion, kFreq);
    if (ver.has_error()) return false;
    s_version = ver.value();

    // **present はモードを入れてから立てる。** ここで落ちたままタスクを立てると、
    // 前のモード（行列）のバイトをそのまま文字として端末に流し込むことになる。
    if (lgfx::i2c::writeRegister8(kPort, kAddr, kRegMode, kModeString, 0, kFreq).has_error()) {
        return false;
    }
    s_present = true;
    // モード切り替えで溜まっていたイベントは捨てる。
    lgfx::i2c::writeRegister8(kPort, kAddr, kRegEventNum, 0, 0, kFreq);
    lgfx::i2c::writeRegister8(kPort, kAddr, kRegIntSta, 0, 0, kFreq);
    return true;
}

int poll(char* out, size_t cap, uint8_t* mod)
{
    auto count = lgfx::i2c::readRegister8(kPort, kAddr, kRegEventNum, kFreq);
    if (count.has_error() || count.value() == 0) {
        // 取り残しの INT を落としておく（イベントが無いのに INT が立ちっぱなしになる）。
        lgfx::i2c::writeRegister8(kPort, kAddr, kRegIntSta, 0, 0, kFreq);
        return 0;
    }
    auto len = lgfx::i2c::readRegister8(kPort, kAddr, kRegCharLen, kFreq);
    if (len.has_error() || len.value() == 0) return 0;

    // 0x40 の長さは「修飾バイト 1 + 文字列」のバイト数（実機で確認: 'a' が 2、'backspace' が 10）。
    uint8_t n = len.value();
    if (n > kMaxCharEventLen) n = kMaxCharEventLen;
    uint8_t buf[kMaxCharEventLen] = {};
    if (lgfx::i2c::readRegister(kPort, kAddr, kRegCharBase, buf, n, kFreq).has_error()) {
        return 0;
    }
    if (mod) *mod = buf[0];
    uint8_t chars = n - 1;
    if (chars > cap) chars = static_cast<uint8_t>(cap);
    for (uint8_t i = 0; i < chars; ++i) out[i] = static_cast<char>(buf[1 + i]);
    return chars;
}

bool present()
{
    return s_present;
}

uint8_t version()
{
    return s_version;
}

void scan()
{
    if (lgfx::i2c::init(kPort, kSda, kScl).has_error()) {
        std::printf("kbd: i2c init failed\n");
        return;
    }
    int found = 0;
    for (int addr = 0x08; addr < 0x78; ++addr) {
        bool ack = !lgfx::i2c::beginTransaction(kPort, addr, kFreq, false).has_error();
        lgfx::i2c::endTransaction(kPort);
        if (ack) {
            std::printf("  0x%02X が応答\n", addr);
            ++found;
        }
    }
    if (found == 0) std::printf("  応答なし（Ext.Port1 に何も繋がっていない）\n");
}

}  // namespace kbd_hw
