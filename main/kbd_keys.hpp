#pragma once
// Tab5 純正キーボードの STRING (Character) モードのキー名を、端末に送るバイト列へ変換する。
//
// **キー名は上流のファームの表そのまま** (`m5stack/M5Tab5-Keyboard-Internal-FW` の
// `user_keyboard_handle.c` の `key_value_map[5][14]`)。文字キーは押した文字がそのまま
// 名前になり、特殊キーだけ "esc" / "enter" のような名前で来る。
//
// **Aa (大文字) モードでは特殊キーの名前も大文字になる** ("ESC" / "BACKSPACE" / "UP")。
// 表を読まずに小文字だけで比べると、**Aa を押した後だけ Enter が効かない**という
// 形でしか出ない。だから比較は大小を無視する（"tab" だけは上流も小文字のままだが、
// 揃えて扱う）。
#include <cstdint>
#include <string>

// 修飾ビットは HID の modifier と同じ（上流ファームの `user_hid_map.h`）。
// Shift は文字名の側に既に効いているので見る必要がない。
constexpr uint8_t kKbdModCtrl = 0x01;
constexpr uint8_t kKbdModAlt  = 0x04;

inline bool kbd_name_is(const std::string& name, const char* want)
{
    size_t i = 0;
    for (; want[i]; ++i) {
        if (i >= name.size()) return false;
        char a = name[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != want[i]) return false;
    }
    return i == name.size();
}

// メニューを開く／閉じるキー (#51)。**端末に送りたいキーを潰さない**のが要点で、
// Esc も Ctrl+C も端末の中で必要なので使えない。Ctrl+Alt+M にしたのは、
// vim も tmux も既定では使わず、片手で押せるため。
// `sym` / `Aa` の単独打鍵は使わない（キーボードのファームが内部の層切り替えにも
// 使っていて、端末側で意味を持たせると挙動が二重になる）。
inline bool kbd_is_menu_key(const std::string& name, uint8_t mod)
{
    return (mod & kKbdModCtrl) && (mod & kKbdModAlt) && kbd_name_is(name, "m");
}

// キー名 + 修飾ビット -> 端末に送るバイト列。空文字列 = 送るものが無い。
// `app_cursor` は DECCKM（vim や less が入れる）。矢印を ESC[A ではなく ESC O A で送る。
inline std::string kbd_key_to_bytes(const std::string& name, uint8_t mod, bool app_cursor)
{
    if (name.empty()) return {};
    const std::string csi = app_cursor ? "\033O" : "\033[";

    std::string out;
    if (kbd_name_is(name, "esc")) {
        out = "\033";
    } else if (kbd_name_is(name, "enter")) {
        out = "\r";
    } else if (kbd_name_is(name, "tab")) {
        out = "\t";
    } else if (kbd_name_is(name, "backspace")) {
        out = "\177";  // DEL。stty の既定が erase = ^? なので BS (0x08) ではない
    } else if (kbd_name_is(name, "del")) {
        out = "\033[3~";  // これは DECCKM の対象外（常に CSI）
    } else if (kbd_name_is(name, "up")) {
        out = csi + "A";
    } else if (kbd_name_is(name, "down")) {
        out = csi + "B";
    } else if (kbd_name_is(name, "right")) {
        out = csi + "C";
    } else if (kbd_name_is(name, "left")) {
        out = csi + "D";
    } else if (name.size() != 1) {
        // **知らない名前は送らない。** 文字キーの名前は必ず 1 バイトなので、
        // 2 文字以上でここに来たのは未対応の特殊キー（上流の表には "sym" や "Aa" もある）。
        // そのまま流すと、シェルのプロンプトに "sym" と打ち込むことになる。
        return {};
    } else {
        // 文字キー。名前がそのまま送る文字。
        out = name;
        if ((mod & kKbdModCtrl) && out.size() == 1) {
            char c = out[0];
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (c >= '@' && c <= '_') {
                out[0] = static_cast<char>(c & 0x1F);  // Ctrl+C = 0x03 など
            } else if (c == '?') {
                out[0] = 0x7F;
            } else if (c == ' ') {
                out[0] = 0x00;  // Ctrl+Space = NUL (tmux の prefix などが使う)
            }
            // それ以外 (Ctrl+1 など) は素の文字のまま送る
        }
    }
    // Alt は ESC を前置する（xterm の metaSendsEscape と同じ）。
    if (mod & kKbdModAlt) out.insert(out.begin(), '\033');
    return out;
}
