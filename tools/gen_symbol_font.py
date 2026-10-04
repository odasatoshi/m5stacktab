#!/usr/bin/env python3
"""efontJA_24 に無い記号を 12x24 のビットマップに焼く (#102)。

出力は main/symbols.bin (EMBED_FILES で埋め込む)。描くのは main/term_glyphs.cpp。

  python3 -m venv /tmp/v && /tmp/v/bin/pip install fonttools pillow
  /tmp/v/bin/python tools/gen_symbol_font.py <フォントを置いたディレクトリ> main/symbols.bin \
      managed_components/m5stack__m5gfx/src/lgfx/Fonts/efont/lgfx_efont_ja.c

efont に**無い**字だけを焼く（有る字は efont で描く。混ぜると字の太さが揃わない）。
例外は OVERRIDE の幾何学図形。どの字を記号フォントで描くかはここだけで決め、
描画側は「symbols.bin に有れば使う」だけにしてある。

フォントは Noto (SIL Open Font License 1.1, ライセンス文は main/symbols-OFL.txt)。
焼いた結果を再現できるように、取得元と SHA-256 を下に固定してある。違うファイルを
渡すと止まる（上流が更新されると字形が変わり、差分の理由が分からなくなるため）。

形式 (リトルエンディアン):
  "SYM1" u32 件数
  件数 x {u32 コードポイント, u32 データの位置, u8 x, u8 y, u8 w, u8 h}  (コードポイントの昇順)
  データ: 各グリフの外接矩形 (セル内の x, y, w, h) を 1 画素 4 bit (0-15 = 濃さ) で
          行ごとに詰める。1 行は (w + 1) / 2 バイト、上位 4 bit が左の画素。
  w = 0 は「何も描かない字」(各種の空白)。
"""
import hashlib
import struct
import sys
import unicodedata

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

BASE = "https://github.com/google/fonts/raw/main/ofl/"
# 探す順。等幅の Mono を先にする（セルに合わせた字形なので、文字と並べたときに揃う）。
FONTS = [
    ("NotoSansMono.ttf", "notosansmono/NotoSansMono%5Bwdth,wght%5D.ttf",
     "2cb2adb378a8f574213e23df697050b83c54c27df465a2015552740b2769a081"),
    ("NotoSansSymbols.ttf", "notosanssymbols/NotoSansSymbols%5Bwght%5D.ttf",
     "f7e7e04b4a24b6c78893d50cbfd2b2f6cae49617ab047bfef668d252adb128f7"),
    ("NotoSansSymbols2.ttf", "notosanssymbols2/NotoSansSymbols2-Regular.ttf",
     "7d5fb73b7ca67a6798101741f5d280a3d016a56a197afcd4199dbb57b4b82a21"),
    ("NotoSansMath.ttf", "notosansmath/NotoSansMath-Regular.ttf",
     "3f495fe933c06786e4d5f6d86b8ee70b6753a68ee3b9d87528726de0f6e2c47d"),
]

CELL_W, CELL_H = 12, 24
SIZE = 20       # Noto Sans Mono の大文字が 14px になる = efont の大文字と同じ高さ
BASELINE = 19   # efont の大文字の下端 (見た目のベースライン)

# 焼く範囲。罫線・ブロック (U+2500-259F) は自前で作図するので入れない。
RANGES = [
    (0x00A0, 0x00FF),  # ラテン 1 補助 (efont は NBSP など 3 字が無い。Claude Code が NBSP を出す)
    (0x2000, 0x206F),  # 一般句読点
    (0x2100, 0x214F),  # 文字様記号
    (0x2190, 0x21FF),  # 矢印
    (0x2200, 0x22FF),  # 数学記号
    (0x2300, 0x23FF),  # その他の技術用記号 (⏺ ⎿ ⏵ ⌘ ⌥)
    (0x2460, 0x24FF),  # 囲み英数字
    (0x25A0, 0x25FF),  # 幾何学図形 (● ○ ▶)
    (0x2600, 0x26FF),  # その他の記号 (⚠)
    (0x2700, 0x27BF),  # 装飾記号 (✓ ✗ ✻ ❯)
    (0x27C0, 0x27FF),  # 数学記号 A・補助矢印 A
    (0x2900, 0x297F),  # 補助矢印 B
    (0x2B00, 0x2BFF),  # その他の記号と矢印 (⬝)
]
# efont に有っても記号フォントで描く字。efont の幾何学図形は全角の字形を半角に
# 押し込んだ縦長で、● が楕円に見える（herdr の状態表示）。
OVERRIDE = range(0x25A0, 0x2600)


def load_fonts(font_dir):
    fonts = []
    for name, path, sha in FONTS:
        data = open(f"{font_dir}/{name}", "rb").read()
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            sys.exit(f"{name}: SHA-256 が違う ({got})。{BASE}{path} から取り直すこと")
        fonts.append((set(TTFont(f"{font_dir}/{name}").getBestCmap()), f"{font_dir}/{name}"))
    return fonts


def efont_codepoints(path):
    """efontJA_24 (u8g2 形式) が持っている字。M5GFX のソース (C の文字列リテラル) から読む。"""
    import ast
    import re
    src = open(path, encoding="latin-1").read()
    pos = src.index("=", src.index("lgfx_efont_ja_24[")) + 1
    lit = re.compile(r'\s*"((?:[^"\\]|\\.)*)"', re.S)
    d = b""
    while (m := lit.match(src, pos)):  # 連結されたリテラルを ; まで読む
        d += ast.literal_eval('b"' + m.group(1) + '"')
        pos = m.end()
    d += b"\0"  # C の文字列リテラルの末尾の NUL。u8g2 はこれを並びの終端として読む
    w = lambda o: d[o] << 8 | d[o + 1]
    have, o = set(), 23
    while d[o + 1] != 0:  # ASCII 部分: [符号, 次までの長さ, ...]
        have.add(d[o])
        o += d[o + 1]
    p = 23 + w(21)        # Unicode 部分の先頭の表の、最初の飛び先がグリフの並びの先頭
    p += w(p)
    while (e := w(p)) != 0:
        have.add(e)
        p += d[p + 2]
    return have


def render(ch, font_path):
    """セルに収まる大きさ・位置で描いて、外接矩形と 4 bit の濃さを返す。"""
    size = SIZE
    while True:
        font = ImageFont.truetype(font_path, size)
        l, t, r, b = font.getbbox(ch, anchor="ls")  # ベースラインからの相対
        if (r - l <= CELL_W and b - t <= CELL_H) or size <= 8:
            break
        size -= 1
    # 横: Mono の字はセル幅 (送り 12px) で設計されているので原点を 0 に置く。
    # はみ出す字 (記号フォント) は外接矩形を中央に置く。
    x = 0 if (l >= 0 and r <= CELL_W) else (CELL_W - (r - l)) // 2 - l
    # 縦: ベースラインを efont に合わせ、はみ出すときだけ内側へずらす。
    y = BASELINE
    if y + t < 0:
        y = -t
    if y + b > CELL_H:
        y = CELL_H - b
    img = Image.new("L", (CELL_W, CELL_H), 0)
    ImageDraw.Draw(img).text((x, y), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    cells = [(cx, cy, px[cx, cy] * 15 // 255) for cy in range(CELL_H) for cx in range(CELL_W)]
    lit = [(cx, cy) for cx, cy, a in cells if a]
    if not lit:
        return 0, 0, 0, 0, b""
    x0 = min(c[0] for c in lit); x1 = max(c[0] for c in lit)
    y0 = min(c[1] for c in lit); y1 = max(c[1] for c in lit)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    data = bytearray()
    for cy in range(y0, y1 + 1):
        row = [px[cx, cy] * 15 // 255 for cx in range(x0, x1 + 1)] + [0]
        for i in range(0, w, 2):
            data.append(row[i] << 4 | row[i + 1])
    return x0, y0, w, h, bytes(data)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: gen_symbol_font.py <フォントのディレクトリ> <出力> <lgfx_efont_ja.c>")
    font_dir, out = sys.argv[1], sys.argv[2]
    efont = efont_codepoints(sys.argv[3])
    fonts = load_fonts(font_dir)
    cps = []
    for lo, hi in RANGES:
        for cp in range(lo, hi + 1):
            ch = chr(cp)
            if unicodedata.category(ch) in ("Cn", "Cf", "Mn", "Me"):
                continue  # 未定義・書式文字・結合文字 (vt100 は幅 0 で捨てる)
            if unicodedata.east_asian_width(ch) in ("W", "F"):
                continue  # 2 セル幅の字 (絵文字など) は対象外
            if cp in efont and cp not in OVERRIDE:
                continue
            src = next((p for cmap, p in fonts if cp in cmap), None)
            if src:
                cps.append((cp, src))
    index, blob = [], bytearray()
    for cp, src in cps:
        x, y, w, h, data = render(chr(cp), src)
        index.append(struct.pack("<IIBBBB", cp, len(blob), x, y, w, h))
        blob += data
    with open(out, "wb") as f:
        f.write(b"SYM1" + struct.pack("<I", len(index)) + b"".join(index) + blob)
    print(f"{out}: {len(index)} glyphs, {len(blob)} bytes of pixels, "
          f"{8 + 12 * len(index) + len(blob)} bytes total")


if __name__ == "__main__":
    main()
