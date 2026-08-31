#!/usr/bin/env python3
"""Emulator test suite for upstream PR #4621 (sustain/damper pedal, MIDI CC 64).

Boots the PR-4621 firmware in DelugEmu with a private SD whose MIDIFollow.XML
routes follow-channel A to MIDI channel 1, so external UDP MIDI notes play the
boot song's synth clip. Firmware state is asserted through the QEMU gdb stub:
`playbackHandler.numHeldSustainNotes`, the per-channel `sustainPedalDown` flags
on the DIN cable, and live voice counts.

Usage: python3 run_suite.py <elf> <sd_dir> <repo_root> [scenario ...]
"""

import sys
import time

from emu_lib import Emu

RESULTS = []
LAUNCH_TIME = 0.0


def report(name, ok, detail=""):
    RESULTS.append((name, ok, detail))
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}", flush=True)


def poll(fn, timeout=8.0, interval=0.4):
    """Poll fn() until truthy or timeout; returns last value."""
    end = time.time() + timeout
    v = fn()
    while not v and time.time() < end:
        time.sleep(interval)
        v = fn()
    return v


class Suite:
    def __init__(self, emu):
        self.emu = emu
        self.saved_song = False

    def settle(self, seconds=0.6):
        time.sleep(seconds)

    def wait_voices(self, pred, timeout=8.0):
        return poll(lambda: pred(self.emu.probe()["voices"]) or None, timeout=timeout)

    def sound_note(self, note, ch=0):
        """note-on, retried until the note is verifiably sounding (the
        emulator's UDP-UART input occasionally drops a message)."""
        for _ in range(3):
            self.emu.note_on(note, ch=ch)
            if poll(lambda: note in self.emu.probe()["notes"] or None, timeout=4):
                return True
        return False

    def wait_held(self, n, timeout=6.0):
        return poll(lambda: (self.emu.probe()["held"] == n) or None, timeout=timeout)

    def all_off(self):
        """Release everything and confirm silence before the next scenario."""
        e = self.emu
        e.pedal(False)
        e.cc(123, 0)
        self.settle()
        ok = self.wait_voices(lambda v: v == 0, timeout=10)
        p = e.probe()
        if not ok or p["held"] != 0:
            raise RuntimeError(f"could not reach silence between scenarios: {p}")

    # ---- scenarios ----

    def s01_sanity_boot(self):
        p = self.emu.probe()
        ok = p["followA"] == 0 and p["held"] == 0 and p["voices"] == 0
        report("01 boot state (follow A on ch1, nothing held)", ok, str(p))
        return ok

    def s02_routing(self):
        e = self.emu
        e.note_on(60)
        sounded = self.wait_voices(lambda v: v >= 1, timeout=6)
        e.note_off(60)
        silent = self.wait_voices(lambda v: v == 0, timeout=10)
        p = e.probe()
        ok = bool(sounded and silent and p["held"] == 0)
        report(
            "02 baseline routing, pedal up: note-off releases",
            ok,
            f"sounded={bool(sounded)} silent={bool(silent)} held={p['held']}",
        )
        return ok

    def s03_basic_sustain(self):
        e = self.emu
        e.pedal(True)
        self.sound_note(60)
        e.note_off(60)
        self.settle(2.0)
        p1 = e.probe()
        still = p1["voices"] >= 1 and p1["held"] == 1 and p1["pedal"][0] == 1
        e.pedal(False)
        released = self.wait_voices(lambda v: v == 0, timeout=10)
        p2 = e.probe()
        ok = still and bool(released) and p2["held"] == 0 and p2["pedal"][0] == 0
        report(
            "03 basic sustain: off deferred, release lets go",
            ok,
            f"during={p1} after={p2}",
        )

    def s04_threshold(self):
        e = self.emu
        e.pedal(True, value=63)  # below threshold: NOT down
        e.note_on(60)
        self.wait_voices(lambda v: v >= 1)
        e.note_off(60)
        self.settle()
        p1 = e.probe()
        not_held = p1["held"] == 0 and p1["pedal"][0] == 0
        self.wait_voices(lambda v: v == 0)
        e.pedal(True, value=64)  # exactly threshold: down
        self.sound_note(60)
        e.note_off(60)
        self.settle()
        p2 = e.probe()
        held = p2["held"] == 1 and p2["pedal"][0] == 1
        e.pedal(False, value=63)  # 63 releases
        self.settle()
        p3 = e.probe()
        ok = not_held and held and p3["held"] == 0
        report(
            "04 threshold: 63=up, 64=down, 63 releases",
            ok,
            f"v63held={p1['held']} v64held={p2['held']} after={p3['held']}",
        )

    def s05_retrigger(self):
        e = self.emu
        e.pedal(True)
        self.sound_note(60)
        e.note_off(60)
        h1 = 1 if self.wait_held(1) else e.probe()["held"]
        # retrigger must forget the deferred off (retry a dropped note-on,
        # detected via the held count instead of the voice, which never stopped)
        e.note_on(60)
        if not self.wait_held(0, timeout=4):
            e.note_on(60)
            self.wait_held(0, timeout=4)
        h2 = e.probe()["held"]
        e.note_off(60)
        h3 = 1 if self.wait_held(1) else e.probe()["held"]
        e.pedal(False)
        self.wait_voices(lambda v: v == 0)
        ok = h1 == 1 and h2 == 0 and h3 == 1
        report("05 retrigger cancels deferred off", ok, f"h1={h1} h2={h2} h3={h3}")

    def s06_chord(self):
        e = self.emu
        e.pedal(True)
        for n in (60, 64, 67):
            self.sound_note(n)
        for n in (60, 64, 67):
            e.note_off(n)
        self.settle(1.5)
        p1 = e.probe()
        held = p1["held"] == 3 and p1["voices"] >= 3
        e.pedal(False)
        released = self.wait_voices(lambda v: v == 0, timeout=10)
        ok = held and bool(released)
        report("06 chord: 3 offs deferred, all release together", ok, f"during={p1}")

    def s07_channel_isolation(self):
        e = self.emu
        e.pedal(True, ch=0)
        e.note_on(
            60, ch=1
        )  # channel 2: not the follow channel, and pedal is not down there
        e.note_off(60, ch=1)
        self.settle()
        p = e.probe()
        ok = p["held"] == 0 and p["pedal"][0] == 1 and p["pedal"][1] == 0
        e.pedal(False, ch=0)
        report("07 channel isolation: ch1 pedal doesn't hold ch2 offs", ok, str(p))

    def s08_overflow(self):
        e = self.emu
        e.pedal(True)
        for n in range(36, 36 + 65):  # 65 distinct notes
            e.note_on(n, vel=1)
        for n in range(36, 36 + 65):
            e.note_off(n)
        self.settle(1.0)
        p = e.probe()
        # Store is 64 deep: 64 deferred, the 65th off passed straight through.
        ok = p["held"] == 64
        e.pedal(False)
        self.settle(1.0)
        p2 = e.probe()
        ok = ok and p2["held"] == 0
        self.wait_voices(lambda v: v == 0, timeout=12)
        report(
            "08 overflow fail-safe: 65th off passes through, all clear after",
            ok,
            f"held={p['held']} after={p2['held']}",
        )

    def s09_all_notes_off(self):
        e = self.emu
        e.pedal(True)
        self.sound_note(60)
        e.note_off(60)
        self.settle()
        h = e.probe()["held"]
        e.cc(123, 0)  # All Notes Off must not be blocked by the pedal
        stopped = self.wait_voices(lambda v: v == 0, timeout=8)
        e.pedal(False)
        self.settle()
        p = e.probe()
        ok = h == 1 and bool(stopped) and p["held"] == 0
        report(
            "09 CC123 All-Notes-Off passes through pedal",
            ok,
            f"heldBefore={h} stopped={bool(stopped)} after={p['held']}",
        )

    def s10_midi_thru(self):
        e = self.emu
        e.gdb_set("midiEngine.midiThru = 1")
        e.midi_drain()
        e.pedal(True)
        e.note_on(60)
        e.note_off(60)
        thru = e.midi_collect(1.5)
        e.pedal(False)
        tail = e.midi_collect(1.0)
        e.gdb_set("midiEngine.midiThru = 0")
        self.wait_voices(lambda v: v == 0)
        # Documented observation: is the note-off echoed immediately (transparent
        # thru) or deferred with the pedal? Both are defensible; record which.
        off_immediate = bytes([0x80, 60]) in thru or bytes([0x90, 60, 0]) in thru
        off_on_release = bytes([0x80, 60]) in tail or bytes([0x90, 60, 0]) in tail
        report(
            "10 MIDI thru behaviour (observational)",
            True,
            f"off echoed immediately={off_immediate}, on pedal release={off_on_release}, "
            f"thru={thru.hex()} tail={tail.hex()}",
        )

    def s11_keyboard_view_internal(self):
        e = self.emu
        e.key("k")  # keyboard view
        time.sleep(1.0)
        e.pedal(True)
        # Audition through keyboard view: hold a pad via key-mapped audition column
        # is not a keyboard pad, so click a grid pad instead.
        x = 98 + 5 * 119
        y = 632 + 4 * 119
        self._click_hold(x, y, hold=1.0)
        self.settle()
        p = e.probe()  # pad released: internal note must NOT be pedal-held
        ok = p["held"] == 0 and p["voices"] == 0
        # External MIDI must still sustain while in keyboard view.
        self.sound_note(72)
        e.note_off(72)
        self.settle()
        p2 = e.probe()
        ok2 = p2["held"] == 1 and p2["voices"] >= 1
        e.pedal(False)
        self.wait_voices(lambda v: v == 0)
        e.key("k")  # back to clip view
        time.sleep(1.0)
        report(
            "11 keyboard view: pads unaffected, external MIDI sustains",
            ok and ok2,
            f"afterPad={p} ext={p2}",
        )

    def _click_hold(self, nx, ny, hold=0.5):
        e = self.emu
        X = int(nx * 0x7FFF / 2256)
        Y = int(ny * 0x7FFF / 1584)
        e.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {"type": "abs", "data": {"axis": "x", "value": X}},
                        {"type": "abs", "data": {"axis": "y", "value": Y}},
                    ]
                },
            }
        )
        time.sleep(0.1)
        e.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {"type": "btn", "data": {"down": True, "button": "left"}}
                    ]
                },
            }
        )
        time.sleep(hold)
        e.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {"type": "btn", "data": {"down": False, "button": "left"}}
                    ]
                },
            }
        )

    def s12_audition_pad_internal(self):
        e = self.emu
        e.pedal(True)
        e.key_down("1")  # audition pad, top row
        time.sleep(1.0)
        mid = e.probe()
        e.key_up("1")
        self.settle(1.0)
        p = e.probe()
        stopped = self.wait_voices(lambda v: v == 0, timeout=8)
        e.pedal(False)
        ok = mid["voices"] >= 1 and p["held"] == 0 and bool(stopped)
        report(
            "12 audition pad: internal, not pedal-held",
            ok,
            f"during={mid['voices']} heldAfter={p['held']} stopped={bool(stopped)}",
        )

    def s13_arp(self):
        e = self.emu
        # Turn the clip's arp on via gdb (menu-driving is fragile). Best effort:
        # field names differ across eras, try the known ones.
        clip_expr = "((InstrumentClip*)currentSong->currentClip)"
        set_ok = False
        for field, val in (
            ("arpSettings.preset", "ArpPreset::UP"),
            ("arpSettings.mode", "ArpMode::UP"),
        ):
            try:
                e.gdb_set(f"{clip_expr}->{field} = {val}")
                set_ok = True
                break
            except RuntimeError:
                continue
        if not set_ok:
            report("13 arp sustain", False, "could not set arp via gdb (field names?)")
            return
        try:
            e.gdb_set(f"{clip_expr}->arpSettings.numOctaves = 1")
        except RuntimeError:
            pass
        e.pedal(True)
        self.sound_note(60)
        time.sleep(1.0)
        e.note_off(60)  # deferred: arp should keep running
        time.sleep(1.5)
        p1 = e.probe()
        running = p1["held"] == 1 and p1["voices"] >= 1
        e.pedal(False)
        stopped = self.wait_voices(lambda v: v == 0, timeout=10)
        # arp off again
        for field, val in (
            ("arpSettings.preset", "ArpPreset::OFF"),
            ("arpSettings.mode", "ArpMode::OFF"),
        ):
            try:
                e.gdb_set(f"{clip_expr}->{field} = {val}")
                break
            except RuntimeError:
                continue
        report(
            "13 arp keeps running on pedal, stops on release",
            running and bool(stopped),
            f"during={p1} stopped={bool(stopped)}",
        )

    def s15_record_and_playback(self):
        """Record an external note with the pedal: the recorded note must extend
        to the pedal release, not the physical note-off. Then, while the
        recorded clip plays back, sustain another external note: sequencer
        note-offs are internal and must be unaffected (no stuck notes).

        Runs LAST: starting playback permanently wedges the emulator's DIN MIDI
        out (pre-existing, also on unmodified firmware), so every MIDI-out
        scenario must have run before this one."""
        e = self.emu
        e.key("r")  # arm record
        time.sleep(0.5)
        e.key("spc")  # start playback+record
        time.sleep(0.8)
        e.note_on(60)
        time.sleep(0.25)  # physical hold: ~0.25s
        e.pedal(True)
        e.note_off(60)  # deferred
        time.sleep(1.2)
        e.pedal(False)  # note should be recorded as ~1.45s long
        time.sleep(0.5)
        e.key("r")  # record off, keep playing
        time.sleep(0.5)
        # Clip now loops the recorded note (internal note-ons/offs).
        e.pedal(True)
        self.sound_note(72)
        e.note_off(72)
        time.sleep(2.5)  # at least one loop of internal note on/off
        p1 = e.probe()
        ext_still_held = (
            p1["held"] == 1 and p1["heldNotes"][0]["note"] == 72 and 72 in p1["notes"]
        )
        e.pedal(False)
        self.settle()
        e.key("spc")  # stop playback
        time.sleep(0.5)
        stopped = self.wait_voices(lambda v: v == 0, timeout=10)
        p2 = e.probe()
        ok = ext_still_held and bool(stopped) and p2["held"] == 0
        report(
            "15 record w/ pedal + playback interplay (live part)",
            ok,
            f"duringLoop={p1} after={p2}",
        )
        # Make sure playback really stopped (a key event can get lost).
        for _ in range(3):
            if e.gdb_eval("playbackHandler.playbackState") == "0":
                break
            e.key("spc")
            time.sleep(1.0)
        # Save the song so the recorded note length can be verified after exit,
        # confirming via the song's name that the save went through.
        for _ in range(3):
            e.key("s")
            time.sleep(1.2)
            e.key("ret")
            time.sleep(4.0)
            name = e.gdb_eval("(char*)currentSong->name.stringMemory")
            if '"' in name and '""' not in name:
                self.saved_song = True
                break
        if not self.saved_song:
            report("15b song save", False, f"save did not take; name={name}")

    def s14_midi_clip_out(self):
        """Convert the clip to a MIDI instrument: incoming notes are echoed out
        the DIN port by the clip. The out note-off must be deferred with the
        pedal and sent on release. Must run before any playback (see s15)."""
        e = self.emu
        # Convert to a MIDI instrument, verifying via gdb (a key event can get
        # lost in the emulator; pressing 'e' again on an already-MIDI clip only
        # cycles its channel, which is harmless here).
        converted = False
        for _ in range(3):
            e.key("e")
            time.sleep(1.5)
            if "MIDI" in e.gdb_eval("currentSong->currentClip->output->type"):
                converted = True
                break
        if not converted:
            report(
                "14 MIDI clip out: out note-off deferred to pedal release",
                False,
                "could not convert clip to MIDI instrument",
            )
            return
        e.midi_drain()
        e.pedal(True)
        on_bytes = b""
        for _ in range(3):  # retry a dropped note-on
            e.note_on(60)
            time.sleep(0.6)
            on_bytes = e.midi_drain()
            if on_bytes:
                break
        e.note_off(60)
        during = e.midi_collect(1.2)
        pmid = e.probe()
        e.pedal(False)
        tail = e.midi_collect(1.5)
        p = e.probe()
        sent_on = any(b & 0xF0 == 0x90 for b in on_bytes)
        off_during = self._has_note_off(during, 60)
        off_after = self._has_note_off(tail, 60)
        ok = sent_on and not off_during and off_after and p["held"] == 0
        report(
            "14 MIDI clip out: out note-off deferred to pedal release",
            ok,
            f"on={on_bytes.hex()} during={during.hex()} tail={tail.hex()} "
            f"midProbe={pmid} held={p['held']}",
        )
        # Back to a synth clip for the record scenario (verify, retrying).
        for _ in range(3):
            e.key("q")
            time.sleep(1.2)
            if "SYNTH" in e.gdb_eval("currentSong->currentClip->output->type"):
                break

    @staticmethod
    def _has_note_off(data, note):
        # note-off = 0x8x nn vv, or 0x9x nn 00
        for i in range(len(data) - 2):
            s, n, v = data[i], data[i + 1], data[i + 2]
            if n == note and ((s & 0xF0) == 0x80 or ((s & 0xF0) == 0x90 and v == 0)):
                return True
        return False

    def run(self, names=None):
        import re

        scenarios = [m for m in sorted(dir(self)) if re.match(r"s\d\d_", m)]
        if names:
            scenarios = [m for m in scenarios if any(n in m for n in names)]
        for m in scenarios:
            try:
                getattr(self, m)()
            except Exception as ex:  # noqa: BLE001
                report(m, False, f"EXCEPTION {ex}")
            try:
                self.all_off()
            except Exception as ex:  # noqa: BLE001
                print(f"WARN cleanup after {m}: {ex}", flush=True)


def list_songs(sd_dir):
    import glob
    import os

    return set(glob.glob(os.path.join(sd_dir, "SONGS", "*.XML")))


def check_saved_song(sd_dir, pre_existing):
    """After a clean exit, verify the recorded note's length in the saved song:
    with the pedal, the note must be recorded ~1.45s (~280 ticks at 120bpm)
    long, far beyond the ~0.25s (~48 ticks) physical hold. Firmware-written
    files carry 1969 FAT timestamps (no RTC), so new files are found by
    comparing against the pre-launch directory snapshot, not by mtime."""
    import os
    import re

    files = sorted(list_songs(sd_dir) - pre_existing)
    if not files:
        report(
            "16 saved song: recorded note extends to pedal release",
            False,
            "no song saved during this run was written back",
        )
        return
    newest = files[-1]
    xml = open(newest, errors="replace").read()
    lengths = [int(m) for m in re.findall(r'<note[^>]*\blength="(\d+)"', xml)]
    if not lengths:
        # The serializer packs notes as hex blobs (pos u32, length u32, then
        # per-format extras). The record size varies by attribute flavour, so
        # try plausible sizes and keep the one whose positions are increasing
        # and lengths sane.
        for blob in re.findall(r'noteDataWith\w+="0x([0-9a-fA-F]+)"', xml):
            for rec in (32, 28, 24, 20):
                if len(blob) % rec:
                    continue
                recs = [
                    (int(blob[i : i + 8], 16), int(blob[i + 8 : i + 16], 16))
                    for i in range(0, len(blob), rec)
                ]
                poss = [p for p, _ in recs]
                if all(0 < ln < 100000 for _, ln in recs) and poss == sorted(poss):
                    lengths += [ln for _, ln in recs]
                    break
    if not lengths:
        snippet = "\n".join(ln for ln in xml.splitlines() if "note" in ln.lower())[:500]
        report(
            "16 saved song: recorded note extends to pedal release",
            False,
            f"{os.path.basename(newest)}: no notes parsed; snippet: {snippet!r}",
        )
        return
    longest = max(lengths)
    ok = longest >= 60
    report(
        "16 saved song: recorded note extends to pedal release",
        ok,
        f"{os.path.basename(newest)} note lengths={lengths} (>=60 ticks expected)",
    )


def main():
    elf, sd_dir, repo_root = sys.argv[1:4]
    names = sys.argv[4:]
    emu = Emu(elf, sd_dir, repo_root)
    print("launching DelugEmu...", flush=True)
    global LAUNCH_TIME
    LAUNCH_TIME = time.time()
    pre_existing = list_songs(emu.sd_dir)
    emu.launch()
    print("booted.", flush=True)
    # DelugEmu artifact (also on unmodified firmware): the first MIDI-clock byte
    # (0xFA Start) sent when playback starts wedges the emulated DIN TX path for
    # good, killing every later MIDI-out assertion. Clock-out is on because the
    # emulator's stubbed flash makes FlashStorage take the reset-defaults path.
    # Disable it; the sustain logic under test is unaffected (receive side).
    emu.gdb_set("playbackHandler.midiOutClockEnabled = 0")
    s = Suite(emu)
    try:
        s.run(names or None)
    finally:
        emu.quit()
    if s.saved_song:
        time.sleep(2)  # SD write-back
        check_saved_song(emu.sd_dir, pre_existing)
    print("\n==== SUMMARY ====")
    for name, ok, detail in RESULTS:
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
    fails = [r for r in RESULTS if not r[1]]
    print(f"{len(RESULTS) - len(fails)}/{len(RESULTS)} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
