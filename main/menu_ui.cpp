#include "menu_ui.hpp"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t kBg      = TFT_BLACK;
constexpr uint16_t kSelBg   = 0x2945;  // 選択行の帯
constexpr uint16_t kHint    = 0x8410;  // 操作説明の灰

// 項目 id。画面ごとに重ならない値にして、どの画面から来たか分かるようにする。
enum : int {
    kIdSsh      = 1,
    kIdVpn      = 2,
    kIdSettings = 3,
    kIdTs       = 10,
    kIdWg       = 11,
    kIdTerminal = 12,
    kIdBack     = 99,
};

}  // namespace

void MenuUi::set_area(int top, int height)
{
    top_      = top;
    height_   = height;
    row_h_    = 48;
    // 見出し 1 行ぶん下げてから項目を並べる。
    list_top_ = top_ + 56;
    dirty_    = true;
    if (menu_.count() == 0) rebuild();
}

void MenuUi::set_visible(bool v)
{
    if (visible_ == v) return;
    visible_ = v;
    dirty_   = true;
    if (v) enter(Screen::kRoot);
}

void MenuUi::refresh()
{
    if (!visible_) return;
    // 選択位置は保つ。状態表示が更新されるたびに先頭へ戻ると使いにくい。
    const int sel = menu_.selected();
    rebuild();
    menu_.set_selected(sel);
    dirty_ = true;
}

void MenuUi::enter(Screen s)
{
    screen_ = s;
    rebuild();
    dirty_ = true;
}

void MenuUi::rebuild()
{
    int n = 0;
    auto add = [&](const char* text, int id, bool enabled) {
        if (n >= ui::Menu::kMaxItems) return;
        std::snprintf(labels_[n], sizeof(labels_[n]), "%s", text);
        items_[n].label   = labels_[n];
        items_[n].id      = id;
        items_[n].enabled = enabled;
        ++n;
    };

    char buf[72];
    switch (screen_) {
        case Screen::kRoot:
            add("SSH", kIdSsh, true);
            add("VPN", kIdVpn, true);
            add("Settings", kIdSettings, true);
            add("Terminal", kIdTerminal, true);
            break;
        case Screen::kVpn:
            // ponytail: 接続先の設定を NVS に持っていないので、今は状態表示だけ。
            // 保存できるようにしたら enabled にして繋ぐ動作を足す（#39 の続き）。
            std::snprintf(buf, sizeof(buf), "Tailscale: %s", info_.ts_state);
            add(buf, kIdTs, false);
            std::snprintf(buf, sizeof(buf), "WireGuard: %s", info_.wg_state);
            add(buf, kIdWg, false);
            add("(接続はシリアルの ts / wg から)", 0, false);
            // 指だけで戻れる経路。全項目が状態表示だと hit_test が常に -1 になり、
            // Esc を送る手段（シリアル）が無いと出られない。
            add("< Back", kIdBack, true);
            break;
        case Screen::kSettings:
            // ponytail: 読み取り専用。編集はキーボード (#15) が来てから。
            std::snprintf(buf, sizeof(buf), "WiFi: %s", info_.wifi);
            add(buf, 0, false);
            std::snprintf(buf, sizeof(buf), "SSH: %s",
                          info_.ssh_target[0] ? info_.ssh_target : "(未設定)");
            add(buf, 0, false);
            add("(設定はシリアルの wifi / ssh から)", 0, false);
            add("< Back", kIdBack, true);
            break;
    }
    menu_.set_items(items_, n);
}

bool MenuUi::key(ui::Key k)
{
    if (!visible_) return false;
    if (!menu_.key(k)) return false;
    dirty_ = true;
    if (menu_.take_back()) {
        // 最上位の Esc / ← は端末へ戻る。**指を使わずメニューから出る経路がここしかない**
        // （Terminal の項目まで下りて Enter でも出られるが、Esc で閉じるほうが速い）。
        if (screen_ == Screen::kRoot) {
            if (action_) action_(Action::kShowTerminal);
            return true;
        }
        enter(Screen::kRoot);
        return true;
    }
    const int id = menu_.take_activated();
    if (id > 0) activate(id);
    return true;
}

void MenuUi::activate(int id)
{
    switch (id) {
        case kIdSsh:
            if (action_) action_(Action::kOpenSsh);
            break;
        case kIdVpn: enter(Screen::kVpn); break;
        case kIdSettings: enter(Screen::kSettings); break;
        case kIdTerminal:
            if (action_) action_(Action::kShowTerminal);
            break;
        case kIdTs:
            if (action_) action_(Action::kTsConnect);
            break;
        case kIdWg:
            if (action_) action_(Action::kWgUp);
            break;
        case kIdBack: enter(Screen::kRoot); break;
        default: break;
    }
}

bool MenuUi::touch_down(int x, int y)
{
    if (!visible_) return false;
    (void)x;
    // 領域の外（キーボードやステータスバー）は自分のものにしない。
    if (y < top_ || y >= top_ + height_) return false;
    const int idx = menu_.hit_test(y, list_top_, row_h_);
    if (idx < 0) return true;  // 領域内の外れたタップは食う（端末に漏らさない）
    // 1 回のタップで移動と決定を兼ねる。2 度押しを要求すると指では使いにくい。
    menu_.set_selected(idx);
    dirty_ = true;
    activate(menu_.item(idx).id);
    return true;
}

void MenuUi::draw(bool force)
{
    if (!visible_ || height_ <= 0) return;  // set_area 前に描くとバーの上に出る
    if (!dirty_ && !force) return;
    dirty_ = false;

    gfx_.fillRect(0, top_, gfx_.width(), height_, kBg);
    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextDatum(textdatum_t::top_left);

    const char* title = "m5stacktab";
    switch (screen_) {
        case Screen::kVpn: title = "VPN"; break;
        case Screen::kSettings: title = "Settings"; break;
        case Screen::kRoot: break;
    }
    gfx_.setTextColor(TFT_CYAN, kBg);
    gfx_.drawString(title, 24, top_ + 16);

    for (int i = 0; i < menu_.count(); ++i) {
        const int  y   = list_top_ + i * row_h_;
        // 選べない項目には目印を付けない。全部が状態表示の画面（VPN / Settings）で
        // 先頭に > が付くと、選べるように見えて紛らわしい。
        const bool sel = (i == menu_.selected()) && menu_.item(i).enabled;
        if (sel) gfx_.fillRect(0, y, gfx_.width(), row_h_, kSelBg);
        const ui::Item& it = menu_.item(i);
        gfx_.setTextColor(it.enabled ? (sel ? TFT_WHITE : TFT_LIGHTGREY) : kHint,
                          sel ? kSelBg : kBg);
        // 選択の目印。指で触るときも「今どこか」が分かるようにする。
        gfx_.drawString(sel ? ">" : " ", 24, y + (row_h_ - 24) / 2);
        gfx_.drawString(it.label, 56, y + (row_h_ - 24) / 2);
    }

    gfx_.setTextColor(kHint, kBg);
    // キーボードが挿さっていればキーの説明も出す。挿さっていなければ指の説明だけ
    // （押せないキーを案内すると、外したときに嘘になる）。
    const char* hint;
    if (has_kbd_) {
        hint = (screen_ == Screen::kRoot)
                   ? "up/down + Enter to open   |   Esc or Ctrl+Alt+M for the terminal"
                   : "up/down + Enter   |   Esc or left to go up   |   Ctrl+Alt+M for the terminal";
    } else {
        hint = (screen_ == Screen::kRoot)
                   ? "tap an item to open   |   tap CLOSE (top left) to go to the terminal"
                   : "tap an item   |   tap < Back to go up   |   tap CLOSE for the terminal";
    }
    gfx_.drawString(hint, 24, top_ + height_ - 32);
    gfx_.setTextColor(TFT_WHITE, TFT_BLACK);
}
