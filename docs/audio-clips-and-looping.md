# Audio Clips and Looping (Session View)

Draft for the Deluge doc set. All behavior verified against firmware source (community
firmware, `main` as of 2026-08-18); `file:line` references are for the doc maintainer and
can be stripped for publication. Items marked **[HW-verified]** were confirmed hands-on
on hardware.

---

## 1. Concepts

An audio **track** is an `AudioOutput`; each **clip** on it holds one recorded or loaded
sample (`AudioClip`). Two per-track settings drive everything:

- **Audio source** — which input the track records (and monitors). See §3.
- **Track mode** — Player / Sampler / Looper-FX. This one setting controls both
  input monitoring and what "overdub" means (`audio_output.h:29-33`,
  `audio_output.cpp:117-134`, `audio_clip.h:65`):

| Mode | You hear the input... | Overdub gesture does... |
|---|---|---|
| **Player** | never | clones a new clip below |
| **Sampler** | only while the clip is empty (mutes input once recorded) | clones a new clip below |
| **Looper/FX** | always | records **in place**, layering onto the same clip |

New audio tracks default to Player mode, input Left (`audio_output.cpp:57-58`).

**Changing the mode:** hold the clip's pad in session view and turn SELECT
(`session_view.cpp:1348-1351`), or turn SELECT inside the clip's waveform view
(`audio_clip_view.cpp:738-741`). 7-seg shows PLAY / SAMP / LOOP.

---

## 2. Creating an audio clip

### Rows layout
Pressing an empty pad always creates an **instrument** (synth) clip — audio is
deliberately excluded because audio clips can't be converted back
(`session_view.cpp:866-868`). The recipe:

1. Press an empty pad → a synth clip appears and you're now holding it.
2. **While still holding the pad, press the SELECT encoder** → the clip becomes an
   audio clip on a new audio track (`session_view.cpp:542-544`, `:1803`).

Only works while the clip is empty (`:1817-1821`). Conversion is one-way: trying to
turn an audio clip into an instrument clip pops "Can't convert type" (`:590-593`).

### Grid layout
Press an empty pad in the **first empty column to the right of the last track**
(green or blue mode; `session_view.cpp:3722-3752`). The "New Clip Type" chooser opens:
**Audio**, Synth, Kit, MIDI, CV (`new_clip_type.cpp:43-53`). Scroll to Audio and press
SELECT — or simply release the pad, which accepts the displayed option
(`session_view.cpp:4081-4085`). Audio is the option with no front-panel LED because
there's no AUDIO button; SELECT-press maps to it (`output.h:64-79`).

Pressing an empty pad in an **existing** audio column clones that track's clip and
clears it — the new clip inherits the input source and mode (`session_view.cpp:3479-3508`).

Grid can also convert an empty instrument clip: hold its pad, press SELECT →
Clip Settings → "Convert to audio" (`clip_settings.cpp:47-52`).

### Starting length matters
New clips are one bar long in grid, or the current zoom width (min one bar) in rows
(`session_view.cpp:3617-3643`). Recordings always come out as **whole multiples of this
starting length**, so resize the empty clip first (SHIFT + horizontal encoder) if you
want a different loop size.

---

## 3. Choosing the input ("Audio source")

Enter the clip, press SELECT → the audio-clip menu; "Audio source" is the second item
(`menus.cpp:1739-1753`, chooser in `context_menu/audio_input_selector.cpp:75-84`):

| Option | Records |
|---|---|
| Disabled | nothing — the clip can be armed but will never record (`audio_clip.cpp:141-142`) |
| Left / Right input | one input jack, mono |
| Stereo input | both jacks |
| Balanced input | (L−R)/2 differential (`audio_output.cpp:243-245`) |
| Deluge mix (no fx) | the whole song, pre-master-FX (`song.cpp:2511-2521`) |
| Deluge output (fx) | the final post-FX output — resampling |
| Specific Track | one other track's output |

Shortcuts:
- While the chooser is open, **press any clip pad in session view** to pick that track
  as the source (`audio_input_selector.cpp:200-227`).
- **LEARN + an audio clip's pad** (rows or grid) jumps straight into this chooser
  (`session_view.cpp:916-930`, `:4396-4415`).

Each change becomes the default for future audio tracks (`song.cpp:5152-5163`).

Feedback is prevented structurally: mix/output sources are never echoed, and a
Specific-Track source being monitored is removed from the song's normal render so it
isn't heard twice (`audio_output.cpp:253-255`, `output.h:200-207`).

---

## 4. Recording a loop **[HW-verified]**

Recording is a two-flag system:
- The **RECORD button** is a global record-mode toggle (`playback_handler.cpp:279-324`).
- Each clip has its own **record-arm** flag, which is **on by default for every new
  clip** (`timeline_counter.h:52`).

An armed audio clip actually records when it **launches** with record mode on, provided
it's empty (or the track is a Looper) and the input isn't Disabled
(`audio_clip.cpp:140-143`). Recording never starts mid-play.

### Basic capture
1. Set the input source; in Sampler/Looper mode you'll hear yourself while the clip is
   empty — that monitoring is your "the track is live" indicator.
2. Press RECORD, then launch the empty clip (status pad in rows, pad in grid green
   mode). If starting playback fresh, press PLAY; count-in is configurable at
   SETTINGS → Recording → "Rec count-in bars".
3. The clip records from its launch. Each time it reaches its end with RECORD still
   lit, it **auto-extends by its original length** (`clip.cpp:808-849`) — takes are
   always whole multiples of the starting length.
4. End the recording **without stopping the transport** (see the trap below):
   - **Tap the clip's pad once** (status pad in rows / clip pad in grid green), any
     time during the pass: this schedules a punch-out at exactly the end of the
     current pass, then the clip **keeps playing** as a loop
     (`session.cpp:1156-1164`, `:449-462` — "After finishing recording linearly,
     normally we just keep playing"). Timing is forgiving; the punch-out always
     quantizes to the clip boundary.
   - Or toggle RECORD off during the final pass: recording closes at the next loop
     point (`clip.cpp:812-813`).

The recorder trims the sample to the exact loop boundary with latency compensation
(294 samples, `definitions_cxx.hpp:851`); with SETTINGS → Recording → "Loop margins"
on, it also keeps pre/post margins and gives the clip a soft attack for click-free
loop starts (`audio_clip.cpp:243-245`).

### ⚠️ The big trap: stopping playback aborts the take **[HW-verified]**
Pressing PLAY to stop while an audio clip is recording **discards the recording
entirely** — stop is treated as "abandon" (`audio_clip.cpp:844-846`,
`expectNoFurtherTicks` → `abortRecording()`). The clip is empty again as if nothing
happened. Always close the loop with a pad tap or RECORD-off *first*, then stop the
transport whenever you like.

### Record-arming view
Hold RECORD for half a second to see/edit which clips are armed
(`session_view.cpp:1228-1238`; in grid this only works in green mode). Pad colors
(`view.cpp:2677-2697`):
- **Bright red flash** — armed, will record on next launch.
- **Dull red** — armed but *can't* record (input Disabled, or clip already has audio
  on a non-Looper track).
- **Magenta** — armed, and the overdub will clone a whole new audio *track*.

### Tempoless record ("tempo follows the take")
RECORD + PLAY with no clips playing and metronome off free-runs the recording with no
clock (`session.cpp:2164-2194`). On stop, the Deluge **derives the song tempo from the
take's length** (`playback_handler.cpp:2649-2683`).

Note: SETTINGS → Recording → "Quantization" affects **note** recording only; it has no
effect on audio clips (sole consumer: `instrument_clip.cpp:4414`).

---

## 5. Hands-free looping with a MIDI pedal **[HW-verified]**

Guitarists can't tap pads mid-pass. The Deluge's global MIDI commands solve this:
**SETTINGS → MIDI → Commands** (`menus.cpp:1184-1190`) — navigate to a command, press
LEARN (LED blinks), and hit the pedal; notes, CCs, and program changes all learn
(`menu_item/midi/command.h:34-39`).

Map the pedal to **Loop** (not Record). Loop is the purpose-built looper-pedal command
(`playback_handler.cpp:3336`):
- If a clip is **recording**: punches it out at the end of the current pass and keeps
  it looping — same as the pad tap.
- If **nothing is recording**: opens the next overdub on the current clip.
- So the workflow is: RECORD on, PLAY, play — **stomp** (close the loop at the bar
  line) — **stomp** (open an overdub layer) — **stomp** (close it) — repeat.

Caveat: if the focused clip isn't record-armed, the stomp falls back to toggling
record mode (`:3380-3392`) — leave the clip armed (it is by default).

**Layering loop** is the variant where each stomp continuously spawns the next layer
without a closing stomp. **Record** as a pedal command is just the front-panel toggle
(`:2868-2870`) — usable but less forgiving.

---

## 6. How looping actually plays back

Audio clips don't loop at the sample level — the sequencer **retriggers the sample
every time the clip wraps to 0** (`audio_clip.cpp:307-434`; comment at `:799-801`:
"the actual loop points don't get obeyed for AudioClips"), with a 100-sample crossfade
at the seam (`:669-723`).

Every render, the clip re-fits its sample into its length at the current tempo
(`audio_clip.cpp:585-593`). With the default "pitch/speed independent" setting it
time-stretches (pitch preserved); linked, it varispeeds like tape. Consequences:

- Change the tempo → the loop follows automatically.
- SHIFT + horizontal encoder (resize clip) → audio **stretches** to fit.
- SHIFT + turn while **holding the ◀▶ encoder button pressed** → resize **without**
  stretching (trims the underlying sample) (`audio_clip_view.cpp:802-846`).
- Press **X and Y encoders together** in the clip → set clip length to the sample's
  natural length, removing all stretching (`:744-755`).

---

## 7. Overdubbing and layering

What RECORD + clip pad (or the Loop command) does depends on track mode:

- **Player/Sampler** (and always in rows layout): a **new clip** is cloned below and
  records at the next launch point (`song.cpp:5522-5553`). If armed magenta, it also
  clones a whole new audio **track**, leaving the original playing — take-stacking.
- **Looper/FX in grid**: the overdub is **in place** — at the launch event the same
  clip re-records while playing. The recorder taps the track's own output (playing
  sample + monitored live input), so passes **layer** (`audio_clip.cpp:168-172`) —
  classic sound-on-sound.

RECORD + a **section pad** in rows = Continuous Layering (auto-spawns the next
overdub every pass, `session_view.cpp:771-772`). Grid has optional red/magenta
sidebar loop pads (community setting "Grid view loop pads",
`session_view.cpp:3920-3934`), and a "Create + Record" grid default where tapping an
empty pad in an audio column mid-playback creates and launches an armed clip in one
gesture (`:4247-4254`).

### Magenta ("purple") arming: take-stacking on new tracks

Magenta is the audio-clip arming state where **every overdub pass becomes a new clip
on a brand-new audio track** — a multitrack looper instead of sound-on-sound.

**Arming it:** in the record-arming view (hold RECORD), pressing an audio clip's pad
cycles three states (`view.cpp:2761-2780`): armed **red** (overdubs stay on this
track) → armed **magenta** (`overdubsShouldCloneOutput = true`) → disarmed. The
choice also becomes the song-wide default for audio clips.

**What it does:** when a magenta overdub begins recording, the firmware creates a
whole new `AudioOutput`, clones the original's settings into it, and points the new
clip there (`audio_clip.cpp:288-305`). The handoff in `AudioOutput::cloneFrom`
(`audio_output.cpp:65-91`) is the key detail:

- The new track inherits the input channel and mode.
- If the original was a Sampler/Looper that already holds a take, **the new track
  becomes the sampler/looper and the original demotes to Player** ("we'll become the
  new sampler/looper and the og will become a player"). Monitoring and
  record-readiness always ride on the *newest* layer, while each finished take keeps
  looping untouched on its own now-Player track.

Each layer therefore gets its own fader, FX chain, mute pad — and can be re-recorded
or deleted independently. That's the trade against Looper/FX in-place layering, where
everything piles into one clip and layers can't be peeled apart afterward.

**The magenta sidebar loop pad** in grid (and RECORD + section pad in rows, and the
"Layering loop" MIDI command) is the continuous-layering variant: it *forces* the
clone-output flag on (`playback_handler.cpp:3396-3398`) and auto-spawns the next
overdub every pass (`session.cpp:232-238`) — each pass lands on its own new track
with no stomps between layers. The red sidebar pad is the plain Loop command
(close/open one layer at a time).

Caveat: every layer spawns a track, so a long jam grows the song wide quickly, and
each new track adds ongoing render cost.

---

## 8. Clip Mode: one-shots and fills

Clip Settings → **"Clip Mode"** → Default / **Fill** / **Once**
(`launch_style.cpp:52-56`; the value applies immediately as you scroll).

Reaching Clip Settings:
- Rows: **hold the clip's status pad + press SELECT** (`view.cpp:2794-2798`,
  `session_view.cpp:525-528`). Note the status-pad press also does its normal job —
  it toggles/launches the clip as you grab it (`view.cpp:2799`); toggle back after
  if unwanted.
- Grid: hold the clip's pad + press SELECT (`session_view.cpp:534-540`) — blue mode,
  or green with "Allow Green Selection".

**Once**: on each launch the clip re-arms its own stop one loop-length out
(`session.cpp:646-651`) — it plays exactly one pass from the start, gets a clean
fade-out instead of a loop crossfade (`audio_clip.cpp:726-752`), and mutes. Launch
again to re-fire. **Fill**: the clip times itself to *end* at the next launch event.

---

## 9. Editing and clearing

In the clip's waveform view:
- Tap the column at the loop end to wake the blinking **red end marker**, then tap
  another column to move it (`audio_clip_view.cpp:519-540`).
- **Clear the audio: hold the ◀▶ encoder button and press BACK**
  (`audio_clip_view.cpp:421-435`). Default shows "Sample cleared" (automation kept —
  clear that from automation view). Undoable. The clip is empty again: it re-records
  on next launch, and Sampler monitoring resumes. The WAV stays on the card.
- Delete the whole clip: in session view, hold the pad + press SAVE/DELETE.

Menu extras under SELECT-press: reverse, transpose, pitch/speed link, attack,
waveform marker editor, "set clip length equal to sample length", per-track FX
(it's a full GlobalEffectable chain: filters, FX, sidechain, compressor).

---

## 10. Where recordings live on the SD card

Clip recordings → **`SAMPLES/CLIPS/`**, staged in `SAMPLES/CLIPS/TEMP/` until the song
is saved (then renamed into place, `save_song_ui.cpp:184-197`) — so save the song if
you want to keep takes. Resampling (SHIFT+RECORD) → `SAMPLES/RESAMPLE/`; the sample
browser's recorder → `SAMPLES/RECORD/` (`audio_file_manager.h:43-48`). Named songs get
per-song subfolders with source-named files like `ExtMic_001.wav`.

---

## Quick-start: live guitar loop over drums **[HW-verified]**

1. Make your drum clip; leave playback stopped.
2. Rows: hold empty pad + press SELECT → new audio track. Hold its pad + turn SELECT
   to **Looper/FX**.
3. Enter the clip, SELECT-press → Audio source → **Left input**. Back out.
4. Map a MIDI pedal to the **Loop** command (once, persists in settings).
5. RECORD on → PLAY → play guitar over the drums (you hear yourself via monitoring).
6. **Stomp** (or tap the clip's pad) any time during the pass you want to keep —
   the loop closes exactly at the bar line and keeps playing.
7. Stomp again to layer, stomp to close the layer. Stop the transport only when
   nothing is recording.
