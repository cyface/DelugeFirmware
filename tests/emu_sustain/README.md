# DelugEmu test suite for the sustain pedal PR (upstream #4621, fork issue #44)

Automated emulator tests for "Add sustain (damper) pedal support (MIDI CC 64)"
(`stevenstreefkerk:feature/sustain-pedal-cc64`, fetched as `pr-4621-sustain-pedal`).

## How it works

- Boots the firmware in DelugEmu headless with a **private SD folder** (`--sd`,
  name must end `_rw` for write-back on clean exit), **UDP MIDI** on private
  ports for note/CC injection and MIDI-out capture, **QMP** for panel keys /
  screendumps / clean quit, and the **QEMU gdb stub** for symbolic assertions.
- Firmware state asserted via gdb (release ELF carries DWARF):
  `playbackHandler.numHeldSustainNotes` + `heldSustainNotes[]` (the PR's
  deferral store), `MIDIDeviceManager::dinMIDIPorts.inputChannels[ch].sustainPedalDown`,
  and live voices incl. per-voice note codes (`AudioEngine::sounds[i]->voices_`).
- Note routing into the boot song's synth clip uses **MIDI follow**: the SD
  copy's `SETTINGS/MIDIFollow.XML` has channel A set to `1` (XML is 1-based).

## Running

```
# one-time: copy the DelugEmu SD and set MIDI follow channel A to 1
cp -R ~/Library/Application\ Support/DelugEmu/sdcard_rw /tmp/sd_sustain_rw
#   edit /tmp/sd_sustain_rw/SETTINGS/MIDIFollow.XML: <a><channel>1</channel></a>

python3 run_suite.py <path/to/deluge.elf> /tmp/sd_sustain_rw <repo_root> [scenario-substr ...]
```

`repo_root` provides the arm-none-eabi toolchain (gdb). Pin the ELF (copy it)
before a run — `build/Release` is a shared artifact. One full run ≈ 9 minutes.

## Scenarios vs the issue #44 checklist

Automated here (all verified passing on the PR firmware):

| # | scenario | issue item |
|---|----------|-----------|
| 01/02 | boot + baseline routing, pedal-up note-off releases | (baseline) |
| 03 | note-off deferred while pedal down, released on pedal up | external MIDI note (via MIDI follow) |
| 04 | CC64 threshold: 63=up, 64=down | — |
| 05 | retrigger while held cancels the deferred off | — |
| 06 | chord: all offs deferred, released together (drone-style synth, infinite sustain) | drone note |
| 07 | pedal state is per cable+channel | — |
| 08 | 64-entry store overflow fail-safe: 65th off passes through | — |
| 09 | CC123 All-Notes-Off passes through the pedal | — |
| 10 | MIDI thru echoes note-off + CC64 immediately (transparent; observational) | midi thru |
| 11 | keyboard view: pads are internal (not pedal-held), external MIDI still sustains | keyboard view |
| 12 | audition pad is internal, not pedal-held | holding audition pad |
| 13 | arp keeps arpeggiating after key-off with pedal, stops on release | sustain with arp |
| 14 | MIDI clip: outgoing note-off deferred to pedal release | note sent out via a midi clip |
| 15 | record with pedal, then sustain over the looping clip (sequencer interplay) | record while sustaining / sequencer + midi in |
| 16 | learnable SUSTAIN command: factory default CC64/any-channel, rebinding moves the pedal and frees CC64, unbinding disables it | — |
| 17 | saved song: recorded note length extends to pedal release (281 vs ~48 ticks) | record while sustaining |

Answered by code construction (the PR only defers **incoming external MIDI
note-offs** in `PlaybackHandler::noteMessageReceived`; internally generated
notes never pass through it): sequencer-held notes, fill notes, probability /
iteration / ratchets / randomizer notes, audition + vertical encoder, clip /
song-macro transpose — none of these can interact with the pedal beyond what
scenarios 11/12/15 already demonstrate (internal paths unaffected while a
pedal-held external note rings).

Left for hardware / manual testing:
- resampling an auditioned note while holding record (audio-in path),
- switching sections / clips / arranger while sustaining: on pedal release the
  deferred note-off is replayed through MIDI follow into the *new* active
  context, so the old clip's voice can ring until then — the same window that
  already exists when holding a key across a clip switch; the pedal widens it,
- MIDI-learned (non-follow) routing — same code path, deferral happens before
  routing, so behavior is identical by construction,
- real CC64 pedal ergonomics.

## Fork additions on top of the PR

This branch extends the PR with a **learnable SUSTAIN command** (SETTINGS >
MIDI > COMMANDS > SUSTAIN, factory default CC64 on any channel of any cable)
and makes the pedal handler **fall through** instead of consuming the CC, so a
CC mapped via MIDI follow or MIDI learn keeps firing (and recording as
automation) alongside the pedal - verified with the MIDI-follow param popup
(map `lpfFrequency` to 64, enable `display_param`): the same CC64 message
shows `LPF FREQUENCY` *and* sustains the note. Rebinding or unbinding the
command frees CC64 entirely (scenario 16). Flash byte 196 uses 0 = "flash
predates the command" (re-applies the default) and 255 = explicitly unbound,
so unlearning survives reboots. Note bindings learned to the command are
inert - only CC bindings act as a pedal.

## Review observations (not test failures)
- Note-offs are deferred even for notes that never sounded (deferral happens
  before routing). Harmless — replayed offs are no-ops.
- `clearSustainedNotes()` is not wired into playback-stop / song-swap (the PR
  notes this). Held entries keep a raw `MIDICable*`; a USB device unplugged
  while notes are held would leave stale pointers to be replayed on release.
- MIDI thru is transparent: the physical note-off is echoed immediately, the
  pedal only shapes the Deluge's own voices / clip outputs.

## Emulator gotchas encountered (not the PR)

- **Starting playback permanently wedges DelugEmu's DIN MIDI out** — reproduced
  identically on the PR's merge base (7ade37f2) without the PR. All MIDI-out
  scenarios must run before the first playback (hence the ordering), and the
  suite disables `playbackHandler.midiOutClockEnabled` at boot.
- The UDP-UART MIDI input occasionally drops a message; every note-on the
  assertions depend on is verified against the live voice list and retried
  (`sound_note`).
- Firmware-written SD files carry 1969 FAT timestamps (no RTC): find saved
  songs by directory diff, not mtime.
- The stubbed SPI flash makes FlashStorage take the reset-defaults path
  (e.g. MIDI clock out enabled), not all-zeros.
- Each gdb attach halts the CPU; the audio engine can respond to the apparent
  overload by culling a sustaining voice, so voice-count assertions after
  several probes must allow `>= 1`, not an exact count (the deferral store is
  the exact observable).
