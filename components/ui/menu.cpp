#include "menu.hpp"

namespace ui {

namespace {
const Item kEmpty{};
}  // namespace

void Menu::set_items(const Item* items, int count)
{
    if (count < 0) count = 0;
    if (count > kMaxItems) count = kMaxItems;
    items_     = items;
    count_     = count;
    activated_ = -1;
    back_      = false;
    selected_  = 0;
    // 先頭が選べない項目なら、選べるところまで送る。
    if (count_ > 0 && !items_[0].enabled) move(1);
}

const Item& Menu::item(int i) const
{
    if (!items_ || i < 0 || i >= count_) return kEmpty;
    return items_[i];
}

void Menu::set_selected(int i)
{
    if (i < 0 || i >= count_) return;
    if (!items_ || !items_[i].enabled) return;
    selected_ = i;
}

void Menu::move(int delta)
{
    if (count_ <= 0 || !items_) return;
    // 一周して戻ってきたら諦める（全部 disabled のとき無限に回らないように）。
    for (int step = 0; step < count_; ++step) {
        selected_ += delta;
        if (selected_ < 0) selected_ = count_ - 1;
        if (selected_ >= count_) selected_ = 0;
        if (items_[selected_].enabled) return;
    }
}

bool Menu::key(Key k)
{
    switch (k) {
        case Key::kUp: move(-1); return true;
        case Key::kDown: move(1); return true;
        case Key::kEnter:
        case Key::kRight:
            if (count_ > 0 && items_ && items_[selected_].enabled) {
                activated_ = items_[selected_].id;
            }
            return true;
        case Key::kEsc:
        case Key::kLeft: back_ = true; return true;
    }
    return false;
}

int Menu::take_activated()
{
    const int v = activated_;
    activated_  = -1;
    return v;
}

bool Menu::take_back()
{
    const bool v = back_;
    back_        = false;
    return v;
}

int Menu::hit_test(int y, int top, int row_h) const
{
    if (row_h <= 0 || !items_) return -1;
    if (y < top) return -1;
    const int idx = (y - top) / row_h;
    if (idx < 0 || idx >= count_) return -1;
    if (!items_[idx].enabled) return -1;
    return idx;
}

VpnState vpn_state(bool ts_running, bool ts_registered, bool wg_up, bool wg_handshake)
{
    // トンネルの鍵が確定していれば、制御プレーンの状態に関係なく使える。
    if (wg_up && wg_handshake) return VpnState::kUp;
    // 制御プレーンに登録できていても、鍵が無ければまだ通らない。
    if (ts_running || ts_registered || wg_up) return VpnState::kConnecting;
    return VpnState::kOff;
}

const char* vpn_label(VpnState s)
{
    switch (s) {
        case VpnState::kUp: return "up";
        case VpnState::kConnecting: return "...";
        case VpnState::kOff: return "off";
    }
    return "off";
}

}  // namespace ui
