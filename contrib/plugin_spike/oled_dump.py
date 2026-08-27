#!/usr/bin/env python3
"""Grab the Deluge OLED framebuffer over sysex (HID command 2 / sub 0) and write it as a PNG.
    DBT_NO_SYNC=1 ./dbt exec 'python3 contrib/plugin_spike/oled_dump.py out.png'
Reply: F0 00 21 7B 01 02 40 <rle> 00 <7-bit packed 768 bytes> F7 ; 6 pages x 128 columns, bit0 = top row of page."""

import struct
import sys
import time
import zlib

import rtmidi

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
W, H, PAGES = 128, 48, 6


def unpack_7_to_8(src: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(src), 8):
        hi = src[i]
        for j, b in enumerate(src[i + 1 : i + 8]):
            out.append(b | (0x80 if hi & (1 << j) else 0))
    return bytes(out)


def png(path, pixels, w, h, scale=4):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            row += bytes([255 if pixels[y][x] else 0]) * scale
        rows += [bytes(row)] * scale
    raw = b"".join(rows)

    def chunk(t, d):
        return (
            struct.pack(">I", len(d))
            + t
            + d
            + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        )

    with open(path, "wb") as f:
        f.write(
            b"\x89PNG\r\n\x1a\n"
            + chunk(
                b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0)
            )
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b"")
        )


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "oled.png"
    mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
    o = [i for i, p in enumerate(mo.get_ports()) if "DELUGE" in p.upper()][-1]
    i_ = [i for i, p in enumerate(mi.get_ports()) if "DELUGE" in p.upper()][-1]
    mo.open_port(o)
    mi.open_port(i_)
    mi.ignore_types(False, True, True)
    mo.send_message(HDR + [0x02, 0x00, 0x00, 0xF7])
    end = time.monotonic() + 3
    while time.monotonic() < end:
        m = mi.get_message()
        if not m:
            time.sleep(0.005)
            continue
        data, _ = m
        if data[:7] == HDR + [0x02, 0x40]:
            img = unpack_7_to_8(bytes(data[9:-1]))[: W * PAGES]
            pixels = [
                [(img[page * W + x] >> (y - page * 8)) & 1 for x in range(W)]
                for page in range(PAGES)
                for y in range(page * 8, page * 8 + 8)
            ]
            png(out, pixels, W, H)
            # ASCII preview too
            for y in range(H):
                print("".join("#" if pixels[y][x] else "." for x in range(W)))
            print("wrote", out)
            return
    sys.exit("no OLED reply")


if __name__ == "__main__":
    main()
