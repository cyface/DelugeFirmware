#!/usr/bin/env python3
# Copyright © 2026 Synthstrom Audible Ltd
#
# This file is part of The Synthstrom Audible Deluge Firmware.
#
# The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program. If not,
# see <https://www.gnu.org/licenses/>.
"""Copy plugin blobs onto a connected Deluge's SD card over USB MIDI - no card removal.

    DBT_NO_SYNC=1 ./dbt exec 'python3 src/deluge/plugin/tools/push_dlp.py build/plugins/*.dlp'

Uses the firmware's JSON sysex file protocol (storage/smsysex.cpp), which creates missing
directories, so the files land in PLUGINS/ ready for the next boot's scan (plugin_loader.h). Each
file is read back afterwards and compared byte for byte: a blob that arrives corrupted would fail
its CRC at boot and be skipped, which is a confusing way to find out about a dropped packet.

Framing: F0 00 21 7B 01 04 <seq> <json> [00 <7-bit packed data>] F7, reply command 05 with the same
seq. 7-bit packing is groups of 7 bytes behind a byte of their high bits.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import rtmidi

HEADER = [0xF0, 0x00, 0x21, 0x7B, 0x01]
JSON_COMMAND, JSON_REPLY = 0x04, 0x05
# The firmware assembles an incoming sysex into MIDICable::incomingSysexBuffer, which since the
# fix/smsysex-bigger-frames branch is 1280 bytes - enough for a full 1024-byte block: header +
# JSON + the block 7-bit packed (8 bytes per 7) + F7 is a ~1220-byte frame. Firmware older than
# that silently drops frames over 1024 bytes (this shows up as a reply timeout, not an error);
# drop BLOCK back to 512 if pushing to one.
BLOCK = 1024


def pack(data: bytes) -> bytes:
    """8-bit bytes as the protocol's 7-bit groups: a byte of high bits, then up to 7 stripped bytes."""
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
    def __init__(self, port_hint):
        self.out, self.inp = rtmidi.MidiOut(), rtmidi.MidiIn()
        hint = (port_hint or "DELUGE").upper()
        outs = [i for i, p in enumerate(self.out.get_ports()) if hint in p.upper()]
        ins = [i for i, p in enumerate(self.inp.get_ports()) if hint in p.upper()]
        if not outs or not ins:
            sys.exit(f"push_dlp: no MIDI port matching {hint!r}")
        # The last Deluge port is the sysex one, same choice `dbt loadfw` makes.
        self.out.open_port(outs[-1])
        self.inp.open_port(ins[-1])
        self.inp.ignore_types(False, True, True)
        self.port = self.out.get_ports()[outs[-1]]
        self.seq = 1

    def request(self, obj, payload=b"", timeout=6.0):
        """One command, one reply: the JSON object, plus any binary block that came back with it."""
        seq = self.seq
        self.seq = (self.seq + 1) & 0x7F or 1
        message = HEADER + [JSON_COMMAND, seq]
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
                or data[5] != JSON_REPLY
                or data[6] != seq
            ):
                continue
            body = data[7:-1]
            spacer = body.find(b"\0")
            if spacer < 0:
                return json.loads(body.decode("ascii", errors="replace")), b""
            return json.loads(body[:spacer].decode("ascii", errors="replace")), unpack(
                body[spacer + 1 :]
            )
        raise TimeoutError(f"no reply to {obj}")

    def open(self, path, write):
        reply, _ = self.request({"open": {"path": path, "write": 1 if write else 0}})
        fid, error = reply["^open"]["fid"], reply["^open"]["err"]
        if error != 0 or fid == 0:
            sys.exit(f"push_dlp: cannot open {path} on the card: {reply}")
        return fid

    def close(self, fid):
        reply, _ = self.request({"close": {"fid": fid}})
        if reply["^close"]["err"] != 0:
            sys.exit(f"push_dlp: close failed: {reply}")


def send(deluge, source: Path, destination: str):
    data = source.read_bytes()
    fid = deluge.open(destination, write=True)
    address = 0
    while address < len(data):
        chunk = data[address : address + BLOCK]
        reply, _ = deluge.request(
            {"write": {"fid": fid, "addr": address, "size": len(chunk)}}, chunk
        )
        written = reply["^write"]
        if written["err"] != 0 or written["size"] != len(chunk):
            sys.exit(f"push_dlp: write failed at {address}: {reply}")
        address += len(chunk)
    deluge.close(fid)
    return data


def verify(deluge, destination: str, expected: bytes):
    """Read the file back off the card. A blob damaged in transit only shows up at boot, as a CRC failure."""
    fid = deluge.open(destination, write=False)
    got = bytearray()
    while len(got) < len(expected):
        reply, block = deluge.request(
            {"read": {"fid": fid, "addr": len(got), "size": BLOCK}}
        )
        read = reply["^read"]
        if read["err"] != 0 or read["size"] == 0:
            sys.exit(f"push_dlp: read back failed at {len(got)}: {reply}")
        got += block[: read["size"]]
    deluge.close(fid)
    if bytes(got[: len(expected)]) != expected:
        sys.exit(f"push_dlp: {destination} does not read back as it was sent")


def main():
    parser = argparse.ArgumentParser(
        description="Copy plugin blobs onto a connected Deluge's card."
    )
    parser.add_argument("files", nargs="+", help="local .dlp files")
    parser.add_argument(
        "--dir", default="PLUGINS", help="directory on the card (default: PLUGINS)"
    )
    parser.add_argument(
        "--port", help="substring of the MIDI port name (default: Deluge)"
    )
    parser.add_argument(
        "--no-verify", action="store_true", help="skip reading the files back"
    )
    arguments = parser.parse_args()

    deluge = Deluge(arguments.port)
    print(f"     {deluge.port}")
    for name in arguments.files:
        source = Path(name)
        destination = f"{arguments.dir}/{source.name}"
        data = send(deluge, source, destination)
        if not arguments.no_verify:
            verify(deluge, destination, data)
        print(
            f"OK   {destination}: {len(data)} bytes{'' if arguments.no_verify else ', reads back identical'}"
        )
    print(
        "NOTE plugins are scanned at boot only - power-cycle the Deluge for these to take effect."
    )


if __name__ == "__main__":
    main()
