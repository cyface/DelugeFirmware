# delugecommunity.com documentation plan

Working plan for closing gaps in the website docs (`website/src/content/docs/`)
with small, reviewable upstream PRs. Lives only on `local-fixes`.

## Goals

- Every claim in a doc PR is traceable to firmware source, so a reviewer can
  verify it in minutes and nothing is hallucinated.
- PRs are small (one page or one correction each), so upstream maintainers
  can merge them without a large review burden.
- Prefer filling stubs and fixing inaccuracies over restyling prose.

## PR recipe

1. Branch from `main` (mirrors `upstream/main`), named `docs/<topic>`.
2. Touch one page (or one tightly related set of lines across two pages).
3. Pick facts from the firmware, not from memory or other docs. Every
   behavioural statement gets a `file:line` citation at the commit the branch
   was cut from.
4. Citations go in the PR description as a claim → source table, not in the
   page itself (the page stays readable; the PR is the audit trail). The
   commit message records the firmware commit the citations refer to.
5. Use site conventions: `:key[...]` for controls, absolute links for pages,
   `<Badge text="c1.x" variant="tip" />` for community-only features, 4-char
   7-segment names in parentheses where the Menu Hierarchies page does.
6. Don't duplicate. If Ports / Menu Hierarchies / Shortcuts already covers a
   fact, link to it.
7. Where a page shows you can't verify behaviour from source (hardware-only
   facts like voltages), leave the existing text alone and say so in the PR.
8. Open the PR against the fork first (`cyface/DelugeFirmware`, base `main`)
   for review, then re-target upstream.
9. No AI attribution in commits or PR text.

## Citation format (PR description)

| Claim | Source |
| --- | --- |
| CV volts/octave range is 0.01–2.00V, 0 = Hz/V | `src/deluge/gui/menu_item/cv/volts.h:30-32,38` |

Line numbers are relative to the `main` commit named in the PR.

## Backlog

Ordered roughly by value ÷ size. Word counts are from the current page.

### Stubs to fill (manual section)

| Page | Words | Notes |
| --- | --- | --- |
| `manual/engines/cv_gate_engine.mdx` | 42 | **PR 1** — see below |
| `manual/user_interfaces/menu.mdx` | 17 | Menu navigation model: select/back, shortcut pads, scroll vs push-turn, how the 7-seg and OLED differ |
| `manual/user_interfaces/view.mdx` | 17 | The view stack: song/arranger/clip/keyboard/automation/perf and how they nest |
| `manual/device_overview/engines.mdx` | 35 | Overview + links; small once the engine pages exist |
| `manual/engines/midi_engine.mdx` | 105 | Headings exist but are empty; large surface, split into 2–3 PRs (out, learn, internal routing) |
| `manual/engines/synth_sample_engine.mdx` | 123 | Literal "Placeholder" list; biggest gap, split by topic (oscillators/modes, voices+envelopes, FM, mod matrix, signal path) |
| `manual/user_interfaces/file_browser.mdx` | 133 | Thin |
| `resources/midi_devices/{definitions,presets}.mdx` | 22 each | Probably index pages; check what they're meant to hold |

### Inaccuracies found so far (while researching PR 1)

- `reference/menu_hierarchies.mdx` Gate menu: Gate Output 3 also offers
  **Run** and Gate Output 4 also offers **Clock** (`gui/menu_item/gate/mode.h`
  `updateOptions`). The page lists only V-Trig / S-Trig for all four.
- `reference/menu_hierarchies.mdx` Trigger Clock → Input → Auto-Start lists
  "Disabled (ON) / Enabled (OFF)" — labels look swapped; verify against the
  toggle item and fix.
- `manual/device_overview/ports.mdx` "turn the select knob to change" CV
  channel — check which encoder this actually is in `view.cpp`
  (`navigateChannels` path) and whether "1 and 2" is mentioned.

### Audit still to do

- Sweep `reference/menu_hierarchies.mdx` against the menu item sources
  (`src/deluge/gui/menu_item/**`) — ranges, option lists, 4-char names.
- Sweep `reference/shortcuts.mdx` against `deluge-docs` matrix findings
  (33 firmware functions were missing from the matrix; some may also be
  missing here).
- `features/community_features.mdx` is 19k words; spot-check the settings
  toggles against `runtime_feature_settings.cpp` rather than read it whole.

## PR 1: CV / Gate Engine

Replace the 42-word stub with a behavioural description of the engine:
channels and what drives them, note → voltage, the "1 and 2" dual-CV mode
and CV2 source, transpose/pitch bend, gate timing and minimum off-time,
gate 3/4 reserved for run/clock, and gate kit drums. Link to Ports for
hardware/standards and to Menu Hierarchies for the settings tree.

Branch: `docs/cv-gate-engine` — fork PR https://github.com/cyface/DelugeFirmware/pull/10 (reviewed); upstream draft https://github.com/SynthstromAudible/DelugeFirmware/pull/4839 (opened 2026-08-22).
