#!/usr/bin/env python3
"""Pipelined smSysex stress test: N requests in flight at windows 1-4, byte-verified.

    DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/hw_pipeline_test.py [KB]'

Writes a deterministic pattern file serially, then for each window W in 1..4 reads it back
with W concurrent ^read requests (distinct msgIds), then rewrites it with W concurrent
^write requests followed by a serial full readback compare. Reports KB/s, retries, and any
corruption. With the reply backpressure in smsysex.cpp (handleNextSysEx defers a request
until its worst-case reply fits the send ring), windows 3-4 should self-throttle to ~window-2
throughput with zero retries; without it they show retryable whole-reply timeouts. Corruption
at any window is a real bug (see fork issues #42/#43). Uses 512-byte chunks so request frames
stay under the 752-byte macOS cap. Cleans up its test file (CLAUDTST.TMP).
"""

import json
import sys
import time

sys.path.insert(0, ".claude/skills/deluge-flash")
from hw_push_bin import HEADER, Deluge, pack, unpack

CHUNK = 512
TEST_FILE = "CLAUDTST.TMP"
FIRST_TIMEOUT = 2.0
MAX_RETRIES = 4


def pattern(size):
    return bytes((i * 7 + 13) & 0xFF for i in range(size))


class Pipeline:
    """Keeps up to `window` requests in flight, matching replies by msgId."""

    def __init__(self, d, window):
        self.d = d
        self.window = window
        self.inflight = {}  # seq -> (deadline, retries, key, obj, payload)

    def _next_seq(self):
        while True:
            seq = self.d.seq
            self.d.seq = (self.d.seq + 1) & 0x7F or 1
            if seq not in self.inflight:
                return seq

    def send(self, key, obj, payload=b""):
        seq = self._next_seq()
        msg = HEADER + [0x04, seq]
        msg += list(json.dumps(obj, separators=(",", ":")).encode())
        if payload:
            msg += [0x00] + list(pack(payload))
        msg.append(0xF7)
        self.d.out.send_message(msg)
        self.inflight[seq] = (time.monotonic() + FIRST_TIMEOUT, 0, key, obj, payload)

    def _resend(self, seq):
        deadline, retries, key, obj, payload = self.inflight.pop(seq)
        del deadline
        if retries >= MAX_RETRIES:
            sys.exit(f"request {obj} gave up after {MAX_RETRIES} retries")
        new_seq = self._next_seq()
        msg = HEADER + [0x04, new_seq]
        msg += list(json.dumps(obj, separators=(",", ":")).encode())
        if payload:
            msg += [0x00] + list(pack(payload))
        msg.append(0xF7)
        self.d.out.send_message(msg)
        self.inflight[new_seq] = (
            time.monotonic() + FIRST_TIMEOUT * (retries + 2),
            retries + 1,
            key,
            obj,
            payload,
        )
        return 1

    def collect_one(self):
        """Blocks until one in-flight request completes; returns (key, reply, data, retries_done)."""
        retries_done = 0
        while True:
            r = self.d.inp.get_message()
            if r:
                data = bytes(r[0])
                if (
                    data[:5] == bytes(HEADER)
                    and len(data) >= 8
                    and data[5] == 0x05
                    and data[6] in self.inflight
                ):
                    seq = data[6]
                    _, _, key, _, _ = self.inflight.pop(seq)
                    body = data[7:-1]
                    spacer = body.find(b"\0")
                    if spacer < 0:
                        return (
                            key,
                            json.loads(body.decode("ascii", "replace")),
                            b"",
                            retries_done,
                        )
                    return (
                        key,
                        json.loads(body[:spacer].decode("ascii", "replace")),
                        unpack(body[spacer + 1 :]),
                        retries_done,
                    )
                continue
            now = time.monotonic()
            for seq, (deadline, _, _, _, _) in list(self.inflight.items()):
                if now > deadline:
                    retries_done += self._resend(seq)
            time.sleep(0.001)


def open_file(d, write):
    reply, _ = d.request({"open": {"path": TEST_FILE, "write": 1 if write else 0}})
    r = reply["^open"]
    if r["err"] != 0 or r["fid"] == 0:
        sys.exit(f"open failed: {reply}")
    return r["fid"]


def serial_write(d, data):
    fid = open_file(d, write=True)
    for addr in range(0, len(data), CHUNK):
        chunk = data[addr : addr + CHUNK]
        reply, _ = d.request(
            {"write": {"fid": fid, "addr": addr, "size": len(chunk)}}, chunk
        )
        if reply["^write"]["err"] != 0 or reply["^write"]["size"] != len(chunk):
            sys.exit(f"serial write failed at {addr}: {reply}")
    d.request({"close": {"fid": fid}})


def serial_read(d, size):
    fid = open_file(d, write=False)
    got = bytearray()
    while len(got) < size:
        reply, block = d.request(
            {"read": {"fid": fid, "addr": len(got), "size": CHUNK}}
        )
        r = reply["^read"]
        if r["err"] != 0 or r["size"] == 0:
            sys.exit(f"serial read failed at {len(got)}: {reply}")
        got += block[: r["size"]]
    d.request({"close": {"fid": fid}})
    return bytes(got[:size])


def piped_read(d, window, size):
    fid = open_file(d, write=False)
    pipe = Pipeline(d, window)
    got = {}
    retries = 0
    next_addr = 0
    t0 = time.monotonic()
    while len(got) * CHUNK < size:
        while next_addr < size and len(pipe.inflight) < window:
            pipe.send(
                next_addr, {"read": {"fid": fid, "addr": next_addr, "size": CHUNK}}
            )
            next_addr += CHUNK
        key, reply, block, r = pipe.collect_one()
        retries += r
        rr = reply["^read"]
        if rr["err"] != 0:
            sys.exit(f"piped read err at {key}: {reply}")
        got[key] = block[: rr["size"]]
    elapsed = time.monotonic() - t0
    d.request({"close": {"fid": fid}})
    data = b"".join(got[a] for a in sorted(got))[:size]
    return data, elapsed, retries


def piped_write(d, window, data):
    fid = open_file(d, write=True)
    pipe = Pipeline(d, window)
    done = 0
    retries = 0
    next_addr = 0
    t0 = time.monotonic()
    while done < len(data):
        while next_addr < len(data) and len(pipe.inflight) < window:
            chunk = data[next_addr : next_addr + CHUNK]
            pipe.send(
                next_addr,
                {"write": {"fid": fid, "addr": next_addr, "size": len(chunk)}},
                chunk,
            )
            next_addr += CHUNK
        key, reply, _, r = pipe.collect_one()
        retries += r
        w = reply["^write"]
        if w["err"] != 0:
            sys.exit(f"piped write err at {key}: {reply}")
        done += w["size"]
    elapsed = time.monotonic() - t0
    d.request({"close": {"fid": fid}})
    return elapsed, retries


def main():
    size = (int(sys.argv[1]) if sys.argv[1:] else 256) * 1024
    d = Deluge()
    data = pattern(size)
    print(f"seeding {size // 1024} KB pattern file serially...", flush=True)
    serial_write(d, data)
    if serial_read(d, size) != data:
        sys.exit("serial seed does not read back correctly - aborting")

    for window in (1, 2, 3, 4):
        got, elapsed, retries = piped_read(d, window, size)
        ok = "OK " if got == data else "CORRUPT"
        print(
            f"read  w={window}: {size / elapsed / 1024:6.1f} KB/s  retries={retries:2d}  {ok}",
            flush=True,
        )
        if got != data:
            first = next(i for i in range(size) if got[i] != data[i])
            print(f"  first divergence at byte {first}")

    for window in (1, 2, 3, 4):
        elapsed, retries = piped_write(d, window, data)
        back = serial_read(d, size)
        ok = "OK " if back == data else "CORRUPT"
        print(
            f"write w={window}: {size / elapsed / 1024:6.1f} KB/s  retries={retries:2d}  {ok} (full readback compare)",
            flush=True,
        )
        if back != data:
            first = next(i for i in range(size) if back[i] != data[i])
            print(f"  first divergence at byte {first}")

    reply, _ = d.request({"delete": {"path": TEST_FILE}})
    print(f"cleanup: delete {TEST_FILE} -> {reply}")


if __name__ == "__main__":
    main()
