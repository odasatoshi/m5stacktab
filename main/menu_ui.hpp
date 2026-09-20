#pragma once
// 電源投入時のメニュー。上下 / Enter / Esc・← とタップで操作する。
//
// 画面の状態は「メニューを出しているか、端末を出しているか」の 2 つだけ。
// メニューの中の入れ子は 1 段しかないので、汎用のスタックは持たない。
#include <functional>
#include <string>
#include <vector>

#include <M5GFX.h>

#include "menu.hpp"
#include "profiles.hpp"
#include "wifi.hpp"

// **画面に必要な行数と ui::Menu の上限を結び付ける。** rebuild() は "< Back" を
// 最後に足すので、溢れると**指で抜ける唯一の経路が黙って消える**（残る行は
// 全部 disabled で hit_test が -1 を返す）。上限を上げたらここで気づけるようにする。
static_assert(ui::Menu::kMaxItems >= 1 + 2 + (int)prof::kMaxVpnProfiles + 1,
              "VPN 画面: 注記 + 状態 2 行 + プロファイル + \"< Back\" が入らない");
static_assert(ui::Menu::kMaxItems >= 1 + (int)prof::kMaxSshProfiles + 1 + 1,
              "SSH 画面: 注記 + プロファイル + 保存済み 1 件 + \"< Back\" が入らない");
// WiFi 画面 (#56): 注記 1 + 保存済み + "Create new wifi setting" + "< Back"。
static_assert(ui::Menu::kMaxItems >= 1 + (int)kMaxWifiNets + 2,
              "WiFi 画面: 注記 + 保存済み + 追加 + \"< Back\" が入らない");
// スキャン結果の画面はここまでしか並べない（注記 1 + 手入力 + "< Back" を残す）。
constexpr int kMaxWifiScanRows = ui::Menu::kMaxItems - 3;
static_assert(kMaxWifiScanRows > 0, "スキャン結果を並べる行が残らない");

class MenuUi {
private:
    enum class Screen { kRoot, kSsh, kVpn, kSettings, kWifi, kWifiNet, kWifiScan, kProfile };

public:
    // メニューから起こす動作。main が実装を差す（この層は描画と選択だけを持つ）。
    enum class Action {
        kOpenSsh,          // NVS に保存した 1 件で SSH を開く（接続先が無いときの経路）
        kShowTerminal,     // 端末に移る（メニューを閉じる）
        kConnectProfile,   // NVS の接続先の N 番目に繋ぐ (#49)
        kDeleteProfile,    // NVS から N 番目を消す (#73)。SD の原本は残る
        kReloadProfiles,   // NVS の接続先を読み直す (#60)。SD からの取り込みは `profiles import`
        // --- WiFi (#56)。index は保存済み / スキャン結果の何番目か ---
        kWifiConnect,      // 保存済みの N 番目に繋ぐ
        kWifiDelete,       // 保存済みの N 番目を消す
        kWifiScan,         // AP を探し始める（終わったら show_wifi_scan を呼ばせる）
        kWifiAddScanned,   // スキャン結果の N 番目を足す（パスワードは呼び出し側が聞く）
        kWifiAddManual,    // SSID から手で入れる（隠し SSID 用）
    };

    // メニューに出す情報。呼び出し側が集める。
    struct Info {
        char ssh_target[64] = {};  // "user@192.168.0.101:22"、空なら未設定
        char ts_state[48]   = {};
        char wg_state[48]   = {};
        char wifi[48]       = {};
        // NVS の接続先の読み込み結果（読めなかった理由もここ）。#60 まで SD だった。
        char profiles[64]   = {};
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
    // 接続先 (#49)。**呼び出し側が保持し続けること**（コピーしない）。
    void set_profiles(const prof::Config* cfg) { profiles_ = cfg; }
    // 第 2 引数は kConnectProfile のときだけ意味がある（profiles の index）。
    void set_action(std::function<void(Action, int)> fn) { action_ = std::move(fn); }

    // --- WiFi (#56) ---
    //
    // 一覧に出す文字列は**呼び出し側が作る**（`*` の印や電波の強さの書き方は
    // wifi.cpp の都合で、この層は行を並べるだけ）。**呼び出し側が保持し続けること。**
    void set_wifi_nets(const std::vector<std::string>* v) { wifi_nets_ = v; }
    void set_wifi_scan(const std::vector<std::string>* v) { wifi_scan_ = v; }
    // 画面の先頭に出す 1 行（「スキャン中…」「5 件で満杯」「消せない理由」など）。
    // **WiFi と接続先で同じ 1 本を使う** — 画面は同時に 1 つしか出ないので、
    // 別々に持つと「どちらを消し忘れたか」だけが増える。空なら出さない。
    void set_note(const std::string& s);
    void set_wifi_note(const std::string& s) { set_note(s); }
    // 今 WiFi の一覧を見ているか。**スキャンの結果に飛ばしてよいかの判定に使う** —
    // 数秒待つ間に Back で抜けていたら、いきなり飛ばすと画面を奪うことになる。
    bool on_wifi_list() const { return screen_ == Screen::kWifi; }
    // スキャンが終わったら呼ぶ。結果の画面に移る。
    void show_wifi_scan();
    // 足した / 消したあとに一覧へ戻る。
    void show_wifi_list();

    // 接続先を消したあとに、入ってきた一覧 (SSH / VPN) へ戻る (#73)。
    // **消した後に詳細画面へ残してはいけない** — 消えた index を指したまま
    // 「接続」が押せてしまう。
    void show_profile_list();

    // キー入力。処理したら true。
    bool key(ui::Key k);
    // タップ。処理したら true。
    bool touch_down(int x, int y);

    void draw(bool force = false);

private:
    // Esc / "< Back" の戻り先。入れ子が 2 段になった (#56) ので表にする。
    Screen parent_of(Screen s) const;
    // 先頭に注記の行を出す画面か。**set_note と rebuild で同じ表を見る**
    // （片方だけ足すと、書いたのに出ない／消しても残るという形で出る）。
    static bool shows_note(Screen s);
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
    // SSH のラベルは "name  user@host:port via vpn" になるので 72 では足りない。
    char     labels_[ui::Menu::kMaxItems][96]{};
    const prof::Config*              profiles_ = nullptr;
    const std::vector<std::string>*  wifi_nets_ = nullptr;
    const std::vector<std::string>*  wifi_scan_ = nullptr;
    char                             note_[96] = {};
    // kWifiNet で見ている保存済みの index。
    int                              wifi_sel_ = -1;
    // kProfile で見ている接続先の index と、そこへ入る前の一覧 (#73)。
    // **戻り先は覚えておく。** 同じ詳細画面に SSH 画面からも VPN 画面からも入るので、
    // parent_of() を型で分岐させると、消した後に戻る先が種類で変わる。
    int                              prof_sel_    = -1;
    Screen                           prof_parent_ = Screen::kSsh;
    std::function<void(Action, int)> action_;
};
