#!/usr/bin/env python3
"""SKK 辞書を実機用のバイナリに変換する。

  python tools/build_dict.py --out build/dict.bin            # SKK-JISYO.L を取得して変換
  python tools/build_dict.py --src SKK-JISYO.M --out d.bin   # 手元の辞書を使う

出力は components/ime/skk_dict.hpp が読む形式。フラッシュに置いて esp_partition_mmap で
そのまま検索できるよう、キーをバイト順にソートしたインデックスを持たせる。
生成物は git に入れない（.gitignore 済み）。
"""
import argparse
import gzip
import struct
import sys
import urllib.request
from pathlib import Path

DEFAULT_URL = "https://raw.githubusercontent.com/skk-dev/dict/master/SKK-JISYO.L"


def fetch(url: str) -> bytes:
    print(f"downloading {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as r:
        data = r.read()
    if url.endswith(".gz"):
        data = gzip.decompress(data)
    return data


def parse(raw: bytes) -> dict[str, str]:
    # SKK 辞書は EUC-JP。壊れた行は捨てる。
    text = raw.decode("euc_jp", errors="replace")
    entries: dict[str, str] = {}
    for line in text.splitlines():
        if not line or line.startswith(";"):
            continue
        try:
            yomi, rest = line.split(" ", 1)
        except ValueError:
            continue
        rest = rest.strip()
        if not rest.startswith("/") or not rest.endswith("/"):
            continue
        cands = [c for c in rest.strip("/").split("/") if c]
        if not yomi or not cands:
            continue
        # 同じよみが送り仮名あり/なしの両方に出ることがあるので結合する。
        if yomi in entries:
            merged = entries[yomi].split("/") + cands
            seen: set[str] = set()
            cands = [c for c in merged if not (c in seen or seen.add(c))]
        entries[yomi] = "/".join(cands)
    return entries


def build(entries: dict[str, str]) -> bytes:
    # キーはバイト順（UTF-8）でソート。実機側は strcmp で二分探索する。
    items = sorted(entries.items(), key=lambda kv: kv[0].encode("utf-8"))
    header_size = 16
    index_size = 4 * len(items)
    strings_off = header_size + index_size

    index = bytearray()
    strings = bytearray()
    for yomi, cands in items:
        index += struct.pack("<I", strings_off + len(strings))
        strings += yomi.encode("utf-8") + b"\0"
        strings += cands.encode("utf-8") + b"\0"

    header = b"SKKD" + struct.pack("<III", 1, len(items), strings_off)
    return bytes(header + index + strings)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", help="ローカルの SKK 辞書ファイル（EUC-JP）")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    raw = Path(args.src).read_bytes() if args.src else fetch(args.url)
    entries = parse(raw)
    blob = build(entries)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    print(f"{len(entries)} entries -> {out} ({len(blob) / 1024 / 1024:.2f} MB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
