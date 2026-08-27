#!/usr/bin/env python3
"""Attach to the Deluge sysex debug console and print what the firmware sends, for N seconds.

Run inside the DBT env:  DBT_NO_SYNC=1 ./dbt exec 'python3 contrib/plugin_spike/console.py 20'
sysexDebugPrint() sends plain ASCII (each byte & 0x7F) after the 8-byte header - no 7-bit packing.
"""

import sys
import time

import rtmidi

HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x40, 0x00]
ENABLE = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x01, 0xF7]
DISABLE = [0xF0, 0x00, 0x21, 0x7B, 0x01, 0x03, 0x00, 0x00, 0xF7]


def deluge_port(m):
    ports = [i for i, p in enumerate(m.get_ports()) if "DELUGE" in p.upper()]
    if not ports:
        sys.exit("no Deluge MIDI port")
    return ports[-1]


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    mo, mi = rtmidi.MidiOut(), rtmidi.MidiIn()
    mo.open_port(deluge_port(mo))
    mi.open_port(deluge_port(mi))
    mi.ignore_types(False, True, True)
    mo.send_message(ENABLE)
    end = time.monotonic() + seconds
    partial = ""
    while time.monotonic() < end:
        m = mi.get_message()
        if not m:
            time.sleep(0.005)
            continue
        msg, _ = m
        if msg[:8] == HDR:
            partial += bytes(msg[8:-1]).decode("ascii", errors="replace")
            while "\n" in partial:
                line, partial = partial.split("\n", 1)
                print(line, flush=True)
    if partial:
        print(partial, flush=True)
    mo.send_message(DISABLE)
    mo.close_port()
    mi.close_port()


if __name__ == "__main__":
    main()
