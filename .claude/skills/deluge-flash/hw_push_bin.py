#!/usr/bin/env python3
"""Push build/Release/deluge.bin onto the Deluge's SD card over smSysex, replacing the existing
root .bin, and verify by full byte-for-byte readback. Usage:
    hw_push_bin.py dir              # just list root *.bin
    hw_push_bin.py push <NAME.bin>  # overwrite that file with build/Release/deluge.bin + verify
Uses 512-byte chunks, safe on any firmware vintage."""

import json
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

    def request(self, obj, payload=b"", timeout=6.0, retries=2):
        for attempt in range(retries + 1):
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
            if attempt < retries:
                print(f"  timeout on {next(iter(obj))}, retrying...", flush=True)
        raise TimeoutError(f"no reply to {obj}")


def list_root_bins(d):
    names = []
    offset = 0
    while True:
        reply, _ = d.request({"dir": {"path": "/", "offset": offset, "lines": 20}})
        entries = reply.get("^dir", {}).get("list", [])
        if not entries:
            break
        for e in entries:
            name = e.get("name", "")
            if name.lower().endswith(".bin"):
                names.append((name, e.get("size", -1)))
        offset += len(entries)
        if len(entries) < 20:
            break
    return names


def main():
    d = Deluge()
    if sys.argv[1:] and sys.argv[1] == "dir":
        for name, size in list_root_bins(d):
            print(f"{name}  {size} bytes")
        return
    if len(sys.argv) < 3 or sys.argv[1] != "push":
        sys.exit(__doc__)
    target = sys.argv[2]
    data = Path("build/Release/deluge.bin").read_bytes()
    print(f"pushing {len(data)} bytes to /{target} at 512-byte chunks", flush=True)

    reply, _ = d.request({"open": {"path": target, "write": 1}})
    r = reply["^open"]
    if r["err"] != 0 or r["fid"] == 0:
        sys.exit(f"open for write failed: {reply}")
    fid = r["fid"]
    t0 = time.monotonic()
    addr = 0
    while addr < len(data):
        chunk = data[addr : addr + 512]
        reply, _ = d.request(
            {"write": {"fid": fid, "addr": addr, "size": len(chunk)}}, chunk
        )
        w = reply["^write"]
        if w["err"] != 0 or w["size"] != len(chunk):
            sys.exit(f"short write at {addr}: {reply}")
        addr += len(chunk)
        if addr % (256 * 1024) < 512:
            rate = addr / (time.monotonic() - t0) / 1024
            print(f"  {addr}/{len(data)} ({rate:.0f} KB/s)", flush=True)
    d.request({"close": {"fid": fid}})
    elapsed = time.monotonic() - t0
    print(
        f"write done in {elapsed:.0f}s ({len(data) / elapsed / 1024:.0f} KB/s), verifying...",
        flush=True,
    )

    reply, _ = d.request({"open": {"path": target, "write": 0}})
    r = reply["^open"]
    if r["err"] != 0:
        sys.exit(f"open for read failed: {reply}")
    fid = r["fid"]
    got = bytearray()
    t0 = time.monotonic()
    while len(got) < len(data):
        reply, block = d.request({"read": {"fid": fid, "addr": len(got), "size": 512}})
        rr = reply["^read"]
        if rr["err"] != 0 or rr["size"] == 0:
            sys.exit(f"readback failed at {len(got)}: {reply}")
        got += block[: rr["size"]]
        if len(got) % (256 * 1024) < 512:
            print(f"  verify {len(got)}/{len(data)}", flush=True)
    d.request({"close": {"fid": fid}})
    if bytes(got) == data:
        print(
            "VERIFIED byte-for-byte identical. Power off, then power on holding SHIFT to install."
        )
    else:
        first = next(i for i in range(len(data)) if got[i] != data[i])
        sys.exit(f"MISMATCH at byte {first} - do NOT flash; rerun the push")


if __name__ == "__main__":
    main()
