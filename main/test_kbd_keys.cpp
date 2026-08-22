// kbd_keys.hpp のホストテスト。実機のキーを打たずに変換だけ固定する。
#include <cstdio>
#include <cstdlib>

#include "kbd_keys.hpp"

namespace {
int g_fail = 0;

void check(const std::string& got, const std::string& want, const char* what)
{
    if (got == want) return;
    std::printf("NG %s: got", what);
    for (char c : got) std::printf(" %02X", (unsigned char)c);
    std::printf(" want");
    for (char c : want) std::printf(" %02X", (unsigned char)c);
    std::printf("\n");
    ++g_fail;
}
}  // namespace

int main()
{
    // 文字はそのまま
    check(kbd_key_to_bytes("a", 0, false), "a", "a");
    check(kbd_key_to_bytes("A", 0, false), "A", "A");
    check(kbd_key_to_bytes(" ", 0, false), " ", "space");
    check(kbd_key_to_bytes("$", 0, false), "$", "sym key");

    // 特殊キー
    check(kbd_key_to_bytes("enter", 0, false), "\r", "enter");
    check(kbd_key_to_bytes("tab", 0, false), "\t", "tab");
    check(kbd_key_to_bytes("esc", 0, false), "\033", "esc");
    check(kbd_key_to_bytes("backspace", 0, false), "\177", "backspace");
    check(kbd_key_to_bytes("del", 0, false), "\033[3~", "del");

    // **Aa モードでは名前が大文字で来る。** 小文字だけ見ていると Aa の後に効かなくなる。
    check(kbd_key_to_bytes("ENTER", 0, false), "\r", "ENTER (Aa)");
    check(kbd_key_to_bytes("BACKSPACE", 0, false), "\177", "BACKSPACE (Aa)");
    check(kbd_key_to_bytes("UP", 0, false), "\033[A", "UP (Aa)");

    // 前方一致で誤爆しない
    check(kbd_key_to_bytes("e", 0, false), "e", "e is not esc");
    check(kbd_key_to_bytes("u", 0, false), "u", "u is not up");

    // 矢印と DECCKM
    check(kbd_key_to_bytes("up", 0, false), "\033[A", "up");
    check(kbd_key_to_bytes("down", 0, false), "\033[B", "down");
    check(kbd_key_to_bytes("right", 0, false), "\033[C", "right");
    check(kbd_key_to_bytes("left", 0, false), "\033[D", "left");
    check(kbd_key_to_bytes("up", 0, true), "\033OA", "up (app cursor)");
    check(kbd_key_to_bytes("del", 0, true), "\033[3~", "del stays CSI");

    // Ctrl
    check(kbd_key_to_bytes("c", kKbdModCtrl, false), "\003", "ctrl+c");
    check(kbd_key_to_bytes("d", kKbdModCtrl, false), "\004", "ctrl+d");
    check(kbd_key_to_bytes("C", kKbdModCtrl, false), "\003", "ctrl+C (Aa)");
    check(kbd_key_to_bytes("[", kKbdModCtrl, false), "\033", "ctrl+[");
    check(kbd_key_to_bytes(" ", kKbdModCtrl, false), std::string(1, '\0'), "ctrl+space");
    check(kbd_key_to_bytes("1", kKbdModCtrl, false), "1", "ctrl+1 は素通し");
    // 特殊キーは Ctrl でも変えない
    check(kbd_key_to_bytes("enter", kKbdModCtrl, false), "\r", "ctrl+enter");

    // Alt は ESC 前置
    check(kbd_key_to_bytes("b", kKbdModAlt, false), "\033b", "alt+b");
    check(kbd_key_to_bytes("left", kKbdModAlt, false), "\033\033[D", "alt+left");
    check(kbd_key_to_bytes("c", kKbdModCtrl | kKbdModAlt, false), "\033\003", "ctrl+alt+c");

    // **知らない名前は送らない**（"sym" と打ち込まれるより無反応の方がまし）
    check(kbd_key_to_bytes("sym", 0, false), "", "sym は未対応");
    check(kbd_key_to_bytes("Aa", 0, false), "", "Aa は未対応");
    check(kbd_key_to_bytes("fn", kKbdModAlt, false), "", "未対応なら Alt でも空");
    check(kbd_key_to_bytes("", 0, false), "", "空は空");

    if (g_fail) {
        std::printf("%d checks failed\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
