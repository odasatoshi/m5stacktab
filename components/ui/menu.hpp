#pragma once
// 初期メニューの純粋ロジック。ESP-IDF に依存させないのでホストでテストできる。
//
// SD の接続先を並べるようになった (#49) ので、画面に入る行数だけ窓を開けて表示する。
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
    // 一番多いのは VPN の画面: 状態 2 行 + プロファイル 5 件 (prof::kMaxVpnProfiles)
    // + "< Back"。SSH は 5 件 + 保存済み 1 + "< Back"。余裕を見て 12。
    static constexpr int kMaxItems = 12;

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

    // 一度に表示できる行数。0 = 制限なし（全部描く）。
    // これを超えると選択位置を追って窓がずれる。
    void set_visible_rows(int rows);
    int  visible_rows() const { return rows_; }
    // 窓の先頭の項目 index。描画側はここから visible_rows 行ぶん描く。
    int  first_visible() const { return first_; }

    // 行の先頭 y = top、行の高さ row_h で描いたときの、タップ座標 y → 項目 index。
    // **窓の先頭を足す**ので、スクロールしていてもタップ位置と項目が一致する。
    // 範囲外や選べない項目は -1。
    int hit_test(int y, int top, int row_h) const;

private:
    // enabled な項目だけを辿る。全部 disabled なら動かさない。
    void move(int delta);
    // 選択位置が窓に入るように first_ を動かす。
    void scroll_into_view();

    const Item* items_     = nullptr;
    int         count_     = 0;
    int         selected_  = 0;
    int         activated_ = -1;
    bool        back_      = false;
    int         rows_      = 0;
    int         first_     = 0;
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
