#!/usr/bin/env python3
"""Rebuild PNG screen captures from a serial log (CONFIG_CKB_DEBUG_SCREENSHOTS).

Usage: screenshot_decode.py LOG OUT_DIR [--names a,b,c,...]

Reads SHOT_BEGIN / SHOT_DATA / SHOT_END lines (format in
firmware/main/screenshot_stream.h) anywhere in each line, so logger timestamp
prefixes are fine. For each capture id keeps the newest capture that is
complete: contiguous sequence numbers, the declared pixel count, and a
matching FNV-1a hash. Anything else is reported and skipped, never written.
"""
import argparse
import base64
import os
import re
import struct
import sys

from PIL import Image

LINE = re.compile(r"SHOT_(BEGIN|DATA|END) (\d+) (\S+)(?: (\S+))?(?: (\S+))?")


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def finish(cap, end_pixels, end_hash):
    w, h = cap["w"], cap["h"]
    if cap["broken"]:
        return None, cap["broken"]
    pixels = bytearray()
    raw = bytes(cap["raw"])
    if len(raw) % 4:
        return None, "payload is not whole run records"
    for count, px in struct.iter_unpack("<HH", raw):
        if count == 0:
            return None, "zero-length run"
        pixels += struct.pack("<H", px) * count
    n = len(pixels) // 2
    if n != w * h or n != end_pixels:
        return None, f"pixel count {n}, expected {w * h} (end says {end_pixels})"
    if fnv1a(bytes(pixels)) != end_hash:
        return None, "FNV-1a mismatch"
    rgb = bytearray(n * 3)
    for i, (v,) in enumerate(struct.iter_unpack("<H", pixels)):
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        rgb[3 * i:3 * i + 3] = bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6))
    return Image.frombytes("RGB", (w, h), bytes(rgb)), None


def decode(log_path):
    good, open_caps = {}, {}
    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            kind, cid = m.group(1), int(m.group(2))
            if kind == "BEGIN":
                open_caps[cid] = {"w": int(m.group(3)), "h": int(m.group(4)),
                                  "seq": 0, "raw": bytearray(), "broken": None}
            elif cid not in open_caps:
                continue
            elif kind == "DATA":
                cap = open_caps[cid]
                seq = int(m.group(3))
                if seq != cap["seq"] and not cap["broken"]:
                    cap["broken"] = f"sequence gap at {cap['seq']} (got {seq})"
                cap["seq"] = seq + 1
                try:
                    cap["raw"] += base64.b64decode(m.group(4), validate=True)
                except (ValueError, TypeError):
                    cap["broken"] = cap["broken"] or f"bad base64 at {seq}"
            else:
                cap = open_caps.pop(cid)
                img, err = finish(cap, int(m.group(3)), int(m.group(4), 16))
                if img is None:
                    print(f"capture {cid}: skipped ({err})", file=sys.stderr)
                else:
                    good[cid] = img
    return good


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out_dir")
    ap.add_argument("--names", default="", help="comma-separated file names by id")
    args = ap.parse_args()
    names = [n for n in args.names.split(",") if n]
    os.makedirs(args.out_dir, exist_ok=True)
    caps = decode(args.log)
    for cid, img in sorted(caps.items()):
        name = names[cid] if cid < len(names) else f"screen-{cid}"
        path = os.path.join(args.out_dir, f"{name}.png")
        img.save(path, optimize=True)
        print(f"capture {cid}: {img.width}x{img.height} -> {path}")
    return 0 if caps else 1


if __name__ == "__main__":
    sys.exit(main())
