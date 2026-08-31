#!/usr/bin/env python3
"""^echo probe including the per-frame USB receive stats (events / terminator CIN)."""

import json
import sys
import time

sys.path.insert(0, ".claude/skills/deluge-flash")
from hw_push_bin import HEADER, Deluge


def main():
    d = Deluge()
    sizes = [int(a) for a in sys.argv[1:]] or [100, 727, 733, 734, 800, 1171]
    for enc in sizes:
        payload = bytes(i & 0x7F for i in range(enc))
        seq = d.seq
        d.seq = (d.seq + 1) & 0x7F or 1
        msg = HEADER + [0x04, seq]
        msg += list(json.dumps({"echo": {}}, separators=(",", ":")).encode())
        msg += [0x00] + list(payload) + [0xF7]
        sent_events = (len(msg) + 2) // 3
        d.out.send_message(msg)
        deadline = time.monotonic() + 4.0
        got = None
        while time.monotonic() < deadline and got is None:
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
                got = json.loads(data[7:-1].decode("ascii", "replace"))["^echo"]
        if got is None:
            print(f"frame {len(msg):4d}B: NO REPLY")
            continue
        short = (len(msg) - 5) - got["frameLen"]
        print(
            f"frame {len(msg):4d}B (host built {sent_events} evts): "
            f"rx evts={got['usbEvts']} lastCIN=0x{got['usbLastCIN']:X} rxLen={got['usbRxLen']} "
            f"short_by={short} div={got['rampDivergesAt']} bails={got['usbBails']}"
        )


if __name__ == "__main__":
    main()
