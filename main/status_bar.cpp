#include "status_bar.hpp"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint16_t kBg = 0x2104;  // 端末の黒と区別できる暗い灰
}

bool StatusBar::Info::operator==(const Info& o) const
{
    return wifi_up == o.wifi_up && rssi == o.rssi && vpn == o.vpn &&
           std::strcmp(ssid, o.ssid) == 0 && std::strcmp(ip, o.ip) == 0 &&
           std::strcmp(vpn_ip, o.vpn_ip) == 0;
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
    gfx_.drawString(line, 8, 0);

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
