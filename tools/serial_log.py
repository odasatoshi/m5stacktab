#!/usr/bin/env python3
"""実機をリセットしてシリアル出力を指定秒数だけ拾う。

idf.py monitor は TTY を要求するので、非対話環境（CI やエージェント実行）では使えない。
Reality Check のログ採取はこれを使う。

  python tools/serial_log.py [--port PORT] [--seconds N] [--no-reset]
"""
import argparse
import sys
import time

import serial  # ESP-IDF の python 環境に含まれる


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--no-reset", action="store_true", help="リセットせず現在の出力だけ拾う")
    args = p.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        if not args.no_reset:
            # USB-Serial-JTAG の hard reset は RTS。esptool と同じ手順。
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
            time.sleep(0.05)
            s.reset_input_buffer()
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            data = s.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
