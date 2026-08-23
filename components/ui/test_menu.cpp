// ui::Menu と vpn_state のホストテスト。
#include <cstdio>
#include <cstring>

#include "menu.hpp"

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const char* expr, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("FAIL %s:%d: %s\n", __FILE__, line, expr);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

const ui::Item kRoot[] = {
    {"SSH", 1, true},
    {"VPN", 2, true},
    {"Settings", 3, true},
};

void test_navigation()
{
    ui::Menu m;
    m.set_items(kRoot, 3);
    CHECK(m.count() == 3);
    CHECK(m.selected() == 0);
    CHECK(std::strcmp(m.item(0).label, "SSH") == 0);

    CHECK(m.key(ui::Key::kDown));
    CHECK(m.selected() == 1);
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 2);
    // 端で折り返す
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 0);
    m.key(ui::Key::kUp);
    CHECK(m.selected() == 2);

    // Enter で id が取れる。取り出したらクリアされる
    CHECK(m.take_activated() == -1);
    m.key(ui::Key::kEnter);
    CHECK(m.take_activated() == 3);
    CHECK(m.take_activated() == -1);

    // → も決定として扱う（メニューを右に潜るのと同じ）
    m.set_selected(0);
    m.key(ui::Key::kRight);
    CHECK(m.take_activated() == 1);

    // Esc と ← は戻る
    CHECK(!m.take_back());
    m.key(ui::Key::kEsc);
    CHECK(m.take_back());
    CHECK(!m.take_back());
    m.key(ui::Key::kLeft);
    CHECK(m.take_back());
}

void test_disabled_items()
{
    // 見出しや区切りを飛ばす
    const ui::Item items[] = {
        {"--- VPN ---", 0, false},
        {"Tailscale", 10, true},
        {"WireGuard", 11, true},
        {"--- info ---", 0, false},
    };
    ui::Menu m;
    m.set_items(items, 4);
    // 先頭が選べないので、選べるところまで送られている
    CHECK(m.selected() == 1);

    m.key(ui::Key::kDown);
    CHECK(m.selected() == 2);
    // 末尾の見出しを飛ばして先頭の選べる項目に回る
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 1);
    m.key(ui::Key::kUp);
    CHECK(m.selected() == 2);

    // 選べない項目は set_selected でも選べない
    m.set_selected(0);
    CHECK(m.selected() == 2);

    // 全部 disabled なら動かない（無限ループしない）
    const ui::Item all_off[] = {{"a", 0, false}, {"b", 0, false}};
    ui::Menu m2;
    m2.set_items(all_off, 2);
    m2.key(ui::Key::kDown);
    m2.key(ui::Key::kUp);
    CHECK(m2.count() == 2);
    m2.key(ui::Key::kEnter);
    CHECK(m2.take_activated() == -1);  // 選べない項目は決定できない
}

void test_hit_test()
{
    ui::Menu m;
    m.set_items(kRoot, 3);
    // top=100, row_h=40 → 100..139 が項目 0
    CHECK(m.hit_test(100, 100, 40) == 0);
    CHECK(m.hit_test(139, 100, 40) == 0);
    CHECK(m.hit_test(140, 100, 40) == 1);
    CHECK(m.hit_test(219, 100, 40) == 2);
    // 範囲外
    CHECK(m.hit_test(99, 100, 40) == -1);
    CHECK(m.hit_test(220, 100, 40) == -1);
    CHECK(m.hit_test(100, 100, 0) == -1);

    // 選べない項目はタップでも選べない
    const ui::Item items[] = {{"head", 0, false}, {"a", 1, true}};
    ui::Menu m2;
    m2.set_items(items, 2);
    CHECK(m2.hit_test(0, 0, 40) == -1);
    CHECK(m2.hit_test(40, 0, 40) == 1);
}

// MenuUi::refresh() が依存している組み合わせ。set_items で選択が 0 に戻り、
// set_selected で復元する。復元先が選べない項目なら 0 のまま（黙って化けない）。
void test_reselect_after_set_items()
{
    ui::Menu m;
    m.set_items(kRoot, 3);
    m.key(ui::Key::kDown);
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 2);

    // 作り直すと先頭に戻る
    m.set_items(kRoot, 3);
    CHECK(m.selected() == 0);
    // 復元できる
    m.set_selected(2);
    CHECK(m.selected() == 2);

    // set_items は取り出されていない Enter / Esc を捨てる。
    // 捨てないと、画面を作り直した直後に前の画面の決定が発火する。
    m.key(ui::Key::kEnter);
    m.set_items(kRoot, 3);
    CHECK(m.take_activated() == -1);
    m.key(ui::Key::kEsc);
    m.set_items(kRoot, 3);
    CHECK(!m.take_back());

    // 復元先が選べない項目なら動かさない
    const ui::Item items[] = {{"a", 1, true}, {"head", 0, false}};
    m.set_items(items, 2);
    CHECK(m.selected() == 0);
    m.set_selected(1);
    CHECK(m.selected() == 0);
}

void test_bounds()
{
    ui::Menu m;
    // 空でも落ちない
    m.set_items(nullptr, 0);
    CHECK(m.count() == 0);
    CHECK(m.hit_test(0, 0, 24) == -1);
    m.key(ui::Key::kDown);
    m.key(ui::Key::kEnter);
    CHECK(m.take_activated() == -1);
    CHECK(std::strcmp(m.item(0).label, "") == 0);
    CHECK(std::strcmp(m.item(-1).label, "") == 0);
    CHECK(std::strcmp(m.item(99).label, "") == 0);

    // kMaxItems で切る
    ui::Item many[ui::Menu::kMaxItems + 4];
    for (auto& it : many) {
        it.label   = "x";
        it.enabled = true;
    }
    m.set_items(many, ui::Menu::kMaxItems + 4);
    CHECK(m.count() == ui::Menu::kMaxItems);
}

void test_vpn_state()
{
    using ui::VpnState;
    // 何も動いていない
    CHECK(ui::vpn_state(false, false, false, false) == VpnState::kOff);
    // 制御プレーンに繋ぎ始めただけ
    CHECK(ui::vpn_state(true, false, false, false) == VpnState::kConnecting);
    // 登録できたが netif も鍵もまだ
    CHECK(ui::vpn_state(true, true, false, false) == VpnState::kConnecting);
    // netif は上がったが鍵が未確定（#37 の状態がまさにこれ）
    CHECK(ui::vpn_state(true, true, true, false) == VpnState::kConnecting);
    // 鍵が確定すれば使える
    CHECK(ui::vpn_state(true, true, true, true) == VpnState::kUp);
    // 素の WireGuard だけでも、鍵が確定していれば使える
    CHECK(ui::vpn_state(false, false, true, true) == VpnState::kUp);
    // netif が下りているのに handshake が立っている状態は「使える」と言わない
    CHECK(ui::vpn_state(false, false, false, true) == VpnState::kOff);

    CHECK(std::strcmp(ui::vpn_label(VpnState::kOff), "off") == 0);
    CHECK(std::strcmp(ui::vpn_label(VpnState::kConnecting), "...") == 0);
    CHECK(std::strcmp(ui::vpn_label(VpnState::kUp), "up") == 0);
}

// SD の接続先を並べると 8 件では収まらない (#49)。窓の中だけ描くので、
// 選択位置に窓が追いつくことと、タップ座標が窓の先頭を足して解決されることを固める。
void test_scroll_window()
{
    ui::Item items[20];
    // **GCC の -Wformat-truncation は %d を最大 11 桁で見積もる。** 幅を詰めると
    // ホスト CI (Linux/gcc) だけ -Werror で落ちる（macOS の clang は黙っている）。
    char     labels[20][16];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(labels[i], sizeof(labels[i]), "i%d", i);
        items[i] = {labels[i], i + 1, true};
    }
    ui::Menu m;
    m.set_items(items, 20);
    m.set_visible_rows(5);
    CHECK(m.first_visible() == 0);

    // 窓の中を動いている間は動かない
    for (int i = 0; i < 4; ++i) m.key(ui::Key::kDown);
    CHECK(m.selected() == 4);
    CHECK(m.first_visible() == 0);
    // 窓からはみ出すと 1 行ずつ送る
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 5);
    CHECK(m.first_visible() == 1);
    // 末尾まで送っても窓は満杯のまま（余白を出さない）
    while (m.selected() != 19) m.key(ui::Key::kDown);
    CHECK(m.first_visible() == 15);
    // 端で折り返したら先頭に戻る
    m.key(ui::Key::kDown);
    CHECK(m.selected() == 0);
    CHECK(m.first_visible() == 0);
    // 上に折り返したら末尾の窓
    m.key(ui::Key::kUp);
    CHECK(m.selected() == 19);
    CHECK(m.first_visible() == 15);

    // **タップは窓の先頭を足して解く。** 足さないと、スクロールした後に
    // 押した行と違う接続先へ繋いでしまう。
    CHECK(m.hit_test(0, 0, 40) == 15);
    CHECK(m.hit_test(40, 0, 40) == 16);
    CHECK(m.hit_test(4 * 40, 0, 40) == 19);
    // 窓の外（描いていない行）は当たらない
    CHECK(m.hit_test(5 * 40, 0, 40) == -1);

    // set_selected でも窓が追う
    m.set_selected(2);
    CHECK(m.first_visible() == 2);

    // 全部入るなら窓は動かない
    m.set_visible_rows(20);
    CHECK(m.first_visible() == 0);
    m.set_selected(19);
    CHECK(m.first_visible() == 0);
    // 0 = 制限なし
    m.set_visible_rows(0);
    CHECK(m.first_visible() == 0);
    CHECK(m.hit_test(19 * 40, 0, 40) == 19);

    // set_items で窓も先頭に戻る
    m.set_visible_rows(5);
    m.set_selected(19);
    CHECK(m.first_visible() == 15);
    m.set_items(items, 20);
    CHECK(m.first_visible() == 0);
}

}  // namespace

int main()
{
    test_navigation();
    test_disabled_items();
    test_hit_test();
    test_reselect_after_set_items();
    test_bounds();
    test_scroll_window();
    test_vpn_state();
    if (g_fails) {
        std::printf("FAILED: %d of %d checks\n", g_fails, g_checks);
        return 1;
    }
    std::printf("ok: %d checks passed\n", g_checks);
    return 0;
}
