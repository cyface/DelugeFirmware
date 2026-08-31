#!/usr/bin/env python3
"""Read SETTINGS/CommunityFeatures.XML off the connected Deluge over smSysex and compare the
devSysexAllowed key with .deluge_hex_key. Prints MATCH/MISMATCH only - never the key itself
(unless --fix is given, in which case the file is silently updated)."""

import json
import re
import sys
import time
from pathlib import Path

import rtmidi

HEADER = [0xF0, 0x00, 0x21, 0x7B, 0x01]


def pack(data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i : i + 7]
        out.append(sum(1 << j for j, b in enumerate(group) if b & 0x80))
        out += bytes(b & 0x7F for b in group)
    return bytes(out)


def unpack(data: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 8):
        group = data[i : i + 8]
        high = group[0]
        for j, b in enumerate(group[1:]):
            out.append(b | (0x80 if high & (1 << j) else 0))
    return bytes(out)


class Deluge:
    def __init__(self):
        self.out, self.inp = rtmidi.MidiOut(), rtmidi.MidiIn()
        outs = [i for i, p in enumerate(self.out.get_ports()) if "DELUGE" in p.upper()]
        ins = [i for i, p in enumerate(self.inp.get_ports()) if "DELUGE" in p.upper()]
        if not outs or not ins:
            sys.exit("no Deluge MIDI port")
        self.out.open_port(outs[-1])
        self.inp.open_port(ins[-1])
        self.inp.ignore_types(False, True, True)
        self.seq = 1

    def request(self, obj, payload=b"", timeout=6.0):
        seq = self.seq
        self.seq = (self.seq + 1) & 0x7F or 1
        message = HEADER + [0x04, seq]
        message += list(json.dumps(obj, separators=(",", ":")).encode())
        if payload:
            message += [0x00] + list(pack(payload))
        message.append(0xF7)
        self.out.send_message(message)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            received = self.inp.get_message()
            if not received:
                time.sleep(0.002)
                continue
            data = bytes(received[0])
            if (
                data[:5] != bytes(HEADER)
                or len(data) < 8
                or data[5] != 0x05
                or data[6] != seq
            ):
                continue
            body = data[7:-1]
            spacer = body.find(b"\0")
            if spacer < 0:
                return json.loads(body.decode("ascii", "replace")), b""
            return json.loads(body[:spacer].decode("ascii", "replace")), unpack(
                body[spacer + 1 :]
            )
        raise TimeoutError(f"no reply to {obj}")


def main():
    d = Deluge()
    reply, _ = d.request(
        {"open": {"path": "SETTINGS/CommunityFeatures.XML", "write": 0}}
    )
    r = reply["^open"]
    if r["err"] != 0 or r["fid"] == 0:
        sys.exit(f"open failed: {reply}")
    fid = r["fid"]
    content = bytearray()
    while True:
        reply, block = d.request(
            {"read": {"fid": fid, "addr": len(content), "size": 512}}
        )
        rr = reply["^read"]
        if rr["err"] != 0 or rr["size"] == 0:
            break
        content += block[: rr["size"]]
        if len(content) >= 65536:
            break
    d.request({"close": {"fid": fid}})
    text = content.decode("ascii", "replace")
    m = re.search(r'devSysexAllowed[^>]*?value="([^"]+)"', text) or re.search(
        r"<devSysexAllowed>([^<]+)</devSysexAllowed>", text
    )
    if not m:
        print("devSysexAllowed not found in XML; first 500 chars follow:")
        print(text[:500])
        sys.exit(1)
    raw = m.group(1).strip()
    # The stored value is the handshake uint32; the menu shows it as 8 hex digits.
    try:
        card_val = int(raw, 0) if raw.lower().startswith("0x") else int(raw)
    except ValueError:
        card_val = int(raw, 16)
    key_file = Path(".deluge_hex_key")
    file_val = int(key_file.read_text().strip(), 16) if key_file.exists() else None
    print(
        f"card devSysexAllowed present: value {'ENABLED' if card_val else 'ZERO/off'}"
    )
    if file_val is None:
        print(".deluge_hex_key missing")
    elif card_val == file_val:
        print("KEY MATCH")
    else:
        print("KEY MISMATCH between card and .deluge_hex_key")
        if "--fix" in sys.argv:
            key_file.write_text(f"{card_val:08x}")
            print("updated .deluge_hex_key from card value")


if __name__ == "__main__":
    main()
