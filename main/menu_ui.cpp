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
    kIdSavedSsh = 13,  // NVS に保存した 1 件（SD が無いときの経路）
    kIdReload   = 14,
    kIdWifi     = 15,  // Settings から WiFi の一覧へ (#56)
    kIdWifiNew  = 20,
    kIdWifiConn = 21,
    kIdWifiDel  = 22,
    kIdWifiMan  = 23,
    kIdBack     = 99,
    // **index を埋めて返す帯域。互いに重ならないように離してある。**
    // 下の static_assert が、上限を上げたときの食い込みを止める。
    kIdWifiNet     = 200,   // 保存済みの N 番目
    kIdWifiScanned = 400,   // スキャン結果の N 番目
    // SD の接続先。id から profiles の index を戻せるようにしておく (#49)。
    kIdProfile  = 1000,
};

static_assert(kIdWifiNet + (int)kMaxWifiNets < kIdWifiScanned,
              "保存済みの id がスキャン結果の帯域に食い込む");
static_assert(kIdWifiScanned + kMaxWifiScanRows < kIdProfile,
              "スキャン結果の id が SD の接続先の帯域に食い込む");

}  // namespace

void MenuUi::set_area(int top, int height)
{
    top_      = top;
    height_   = height;
    row_h_    = 48;
    // 見出し 1 行ぶん下げてから項目を並べる。
    list_top_ = top_ + 56;
    // 操作説明の 1 行ぶん (32px) を残して、入るだけ並べる。
    // **画面キーボードを出していると 4 行ぶんしか残らない**ので、窓からはみ出す分は
    // スクロールする（1 画面の項目数は最大 8 = VPN の 状態 2 + 5 件 + "< Back"）。
    menu_.set_visible_rows((top_ + height_ - 40 - list_top_) / row_h_);
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

    char buf[96];
    switch (screen_) {
        case Screen::kRoot:
            add("SSH", kIdSsh, true);
            add("VPN", kIdVpn, true);
            add("Settings", kIdSettings, true);
            add("Terminal", kIdTerminal, true);
            break;
        case Screen::kSsh:
            add_profiles(/*ssh=*/true, &n);
            // NVS の 1 件はいつでも残す。SD が読めなくても繋げる経路が要る。
            std::snprintf(buf, sizeof(buf), "保存済み: %s",
                          info_.ssh_target[0] ? info_.ssh_target : "(未設定)");
            add(buf, kIdSavedSsh, info_.ssh_target[0] != '\0');
            add("< Back", kIdBack, true);
            break;
        case Screen::kVpn:
            std::snprintf(buf, sizeof(buf), "Tailscale: %s", info_.ts_state);
            add(buf, 0, false);
            std::snprintf(buf, sizeof(buf), "WireGuard: %s", info_.wg_state);
            add(buf, 0, false);
            add_profiles(/*ssh=*/false, &n);
            // 指だけで戻れる経路。全項目が状態表示だと hit_test が常に -1 になり、
            // Esc を送る手段（シリアル）が無いと出られない。
            add("< Back", kIdBack, true);
            break;
        case Screen::kSettings:
            // ponytail: WiFi 以外は読み取り専用。編集はコンソールから（`ssh`）。
            std::snprintf(buf, sizeof(buf), "WiFi: %s", info_.wifi);
            add(buf, kIdWifi, true);
            std::snprintf(buf, sizeof(buf), "SSH: %s",
                          info_.ssh_target[0] ? info_.ssh_target : "(未設定)");
            add(buf, 0, false);
            std::snprintf(buf, sizeof(buf), "SD: %s", info_.sd);
            add(buf, 0, false);
            add("SD を読み直す", kIdReload, true);
            add("< Back", kIdBack, true);
            break;
        case Screen::kWifi:
            // 注記は「スキャン中…」「5 件で満杯」など。**画面に理由を出す唯一の場所**
            // （端末に書いてもメニューを出している間は描かれない）。
            if (wifi_note_[0]) add(wifi_note_, 0, false);
            if (wifi_nets_) {
                for (size_t i = 0; i < wifi_nets_->size() && i < kMaxWifiNets; ++i) {
                    add((*wifi_nets_)[i].c_str(), kIdWifiNet + static_cast<int>(i), true);
                }
            }
            if (!wifi_nets_ || wifi_nets_->empty()) add("(保存済みなし)", 0, false);
            add("Create new wifi setting", kIdWifiNew, true);
            add("< Back", kIdBack, true);
            break;
        case Screen::kWifiNet:
            if (wifi_nets_ && wifi_sel_ >= 0 &&
                wifi_sel_ < static_cast<int>(wifi_nets_->size())) {
                add((*wifi_nets_)[wifi_sel_].c_str(), 0, false);
            }
            add("接続", kIdWifiConn, true);
            add("削除", kIdWifiDel, true);
            add("< Back", kIdBack, true);
            break;
        case Screen::kWifiScan: {
            if (wifi_note_[0]) add(wifi_note_, 0, false);
            int shown = 0;
            if (wifi_scan_) {
                for (size_t i = 0; i < wifi_scan_->size() && shown < kMaxWifiScanRows; ++i) {
                    add((*wifi_scan_)[i].c_str(), kIdWifiScanned + static_cast<int>(i), true);
                    ++shown;
                }
            }
            if (shown == 0) add("(AP が見つからない)", 0, false);
            // 隠し SSID はスキャンに出ないので、打つ逃げ道を残す。
            add("SSID を手入力", kIdWifiMan, true);
            add("< Back", kIdBack, true);
            break;
        }
    }
    menu_.set_items(items_, n);
}

// **戻り先を 1 か所にまとめる。** 入れ子が 2 段になったので、"< Back" と Esc が
// 別々に kRoot へ飛ぶと、WiFi の中から一気に最上位まで戻ってしまう。
MenuUi::Screen MenuUi::parent_of(Screen s)
{
    switch (s) {
        case Screen::kWifi:     return Screen::kSettings;
        case Screen::kWifiNet:  return Screen::kWifi;
        case Screen::kWifiScan: return Screen::kWifi;
        default:                return Screen::kRoot;
    }
}

void MenuUi::set_wifi_note(const std::string& s)
{
    std::snprintf(wifi_note_, sizeof(wifi_note_), "%s", s.c_str());
    if (screen_ == Screen::kWifi || screen_ == Screen::kWifiScan) {
        // 選択位置は保たない。注記が増減すると行がずれるので、先頭から選び直す。
        rebuild();
        dirty_ = true;
    }
}

void MenuUi::show_wifi_scan() { enter(Screen::kWifiScan); }
void MenuUi::show_wifi_list() { enter(Screen::kWifi); }

// 接続先を並べる。**id に index を埋めて返す**ので、並び順が変わっても
// 選んだ項目と繋ぐ先がずれない。
void MenuUi::add_profiles(bool ssh, int* n)
{
    auto add = [&](const char* text, int id, bool enabled) {
        if (*n >= ui::Menu::kMaxItems) return;
        std::snprintf(labels_[*n], sizeof(labels_[*n]), "%s", text);
        items_[*n].label   = labels_[*n];
        items_[*n].id      = id;
        items_[*n].enabled = enabled;
        ++(*n);
    };

    if (!profiles_) return;
    if (!profiles_->error.empty()) {
        // **読めなかった理由を画面に出す。** 黙って空にすると
        // 「SD を挿し忘れた」のか「JSON を壊した」のか分からない。
        char buf[96];
        std::snprintf(buf, sizeof(buf), "SD: %s", profiles_->error.c_str());
        add(buf, 0, false);
        return;
    }
    int shown = 0;
    for (size_t i = 0; i < profiles_->profiles.size(); ++i) {
        const prof::Profile& p = profiles_->profiles[i];
        const bool is_ssh = (p.type == prof::Type::kSsh);
        if (is_ssh != ssh) continue;
        char buf[96];
        if (is_ssh) {
            // via があれば「先に張る VPN」も見せる。繋いだ後で気づくと遅い。
            char via[24] = {};
            if (!p.via.empty()) std::snprintf(via, sizeof(via), " via %s", p.via.c_str());
            std::snprintf(buf, sizeof(buf), "%s  %s@%s:%u%s", p.name.c_str(), p.user.c_str(),
                          p.host.c_str(), (unsigned)p.port, via);
        } else {
            std::snprintf(buf, sizeof(buf), "%s  %s  %s", p.name.c_str(), prof::type_name(p.type),
                          p.type == prof::Type::kTailscale ? p.control.c_str()
                                                           : p.peer.endpoint.c_str());
        }
        add(buf, kIdProfile + static_cast<int>(i), true);
        ++shown;
    }
    if (shown == 0) add(ssh ? "(SD に ssh のプロファイルが無い)"
                            : "(SD に VPN のプロファイルが無い)", 0, false);
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
            if (action_) action_(Action::kShowTerminal, -1);
            return true;
        }
        enter(parent_of(screen_));
        return true;
    }
    const int id = menu_.take_activated();
    if (id > 0) activate(id);
    return true;
}

void MenuUi::activate(int id)
{
    // SD の接続先。id に埋めた index をそのまま渡す。
    if (id >= kIdProfile) {
        if (action_) action_(Action::kConnectProfile, id - kIdProfile);
        return;
    }
    // **上から順に見る。** 帯域が広いほうから判定しないと、スキャン結果の id が
    // 保存済みの条件に先に引っかかる。
    if (id >= kIdWifiScanned) {
        if (action_) action_(Action::kWifiAddScanned, id - kIdWifiScanned);
        return;
    }
    if (id >= kIdWifiNet) {
        wifi_sel_ = id - kIdWifiNet;
        enter(Screen::kWifiNet);
        return;
    }
    switch (id) {
        case kIdSsh: enter(Screen::kSsh); break;
        case kIdVpn: enter(Screen::kVpn); break;
        case kIdSettings: enter(Screen::kSettings); break;
        case kIdTerminal:
            if (action_) action_(Action::kShowTerminal, -1);
            break;
        case kIdSavedSsh:
            if (action_) action_(Action::kOpenSsh, -1);
            break;
        case kIdTs:
            if (action_) action_(Action::kTsConnect, -1);
            break;
        case kIdWg:
            if (action_) action_(Action::kWgUp, -1);
            break;
        case kIdReload:
            if (action_) action_(Action::kReloadProfiles, -1);
            break;
        case kIdWifi:
            wifi_note_[0] = '\0';
            enter(Screen::kWifi);
            break;
        case kIdWifiNew:
            // スキャンは数秒かかる。**先に「探している」と出してから**呼び出し側に渡す
            // （何も出さないと固まったように見える）。
            set_wifi_note("スキャン中...");
            if (action_) action_(Action::kWifiScan, -1);
            break;
        case kIdWifiConn:
            if (action_) action_(Action::kWifiConnect, wifi_sel_);
            break;
        case kIdWifiDel:
            if (action_) action_(Action::kWifiDelete, wifi_sel_);
            break;
        case kIdWifiMan:
            if (action_) action_(Action::kWifiAddManual, -1);
            break;
        case kIdBack: enter(parent_of(screen_)); break;
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
        case Screen::kSsh: title = "SSH"; break;
        case Screen::kVpn: title = "VPN"; break;
        case Screen::kSettings: title = "Settings"; break;
        case Screen::kWifi:
        case Screen::kWifiNet: title = "WiFi"; break;
        case Screen::kWifiScan: title = "WiFi scan"; break;
        case Screen::kRoot: break;
    }
    gfx_.setTextColor(TFT_CYAN, kBg);
    gfx_.drawString(title, 24, top_ + 16);

    // 窓の中だけ描く。**hit_test と同じ first_visible を使う**（片方だけ直すと
    // 押した行と繋ぐ先がずれる）。
    const int first = menu_.first_visible();
    const int rows  = (menu_.visible_rows() > 0) ? menu_.visible_rows() : menu_.count();
    for (int i = first; i < menu_.count() && i < first + rows; ++i) {
        const int  y   = list_top_ + (i - first) * row_h_;
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

    // 窓の外にまだ項目があることを示す。出さないと「4 件しかない」と読める。
    if (first + rows < menu_.count() || first > 0) {
        char more[48];
        std::snprintf(more, sizeof(more), "%d-%d / %d", first + 1,
                      (first + rows < menu_.count()) ? first + rows : menu_.count(),
                      menu_.count());
        gfx_.setTextColor(kHint, kBg);
        gfx_.setTextDatum(textdatum_t::top_right);
        gfx_.drawString(more, gfx_.width() - 24, top_ + 16);
        gfx_.setTextDatum(textdatum_t::top_left);
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
