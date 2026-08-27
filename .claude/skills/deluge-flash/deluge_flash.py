#!/usr/bin/env python3
"""Pre-flight, send, and post-flight wrapper around `dbt loadfw`.

Run it inside the DBT environment so python-rtmidi is available:

    DBT_NO_SYNC=1 ./dbt exec python3 .claude/skills/deluge-flash/deluge_flash.py <command> [options]

Commands
  probe      list Deluge MIDI ports, ping the device, read its firmware version
  preflight  every check short of sending (branch, tree, bin freshness, build flag, key, port, ping)
  flash      preflight -> `dbt loadfw` -> wait for the device to come back -> verify (default)
  syx        write the sysex stream to a .syx file instead of sending it

The transport itself is `scripts/tasks/task-loadfw.py`; this script never re-implements it.
It is a RAM chainload: SPI flash is untouched and a power-cycle reverts to the SD-installed firmware.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

DELUGE_HDR = [0xF0, 0x00, 0x21, 0x7B, 0x01]
PING = DELUGE_HDR + [0x00, 0x00, 0xF7]  # SysexCommands::Ping, reply 7F <tag>
IDENTITY_REQUEST = [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]  # universal non-RT identity
VERSION_RE = re.compile(rb"\d+\.\d+\.\d+-[A-Za-z]+-[0-9a-f]{7,10}")
KEY_FILE = ".deluge_hex_key"


class Fail(Exception):
    pass


def say(msg: str) -> None:
    print(msg, flush=True)


def run(cmd: list[str], **kw) -> str:
    return subprocess.run(
        cmd, check=True, capture_output=True, text=True, **kw
    ).stdout.strip()


# --------------------------------------------------------------------------- git / build checks


def repo_root() -> Path:
    return Path(run(["git", "rev-parse", "--show-toplevel"]))


def bin_path(build: str) -> Path:
    p = Path(build)
    if p.is_file():
        return p
    table = {
        "release": "build/Release/deluge.bin",
        "debug": "build/Debug/deluge.bin",
        "relwithdebinfo": "build/RelWithDebInfo/deluge.bin",
    }
    if build.lower() not in table:
        raise Fail(
            f"unknown build '{build}': give release|debug|relwithdebinfo or a path to a .bin"
        )
    return Path(table[build.lower()])


def embedded_version(bin_file: Path) -> str | None:
    m = VERSION_RE.search(bin_file.read_bytes())
    return m.group(0).decode() if m else None


def check_git(expect_branch: str | None, allow_dirty: bool) -> None:
    branch = run(["git", "branch", "--show-current"])
    if expect_branch and branch != expect_branch:
        raise Fail(
            f"on branch '{branch}', expected '{expect_branch}' (pass --branch to override)"
        )
    dirty = run(["git", "status", "--porcelain", "--untracked-files=no"])
    if dirty:
        msg = "working tree has uncommitted changes:\n" + dirty
        if not allow_dirty:
            raise Fail(msg + "\n(commit them, or pass --allow-dirty to flash anyway)")
        say("WARN " + msg)
    say(f"ok   branch {branch} @ {run(['git', 'rev-parse', '--short', 'HEAD'])}")


def check_bin(bin_file: Path) -> str:
    if not bin_file.is_file():
        raise Fail(f"{bin_file} does not exist - run ./dbt build release")
    head = run(["git", "rev-parse", "--short", "HEAD"])
    commit_time = int(run(["git", "log", "-1", "--format=%ct"]))
    mtime = int(bin_file.stat().st_mtime)
    ver = embedded_version(bin_file)
    problems = []
    if mtime < commit_time:
        problems.append(
            f"{bin_file} is {commit_time - mtime}s OLDER than the tip commit"
        )
    if ver is None:
        problems.append("no version string found inside the .bin")
    else:
        built_from = ver.rsplit("-", 1)[-1]
        full_head = run(["git", "rev-parse", "HEAD"])
        if not full_head.startswith(built_from):
            problems.append(f"bin was built from {built_from}, HEAD is {head}")
    if problems:
        raise Fail(
            "; ".join(problems)
            + "\n(stale binary - rebuild with ./dbt build release, or pass --rebuild)"
        )
    say(f"ok   {bin_file} ({bin_file.stat().st_size} bytes) = {ver}")
    return ver or ""


def check_build_flag(root: Path) -> None:
    cache = root / "build" / "CMakeCache.txt"
    if not cache.is_file():
        raise Fail("build/CMakeCache.txt missing - run ./dbt build release first")
    m = re.search(r"^ENABLE_SYSEX_LOAD:BOOL=(\w+)", cache.read_text(), re.MULTILINE)
    if not m or m.group(1).upper() not in ("ON", "TRUE", "1"):
        raise Fail(
            "ENABLE_SYSEX_LOAD is OFF in build/CMakeCache.txt: the firmware you build will NOT accept "
            "sysex loads.\nFix: ./dbt configure -DENABLE_SYSEX_LOAD:BOOL=ON && ./dbt build release, "
            "then install that build ONCE from the SD card so the running firmware has the receiver."
        )
    say("ok   ENABLE_SYSEX_LOAD=ON in build config")


def check_key(root: Path, key_arg: str | None) -> str:
    if key_arg:
        key = key_arg
    else:
        f = root / KEY_FILE
        if not f.is_file():
            raise Fail(
                f"{KEY_FILE} not found. On the Deluge: SETTINGS > COMMUNITY FEATURES > "
                "ALLOW INSECURE DEVELOP SYSEX MESSAGES - enable it and it shows an 8-digit hex key.\n"
                f"Write it with:  printf '%s' <KEY> > {KEY_FILE}   (the file is gitignored; never commit it)"
            )
        key = f.read_text().strip()
    if not re.fullmatch(r"[0-9a-fA-F]{8}", key):
        raise Fail(f"hex key '{key}' is not 8 hex digits")
    say("ok   hex key present")
    return key


def rebuild(root: Path) -> None:
    say("---- ./dbt build release")
    env = dict(os.environ, DBT_NO_SYNC="1")
    r = subprocess.run(["./dbt", "build", "release"], cwd=root, env=env, check=False)
    if r.returncode:
        raise Fail("build failed")


# --------------------------------------------------------------------------- MIDI


def midi():
    try:
        import rtmidi
    except ImportError:
        raise Fail(
            "python-rtmidi not installed: run `./dbt loadfw -h` once (it installs it), "
            "or run this script via `./dbt exec python3 ...`"
        ) from None
    import rtmidi

    return rtmidi


def deluge_ports(rt) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    try:
        outs = [
            (i, p)
            for i, p in enumerate(rt.MidiOut().get_ports())
            if "DELUGE" in p.upper()
        ]
        ins = [
            (i, p)
            for i, p in enumerate(rt.MidiIn().get_ports())
            if "DELUGE" in p.upper()
        ]
    except Exception:  # noqa: BLE001 - rtmidi raises InvalidPortError if the device unplugs mid-enumeration
        return [], []
    return outs, ins


def open_pair(rt, port_index: int | None):
    outs, ins = deluge_ports(rt)
    if not outs or not ins:
        raise Fail(
            "no MIDI port named 'Deluge'. Check: USB cable plugged in; Deluge is NOT in USB host mode "
            "(host mode does not enumerate as a device); no other app has the port open; try another cable/hub."
        )
    # loadfw defaults to the LAST Deluge port (port 3 is reserved for sysex)
    out_i = port_index if port_index is not None else outs[-1][0]
    in_name = rt.MidiOut().get_port_name(out_i)
    in_i = next((i for i, p in ins if p == in_name), ins[-1][0])
    mo, mi = rt.MidiOut(), rt.MidiIn()
    mo.open_port(out_i)
    mi.open_port(in_i)
    mi.ignore_types(False, True, True)  # want sysex
    return mo, mi, in_name, out_i


def wait_reply(mi, pred, timeout: float):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        m = mi.get_message()
        if m:
            msg, _ = m
            if pred(msg):
                return msg
        else:
            time.sleep(0.01)
    return None


def ping(mo, mi, timeout=2.0) -> bool:
    mo.send_message(PING)
    return wait_reply(mi, lambda m: m[:6] == DELUGE_HDR + [0x7F], timeout) is not None


def identity(mo, mi, timeout=2.0) -> str | None:
    mo.send_message(IDENTITY_REQUEST)
    m = wait_reply(
        mi,
        lambda m: len(m) >= 16 and m[1] == 0x7E and m[3] == 0x06 and m[4] == 0x02,
        timeout,
    )
    if not m:
        return None
    return f"{m[12]}.{m[13]}.{m[14]}"


def probe(port_index: int | None, quiet=False) -> str | None:
    rt = midi()
    outs, _ins = deluge_ports(rt)
    if not quiet:
        for i, p in outs:
            say(f"     out {i}: {p}")
    mo, mi, name, _ = open_pair(rt, port_index)
    try:
        if not ping(mo, mi):
            raise Fail(
                f"no PONG from '{name}' - device not responding to sysex (frozen? still booting? wrong port?)"
            )
        ver = identity(mo, mi)
        say(
            f"ok   '{name}' answers ping; device reports firmware {ver or '(no identity reply)'}"
        )
        return ver
    finally:
        mo.close_port()
        mi.close_port()
        del mo, mi


def wait_for_return(port_index: int | None, timeout: float) -> str | None:
    """After a chainload the firmware restarts and USB re-enumerates: the port goes away and comes back."""
    rt = midi()
    say("     waiting for the Deluge to restart...")
    end = time.monotonic() + timeout
    seen_gone = False
    while time.monotonic() < end:
        outs, _ = deluge_ports(rt)
        if not outs:
            seen_gone = True
        elif seen_gone or time.monotonic() > end - timeout + 8:
            time.sleep(2.0)  # let the MIDI stack settle before opening
            # not back yet (Fail) or ports vanished under rtmidi mid-enumeration: keep polling
            with contextlib.suppress(Exception):
                return probe(port_index, quiet=True)
        time.sleep(0.5)
    raise Fail(
        "device did not answer within the timeout after the load. If the screen is blank or shows Exxx, "
        "power-cycle it: this reverts to the SD-installed firmware, nothing is lost."
    )


# --------------------------------------------------------------------------- commands


def do_preflight(a, root: Path) -> tuple[Path, str]:
    say("---- pre-flight")
    check_git(a.branch, a.allow_dirty)
    bin_file = bin_path(a.build)
    if a.rebuild:
        rebuild(root)
    try:
        ver = check_bin(bin_file)
    except Fail as e:
        if a.rebuild or a.no_auto_rebuild:
            raise
        say("WARN " + str(e).splitlines()[0] + " - rebuilding")
        rebuild(root)
        ver = check_bin(bin_file)
    check_build_flag(root)
    check_key(root, a.key)
    probe(a.port)
    say(
        "NOTE stop playback on the Deluge before sending; the load blanks the display and streams for a few seconds."
    )
    return bin_file, ver


def loadfw(
    root: Path,
    bin_file: Path,
    delay: int,
    key: str | None,
    port: int | None,
    outfile: str | None,
) -> int:
    cmd = ["./dbt", "loadfw", str(bin_file), "-d", str(delay)]
    if key:
        cmd += ["-k", key]
    if port is not None:
        cmd += ["-p", str(port)]
    if outfile:
        cmd += ["-o", outfile]
    say("---- " + " ".join(c if c != key else "<key>" for c in cmd))
    env = dict(os.environ, DBT_NO_SYNC="1")
    if sys.stdout.isatty():
        return subprocess.run(cmd, cwd=root, env=env, check=False).returncode
    # Not a terminal (e.g. an agent capturing output): loadfw redraws its progress bar with '\r'
    # thousands of times, so collapse each line to its final state before printing.
    # bytes, not text=True: universal-newline mode would turn the "\r" redraws into "\n".
    r = subprocess.run(cmd, cwd=root, env=env, capture_output=True, check=False)
    for stream in (r.stdout, r.stderr):
        for line in stream.decode(errors="replace").split("\n"):
            last = line.rstrip("\r").rsplit("\r", 1)[-1].strip()
            if last:
                say("     " + last)
    return r.returncode


def do_flash(a, root: Path) -> None:
    bin_file, ver = do_preflight(a, root)
    if loadfw(root, bin_file, a.delay, a.key, a.port, None):
        raise Fail("dbt loadfw failed (see output above)")
    say("---- post-flight")
    try:
        new_ver = wait_for_return(a.port, a.timeout)
    except Fail as e:
        raise Fail(
            str(e) + "\nIf the Deluge shows CHECKSUM FAIL, re-run with --delay 5. "
            "BAD KEY means the key in .deluge_hex_key does not match the device; re-read it from the menu. "
            "E997 (frozen) = wrong key on the first segment; E996 = the dev-sysex toggle is OFF on the device; "
            "E999/E995 = device could not allocate the load buffer - power-cycle and retry."
        ) from e
    say(f"DONE running {ver} (device identity {new_ver}).")
    say(
        "NOTE this was a RAM load: a power-cycle reverts to the SD-installed firmware. To keep it, copy this ONE "
        ".bin to the SD card (replace the existing one, never stack several) and flash with Shift+Select on power-up."
    )


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument(
        "command",
        nargs="?",
        default="flash",
        choices=["probe", "preflight", "flash", "syx"],
    )
    ap.add_argument(
        "build",
        nargs="?",
        default="release",
        help="release|debug|relwithdebinfo or path to .bin",
    )
    ap.add_argument(
        "--branch",
        default="local-fixes",
        help="branch you expect to be on ('' to skip)",
    )
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument(
        "--rebuild", action="store_true", help="always rebuild before sending"
    )
    ap.add_argument(
        "--no-auto-rebuild",
        action="store_true",
        help="fail instead of rebuilding a stale bin",
    )
    ap.add_argument("-k", "--key", help="8-digit hex key (default: .deluge_hex_key)")
    ap.add_argument(
        "-p", "--port", type=int, help="MIDI port index (default: last 'Deluge' port)"
    )
    ap.add_argument(
        "-d",
        "--delay",
        type=int,
        default=2,
        help="ms between packets; raise to 5 on checksum errors",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=60,
        help="seconds to wait for the device after loading",
    )
    ap.add_argument("-o", "--outfile", default="deluge.syx", help="syx: output file")
    a = ap.parse_args()
    a.branch = a.branch or None
    root = repo_root()
    os.chdir(root)
    try:
        if a.command == "probe":
            probe(a.port)
        elif a.command == "preflight":
            do_preflight(a, root)
        elif a.command == "syx":
            bin_file = bin_path(a.build)
            check_bin(bin_file)
            key = check_key(root, a.key)
            return loadfw(root, bin_file, a.delay, key, None, a.outfile)
        else:
            do_flash(a, root)
    except Fail as e:
        say(f"FAIL {e}")
        return 1
    except subprocess.CalledProcessError as e:
        say(f"FAIL {' '.join(e.cmd)}: {e.stderr.strip()}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
