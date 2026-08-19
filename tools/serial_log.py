#!/usr/bin/env python3
"""実機をリセットしてシリアル出力を指定秒数だけ拾う。

idf.py monitor は TTY を要求するので、非対話環境（CI やエージェント実行）では使えない。
Reality Check のログ採取はこれを使う。

  python tools/serial_log.py [--port PORT] [--seconds N] [--no-reset]
  python tools/serial_log.py --no-reset --send wifi-status --seconds 3

1 バイトも受信できなかったときは終了コード 1 を返す（ポート違いや起動失敗を
「採取成功」と誤認しないため）。
"""
import argparse
import codecs
import sys
import time

import serial  # ESP-IDF の python 環境に含まれる


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default="/dev/cu.usbmodem101")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--seconds", type=float, default=20.0)
    p.add_argument("--no-reset", action="store_true", help="リセットせず現在の出力だけ拾う")
    p.add_argument("--send", action="append", default=[],
                   help="送る行（コンソールコマンド用、複数指定可）")
    p.add_argument("--send-delay", type=float, default=0.0,
                   help="送信までの待ち秒数。ポートを開くとリセットが入るので、"
                        "起動を待ってから送りたいときに使う")
    args = p.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        # pyserial は open 時に DTR/RTS を両方 assert する。USB-Serial-JTAG では
        # RTS assert = EN LOW なので、放置するとチップがリセットに保持されて何も出てこない。
        if args.no_reset:
            s.rts = False
            s.dtr = False
        else:
            # esptool の USB-JTAG リセット手順: (DTR,RTS) = (1,1) -> (0,1) -> (0,0)
            s.dtr = False           # EN LOW
            time.sleep(0.1)
            s.reset_input_buffer()  # EN を放す前に捨てる。起動ログを取り逃さない
            s.rts = False           # EN 解放 -> ブート開始

        # チャンク境界でマルチバイト文字が割れるので逐次デコーダを使う。
        decoder = codecs.getincrementaldecoder("utf-8")("replace")
        total = 0
        start = time.time()
        deadline = start + args.seconds
        sent = not args.send
        while time.time() < deadline:
            if not sent and time.time() - start >= args.send_delay:
                for line in args.send:
                    s.write((line + "\r\n").encode())
                s.flush()
                sent = True
            data = s.read(4096)
            if data:
                total += len(data)
                sys.stdout.write(decoder.decode(data))
                sys.stdout.flush()

    if total == 0:
        print(f"no data received from {args.port}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
