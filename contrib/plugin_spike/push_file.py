#!/usr/bin/env python3
"""Write a local file onto the Deluge's SD card over USB MIDI using the firmware's JSON sysex
file protocol (storage/smsysex.cpp). Missing directories in the path are created by the device.

    DBT_NO_SYNC=1 ./dbt exec 'python3 contrib/plugin_spike/push_file.py contrib/plugin_spike/spike.dlp PLUGINS/spike.dlp'

Framing: F0 00 21 7B 01 04 <seq> <json> [00 <7-bit packed data>] F7 ; reply command 05 with the same seq.
Blocks are at most 1024 bytes (blockBufferMax). Packing: groups of 7 bytes prefixed by a high-bits byte.
"""

import json
import sys
import time

import rtmidi

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
JSON_CMD, JSON_REPLY = 0x04, 0x05
BLOCK = 1024


def pack_8_to_7(src: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(src), 7):
        chunk = src[i : i + 7]
        hi = 0
        body = bytearray()
        for j, b in enumerate(chunk):
            if b & 0x80:
                hi |= 1 << j
            body.append(b & 0x7F)
        out.append(hi)
        out += body
    return bytes(out)


class Deluge:
    def __init__(self):
        self.mo, self.mi = rtmidi.MidiOut(), rtmidi.MidiIn()
        outs = [i for i, p in enumerate(self.mo.get_ports()) if "DELUGE" in p.upper()]
        ins = [i for i, p in enumerate(self.mi.get_ports()) if "DELUGE" in p.upper()]
        if not outs or not ins:
            sys.exit("no Deluge MIDI port")
        self.mo.open_port(outs[-1])
        self.mi.open_port(ins[-1])
        self.mi.ignore_types(False, True, True)
        self.seq = 1

    def request(self, obj, payload: bytes = b"", timeout=5.0):
        seq = self.seq
        self.seq = (self.seq + 1) & 0x7F or 1
        msg = (
            HDR
            + [JSON_CMD, seq]
            + list(json.dumps(obj, separators=(",", ":")).encode())
        )
        if payload:
            msg += [0x00] + list(pack_8_to_7(payload))
        msg.append(0xF7)
        self.mo.send_message(msg)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            m = self.mi.get_message()
            if not m:
                time.sleep(0.002)
                continue
            data, _ = m
            if data[:5] == HDR and data[5] == JSON_REPLY and data[6] == seq:
                text = bytes(data[7:-1]).decode("ascii", errors="replace")
                return json.loads(text)
        raise TimeoutError(f"no reply to {obj}")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as f:
        data = f.read()
    d = Deluge()
    r = d.request({"open": {"path": dst, "write": 1}})
    fid, err = r["^open"]["fid"], r["^open"]["err"]
    if err != 0 or fid == 0:
        sys.exit(f"open failed: {r}")
    addr = 0
    while addr < len(data):
        chunk = data[addr : addr + BLOCK]
        r = d.request({"write": {"fid": fid, "addr": addr, "size": len(chunk)}}, chunk)
        w = r["^write"]
        if w["err"] != 0 or w["size"] != len(chunk):
            sys.exit(f"write failed at {addr}: {r}")
        addr += len(chunk)
    r = d.request({"close": {"fid": fid}})
    if r["^close"]["err"] != 0:
        sys.exit(f"close failed: {r}")
    print(f"wrote {len(data)} bytes to {dst}")


if __name__ == "__main__":
    main()
