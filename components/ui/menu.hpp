#pragma once
// 初期メニューの純粋ロジック。ESP-IDF に依存させないのでホストでテストできる。
//
// 項目数は 3〜5 なのでスクロールは持たない。必要になってから足す。
// 入れ子は「今どの画面か」を呼び出し側が持つ形にして、ここでは 1 画面ぶんだけ扱う
// （深さ 2 の入れ子に汎用スタックを用意しても使い道がない）。
#include <cstdint>

namespace ui {

// キー入力。#15 の純正キーボードが来たら、そのキーコードをここに写すだけにする。
enum class Key : uint8_t { kUp, kDown, kEnter, kEsc, kLeft, kRight };

struct Item {
    const char* label = "";
    int         id    = 0;
    // 選べない見出しや区切りに使う。上下移動で飛ばす。
    bool        enabled = true;
};

class Menu {
public:
    static constexpr int kMaxItems = 8;

    // items は呼び出し側が保持し続けること（コピーしない）。
    void set_items(const Item* items, int count);
    int  count() const { return count_; }
    const Item& item(int i) const;

    int  selected() const { return selected_; }
    void set_selected(int i);

    // 処理したら true。上下は端で折り返す（項目が少ないほうが速い）。
    bool key(Key k);

    // Enter で選ばれた項目の id。取り出したらクリアする。-1 なら無し。
    int take_activated();
    // Esc / ← が押されたか。取り出したらクリアする。
    bool take_back();

    // 行の先頭 y = top、行の高さ row_h で描いたときの、タップ座標 y → 項目 index。
    // 範囲外や選べない項目は -1。
    int hit_test(int y, int top, int row_h) const;

private:
    // enabled な項目だけを辿る。全部 disabled なら動かさない。
    void move(int delta);

    const Item* items_     = nullptr;
    int         count_     = 0;
    int         selected_  = 0;
    int         activated_ = -1;
    bool        back_      = false;
};

// --- VPN の状態表示 ---
//
// 判定材料が 4 つあって取り違えやすいので、純粋関数にしてテストで固める。
enum class VpnState : uint8_t {
    kOff,         // 何も動いていない
    kConnecting,  // 制御プレーンに繋ぎ始めた / netif は上がったが鍵が未確定
    kUp,          // 使える状態
};

// ts_registered: 制御プレーンに登録できた。wg_handshake: トンネルの鍵が確定した。
VpnState vpn_state(bool ts_running, bool ts_registered, bool wg_up, bool wg_handshake);
const char* vpn_label(VpnState s);

}  // namespace ui
