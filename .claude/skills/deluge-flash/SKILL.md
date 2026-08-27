---
name: deluge-flash
description: Push a freshly built firmware to the connected Deluge over USB MIDI sysex (RAM chainload via `dbt loadfw`) with pre-flight and post-flight checks, so build → test doesn't need the SD card
allowed-tools: Bash, Read
---

# deluge-flash

Sends `build/Release/deluge.bin` (or another build/path) to the Deluge over USB MIDI using the
existing `dbt loadfw` transport, wrapped in the checks that have bitten us before. Everything lives
in `.claude/skills/deluge-flash/deluge_flash.py`; run it inside the DBT environment:

```bash
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/deluge_flash.py flash'            # the whole thing
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/deluge_flash.py probe'            # is a Deluge there, what version
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/deluge_flash.py preflight'        # all checks, no send
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/deluge_flash.py syx -o out.syx'   # dump the stream to a file
```

Options: `<build>` positional (`release` default, `debug`, `relwithdebinfo`, or a `.bin` path),
`--branch NAME` (expects `local-fixes`; `--branch ''` to skip), `--allow-dirty`, `--rebuild`,
`--no-auto-rebuild`, `-k KEY`, `-p PORT`, `-d MS` (packet delay, default 2), `--timeout S`.

## What it is — and is not

**It is a RAM chainload.** The firmware on the device receives the image into a RAM buffer,
checks the key and CRC, and `chainload_from_buf()` jumps into it. **SPI flash is untouched: a
power-cycle reverts to whatever was last installed from the SD card.** Use it for the fast
iterate loop; when a build is a keeper, the persistent path is still: copy this ONE `.bin` to the
card (replace the existing one — only one `.bin` on the card, never stack), eject, power on holding
Shift+Select. `loadfw` itself prints an advisory that behaviour can differ slightly from an SD flash
— test via SD before opening a PR.

## One-time bootstrap (read this before the first use)

The receiver is compiled out unless the firmware was built with `ENABLE_SYSEX_LOAD=ON`
(`CMakeLists.txt` option, default **OFF**; the flag reaches `src/deluge/io/midi/sysex.cpp` as
`#ifdef ENABLE_SYSEX_LOAD`). A stock/default build silently ignores the whole transfer. So, once:

1. `./dbt configure -DENABLE_SYSEX_LOAD:BOOL=ON` then `./dbt build release` (the flag is sticky in
   `build/CMakeCache.txt`; `preflight` checks it). `./dbt configure -f` / `dbt nuke` resets it.
2. Install that build **from the SD card** so the *running* firmware has the receiver.
3. On the device: SETTINGS → COMMUNITY FEATURES → *Allow Insecure Develop Sysex Messages* → ON. The
   menu shows an 8-digit hex key. Save it: `printf '%s' KEY > .deluge_hex_key` (gitignored — never
   commit it, never paste it into an issue/PR/chat log).

From then on every build carries the receiver and `flash` just works.

## What `flash` does

Pre-flight
- On the expected branch (`local-fixes`), working tree clean (or `--allow-dirty`).
- The `.bin` is fresh: mtime not older than the tip commit **and** the version string embedded in
  the binary (`1.3.0-dev-<commit>`) matches `HEAD`. A stale bin silently predating a merge has
  bitten us more than once; by default it rebuilds instead of trusting the file.
- `ENABLE_SYSEX_LOAD=ON` in `build/CMakeCache.txt`.
- `.deluge_hex_key` present and 8 hex digits (or `-k`).
- A MIDI port named `Deluge` exists, and the device answers a sysex ping
  (`F0 00 21 7B 01 00 00 F7` → `F0 00 21 7B 01 7F 00 F7`) plus a universal identity request
  (`F0 7E 7F 06 01 F7` → major.minor.patch). `loadfw` uses the *last* Deluge port (Port 3 is the
  sysex port); `-p` overrides.
- Reminds you to stop playback: the load blanks the OLED, shows the screensaver and drives the pads
  as a progress bar while streaming.

Send — `./dbt loadfw <bin> -d <delay>`: ~3,400 segments of 512 bytes (7-bit packed to 586) for a
1.75 MB image, a few seconds at the default 2 ms; then the load message with length + CRC.

Post-flight — waits for the USB port to drop and re-enumerate as the new image boots, pings again,
re-reads the identity version, and prints the commit the sent bin was built from. The device
identity only carries major.minor.patch, so the *commit* comes from the bin, not the device.

## Failure table

| Symptom | Meaning | Do |
|---|---|---|
| `no MIDI port named 'Deluge'` | not enumerated | cable/hub; Deluge in USB **host** mode doesn't enumerate as a device; another app holding the port |
| ping OK but nothing happens on the device during send | running firmware lacks `ENABLE_SYSEX_LOAD` | do the bootstrap above (SD install of a sysex-enabled build) |
| device freezes `E997` on the first segment | key mismatch (handshake) | re-read the key from the menu, fix `.deluge_hex_key`; power-cycle |
| device freezes `E996` on the load message | dev-sysex toggle is OFF on the device | enable it in Community Features; power-cycle |
| `E999` / `E995` | load buffer could not be allocated / load message before segments | power-cycle (frees RAM) and retry; don't send while a big song is loaded |
| `BAD KEY` popup | key mismatch caught at the load message | as E997 |
| `CHECKSUM FAIL` popup | dropped/garbled packets | retry with `-d 5` |
| no pong after the load | image didn't boot | power-cycle: reverts to the SD firmware, nothing is lost; check the build |
| send completes, USB never re-enumerates, screen stuck on screensaver, no pong | the **running** firmware predates the chainload L2 fix (`chainload-l2-cold`, 2026-08-27): stale L2 lines crash any image that isn't byte-identical to the running one | power-cycle, then SD-install a build that has the fix once; after that any image loads |

## Nice-to-haves not yet implemented

- `--sd` persistent mode (locate `/Volumes/DELUGE`, replace the single `.bin`, eject, print the
  Shift+Select steps). Beware: DelugEmu's SD image also mounts as `DELUGE`.
- `--emulator` mode sending the same sysex to DelugEmu over its UDP MIDI chardev (same framing,
  command `03 01`/`03 02`).

Skill files stay on `local-fixes` only — like `CLAUDE.md`, do not merge them into fix branches
destined for upstream.
