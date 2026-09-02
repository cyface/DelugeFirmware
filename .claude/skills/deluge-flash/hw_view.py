#!/usr/bin/env python3
"""Poll the `view` SysEx query and print what the Deluge says is on its pad grid.

./dbt exec 'python3 .claude/skills/deluge-flash/hw_view.py'          # one reply, pretty
./dbt exec 'python3 .claude/skills/deluge-flash/hw_view.py --raw'    # the JSON as sent
./dbt exec 'python3 .claude/skills/deluge-flash/hw_view.py --watch'  # repaint as it changes
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from deluge_flash import midi, open_pair

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
SEQ = 0x08  # any session/sequence byte; the reply echoes it

TYPE_GLYPH = {
    "synth": "S",
    "kit": "K",
    "midi": "M",
    "cv": "C",
    "audio": "A",
    "drum": "d",  # clip editor: a kit row
    "gate": "g",
    "note": "n",
    "empty": ".",
    "none": " ",
}

# ACTIVE, SOLOED, ARMED, then the two the clip editor adds: has notes, is selected.
FLAGS = ((1, ">"), (2, "!"), (4, "*"), (8, "N"), (16, "@"))


def request(mo, mi, timeout=2.0):
    mo.send_message(HDR + [0x04, SEQ] + list(b'{"view":{}}') + [0xF7])
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        m = mi.get_message()
        if m and m[0][:6] == HDR + [0x05]:
            return bytes(m[0])
        if not m:
            time.sleep(0.005)
    return None


def show(frame: bytes, raw: bool) -> str:
    body = frame[7:-1].decode("ascii")
    if raw:
        return f"{len(frame)} bytes\n{body}"
    view = json.loads(body)["^view"]
    inst = f" inst={view['inst']!r}" if "inst" in view else ""
    head = (
        f"ui={view['ui']} layout={view['layout']} song={view['song']!r}{inst} "
        f"yScroll={view['yScroll']} playing={view['playing']} gen={view['gen']} "
        f"({len(frame)} bytes)"
    )
    lines = [head]
    # A kit's rows are drums and a melodic clip's are notes unless the row says otherwise;
    # the firmware leaves that off to spend the bytes on names.
    default_type = {"kit": "drum", "notes": "note"}.get(view["layout"], "none")
    for r in view["rows"]:
        s = r.get("s", 0)
        flags = "".join(c for bit, c in FLAGS if s & bit) or "-"
        lines.append(
            f"  y{r['y']} {TYPE_GLYPH.get(r.get('t', default_type), '?')} "
            f"{r.get('n', ''):<24.24}"
            f" {r.get('c', ''):<14.14} #{r.get('k', '------')} {flags:<5} x={r.get('x', '-')}"
        )
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--raw", action="store_true", help="print the JSON exactly as received"
    )
    ap.add_argument(
        "--watch", action="store_true", help="poll at 4 Hz, reprint when gen changes"
    )
    ap.add_argument("-p", "--port", type=int, default=None)
    a = ap.parse_args()

    mo, mi, name, _ = open_pair(midi(), a.port)
    print(f"# {name}")

    if not a.watch:
        frame = request(mo, mi)
        if not frame:
            print("no reply - is this firmware built with the view query?")
            return 1
        print(show(frame, a.raw))
        return 0

    last = None
    while True:
        frame = request(mo, mi)
        if frame and frame != last:
            last = frame
            print(show(frame, a.raw), flush=True)
        time.sleep(0.25)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
