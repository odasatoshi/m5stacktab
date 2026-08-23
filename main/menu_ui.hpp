#pragma once
// 電源投入時のメニュー。上下 / Enter / Esc・← とタップで操作する。
//
// 画面の状態は「メニューを出しているか、端末を出しているか」の 2 つだけ。
// メニューの中の入れ子は 1 段しかないので、汎用のスタックは持たない。
#include <functional>

#include <M5GFX.h>

#include "menu.hpp"
#include "profiles.hpp"

// **画面に必要な行数と ui::Menu の上限を結び付ける。** rebuild() は "< Back" を
// 最後に足すので、溢れると**指で抜ける唯一の経路が黙って消える**（残る行は
// 全部 disabled で hit_test が -1 を返す）。上限を上げたらここで気づけるようにする。
static_assert(ui::Menu::kMaxItems >= 2 + (int)prof::kMaxVpnProfiles + 1,
              "VPN 画面: 状態 2 行 + プロファイル + \"< Back\" が入らない");
static_assert(ui::Menu::kMaxItems >= (int)prof::kMaxSshProfiles + 1 + 1,
              "SSH 画面: プロファイル + 保存済み 1 件 + \"< Back\" が入らない");

class MenuUi {
public:
    // メニューから起こす動作。main が実装を差す（この層は描画と選択だけを持つ）。
    enum class Action {
        kOpenSsh,          // NVS に保存した 1 件で SSH を開く（SD が無いときの経路）
        kTsConnect,        // 保存済みの設定で Tailscale に繋ぐ
        kWgUp,             // 保存済みの設定で WireGuard を上げる
        kShowTerminal,     // 端末に移る（メニューを閉じる）
        kConnectProfile,   // SD の profiles.json の N 番目に繋ぐ (#49)
        kReloadProfiles,   // SD を読み直す
    };

    // メニューに出す情報。呼び出し側が集める。
    struct Info {
        char ssh_target[64] = {};  // "oda@192.168.0.101:22"、空なら未設定
        char ts_state[48]   = {};
        char wg_state[48]   = {};
        char wifi[48]       = {};
        char sd[64]         = {};  // SD の読み込み結果（読めなかった理由もここ）
    };

    explicit MenuUi(M5GFX& gfx) : gfx_(gfx) {}
    // items_ が自分の labels_ を指しているので、コピーすると原本を指してしまう。
    MenuUi(const MenuUi&)            = delete;
    MenuUi& operator=(const MenuUi&) = delete;

    // メニューに使う領域。上はステータスバー、下はキーボード（表示中なら）。
    // キーボードの表示を切り替えたら呼び直す。
    void set_area(int top, int height);
    bool visible() const { return visible_; }
    void set_visible(bool v);

    void set_info(const Info& info) { info_ = info; }
    // 純正キーボードが挿さっているか。操作説明の文言を変えるだけに使う (#51)。
    void set_has_keyboard(bool v)
    {
        if (has_kbd_ == v) return;
        has_kbd_ = v;
        dirty_   = true;
    }
    // set_info のあとに呼ぶと、項目の文字列を作り直して次の draw で反映する。
    void refresh();
    // SD から読んだ接続先 (#49)。**呼び出し側が保持し続けること**（コピーしない）。
    void set_profiles(const prof::Config* cfg) { profiles_ = cfg; }
    // 第 2 引数は kConnectProfile のときだけ意味がある（profiles の index）。
    void set_action(std::function<void(Action, int)> fn) { action_ = std::move(fn); }

    // キー入力。処理したら true。
    bool key(ui::Key k);
    // タップ。処理したら true。
    bool touch_down(int x, int y);

    void draw(bool force = false);

private:
    enum class Screen { kRoot, kSsh, kVpn, kSettings };

    void enter(Screen s);
    void activate(int id);
    void rebuild();
    // 一覧に出す接続先。ssh なら SSH のプロファイル、そうでなければ VPN のもの。
    void add_profiles(bool ssh, int* n);

    M5GFX&   gfx_;
    ui::Menu menu_;
    Screen   screen_  = Screen::kRoot;
    bool     visible_ = false;
    int      top_     = 0;
    int      height_  = 0;
    int      row_h_   = 48;
    int      list_top_ = 0;
    bool     dirty_   = true;
    bool     has_kbd_ = false;
    Info     info_{};
    // 画面ごとの項目。label は下の文字列バッファを指すので、寿命はこのクラスと同じ。
    ui::Item items_[ui::Menu::kMaxItems]{};
    // SD の接続先は "name  user@host:port via vpn" になるので 72 では足りない。
    char     labels_[ui::Menu::kMaxItems][96]{};
    const prof::Config*              profiles_ = nullptr;
    std::function<void(Action, int)> action_;
};
