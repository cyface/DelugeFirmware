#!/usr/bin/env python3
"""Issue #42 hardware bisect on current firmware, over rtmidi (CoreMIDI, no Web MIDI).
1. ^echo fingerprint + ramp payloads bracketing the old 753-byte frame cliff
2. ^write bisect at the failing chunk sizes with full readback verify
3. throughput at 512 vs 1024-byte chunks
Writes go to CLAUDTST.TMP on the card root; deleted afterwards."""

import binascii
import json
import sys
import time

sys.path.insert(0, ".claude/skills/deluge-flash")
from hw_push_bin import HEADER, Deluge

TESTFILE = "CLAUDTST.TMP"


def echo_probe(d, encoded_len):
    payload = bytes(i & 0x7F for i in range(encoded_len))
    seq = d.seq
    d.seq = (d.seq + 1) & 0x7F or 1
    msg = HEADER + [0x04, seq]
    msg += list(json.dumps({"echo": {}}, separators=(",", ":")).encode())
    msg += [0x00] + list(payload)
    msg.append(0xF7)
    frame_len = len(msg)
    sent_crc = binascii.crc32(payload) & 0xFFFFFFFF
    d.out.send_message(msg)
    deadline = time.monotonic() + 4.0
    while time.monotonic() < deadline:
        r = d.inp.get_message()
        if not r:
            time.sleep(0.002)
            continue
        data = bytes(r[0])
        if (
            data[:5] == bytes(HEADER)
            and len(data) > 8
            and data[5] == 0x05
            and data[6] == seq
        ):
            e = json.loads(data[7:-1].decode("ascii", "replace"))["^echo"]
            dev_crc = int(str(e["crc"]).replace("0x", ""), 16)
            ok = "CRC OK " if dev_crc == sent_crc else "CRC BAD"
            # frameLen from device = payload after 5-byte F0+ID header; ours to compare = frame_len - 5
            fl_note = (
                "len OK "
                if e["frameLen"] == frame_len - 5
                else f"len SHORT by {frame_len - 5 - e['frameLen']}"
            )
            enc_note = (
                "enc OK"
                if e["encoded"] == encoded_len
                else f"enc {e['encoded']}/{encoded_len}"
            )
            return f"frame {frame_len:4d}B: {fl_note} {enc_note} {ok} div={e['rampDivergesAt']}"
    return f"frame {frame_len:4d}B: NO REPLY"


def write_test(d, n):
    data = bytes((0x30 + (i % 10)) for i in range(n))
    reply, _ = d.request({"open": {"path": TESTFILE, "write": 1}})
    fid = reply["^open"]["fid"]
    if reply["^open"]["err"] != 0 or fid == 0:
        return f"chunk {n:4d}: open failed {reply}"
    reply, _ = d.request({"write": {"fid": fid, "addr": 0, "size": n}}, data)
    w = reply["^write"]
    d.request({"close": {"fid": fid}})
    if w["err"] != 0 or w["size"] != n:
        return f"chunk {n:4d}: SHORT/ERR err={w['err']} committed {w['size']}/{n}"
    reply, _ = d.request({"open": {"path": TESTFILE, "write": 0}})
    fid = reply["^open"]["fid"]
    got = bytearray()
    while len(got) < n:
        reply, block = d.request(
            {"read": {"fid": fid, "addr": len(got), "size": min(512, n - len(got))}}
        )
        rr = reply["^read"]
        if rr["err"] != 0 or rr["size"] == 0:
            d.request({"close": {"fid": fid}})
            return f"chunk {n:4d}: readback err {rr}"
        got += block[: rr["size"]]
    d.request({"close": {"fid": fid}})
    if bytes(got) != data:
        first = next(i for i in range(n) if got[i] != data[i])
        return f"chunk {n:4d}: committed {n}/{n} but DIVERGES at {first}"
    return f"chunk {n:4d}: ok, committed {n}/{n}, readback identical"


def throughput(d, chunk, total=131072):
    data = bytes(i & 0xFF for i in range(total))
    reply, _ = d.request({"open": {"path": TESTFILE, "write": 1}})
    fid = reply["^open"]["fid"]
    t0 = time.monotonic()
    addr = 0
    while addr < total:
        c = data[addr : addr + chunk]
        reply, _ = d.request({"write": {"fid": fid, "addr": addr, "size": len(c)}}, c)
        w = reply["^write"]
        if w["err"] != 0 or w["size"] != len(c):
            d.request({"close": {"fid": fid}})
            return f"chunk {chunk}: FAILED at {addr}: {w}"
        addr += len(c)
    dt = time.monotonic() - t0
    d.request({"close": {"fid": fid}})
    return (
        f"chunk {chunk:4d}: {total} bytes in {dt:.1f}s = {total / dt / 1024:.0f} KB/s"
    )


def main():
    d = Deluge()
    print(
        "== ^echo fingerprint + cliff bracket (old firmware died at frame 753) ==",
        flush=True,
    )
    for enc in [100, 727, 733, 734, 735, 736, 800, 1000, 1171, 1261]:
        print(" ", echo_probe(d, enc), flush=True)

    print("== ^write bisect (chunks that failed on c1.3.0: 615+) ==", flush=True)
    for n in [512, 613, 614, 615, 616, 640, 768, 848, 1000, 1024]:
        print(" ", write_test(d, n), flush=True)

    print("== throughput, 128 KB ==", flush=True)
    print(" ", throughput(d, 512), flush=True)
    print(" ", throughput(d, 1024), flush=True)

    reply, _ = d.request({"delete": {"path": TESTFILE}})
    print("cleanup:", reply, flush=True)


if __name__ == "__main__":
    main()
