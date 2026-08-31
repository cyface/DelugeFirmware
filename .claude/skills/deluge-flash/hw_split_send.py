#!/usr/bin/env python3
"""Does splitting a large sysex across two CoreMIDI send calls dodge the offset-750 byte drop?"""

import json
import sys
import time

sys.path.insert(0, ".claude/skills/deluge-flash")
from hw_push_bin import HEADER, Deluge


def probe(d, enc, split=None):
    payload = bytes(i & 0x7F for i in range(enc))
    seq = d.seq
    d.seq = (d.seq + 1) & 0x7F or 1
    msg = HEADER + [0x04, seq]
    msg += list(json.dumps({"echo": {}}, separators=(",", ":")).encode())
    msg += [0x00] + list(payload) + [0xF7]
    try:
        if split is None:
            d.out.send_message(msg)
        else:
            d.out.send_message(msg[:split])
            d.out.send_message(msg[split:])
    except (ValueError, RuntimeError, OSError) as e:
        return f"frame {len(msg):4d}B split={split}: SEND REFUSED ({e})"
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
            short = (len(msg) - 5) - e["frameLen"]
            return (
                f"frame {len(msg):4d}B split={split}: short_by={short} div={e['rampDivergesAt']} "
                f"evts={e['usbEvts']} lastCIN=0x{e['usbLastCIN']:X}"
            )
    return f"frame {len(msg):4d}B split={split}: NO REPLY"


def main():
    d = Deluge()
    print(probe(d, 800, split=None), flush=True)
    for split in [600, 375, 749]:
        print(probe(d, 800, split=split), flush=True)
        time.sleep(0.1)


if __name__ == "__main__":
    main()
