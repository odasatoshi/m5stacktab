#pragma once
// 画面上端の細い帯に WiFi と VPN の状態を出す。
//
// 状態の取得はしない。呼び出し側が集めた Info を渡す（この層をパネルの描画だけに
// 保つと、状態の出所が増えても描画を触らずに済む）。
#include <M5GFX.h>

#include "menu.hpp"

class StatusBar {
public:
    explicit StatusBar(M5GFX& gfx) : gfx_(gfx) {}

    void begin(int height);
    int  height() const { return height_; }

    struct Info {
        bool         wifi_up  = false;
        char         ssid[33] = {};
        int          rssi     = 0;
        char         ip[16]   = {};
        ui::VpnState vpn      = ui::VpnState::kOff;
        char         vpn_ip[24] = {};

        bool operator==(const Info& o) const;
        bool operator!=(const Info& o) const { return !(*this == o); }
    };

    // 中身が変わっていなければ何も描かない。**毎フレーム描いてはいけない**。
    // PPA は転送のたびに出力側のキャッシュを無効化するので、端末の描画と
    // 競合する（CLAUDE.md 参照）。force で強制的に描き直す。
    // 実際に描いたら true。呼び出し側が「状態が変わった」の合図に使える。
    bool draw(const Info& info, bool force = false);

private:
    M5GFX& gfx_;
    int    height_ = 0;
    Info   last_{};
    bool   drawn_ = false;
};
