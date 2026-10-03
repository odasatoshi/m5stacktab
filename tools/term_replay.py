#!/usr/bin/env python3
"""端末に流れたバイト列を実機の端末にそのまま流し込み、termdump を取る (#100)。

TUI の描画を確かめたいとき、SSH 越しに実物を動かすと毎回同じ画面にならない。
ホストで採ったバイト列 (tmux の pipe-pane など) を `term` コマンドで流せば、
本番の描画経路 (vt100 → TermRenderer → PPA) を同じ入力で何度でも通せる。

  python tools/term_replay.py capture.raw > dump.txt
  python tools/serial_log.py --no-reset --seconds 60 --send "screencap 2" > cap.log

1 行は 256 文字未満に分ける（serial_log.py の check_line を参照）。
空白・バックスラッシュ・引用符・非 ASCII は `\\\\xHH` にする（コンソールが argv を
空白で割り、バックスラッシュを 1 段解釈するため二重にする）。
"""
import argparse
import sys
import time

import serial

from serial_log import MAX_CMDLINE, check_line

SAFE = set(range(0x21, 0x7F)) - {ord("\\"), ord('"'), ord("'")}


def commands(data: bytes, limit: int):
    head = "term "
    cur = ""
    for b in data:
        tok = chr(b) if b in SAFE else f"\\\\x{b:02x}"
        if len(head) + len(cur) + len(tok) >= limit:
            yield head + cur
            cur = ""
        cur += tok
    if cur:
        yield head + cur


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("raw")
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--clear", action="store_true", help="先に画面を消して原点に戻す")
    args = p.parse_args()

    data = open(args.raw, "rb").read()
    if args.clear:
        data = b"\x1bc" + data
    cmds = list(commands(data, MAX_CMDLINE - 16))
    for c in cmds:
        check_line(c)

    with serial.Serial(args.port, 115200, timeout=0.2) as s:
        s.rts = False
        s.dtr = False
        # メニューを閉じると画面キーボードが出て端末の行数が減る。採ったときの寸法
        # (全画面) に合わせるため、キーボードも畳む。
        s.write(b"menu hide\r\n")
        time.sleep(0.5)
        s.write(b"kbd off\r\n")
        time.sleep(0.5)
        s.reset_input_buffer()
        # 1 行ずつ「wrote」を待ってから次を送る。待たずに送るとコンソールの受信が溢れる。
        for i, c in enumerate(cmds):
            s.write((c + "\r\n").encode())
            buf = b""
            deadline = time.time() + 5
            while b"wrote " not in buf and b"busy" not in buf and time.time() < deadline:
                buf += s.read(256)
            if b"wrote " not in buf:
                print(f"no ack for command {i + 1}/{len(cmds)}: {buf[-200:]!r}", file=sys.stderr)
                return 1
        s.write(b"termdump\r\n")
        out = b""
        deadline = time.time() + 5
        while b"--- end" not in out and time.time() < deadline:
            out += s.read(4096)
    sys.stdout.write(out.decode("utf-8", "replace"))
    print(f"sent {len(data)} bytes in {len(cmds)} commands", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
