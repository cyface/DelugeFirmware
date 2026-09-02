# Rows Sidecar App – Specification

Status: draft, not yet implemented.

## 1. Summary

The Deluge's pad grid shows eight rows of clips or tracks, but the names of those
rows are only visible on the small OLED when a pad is pressed. This project adds
a **sidecar display**: a phone placed beside the pad grid that continuously shows
the name, type, colour and play state of each of the eight rows currently on screen.

An iPhone 16 Pro Max is roughly the height of the pad grid, so in portrait it can
sit alongside the grid with one on-screen row per pad row.

The work splits into three independent pieces:

| Piece | Where | Purpose |
|---|---|---|
| Firmware `view` query | `src/deluge/storage/smsysex.cpp` | Report the eight on-screen rows over the existing JSON-over-SysEx API |
| Web page | `contrib/rows_sidecar/` | Poll the query and render the rows, sized to match the pads |
| Bridge (later) | `contrib/rows_sidecar/bridge/` | Make the phone link wireless once the display has proved useful |

The first two pieces are enough to evaluate the idea with a wired phone. The bridge
is deliberately deferred.

## 2. Goals and non-goals

Goals:

- Show the eight on-screen rows with enough information to identify them at a glance.
- Track scrolling and layout changes on the Deluge with visible latency under about
  250 ms.
- Work with no firmware knowledge in the phone beyond one JSON query.
- Reuse the existing SysEx JSON request/reply machinery; no new SysEx command IDs.
- Keep the page a single HTML file with no build step, so it can be opened from a
  local file, a tiny HTTP server, or a bridge device.

Non-goals for the first version:

- Sending anything to the Deluge other than the query (no remote control).
- Mirroring the OLED, showing tempo, or browsing the SD card. These are listed as
  later extensions in section 8.
- Supporting the 7-segment display model differently; the query is display-agnostic.

## 3. Existing firmware pieces this builds on

- **Vendor SysEx framing.** All Deluge SysEx starts `F0 00 21 7B 01`, followed by a
  command byte from `SysEx::SysexCommands` in `src/deluge/io/midi/sysex.h`
  (`Ping=0, Popup=1, HID=2, Debug=3, Json=4, JsonReply=5, Pong=0x7F`). A development
  header with manufacturer byte `7D` is also accepted.
- **JSON request/reply.** `MidiEngine::midiSysexReceived` routes command `Json` to
  `smSysex::sysexReceived`, which queues the message. `smSysex::handleNextSysEx`
  runs as a repeating task from `deluge.cpp` (not in the USB receive path), reads
  the sequence byte, parses the JSON object and dispatches on its first key
  (`open`, `close`, `dir`, `read`, `write`, `delete`, `mkdir`, `rename`, `copy`,
  `move`, `utime`, `session`, `ping`). Replies use `startReply`, which writes
  `F0 00 21 7B 01 05 <seq>` and then JSON text, then `sendMsg` appends `F7` and calls
  `cable.sendSysex`. Reply object keys are prefixed with `^` (for example `^ping`).
- **Row lookup.** `SessionView::getClipOnScreen(yDisplay)` already resolves a pad
  row to a `Clip*` for both the rows layout (`sessionClips[yDisplay + songViewYScroll]`)
  and the grid layout (`gridClipFromCoords`). `View::displayOutputName` shows how an
  output's display name is derived from its type, channel and user-set name.
- **USB host port.** Hosted and upstream USB MIDI cables share `sendSysex`
  (`src/deluge/io/midi/cable_types/usb_common.h`), so a bridge device plugged into
  the Deluge's host port receives replies just like a computer on the device port.

## 4. Firmware: the `view` query

### 4.1 Request

```
F0 00 21 7B 01 04 <seq> {"view":{}} F7
```

`seq` is the session/sequence byte used by the existing JSON API. The page obtains
one with the `session` request or uses `0`, following whatever the existing web
tools do. Optional request fields, all ignored in the first version, are reserved
for later: `{"view":{"gen":123}}` may be used to ask for a reply only if the state
has changed since generation 123.

### 4.2 Reply

```
F0 00 21 7B 01 05 <seq> {"^view":{ ... }} F7
```

```json
{"^view":{
  "gen": 417,
  "ui": "session",
  "layout": "rows",
  "song": "My Song",
  "yScroll": 0,
  "xScroll": 0,
  "playing": true,
  "rows": [
    {"y":7,"type":"synth","name":"Lead Pluck","clip":"","colour":"ff8000",
     "playing":true,"muted":false,"soloed":false,"armed":false},
    {"y":6,"type":"kit","name":"Drums 1","clip":"Verse", ...},
    {"y":5,"type":"none"},
    ...
  ]
}}
```

Field semantics:

| Field | Type | Meaning |
|---|---|---|
| `gen` | integer | Generation counter, incremented whenever anything in the reply could have changed. The page repaints only when it changes. |
| `ui` | string | Current root UI: `session`, `arranger`, `clip`, `other`. The page only renders rows for `session` and `arranger`. |
| `layout` | string | `rows` or `grid` (session view only). Tells the page which physical axis the rows map to. |
| `song` | string | Song name, may be empty. |
| `yScroll` / `xScroll` | integer | Current scroll offsets, for the page to show "more above/below" hints. |
| `playing` | boolean | Playback state. |
| `rows` | array of 8 | One entry per pad row, `y` from 7 (top) to 0 (bottom), always present. |
| `y` | integer | Pad row index. |
| `type` | string | `synth`, `kit`, `midi`, `cv`, `audio`, or `none` for an empty row. Maps from `OutputType`. |
| `name` | string | Output display name as the OLED would show it (user name, or a generated one such as `MIDI 3`). |
| `clip` | string | The clip's own name, empty when unset. |
| `colour` | string | Six hex digits, the colour the pad row is rendered with. |
| `playing` | boolean | Clip is currently active/playing. |
| `muted` | boolean | Clip is stopped (not active and not soloed). |
| `soloed` | boolean | Clip is soloing in session mode. |
| `armed` | boolean | Clip has a pending launch or stop. |

Rules:

- The reply must be **7-bit clean**. Names are user text and the Deluge allows
  characters outside ASCII; the handler escapes any byte with the high bit set as
  `\u00XX` and escapes `"` and `\` per JSON. Nothing in the reply may be `F7`.
- Rows are always eight entries so the page can index by `y` without bounds checks.
- In the **grid layout** the eight pad rows correspond to clips within the visible
  tracks rather than to tracks. The first version reports the track (column) under
  each of the leftmost eight columns as `rows[]` and sets `layout` to `grid`, so
  the phone can be laid along the top edge in landscape. A later version may add a
  `cols` array instead.
- In the **arranger** each row is an output; `clip` is empty and play state reflects
  the output's mute/solo state.
- In **clip view** or any other UI, `rows` is still present but every entry has
  `type: "none"`, and `ui` tells the page why.
- Reply size is roughly 600 to 900 bytes, well within what the existing file API
  already sends per message.

### 4.3 Implementation notes

- Add a `view` branch to the dispatch in `smSysex::handleNextSysEx` next to `ping`,
  calling a new `smSysex::sendView(cable, reader)`.
- Build the reply with the existing `JsonSerializer` (`startReply`,
  `writeOpeningTag("^view", ...)`, `writeAttribute`, `writeArrayStart("rows", ...)`,
  `sendMsg`) exactly as the file handlers do.
- Row resolution reuses `sessionView.getClipOnScreen(y)` and
  `View::displayOutputName` logic (factor the name-derivation into a helper that
  writes into a `String` rather than to the display).
- `gen` is a global counter bumped from the same places that mark the session
  view's grid rows for redraw (scroll, layout change, clip launch state change,
  rename, song load). If that proves too invasive, the first version can bump it
  every time `sendView` runs and let the page compare the row payload instead.
- The handler runs in the `handleNextSysEx` task, which already avoids running
  while the SD card is in use. It must not run from the USB receive callback.
- Rate limiting: the page polls at 4 to 5 Hz. No firmware throttle is needed at
  that rate, but the handler must be cheap enough not to affect audio; it only
  reads existing state and writes a few hundred bytes.
- Names containing non-ASCII bytes are the only place the reply can break the
  SysEx frame. Test with a synth named using the Deluge's extended characters.

### 4.4 Later: push mode

Polling is simple and sufficient for the evaluation. If the display is kept, add a
lease-based push in the same style as the OLED mirror in `hid_sysex.cpp`: the page
sends `{"view":{"stream":true}}`, the firmware records `viewStreamUntil = now + 2 s`
and sends an unsolicited `^view` whenever `gen` changes while the lease is live.
The page renews the lease every second. Unsolicited replies carry the last known
sequence byte.

## 5. Web page

### 5.1 Transport

The page talks to the Deluge through a tiny transport interface:

```js
interface Transport {
  connect(): Promise<void>;
  send(bytes: Uint8Array): void;
  onMessage: (bytes: Uint8Array) => void;
}
```

Implementations:

1. **Web MIDI** (first version). Uses `navigator.requestMIDIAccess({sysex: true})`,
   picks the port whose name contains `Deluge`, and filters incoming SysEx by the
   vendor header. Works in Chrome/Edge on desktop and in Web MIDI capable iOS
   browsers such as *Web MIDI Browser*. Plain Safari has no Web MIDI.
2. **WebSocket** (bridge phase). Raw MIDI bytes both ways to a bridge device. Added
   only when a bridge exists.

All SysEx parsing and JSON handling lives in the page. The bridge, when it exists,
is a byte relay or a JSON cache and knows nothing about rows.

### 5.2 Behaviour

- On connect: send `ping`, wait for `^ping`, then start polling `view` at 250 ms.
- Repaint when `gen` changes. Show a disconnected banner if no reply arrives for
  two seconds and keep retrying.
- Save the last good reply so a reconnect does not blank the display.

### 5.3 Rendering

- Portrait, eight rows filling the viewport height, dark background, large type
  readable from arm's length.
- Each row: colour swatch, type glyph (synth, kit, MIDI, CV, audio), output name in
  large type, clip name in smaller type, and a status dot (playing, stopped, soloed,
  armed).
- Empty rows render dim with no text.
- In `grid` layout or `arranger` the row semantics change; the page shows a small
  label naming the current layout so the user knows what the rows mean.
- When `ui` is not `session` or `arranger`, dim the rows and show the UI name.

### 5.4 Alignment

Pad pitch on the Deluge is fixed, but where the phone sits relative to the grid is
not. A calibration control (hidden behind a long press) exposes:

- row pitch in CSS pixels,
- vertical offset of the first row,
- text scale.

Values persist in `localStorage`. Defaults target the iPhone 16 Pro Max in portrait.

### 5.5 iOS notes

- No wake lock over plain `http` or `file` URLs; set Auto-Lock to Never for a
  session, or serve over `https` later.
- The page may be opened from a file the Web MIDI Browser app can load, or served
  from a laptop on the same network during evaluation.
- Add-to-Home-Screen works without a service worker; offline caching does not.

## 6. Evaluation setup (no bridge)

Purpose: decide whether the display is worth a wireless bridge before building one.

1. Build firmware with the `view` query and flash it.
2. Connect the Deluge to a computer over USB and open the page in Chrome. Verify
   ping, then rows, then scrolling and launch states.
3. Connect the Deluge's USB device port to the iPhone with a USB-C to USB-B cable.
   iOS is a class-compliant USB MIDI host; the Deluge is self-powered.
4. Open the page in a Web MIDI capable iOS browser. Confirm it grants SysEx (ping
   must return pong), then place the phone beside the grid and play for a session.

Success criterion: the user finds themselves reading names from the phone rather
than pressing pads to see them on the OLED.

## 7. Wireless bridge (deferred)

Two designs are possible with hardware already on hand: an Adafruit Feather
nRF52840 Express and an AirLift FeatherWing (ESP32 Wi-Fi co-processor on SPI).

| | Bluetooth MIDI relay | Wi-Fi state cache |
|---|---|---|
| Hardware | Feather alone | Feather + AirLift |
| Deluge side | USB MIDI device on the Deluge host port | Same |
| Phone side | BLE MIDI via CoreMIDI | Phone joins the Feather's own access point |
| Browser | Web MIDI capable app | Plain Safari |
| Bridge logic | Copies bytes USB to BLE and back | Polls `view` itself, caches the JSON, serves it over HTTP |
| Bidirectional | Yes | No |
| Extras later (OLED mirror, file browser) | Possible | Needs a different design |
| Known risks | SysEx fragmentation over BLE packets, BLE MIDI throughput, composite USB enumeration on the Deluge host port | ESP32SPI throughput, access point mode in NINA firmware, no WebSockets, Wi-Fi power bursts from the host port |

Both designs are CircuitPython. For either, `boot.py` must disable the CDC, mass
storage and HID interfaces so the Deluge sees a plain MIDI device.

The Wi-Fi cache is the better fit if the goal stays "show me the names". The BLE
relay is the better foundation if the page grows into a remote control. The page
transport abstraction is designed so either can be added without touching the
rendering code.

## 8. Later extensions

- Tempo from MIDI clock: computed in the page with no firmware change.
- OLED mirror pane using the existing `HID` SysEx stream from `hid_sysex.cpp`.
- Current section and song position.
- SD card browser using the existing file API.
- Push mode for the `view` query (section 4.4).

## 9. Repository layout

```
docs/dev/rows_sidecar_app.md          this document
src/deluge/storage/smsysex.cpp        `view` handler
contrib/rows_sidecar/index.html       single-file page
contrib/rows_sidecar/README.md        setup and calibration notes
contrib/rows_sidecar/bridge/          CircuitPython bridge scripts (deferred)
```

## 10. Open questions

- Should `gen` be a real change counter, or is a per-reply counter plus payload
  comparison in the page good enough for the first version?
- Grid layout: report tracks along the top edge (current plan) or the clips of one
  selected track down the side?
- Where should the output display-name derivation live so both the OLED code and
  the `view` handler share it without duplicating `drawOutputNameFromDetails`?
- Does the Web MIDI Browser app on iOS pass SysEx of 900 bytes intact? Needs a test
  before trusting evaluation results.

## 11. Milestones

1. Firmware `view` query, verified from a desktop browser.
2. Page over Web MIDI with calibration, verified on the desktop.
3. Wired iPhone evaluation.
4. Decision point: build a bridge or stop.
5. Bridge, push mode, and extensions as warranted.
