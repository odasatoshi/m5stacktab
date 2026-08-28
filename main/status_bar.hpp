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
        // メニューが開いているか。左端のラベルを MENU / CLOSE に切り替える。
        bool         menu_open = false;
        // 画面キーボードの段の名前（"なし" / "ABC" / "かな"）。**静的な文字列を指す**
        // 前提で、他と同じく strcmp で比べる。ここをタップすると段が巡回する (#65)。
        const char*  kbd       = "";
        // その段でキーボードが出ているか（面の色を変えるだけ。文字列で判定すると
        // ラベルを直したときに黙って色が固定される）。
        bool         kbd_on    = false;

        bool operator==(const Info& o) const;
        bool operator!=(const Info& o) const { return !(*this == o); }
    };

    // 左端の「MENU」ラベルの幅。ここをタップするとメニューが開く。
    // タップできる場所だと分かるように、必ず文字で出す。
    static constexpr int kLabelW = 96;
    // その隣の画面キーボードの巡回ボタンの幅 (#65)。
    // **バーの残りはメニューの開閉のまま**にしてある — 純正キーボードが無いとき、
    // 指だけでメニューへ戻れる経路はそこしかない。
    static constexpr int kKbdW   = 88;

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
