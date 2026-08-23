#pragma once
// 電源投入時のメニュー。上下 / Enter / Esc・← とタップで操作する。
//
// 画面の状態は「メニューを出しているか、端末を出しているか」の 2 つだけ。
// メニューの中の入れ子は 1 段しかないので、汎用のスタックは持たない。
#include <functional>

#include <M5GFX.h>

#include "menu.hpp"

class MenuUi {
public:
    // メニューから起こす動作。main が実装を差す（この層は描画と選択だけを持つ）。
    enum class Action {
        kOpenSsh,       // SSH を開く（端末に移る）
        kTsConnect,     // 保存済みの設定で Tailscale に繋ぐ
        kWgUp,          // 保存済みの設定で WireGuard を上げる
        kShowTerminal,  // 端末に移る（メニューを閉じる）
    };

    // メニューに出す情報。呼び出し側が集める。
    struct Info {
        char ssh_target[64] = {};  // "oda@192.168.0.101:22"、空なら未設定
        char ts_state[48]   = {};
        char wg_state[48]   = {};
        char wifi[48]       = {};
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
    void set_action(std::function<void(Action)> fn) { action_ = std::move(fn); }

    // キー入力。処理したら true。
    bool key(ui::Key k);
    // タップ。処理したら true。
    bool touch_down(int x, int y);

    void draw(bool force = false);

private:
    enum class Screen { kRoot, kVpn, kSettings };

    void enter(Screen s);
    void activate(int id);
    void rebuild();

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
    char     labels_[ui::Menu::kMaxItems][72]{};
    std::function<void(Action)> action_;
};
