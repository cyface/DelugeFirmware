# What's in local-fixes

What this branch adds on top of upstream `main`. This file only exists on `local-fixes`,
like `CLAUDE.md`. Update it whenever a branch is merged in, a feature is removed, or upstream
absorbs something. Source branches are listed so a feature can be traced or re-cut for a PR.

Last synced with upstream main: `7aa229721` (2026-10-28).

## Firmware features

| Feature | What it does | Toggle | Source branch(es) |
|---|---|---|---|
| Plugin system | Fixed plugin interface for insert FX and sound sources (`src/deluge/plugin/`). `.dlp` plugin files in `PLUGINS/` on the card load at boot; a misbehaving plugin is identified, BACK at power-on boots without plugins; Settings > Plugins status page. FX side is dormant (no FX registered). | — | `feature/plugin-architecture`, `feature/fx-plugin-param-bank`, `feature/plugin-loader` |
| Plaits drum models | Oscillator type DRUM (808/909 kick, snare, hi-hats, cymbal) as a source plugin, with Model/Tone/Decay/Snap menus. Example kits in `contrib/plaits_drums`. | — | `plaits-drum-models`, `feature/plaits-drums-plugin` |
| Drum velocity layers | Kit drums switch samples by velocity; the sample browser imports a folder as one drum's layers. | Drum Velocity Layers | `feature/drum-velocity-layers` |
| Chord library keyboard | Chord sets from `CHORDS/*.XML` (Default, Jazz, Progressions in `contrib/sd_card/CHORDS`), control column with lead mode and octave pads, split mode (degree chords + fourths grid), in-key roots, follows key changes. | — | `feature/jazz-chord-library`, `feature/chord-library-control-column`, `feature/chord-library-shared-table`, `feature/chord-library-split` |
| Battery level | Fixes 100% while plugged in, recalibrates the scale, ignores the boot transient. | — | `claude/battery-level-global-settings-w1nrjl` |
| Unarmed clip brightness | Grid view brightness for unarmed clips: Dim / Medium / Bright. | Grid Unarmed Clip Brightness | `fix/grid-unarmed-clip-brightness` |
| Grid green-mode stuck press | With Select in green mode off, tapping an empty pad in green mode no longer leaves song view stuck as if a clip were still held (blinking type button, BACK dead, next tap opens the clip). | — | `fix/grid-green-launch-stuck-press` |
| Loop command overdubs | Falls back to an armed audio clip when the loop command has no source clip. | — | `fix/loop-command-source-fallback` |

## Sysex protocol (smSysex) and USB MIDI

| Feature | What it does | Toggle | Source branch(es) |
|---|---|---|---|
| Preset hot-reload | A synth/kit preset reloads when its file is overwritten over sysex. | Sysex Preset Reload | `claude/sysex-preset-hot-reload` |
| Live edit | An external editor can change the loaded synth/kit live (`^chg` / `^dirty` notifications). | Sysex Live Edit | `feature/live-edit-sysex` |
| `view` query | Reports the 8 on-screen rows (incl. clip editor rows); feeds the rows sidecar app. | — | `feature/rows-sidecar-app` |
| Transport | Full 1024-byte write block per frame, `^echo` diagnostic, `pipe: 2` in the session grant, requests wait until the reply fits the send buffer (USB only), directory replies capped to fit. | — | `fix/smsysex-bigger-frames`, `fix/usb-send-ring-overflow`, `fix/usb-send-ring-overflow-reland` |
| USB MIDI send ring | No corruption of large/pipelined sysex replies; drops without logging when full. Open upstream as #4901 (re-land of reverted #4886). | — | `fix/usb-send-ring-overflow-reland` |

## Robustness and error reporting

| Fix | What it does | Source branch(es) |
|---|---|---|
| Error reporting | Preset load, file close and recording-finalisation errors no longer show as "SD card error"; keeps loaded favourites; shows pattern errors. | `fix/preset-load-error-masking`, `fix/sample-recorder-error-masking` |
| Startup song | Guard against the startup task running twice (false "F1"), bad startup-song paths, removed a dead Song-sized allocation. The scheduler fix itself is upstream. | `fix/startup-song-*` |
| Chainload (USB firmware load into RAM) | Cleans/disables L2 before the jump, binds jump arguments to registers, waits for OLED/PIC transfers, re-initialises the OLED after the jump. | `fix/usb-chainload` (from `chainload-l2-cold`, `chainload-oled-idle`) |
| Fault handler | Upstream's handler plus plugin attribution. | `feature/plugin-loader` |

## Tooling and docs (never for upstream)

- **Skills** (`.claude/skills/`): `deluge-flash` (chainload + `hw_*` hardware probes incl. `hw_push_bin.py` for writing a bin to the card over USB), `deluge-preset-xml`, `deluge-sd-backup`.
- **Contrib**: rows sidecar web app (`contrib/rows_sidecar`), `contrib/usb_midi_stress/smsysex_pipeline_stress.py`, Plaits example kits, chord set XMLs; `scripts/export_chord_libraries.py`.
- **Unit tests**: chord library mock; chord tests updated for the larger chord capacity.
- **CI**: on forks the website format check fails instead of running autofix.ci.
- **Docs**: `CLAUDE.md`, this file, `docs/audio-clips-and-looping.md`, `docs/dev/authoring_kits_and_songs.md`, design notes for velocity layers and the rows sidecar, `docs/website-docs-plan.md`, drum model website/menu pages.

## Parked on a branch (removed from local-fixes)

- **Tape saturation** (insert FX, v2 with Head Bump) — on `feature/plugin-architecture` @ `9121e796d`. Removed 2026-10-28 (`70af7b965`). To restore: revert that commit (re-merging the branch won't bring it back).

## Upstreamed (in history here, but no longer a difference)

- Audio clip voice culling fixes (`fix/audio-clip-cull-bugs`)
- First-play click on audio tracks (#4858, `fix/audio-clip-first-play-click`)
- USB host string descriptor length cap (`fix/usb-string-descriptor-length`)
- Save-name collision in the browser (superseded by upstream #4810)
- Loading a synth into a kit row (superseded by upstream #4964/#4993, `fix/synth-drum-load-cleanup`)
- Scheduler once-task re-entry fix in `runTask()` (`fix/startup-song-task-reentry`)
- L1 cache code placed at VMA 0
