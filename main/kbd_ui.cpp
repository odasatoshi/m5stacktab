#include <cstdint>
#include "kbd_ui.hpp"

#include <algorithm>
#include <cstdio>

#include <esp_log.h>
#include <esp_timer.h>

#include "kbd_keys.hpp"  // kKbdModCtrl（修飾ビットは純正キーボードと同じ表を使う）

namespace {

const char* TAG = "kbd";

constexpr uint16_t kBg       = 0x2104;  // 暗いグレー
constexpr uint16_t kKeyBg    = 0x4208;
constexpr uint16_t kKeyDown  = 0x05BF;  // 押下中は青
constexpr uint16_t kKeyFunc  = 0x2945;  // 制御キーは少し暗く（文字キーと見分ける）
constexpr uint16_t kKeyLatch = 0x8200;  // ラッチ中の ⇧ / Ctrl
constexpr uint16_t kKeyLine  = 0x8410;
constexpr uint16_t kText     = 0xFFFF;
constexpr uint16_t kStatusBg = 0x0000;
constexpr uint16_t kComposeC = 0xFFE0;  // 未確定は黄色

// --- PAD（端末に薄く重ねる小さなパッド）---
constexpr int      kPadKeyW  = 88;
constexpr int      kPadKeyH  = 68;
constexpr int      kPadEdge  = 16;      // 画面の縁からの余白
constexpr uint16_t kPadDim   = 0x2965;  // 重ねる暗い灰色（端末の黒より少し明るい）
constexpr uint16_t kPadTransp = 0xF81F; // 透過色（面のどこにも使わないマゼンタ）

}  // namespace

KeyboardUi::Mode KeyboardUi::next_mode(Mode m)
{
    switch (m) {
        case Mode::kOff:   return Mode::kAscii;
        case Mode::kAscii: return Mode::kKana;
        case Mode::kKana:  return Mode::kPad;
        case Mode::kPad:   return Mode::kOff;
    }
    return Mode::kOff;
}

const char* KeyboardUi::mode_label(Mode m)
{
    switch (m) {
        case Mode::kOff:   return "なし";
        case Mode::kAscii: return "ABC";
        case Mode::kKana:  return "かな";
        case Mode::kPad:   return "PAD";
    }
    return "?";
}

void KeyboardUi::begin(int height)
{
    height_   = height;
    status_h_ = 32;  // 未確定と候補、ラッチの状態を出す帯

    // かな面: 画面幅いっぱいに 4 列だと 1 キー 320px で間延びする。親指で押せる幅に
    // 収めて中央に置き、空いた右側は候補表示に使う。
    constexpr int kMaxKeyW = 160;
    ime::FlickLayout l;
    l.width  = std::min<int>(gfx_.width(), kMaxKeyW * 4);
    l.x      = (gfx_.width() - l.width) / 2;
    l.y      = gfx_.height() - height_ + status_h_;
    l.height = height_ - status_h_;
    l.cols   = 4;
    l.rows   = 4;
    kb_.set_layout(l);

    // ASCII 面: 12 列を画面幅いっぱいに。**端数は左右の余白に逃がす** —
    // 割り切れないまま使うと右端のキーだけ当たり判定がずれる。
    ime::FlickLayout a;
    a.cols   = ime::AsciiKeyboard::kCols;
    a.rows   = ime::AsciiKeyboard::kRows;
    a.width  = (gfx_.width() / a.cols) * a.cols;
    a.x      = (gfx_.width() - a.width) / 2;
    a.y      = l.y;
    // **端数を切り捨てない。** 切ると draw_ascii が塗る帯より当たり判定が短くなり、
    // 下端の数 px が端末領域（スクロールバックのスワイプ）に流れる。
    a.height = height_ - status_h_;
    ascii_.set_layout(a);

    build_pad();

    ESP_LOGI(TAG, "keyboard %dpx: kana key %dx%d, ascii key %dx%d, pad %dx%d at (%d,%d)", height_,
             l.key_w(), l.key_h(), a.key_w(), a.key_h(), kPadKeyW * ime::kPadCols,
             kPadKeyH * ime::kPadRows, pad_x_, pad_y_);
}

// PAD の見た目を 1 枚のスプライトに焼いておく。**毎回描き直すのは重ねる 1 回だけ**にする。
//
// **市松に間引いて薄く見せる。** M5GFX に α 合成は無い。フレームバッファを読んで
// 平均する方法は端末の差分描画と噛み合わない — 読むのは「前回薄くした画素」なので、
// 重ねるたびに暗くなっていく。市松なら何度重ねても同じ見た目になる（べき等）。
void KeyboardUi::build_pad()
{
    const int w = kPadKeyW * ime::kPadCols;
    const int h = kPadKeyH * ime::kPadRows;
    pad_x_      = gfx_.width() - w - kPadEdge;
    pad_y_      = gfx_.height() - h - kPadEdge;

    pad_.setPsram(true);  // 内蔵 RAM は 260KB しかない。95KB を常駐させない
    // **色深度は明示する。** `16` を渡すと swap565 になり、透過色の比較が
    // バイト入れ替わりで一致しなくなる（CLAUDE.md）。
    pad_.setColorDepth(lgfx::v1::color_depth_t::rgb565_nonswapped);
    if (!pad_.createSprite(w, h)) {
        ESP_LOGE(TAG, "PAD のスプライトを作れない (%dx%d)", w, h);
        return;
    }

    // **4 画素に 1 つだけ透かす。** 半分ずつだと下の端末の文字が明るいままはっきり
    // 見えてしまい、パッドの文字と競合する（実機で確認）。3/4 を潰すと
    // 「下に何かある」ことは分かるまま、パッドが読めるようになる。
    pad_.fillSprite(kPadDim);
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) pad_.drawPixel(x, y, kPadTransp);
    }

    pad_.setFont(&fonts::efontJA_24);
    for (int row = 0; row < ime::kPadRows; ++row) {
        for (int col = 0; col < ime::kPadCols; ++col) {
            const ime::AsciiKey* k = ime::pad_key(row, col);
            if (!k) continue;
            const int kx = col * kPadKeyW;
            const int ky = row * kPadKeyH;
            // **枠は 2px 焼く。** 押下の色はここと文字の背景箱にしか塗れない
            // （下の draw_pad_key を参照）ので、1px だと手応えが細すぎる。
            pad_.drawRect(kx + 1, ky + 1, kPadKeyW - 2, kPadKeyH - 2, kKeyLine);
            pad_.drawRect(kx + 2, ky + 2, kPadKeyW - 4, kPadKeyH - 4, kKeyLine);
            // **文字の後ろは塗る。** 市松のままだと下の端末の文字が字画の隙間に
            // 出て読めない。
            pad_.setTextColor(kText, kPadDim);
            const int tw = pad_.textWidth(k->label);
            pad_.drawString(k->label, kx + (kPadKeyW - tw) / 2, ky + (kPadKeyH - 24) / 2);
        }
    }
}

void KeyboardUi::draw_overlay()
{
    if (mode_ != Mode::kPad || !pad_.getBuffer()) return;
    const int64_t t0 = esp_timer_get_time();
    pad_.pushSprite(pad_x_, pad_y_, kPadTransp);
    last_overlay_us_ = (uint32_t)(esp_timer_get_time() - t0);
}

// 押下の手応え。**塗ってよいのはスプライト側が不透明な画素だけ** — 枠 2px と
// 文字の背景箱。キー全面を塗ると、離して重ね直しても**透かした 1/4 の画素に
// 押下色が残る**（市松がべき等なのは「下が端末の画素のまま」の間だけ）。
void KeyboardUi::draw_pad_key(int row, int col, bool pressed)
{
    const ime::AsciiKey* k = ime::pad_key(row, col);
    if (!k || !pad_.getBuffer()) return;
    if (!pressed) {
        draw_overlay();  // 素の見た目はスプライトにしかない。全体を重ね直す
        return;
    }
    const int x = pad_x_ + col * kPadKeyW;
    const int y = pad_y_ + row * kPadKeyH;
    gfx_.drawRect(x + 1, y + 1, kPadKeyW - 2, kPadKeyH - 2, kKeyDown);
    gfx_.drawRect(x + 2, y + 2, kPadKeyW - 4, kPadKeyH - 4, kKeyDown);
    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextColor(kText, kKeyDown);
    const int tw = gfx_.textWidth(k->label);
    gfx_.drawString(k->label, x + (kPadKeyW - tw) / 2, y + (kPadKeyH - 24) / 2);
}

bool KeyboardUi::pad_hit(int x, int y, int* row, int* col) const
{
    // **スプライトが取れなかったら触らせない。** 画面には何も出ていないのに
    // 右下のタップが Esc や ^C を端末へ送ることになる。
    if (!pad_.getBuffer()) return false;
    const int c = (x - pad_x_) / kPadKeyW;
    const int r = (y - pad_y_) / kPadKeyH;
    if (x < pad_x_ || y < pad_y_ || c >= ime::kPadCols || r >= ime::kPadRows) return false;
    *row = r;
    *col = c;
    return true;
}

void KeyboardUi::set_mode(Mode m)
{
    if (mode_ == m) return;
    // **帯を持っていたかで見る。** PAD は visible() が true でも帯を占めないので、
    // visible() で判定すると PAD を抜けるときに端末の画素を黒く塗り潰す。
    const bool had_band = (height() > 0);
    mode_   = m;
    shift_  = false;
    ctrl_   = false;
    // **かな面の押下も捨てる。** 指を置いたまま段が変わる経路がある
    // （純正キーボードの Ctrl+Alt+M でメニューを開閉する）。捨てないと、
    // 離したときに選んでいないかなが出る。
    kb_.cancel();
    pressed_key_ = -1;
    press_row_   = -1;
    press_idx_   = -1;
    if (visible()) {
        draw();
    } else if (had_band) {
        // 端末側を描き直させる（呼び出し側が force render する）。
        gfx_.fillRect(0, gfx_.height() - height_, gfx_.width(), height_, TFT_BLACK);
    }
}

void KeyboardUi::draw_key(int row, int col, bool pressed)
{
    const auto& l  = kb_.layout();
    const int   kw = l.key_w();
    const int   kh = l.key_h();
    const int   x  = l.x + col * kw;
    const int   y  = l.y + row * kh;

    gfx_.fillRect(x + 2, y + 2, kw - 4, kh - 4, pressed ? kKeyDown : kKeyBg);
    gfx_.drawRect(x + 2, y + 2, kw - 4, kh - 4, kKeyLine);

    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextColor(kText, pressed ? kKeyDown : kKeyBg);
    const char* label = ime::FlickKeyboard::key_label(row, col);
    const int   tw    = gfx_.textWidth(label);
    gfx_.drawString(label, x + (kw - tw) / 2, y + (kh - 24) / 2);

    // 押下中はフリック先を四方に薄く出す（どこに飛ぶか分かるように）。
    if (pressed && col < 3) {
        gfx_.setFont(&fonts::efontJA_16);
        struct { ime::Flick f; int dx; int dy; } dirs[] = {
            {ime::Flick::kLeft, -kw / 3, 0},
            {ime::Flick::kUp, 0, -kh / 3},
            {ime::Flick::kRight, kw / 3, 0},
            {ime::Flick::kDown, 0, kh / 3},
        };
        for (const auto& d : dirs) {
            const char* k = ime::FlickKeyboard::kana_for(row, col, d.f);
            if (!k) continue;
            const bool sel = (preview_ == d.f);
            gfx_.setTextColor(sel ? kComposeC : kKeyLine, kKeyDown);
            gfx_.drawString(k, x + kw / 2 - 8 + d.dx, y + kh / 2 - 8 + d.dy);
        }
    }
}

void KeyboardUi::draw_ascii_key(int row, int index, bool pressed)
{
    const ime::AsciiKey* k = ime::AsciiKeyboard::key_at(row, index);
    if (!k) return;
    const auto& l  = ascii_.layout();
    const int   kw = l.key_w();
    const int   kh = l.key_h();
    const int   x  = l.x + ime::AsciiKeyboard::col_of(row, index) * kw;
    const int   y  = l.y + row * kh;
    const int   w  = kw * k->span;

    const bool latched = (k->mod == ime::AsciiMod::kShift && shift_) ||
                         (k->mod == ime::AsciiMod::kCtrl && ctrl_);
    const uint16_t bg = pressed  ? kKeyDown
                        : latched ? kKeyLatch
                        : (k->mod != ime::AsciiMod::kNone || !k->shift) ? kKeyFunc
                                                                        : kKeyBg;
    gfx_.fillRect(x + 2, y + 2, w - 4, kh - 4, bg);
    gfx_.drawRect(x + 2, y + 2, w - 4, kh - 4, kKeyLine);

    gfx_.setFont(&fonts::efontJA_24);
    gfx_.setTextColor(kText, bg);
    const char* label = (shift_ && k->shift) ? k->shift : k->label;
    const int   tw    = gfx_.textWidth(label);
    gfx_.drawString(label, x + (w - tw) / 2, y + (kh - 24) / 2);
}

void KeyboardUi::draw_ascii()
{
    const auto& l = ascii_.layout();
    gfx_.fillRect(0, l.y, gfx_.width(), height_ - status_h_, kBg);
    for (int row = 0; row < ime::AsciiKeyboard::kRows; ++row) {
        for (int i = 0; ime::AsciiKeyboard::key_at(row, i); ++i) {
            draw_ascii_key(row, i, press_row_ == row && press_idx_ == i);
        }
    }
}

void KeyboardUi::draw_status()
{
    const int y = gfx_.height() - height_;
    gfx_.fillRect(0, y, gfx_.width(), status_h_, kStatusBg);
    gfx_.setFont(&fonts::efontJA_24);

    if (mode_ == Mode::kAscii) {
        // ラッチは面（キーの色）にも出るが、**指の下に隠れる**ので帯にも出す。
        std::string line = "[ABC]";
        if (shift_) line += "  Shift";
        if (ctrl_) line += "  Ctrl";
        gfx_.setTextColor((shift_ || ctrl_) ? kComposeC : kKeyLine, kStatusBg);
        gfx_.drawString(line.c_str(), 8, y + 4);
        return;
    }

    std::string line = "[かな] ";
    // composing() は未確定のローマ字も含んでいる。ここで pending_romaji を足すと二重になる。
    const std::string composing = ime_.composing();
    if (!composing.empty()) {
        line += composing;
        if (ime_.mode() == ime::Mode::kSelect) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "  (%d/%d)", ime_.candidate_index() + 1,
                          (int)ime_.candidates().size());
            line += buf;
        }
    }
    gfx_.setTextColor(composing.empty() ? kKeyLine : kComposeC, kStatusBg);
    gfx_.drawString(line.c_str(), 8, y + 4);

    // 候補を横に並べる（選択中のものを強調）。
    if (ime_.mode() == ime::Mode::kSelect) {
        int cx = 8 + gfx_.textWidth(line.c_str()) + 24;
        for (size_t i = 0; i < ime_.candidates().size() && cx < gfx_.width() - 60; ++i) {
            const bool sel = (static_cast<int>(i) == ime_.candidate_index());
            gfx_.setTextColor(sel ? kStatusBg : kText, sel ? kComposeC : kStatusBg);
            const std::string& c = ime_.candidates()[i];
            gfx_.drawString(c.c_str(), cx, y + 4);
            cx += gfx_.textWidth(c.c_str()) + 16;
        }
    }
}

void KeyboardUi::draw()
{
    if (!visible()) return;
    if (mode_ == Mode::kPad) {
        // **PAD は端末を消さない。** 帯を塗らずに重ねるだけ。
        draw_overlay();
        return;
    }
    if (mode_ == Mode::kAscii) {
        draw_ascii();
        draw_status();
        return;
    }
    const auto& l = kb_.layout();
    gfx_.fillRect(0, l.y, gfx_.width(), l.height, kBg);
    // キーボードの左右に余白ができるので、そこは背景色で塗るだけにする。
    for (int row = 0; row < l.rows; ++row) {
        for (int col = 0; col < l.cols; ++col) {
            draw_key(row, col, pressed_key_ == row * l.cols + col);
        }
    }
    draw_status();
}

void KeyboardUi::emit(const std::string& s)
{
    if (!s.empty() && output_) output_(s);
}

bool KeyboardUi::touch_down(int x, int y)
{
    if (!visible()) return false;
    if (mode_ == Mode::kPad) {
        int row = -1, col = -1;
        if (!pad_hit(x, y, &row, &col)) return false;
        press_row_ = row;
        press_idx_ = col;
        draw_pad_key(row, col, true);
        return true;
    }
    if (mode_ == Mode::kAscii) {
        int row = -1, idx = -1;
        if (!ascii_.hit(x, y, &row, &idx)) return false;
        press_row_ = row;
        press_idx_ = idx;
        draw_ascii_key(row, idx, true);
        return true;
    }
    if (!kb_.touch_down(x, y)) return false;
    pressed_key_ = kb_.pressed_key();
    preview_     = ime::Flick::kCenter;
    const auto& l = kb_.layout();
    draw_key(pressed_key_ / l.cols, pressed_key_ % l.cols, true);
    return true;
}

bool KeyboardUi::touch_move(int x, int y)
{
    if (!visible()) return false;
    // ASCII 面と PAD にフリックは無い。指がずれても押したキーのまま（キーボードとして自然）。
    if (mode_ == Mode::kAscii || mode_ == Mode::kPad) return press_row_ >= 0;
    if (!kb_.is_pressed()) return false;
    const ime::Flick f = kb_.current_flick(x, y);
    if (f == preview_) return true;
    preview_ = f;
    const auto& l = kb_.layout();
    draw_key(pressed_key_ / l.cols, pressed_key_ % l.cols, true);
    return true;
}

// PAD の離し。**ラッチは無い** — 面に文字キーが無いので、掛ける先が無い。
// Ctrl が要る唯一のキー (`^C`) は表側で修飾ビットを持っている。
bool KeyboardUi::pad_touch_up()
{
    const int row = press_row_;
    const int col = press_idx_;
    press_row_ = press_idx_ = -1;
    const ime::AsciiKey* k = ime::pad_key(row, col);
    if (!k) return true;

    const Mode before = mode_;
    if (key_output_) key_output_(k->name, k->send_mod);
    // ASCII 面と同じ理由（送った先で段が変わることがある）。
    if (mode_ != before) return true;
    draw_pad_key(row, col, false);
    return true;
}

// ASCII 面の離し。押していたキーを処理する。
bool KeyboardUi::ascii_touch_up()
{
    const int row = press_row_;
    const int idx = press_idx_;
    press_row_ = press_idx_ = -1;
    const ime::AsciiKey* k = ime::AsciiKeyboard::key_at(row, idx);
    if (!k) return true;

    switch (k->mod) {
        case ime::AsciiMod::kShift:
            shift_ = !shift_;
            draw_ascii();  // 面の文字が全部変わる
            draw_status();
            return true;
        case ime::AsciiMod::kCtrl:
            ctrl_ = !ctrl_;
            draw_ascii_key(row, idx, false);
            draw_status();
            return true;
        case ime::AsciiMod::kKana:
            if (mode_request_) mode_request_(Mode::kKana);
            return true;
        case ime::AsciiMod::kNone:
            break;
    }

    const char* name = (shift_ && k->shift) ? k->shift : k->name;
    const uint8_t mod = ctrl_ ? kKbdModCtrl : 0;
    // **ラッチは 1 打で落とす。** 落とさないと Ctrl を押した後の全部が Ctrl 付きになる。
    const bool had_shift = shift_;
    const bool had_latch = shift_ || ctrl_;
    shift_ = ctrl_ = false;

    const Mode before = mode_;
    if (key_output_) key_output_(name, mod);
    // **段が変わったら何も描かない。** 送った先で 1 行入力が確定して元の段へ戻ることが
    // あり（パスワード入力の Enter）、そのまま描くと新しい画面の上に ASCII の面を描く。
    // 新しい面は set_mode がもう描いている。
    if (mode_ != before) return true;

    if (had_shift) draw_ascii();  // 面の文字が戻る
    else draw_ascii_key(row, idx, false);
    if (had_latch) draw_status();
    return true;
}

bool KeyboardUi::touch_up(int x, int y)
{
    if (!visible()) return false;
    if (mode_ == Mode::kPad) {
        if (press_row_ < 0) return false;
        return pad_touch_up();
    }
    if (mode_ == Mode::kAscii) {
        if (press_row_ < 0) return false;
        return ascii_touch_up();
    }
    if (!kb_.is_pressed()) return false;

    const int  released = pressed_key_;
    const auto r        = kb_.touch_up(x, y);
    pressed_key_        = -1;
    preview_            = ime::Flick::kCenter;

    const auto& l = kb_.layout();
    if (released >= 0) draw_key(released / l.cols, released % l.cols, false);

    if (!r.valid) return true;

    const Mode before = mode_;
    if (!r.kana.empty()) {
        // 「゛」「゜」「small」は文字ではなく直前のかなへの修飾。
        if (r.kana == "゛") {
            ime_.modify_last(ime::Ime::Modifier::kDakuten);
        } else if (r.kana == "゜") {
            ime_.modify_last(ime::Ime::Modifier::kHandakuten);
        } else if (r.kana == "small") {
            ime_.modify_last(ime::Ime::Modifier::kSmall);
        } else {
            emit(ime_.input_kana(r.kana));
        }
    } else {
        switch (r.func) {
            case ime::FuncKey::kBackspace:
                // 未確定が無ければ端末に BS を送る。
                if (!ime_.backspace()) emit("\x7f");
                break;
            case ime::FuncKey::kConvert:
                ime_.convert();
                break;
            case ime::FuncKey::kCommit:
                if (ime_.empty()) {
                    emit("\r");  // 何も入力中でなければ Enter
                } else {
                    emit(ime_.commit());
                }
                break;
            case ime::FuncKey::kMode:
                // かな面の "abc" は ASCII 面へ切り替える（IME の直接入力ではなく、
                // 本物の ASCII 配列がある (#65)）。
                if (mode_request_) mode_request_(Mode::kAscii);
                return true;
            case ime::FuncKey::kNone:
                break;
        }
    }
    // ASCII 面と同じ理由（emit の先で段が変わることがある）。
    if (mode_ != before) return true;
    draw_status();
    return true;
}
