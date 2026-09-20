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
    kIdMisc     = 3,
    kIdTerminal = 12,
    kIdSavedSsh = 13,  // NVS に保存した 1 件（接続先一覧とは別の最短経路）
    kIdReload   = 14,
    kIdWifi     = 15,  // 最上位から WiFi の一覧へ (#56, #82)
    kIdWifiNew  = 20,
    kIdWifiConn = 21,
    kIdWifiDel  = 22,
    kIdWifiMan  = 23,
    kIdProfConn = 24,  // 接続先の詳細から繋ぐ (#73)
    kIdProfDel  = 25,  // 接続先の詳細から消す (#73)
    kIdNewSsh   = 26,  // SSH の一覧から新規作成 (#82)
    kIdNewVpn   = 27,  // VPN の一覧から新規作成 (#82)
    kIdFormSave = 28,  // 新規作成の画面の "保存" (#82)
    kIdBack     = 99,
    // **index を埋めて返す帯域。互いに重ならないように離してある。**
    // 下の static_assert が、上限を上げたときの食い込みを止める。
    kIdWifiNet     = 200,   // 保存済みの N 番目
    kIdWifiScanned = 400,   // スキャン結果の N 番目
    kIdFormField   = 600,   // 新規作成の画面の N 番目の項目 (#82)
    // NVS の接続先。id から profiles の index を戻せるようにしておく (#49)。
    kIdProfile  = 1000,
};

static_assert(kIdWifiNet + (int)kMaxWifiNets < kIdWifiScanned,
              "保存済みの id がスキャン結果の帯域に食い込む");
static_assert(kIdWifiScanned + kMaxWifiScanRows < kIdFormField,
              "スキャン結果の id が新規作成の項目の帯域に食い込む");
static_assert(kIdFormField + kMaxFormFields < kIdProfile,
              "新規作成の項目の id が接続先の帯域に食い込む");

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
            // **接続先の種類をそのまま並べる (#82)。** どれも「一覧 → 選ぶ →
            // 接続 / 削除、末尾に新規作成」の同じ形で、WiFi だけ 1 段深いのをやめた。
            add("SSH", kIdSsh, true);
            add("VPN", kIdVpn, true);
            add("WiFi", kIdWifi, true);
            add("Miscellanea", kIdMisc, true);
            break;
        case Screen::kSsh:
            if (note_[0]) add(note_, 0, false);
            add_profiles(/*ssh=*/true, &n);
            // `ssh <user> <host>` で保存した 1 件はいつでも残す。一覧が空でも
            // 繋げる経路が残るようにする (#66 で前提は NVS に移ったが、役割は同じ)。
            std::snprintf(buf, sizeof(buf), "保存済み: %s",
                          info_.ssh_target[0] ? info_.ssh_target : "(未設定)");
            add(buf, kIdSavedSsh, info_.ssh_target[0] != '\0');
            add("Create new SSH connection", kIdNewSsh, true);
            add("< Back", kIdBack, true);
            break;
        case Screen::kVpn:
            if (note_[0]) add(note_, 0, false);
            std::snprintf(buf, sizeof(buf), "Tailscale: %s", info_.ts_state);
            add(buf, 0, false);
            std::snprintf(buf, sizeof(buf), "WireGuard: %s", info_.wg_state);
            add(buf, 0, false);
            add_profiles(/*ssh=*/false, &n);
            add("Create new VPN connection", kIdNewVpn, true);
            // 指だけで戻れる経路。全項目が状態表示だと hit_test が常に -1 になり、
            // Esc を送る手段（シリアル）が無いと出られない。
            add("< Back", kIdBack, true);
            break;
        case Screen::kMisc:
            // **どこにも属さないものだけを置く (#82)。** 接続先そのものは
            // SSH / VPN / WiFi の 3 つに寄せてあるので、ここに来るのは
            // 端末へ戻る経路と、接続先の読み込み結果の表示。
            add("Terminal", kIdTerminal, true);
            // ponytail: 表示は読み取り専用。編集はコンソールから（`ssh`）。
            std::snprintf(buf, sizeof(buf), "SSH: %s",
                          info_.ssh_target[0] ? info_.ssh_target : "(未設定)");
            add(buf, 0, false);
            std::snprintf(buf, sizeof(buf), "接続先: %s", info_.profiles);
            add(buf, 0, false);
            add("接続先を読み直す", kIdReload, true);
            add("< Back", kIdBack, true);
            break;
        case Screen::kWifi:
            // 注記は「スキャン中…」「5 件で満杯」など。**画面に理由を出す唯一の場所**
            // （端末に書いてもメニューを出している間は描かれない）。
            if (note_[0]) add(note_, 0, false);
            // **繋がっているかをここに出す (#82)。** Settings を畳んだので、
            // 状態を出す場所がここしか残っていない。
            std::snprintf(buf, sizeof(buf), "状態: %s", info_.wifi);
            add(buf, 0, false);
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
        case Screen::kProfile: {
            if (note_[0]) add(note_, 0, false);
            // WiFi と同じ「一覧 → 詳細 → 削除」の 2 段。**確認ダイアログの代わり**に
            // なっているので、一覧から直に消せるようにはしない。
            const prof::Profile* p = nullptr;
            if (profiles_ && prof_sel_ >= 0 &&
                prof_sel_ < static_cast<int>(profiles_->profiles.size())) {
                p = &profiles_->profiles[prof_sel_];
            }
            add(p ? p->name.c_str() : "(消えた)", 0, false);
            if (p) {
                std::snprintf(buf, sizeof(buf), "%s  %s", prof::type_name(p->type),
                              p->type == prof::Type::kSsh ? p->host.c_str()
                              : p->type == prof::Type::kTailscale ? p->control.c_str()
                                                                  : p->peer.endpoint.c_str());
                add(buf, 0, false);
            }
            add("接続", kIdProfConn, p != nullptr);
            add("削除", kIdProfDel, p != nullptr);
            add("< Back", kIdBack, true);
            break;
        }
        case Screen::kForm: {
            // **行の中身は呼び出し側が作る。** 項目は type で変わるので、
            // この層は並べて「何番目が押されたか」を返すだけにする (#82)。
            if (note_[0]) add(note_, 0, false);
            int fields = 0;
            if (form_rows_) {
                for (size_t i = 0; i < form_rows_->size() && fields < kMaxFormFields; ++i) {
                    add((*form_rows_)[i].c_str(), kIdFormField + static_cast<int>(i), true);
                    ++fields;
                }
            }
            if (fields == 0) add("(項目が無い)", 0, false);
            add("保存", kIdFormSave, fields > 0);
            add("< Back", kIdBack, true);
            break;
        }
        case Screen::kWifiScan: {
            if (note_[0]) add(note_, 0, false);
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
MenuUi::Screen MenuUi::parent_of(Screen s) const
{
    switch (s) {
        // **WiFi は最上位の下 (#82)。** Settings の中の 1 段は無くなった。
        case Screen::kWifi:     return Screen::kRoot;
        case Screen::kWifiNet:  return Screen::kWifi;
        case Screen::kWifiScan: return Screen::kWifi;
        // 同じ詳細画面に SSH 画面からも VPN 画面からも入るので、入り口を覚えて戻す。
        case Screen::kProfile:  return prof_parent_;
        // 新規作成も、SSH の一覧からも VPN の一覧からも入る (#82)。
        case Screen::kForm:     return form_parent_;
        default:                return Screen::kRoot;
    }
}

bool MenuUi::shows_note(Screen s)
{
    return s == Screen::kWifi || s == Screen::kWifiScan || s == Screen::kSsh ||
           s == Screen::kVpn || s == Screen::kProfile || s == Screen::kForm;
}

void MenuUi::set_note(const std::string& s)
{
    std::snprintf(note_, sizeof(note_), "%s", s.c_str());
    if (shows_note(screen_)) {
        // 選択位置は保たない。注記が増減すると行がずれるので、先頭から選び直す。
        rebuild();
        dirty_ = true;
    }
}

void MenuUi::show_wifi_scan() { enter(Screen::kWifiScan); }
void MenuUi::show_form() { enter(Screen::kForm); }
void MenuUi::show_form_list() { enter(form_parent_); }
void MenuUi::show_wifi_list() { enter(Screen::kWifi); }

void MenuUi::show_profile_list()
{
    prof_sel_ = -1;
    enter(prof_parent_);
}

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
        // 「まだ取り込んでいない」のか「JSON を壊した」のか分からない。
        char buf[96];
        std::snprintf(buf, sizeof(buf), "接続先: %s", profiles_->error.c_str());
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
    if (shown == 0) add(ssh ? "(ssh の接続先が無い)"
                            : "(VPN の接続先が無い)", 0, false);
}

// **見出しは 1 か所で決める。** 画面とコンソールで別々に持つと、片方だけ
// 取り残されて「ログの画面名と実際の画面が違う」という一番たちの悪い形になる。
const char* MenuUi::screen_name() const
{
    switch (screen_) {
        case Screen::kSsh: return "SSH";
        case Screen::kVpn: return "VPN";
        case Screen::kMisc: return "Miscellanea";
        case Screen::kWifi:
        case Screen::kWifiNet: return "WiFi";
        case Screen::kWifiScan: return "WiFi scan";
        // 詳細画面は入ってきた一覧の見出しを引き継ぐ（どこから入ったか分かるように）。
        case Screen::kProfile: return (prof_parent_ == Screen::kVpn) ? "VPN" : "SSH";
        case Screen::kForm: return (form_parent_ == Screen::kVpn) ? "New VPN" : "New SSH";
        case Screen::kRoot: break;
    }
    return "m5stacktab";
}

std::string MenuUi::state_line() const
{
    const int sel = menu_.selected();
    const char* label = (sel >= 0 && sel < menu_.count()) ? menu_.item(sel).label : "(なし)";
    return std::string(screen_name()) + " | > " + (label ? label : "(空)") + " | " +
           std::to_string(sel + 1) + "/" + std::to_string(menu_.count());
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
    // NVS の接続先。id に埋めた index で詳細画面に入る (#73)。
    // **一覧から直に繋がない。** 繋ぐのも消すのも詳細画面の中でだけできるようにして、
    // 削除に「1 段挟む」という確認を持たせる（WiFi と同じ形）。
    if (id >= kIdProfile) {
        prof_sel_    = id - kIdProfile;
        prof_parent_ = screen_;
        enter(Screen::kProfile);
        return;
    }
    // **上から順に見る。** 帯域が広いほうから判定しないと、スキャン結果の id が
    // 保存済みの条件に先に引っかかる。
    if (id >= kIdFormField) {
        if (action_) action_(Action::kFormEdit, id - kIdFormField);
        return;
    }
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
        case kIdSsh:
            note_[0] = '\0';
            enter(Screen::kSsh);
            break;
        case kIdVpn:
            note_[0] = '\0';
            enter(Screen::kVpn);
            break;
        case kIdMisc: enter(Screen::kMisc); break;
        case kIdTerminal:
            if (action_) action_(Action::kShowTerminal, -1);
            break;
        case kIdSavedSsh:
            if (action_) action_(Action::kOpenSsh, -1);
            break;
        case kIdReload:
            if (action_) action_(Action::kReloadProfiles, -1);
            break;
        case kIdWifi:
            note_[0] = '\0';
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
        case kIdProfConn:
            if (action_) action_(Action::kConnectProfile, prof_sel_);
            break;
        case kIdProfDel:
            if (action_) action_(Action::kDeleteProfile, prof_sel_);
            break;
        case kIdNewSsh:
        case kIdNewVpn:
            // **入ってきた一覧を覚えてから渡す。** 新規作成の画面から "< Back" で
            // 戻る先は、SSH から入ったか VPN から入ったかで変わる。
            form_parent_ = screen_;
            note_[0]     = '\0';
            if (action_) action_(Action::kNewProfile, id == kIdNewSsh ? 0 : 1);
            break;
        case kIdFormSave:
            if (action_) action_(Action::kFormSave, -1);
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

    gfx_.setTextColor(TFT_CYAN, kBg);
    gfx_.drawString(screen_name(), 24, top_ + 16);

    // 窓の中だけ描く。**hit_test と同じ first_visible を使う**（片方だけ直すと
    // 押した行と繋ぐ先がずれる）。
    const int first = menu_.first_visible();
    const int rows  = (menu_.visible_rows() > 0) ? menu_.visible_rows() : menu_.count();
    for (int i = first; i < menu_.count() && i < first + rows; ++i) {
        const int  y   = list_top_ + (i - first) * row_h_;
        // 選べない項目には目印を付けない。全部が状態表示の画面（VPN / Miscellanea）で
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
