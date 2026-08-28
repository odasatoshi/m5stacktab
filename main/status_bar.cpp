#include "status_bar.hpp"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t kBg = 0x2104;  // 端末の黒と区別できる暗い灰
}

bool StatusBar::Info::operator==(const Info& o) const
{
    // **RSSI は 10dBm 単位で比べる。** 1dBm ごとに差分ありと判定すると、
    // 揺れるだけでほぼ毎秒「状態が変わった」ことになり、呼び出し側が
    // メニューの全面再描画と C6 への RPC をやり直してしまう。
    return wifi_up == o.wifi_up && menu_open == o.menu_open &&
           (rssi / 10) == (o.rssi / 10) && vpn == o.vpn &&
           std::strcmp(ssid, o.ssid) == 0 && std::strcmp(ip, o.ip) == 0 &&
           std::strcmp(vpn_ip, o.vpn_ip) == 0 && kbd_on == o.kbd_on &&
           std::strcmp(kbd, o.kbd) == 0;
}

void StatusBar::begin(int height)
{
    height_ = height;
    drawn_  = false;
}

bool StatusBar::draw(const Info& info, bool force)
{
    if (height_ <= 0) return false;
    if (!force && drawn_ && info == last_) return false;
    last_  = info;
    drawn_ = true;

    gfx_.fillRect(0, 0, gfx_.width(), height_, kBg);
    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextDatum(textdatum_t::top_left);

    // **左端に必ずラベルを出す。** ここをタップするとメニューが開く／閉じる、と
    // 分かる手掛かりが無いと、端末に入ったあと戻り方が無いように見える
    // （実機でそう報告された）。
    gfx_.setTextColor(TFT_BLACK, info.menu_open ? TFT_ORANGE : TFT_CYAN);
    gfx_.fillRect(0, 0, kLabelW, height_, info.menu_open ? TFT_ORANGE : TFT_CYAN);
    gfx_.drawString(info.menu_open ? " CLOSE" : " MENU", 4, 0);

    // 画面キーボードの段 (#65)。**「なし」も文字で出す** — 空にすると、
    // そこがタップできる場所だと分からない（MENU と同じ理由）。
    const uint16_t kbd_bg = info.kbd_on ? TFT_YELLOW : 0x52AA;
    gfx_.fillRect(kLabelW, 0, kKbdW, height_, kbd_bg);
    gfx_.setTextColor(info.kbd_on ? TFT_BLACK : TFT_WHITE, kbd_bg);
    gfx_.drawString(info.kbd, kLabelW + (kKbdW - gfx_.textWidth(info.kbd)) / 2, 0);

    char line[96];
    if (info.wifi_up) {
        // RSSI は esp_hosted 経由だと 0 が返ることがある（実機で確認）。
        // 「0dBm」は強電界に見えて紛らわしいので、取れないときは出さない。
        if (info.rssi != 0) {
            std::snprintf(line, sizeof(line), "WiFi %s  %ddBm  %s", info.ssid, info.rssi, info.ip);
        } else {
            std::snprintf(line, sizeof(line), "WiFi %s  %s", info.ssid, info.ip);
        }
        gfx_.setTextColor(TFT_GREEN, kBg);
    } else {
        std::snprintf(line, sizeof(line), "WiFi --");
        gfx_.setTextColor(TFT_DARKGREY, kBg);
    }
    gfx_.drawString(line, kLabelW + kKbdW + 8, 0);

    // VPN は右寄せ。左の SSID が伸びても位置が動かないようにする。
    char vpn[64];
    if (info.vpn == ui::VpnState::kUp && info.vpn_ip[0]) {
        std::snprintf(vpn, sizeof(vpn), "VPN %s %s", ui::vpn_label(info.vpn), info.vpn_ip);
    } else {
        std::snprintf(vpn, sizeof(vpn), "VPN %s", ui::vpn_label(info.vpn));
    }
    switch (info.vpn) {
        case ui::VpnState::kUp: gfx_.setTextColor(TFT_GREEN, kBg); break;
        case ui::VpnState::kConnecting: gfx_.setTextColor(TFT_YELLOW, kBg); break;
        case ui::VpnState::kOff: gfx_.setTextColor(TFT_DARKGREY, kBg); break;
    }
    gfx_.setTextDatum(textdatum_t::top_right);
    gfx_.drawString(vpn, gfx_.width() - 8, 0);
    gfx_.setTextDatum(textdatum_t::top_left);
    gfx_.setTextColor(TFT_WHITE, TFT_BLACK);
    return true;
}
