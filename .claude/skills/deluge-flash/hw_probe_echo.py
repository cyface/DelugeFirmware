#!/usr/bin/env python3
"""One-shot: does the connected Deluge answer the new ^echo op (fingerprint of e20fa90f+)?"""

import json
import sys
import time

import rtmidi

HEADER = [0xF0, 0x00, 0x21, 0x7B, 0x01]

out, inp = rtmidi.MidiOut(), rtmidi.MidiIn()
outs = [i for i, p in enumerate(out.get_ports()) if "DELUGE" in p.upper()]
ins = [i for i, p in enumerate(inp.get_ports()) if "DELUGE" in p.upper()]
if not outs or not ins:
    sys.exit("no Deluge MIDI port")
out.open_port(outs[-1])
inp.open_port(ins[-1])
inp.ignore_types(False, True, True)

msg = (
    HEADER
    + [0x04, 0x01]
    + list(json.dumps({"echo": {}}, separators=(",", ":")).encode())
    + [0xF7]
)
out.send_message(msg)
deadline = time.monotonic() + 3.0
while time.monotonic() < deadline:
    r = inp.get_message()
    if not r:
        time.sleep(0.002)
        continue
    data = bytes(r[0])
    if (
        data[:5] == bytes(HEADER)
        and len(data) > 8
        and data[5] == 0x05
        and data[6] == 0x01
    ):
        print("ECHO OK:", data[7:-1].decode("ascii", "replace"))
        sys.exit(0)
print("no ^echo reply (old firmware or device down)")
sys.exit(1)
