"""DelugEmu control library for the sustain-pedal (PR #4621) emulator test suite.

Launches DelugEmu headless with:
  - a private SD folder (--sd <dir>), so the shared sdcard_rw is never touched
  - UDP MIDI on private ports (raw MIDI bytes in both directions)
  - QMP on a private unix socket (panel keys, screendumps, clean quit)
  - the QEMU gdb stub (symbolic probes of firmware state via arm-none-eabi-gdb)
"""

import json
import os
import re
import socket
import subprocess
import time

DELUGEMU = "/Applications/DelugEmu.app/Contents/MacOS/DelugEmu"
LOG = os.path.expanduser("~/Library/Application Support/DelugEmu/delugemu.log")
GDB = None  # resolved from repo root at runtime

# MIDI status bytes
NOTE_ON = 0x90
NOTE_OFF = 0x80
CC = 0xB0


def find_gdb(repo_root):
    p = os.path.join(
        repo_root,
        "toolchain/v22/darwin-arm64/arm-none-eabi-gcc/bin/arm-none-eabi-gdb-py3",
    )
    if not os.path.exists(p):
        raise RuntimeError(f"gdb not found at {p}")
    return p


class Emu:
    def __init__(
        self,
        elf,
        sd_dir,
        repo_root,
        midi_send_port=24998,
        midi_recv_port=24999,
        qmp_sock="/tmp/sustain_qmp.sock",
        gdb_port=23333,
    ):
        self.elf = os.path.abspath(elf)
        self.sd_dir = os.path.abspath(sd_dir)
        self.repo_root = os.path.abspath(repo_root)
        self.midi_send_port = midi_send_port  # QEMU listens here; we sendto it
        self.midi_recv_port = midi_recv_port  # QEMU sends here; we bind it
        self.qmp_sock_path = qmp_sock
        self.gdb_port = gdb_port
        self.gdb = find_gdb(repo_root)
        self.proc = None
        self.qmp = None
        self.midi = None
        self.probe_script = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "probe.py"
        )

    # ---------- lifecycle ----------

    def launch(self, boot_wait=60):
        if os.path.exists(LOG):
            os.unlink(LOG)
        if os.path.exists(self.qmp_sock_path):
            os.unlink(self.qmp_sock_path)
        # Bind the MIDI receive socket BEFORE launch so QEMU's connected udp
        # chardev (send_port -> recv_port) never sees ICMP unreachable.
        self.midi = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.midi.bind(("127.0.0.1", self.midi_recv_port))
        self.midi.setblocking(False)

        args = [
            DELUGEMU,
            self.elf,
            "--sd",
            self.sd_dir,
            "--midi",
            f"udp:127.0.0.1:{self.midi_recv_port}@127.0.0.1:{self.midi_send_port}",
            "--display",
            "none",
            "--",
            "-qmp",
            f"unix:{self.qmp_sock_path},server,nowait",
            "-gdb",
            f"tcp::{self.gdb_port}",
        ]
        self.proc = subprocess.Popen(
            args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        # Wait for the firmware audio loop to come up.
        deadline = time.time() + boot_wait
        booted = False
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"DelugEmu exited early (rc={self.proc.returncode}); see {LOG}"
                )
            try:
                with open(LOG) as f:
                    if "tx-render-head auto-detected" in f.read():
                        booted = True
                        break
            except FileNotFoundError:
                pass
            time.sleep(1)
        if not booted:
            raise RuntimeError(f"firmware did not boot within {boot_wait}s; see {LOG}")
        # Firmware needs a while after the audio loop starts before UI/MIDI are live.
        time.sleep(15)
        self._qmp_connect()

    def _qmp_connect(self):
        self.qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.qmp.connect(self.qmp_sock_path)
        self.qmp.settimeout(10)
        self._qmp_recv_json()  # greeting
        self.qmp_cmd({"execute": "qmp_capabilities"})

    def _qmp_recv_json(self):
        buf = b""
        while True:
            buf += self.qmp.recv(65536)
            for line in buf.split(b"\r\n"):
                line = line.strip()
                if line:
                    try:
                        return json.loads(line)
                    except json.JSONDecodeError:
                        continue

    def qmp_cmd(self, obj):
        self.qmp.sendall(json.dumps(obj).encode() + b"\n")
        while True:
            r = self._qmp_recv_json()
            if "return" in r or "error" in r:
                return r

    def quit(self):
        """Clean QMP quit (required for SD write-back)."""
        try:
            self.qmp.sendall(json.dumps({"execute": "quit"}).encode() + b"\n")
        except OSError:
            pass
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        if self.midi:
            self.midi.close()

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
        if self.midi:
            self.midi.close()

    # ---------- MIDI ----------

    def midi_send(self, *msgs, gap=0.008):
        """Send raw MIDI messages (each an iterable of ints), paced."""
        for m in msgs:
            self.midi.sendto(bytes(m), ("127.0.0.1", self.midi_send_port))
            time.sleep(gap)

    def note_on(self, note, vel=100, ch=0):
        self.midi_send([NOTE_ON | ch, note, vel])

    def note_off(self, note, vel=64, ch=0):
        self.midi_send([NOTE_OFF | ch, note, vel])

    def cc(self, num, val, ch=0):
        self.midi_send([CC | ch, num, val])

    def pedal(self, down, ch=0, value=None):
        if value is None:
            value = 127 if down else 0
        self.cc(64, value, ch=ch)

    def midi_drain(self):
        """Return all pending MIDI-out bytes from the emulator."""
        out = b""
        while True:
            try:
                data, _ = self.midi.recvfrom(65536)
                out += data
            except BlockingIOError:
                return out

    def midi_collect(self, seconds):
        end = time.time() + seconds
        out = b""
        while time.time() < end:
            out += self.midi_drain()
            time.sleep(0.02)
        return out

    # ---------- panel ----------

    def key(self, qcode, hold=0.15):
        self.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {
                            "type": "key",
                            "data": {
                                "down": True,
                                "key": {"type": "qcode", "data": qcode},
                            },
                        }
                    ]
                },
            }
        )
        time.sleep(hold)
        self.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {
                            "type": "key",
                            "data": {
                                "down": False,
                                "key": {"type": "qcode", "data": qcode},
                            },
                        }
                    ]
                },
            }
        )
        time.sleep(0.15)

    def key_down(self, qcode):
        self.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {
                            "type": "key",
                            "data": {
                                "down": True,
                                "key": {"type": "qcode", "data": qcode},
                            },
                        }
                    ]
                },
            }
        )

    def key_up(self, qcode):
        self.qmp_cmd(
            {
                "execute": "input-send-event",
                "arguments": {
                    "events": [
                        {
                            "type": "key",
                            "data": {
                                "down": False,
                                "key": {"type": "qcode", "data": qcode},
                            },
                        }
                    ]
                },
            }
        )

    def screendump(self, path_ppm):
        self.qmp_cmd(
            {
                "execute": "human-monitor-command",
                "arguments": {"command-line": f"screendump {path_ppm}"},
            }
        )

    # ---------- gdb probes ----------

    def probe(self, extra_cmds=()):
        """Attach gdb, run probe.py (prints PROBE:{json}), detach. Returns dict."""
        cmd = [
            self.gdb,
            "-batch",
            "-ex",
            "set confirm off",
            "-ex",
            "set pagination off",
            "-ex",
            f"target remote :{self.gdb_port}",
        ]
        for c in extra_cmds:
            cmd += ["-ex", c]
        cmd += ["-x", self.probe_script, "-ex", "detach", self.elf]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        m = re.search(r"PROBE:(\{.*\})", r.stdout)
        if not m:
            raise RuntimeError(
                f"probe failed:\nSTDOUT:{r.stdout[-3000:]}\nSTDERR:{r.stderr[-2000:]}"
            )
        return json.loads(m.group(1))

    def gdb_eval(self, expr):
        """Evaluate one expression on the live target, return gdb's `$1 = ...` text."""
        cmd = [
            self.gdb,
            "-batch",
            "-ex",
            "set confirm off",
            "-ex",
            "set pagination off",
            "-ex",
            f"target remote :{self.gdb_port}",
            "-ex",
            f"p {expr}",
            "-ex",
            "detach",
            self.elf,
        ]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        m = re.search(r"\$1 = (.*)", r.stdout)
        if not m:
            raise RuntimeError(f"gdb_eval({expr}) failed: {r.stderr[-1500:]}")
        return m.group(1).strip()

    def gdb_set(self, *assignments):
        """Run `set var <a>` statements on the live target (no probe output)."""
        cmd = [
            self.gdb,
            "-batch",
            "-ex",
            "set confirm off",
            "-ex",
            "set pagination off",
            "-ex",
            f"target remote :{self.gdb_port}",
        ]
        for a in assignments:
            cmd += ["-ex", f"set var {a}"]
        cmd += ["-ex", "detach", self.elf]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise RuntimeError(f"gdb_set failed: {r.stderr[-2000:]}")
        return r.stdout
