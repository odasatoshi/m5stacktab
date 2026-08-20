#!/usr/bin/env python3
"""実機の `screencap` の出力を PNG にする。

実機の写真が撮れない状況（遠隔、CI、エージェント実行）でも、画面が絡む変更の
証跡を残せるようにするためのもの。フレームバッファを読んでいるので、
視差も照明も入らない。

  python tools/serial_log.py --no-reset --seconds 30 --send screencap > cap.log
  python tools/screencap.py cap.log out.png
"""
import re
import struct
import sys
import zlib


def write_png(path, width, height, rows_rgb):
    """PIL を使わずに PNG を書く（依存を増やしたくないので）。"""
    raw = b"".join(b"\x00" + row for row in rows_rgb)  # filter type 0

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    text = open(sys.argv[1], "rb").read().decode("utf-8", "replace")

    m = re.search(r"SCREENCAP (\d+) (\d+) (\d+)", text)
    if not m:
        print("SCREENCAP のヘッダが見つからない", file=sys.stderr)
        return 1
    width, height = int(m.group(1)), int(m.group(2))

    rows, bad = [], 0
    for line in text[m.end():].splitlines():
        line = line.strip()
        if line.startswith("SCREENCAP END"):
            break
        if len(line) != width * 4 or not re.fullmatch(r"[0-9a-f]+", line):
            # ログが混ざった行は捨てる。捨てた数は出す（黙って詰めると絵がずれる）。
            if line:
                bad += 1
            continue
        row = bytearray()
        for i in range(0, len(line), 4):
            v = int(line[i:i + 4], 16)
            # RGB565 -> RGB888。上位ビットを下位に複製して黒白を潰さない。
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))

    if not rows:
        print("画素の行が 1 つも取れなかった", file=sys.stderr)
        return 1
    if len(rows) != height:
        print(f"warning: {height} 行のはずが {len(rows)} 行 (捨てた行 {bad})", file=sys.stderr)
    write_png(sys.argv[2], width, len(rows), rows)
    print(f"wrote {sys.argv[2]} ({width}x{len(rows)}, 捨てた行 {bad})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
