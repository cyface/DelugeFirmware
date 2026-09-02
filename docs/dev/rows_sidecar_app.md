# Rows Sidecar App – Specification

Status: implemented. The firmware `view` query and the web page are in the tree; the
wireless bridge (section 7) is still deferred. Section 4.2 describes what the firmware
actually sends.

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
{"^view": { "ui": "session", "layout": "rows", "song": "Night Drive",
"yScroll": 0, "xScroll": 0, "playing": 1,
"rows": [{ "y": 7, "t": "synth", "n": "Lead Pluck", "k": "ff8000", "s": 1, "x": 1},
         { "y": 6, "t": "kit", "n": "Drums 1", "c": "Verse", "k": "22aaff", "s": 1, "x": 2},
         { "y": 5, "t": "none"}], "gen": 179159252}}
```

Keys are short because the whole reply has to fit one SysEx frame (see the rules below);
a full eight rows of named clips is around 600 bytes.

| Field | Type | Meaning |
|---|---|---|
| `ui` | string | Current root UI: `session`, `arranger`, `clip`, `other`. The page only renders rows for `session` and `arranger`. |
| `layout` | string | `rows`, `grid` or `arranger`. Tells the page what an entry means. |
| `song` | string | Song name, empty for an unsaved song. |
| `yScroll` / `xScroll` | integer | Current scroll offsets. `xScroll` is only meaningful in the grid layout. |
| `playing` | 0/1 | Whether a clock is running. |
| `rows` | array of 8 | One entry per row, in the order the page should display them: topmost pad row first. |
| `gen` | integer | FNV-1a hash of everything else in the reply. The page repaints only when it changes, so no firmware state has to track staleness. |

Per row:

| Field | Type | Meaning |
|---|---|---|
| `y` | integer | Pad row index, 7 at the top down to 0. In the grid layout it is the track column index instead, counting from the left. |
| `t` | string | `synth`, `kit`, `midi`, `cv`, `audio`, or `none` for an empty row. An empty row carries `y` and `t` and nothing else. |
| `n` | string | Output display name as the OLED would show it (the user's name, or a generated one such as `MIDI 3`). |
| `c` | string | The clip's own name. Omitted when unset, and the first thing dropped when the reply is running out of room. |
| `k` | string | Six hex digits: the section colour, which is what the section pad column shows for that row. |
| `s` | integer | State bits: 1 active, 2 soloing, 4 armed to launch or stop, 8 armed to record. |
| `x` | integer | Section number, 1-based. Omitted when the row has no clip. |

Rules:

- The reply is **7-bit clean**. Names are user text and the Deluge allows characters
  outside ASCII; the handler escapes any byte with the high bit set as `\u00XX`, escapes
  `"` and `\`, and so can never put an `F7` or an 8-bit byte inside the frame.
- The reply is capped at 740 bytes so it fits inside one 752-byte frame, which is as far
  as some host MIDI stacks are dependably transparent (see the byte-750 CoreMIDI bug).
  The cap is enforced by shrinking text: each row gets a share of what is left, the clip
  name goes before the output name does, and a name is truncated rather than dropped.
- Rows are always eight entries so the page can index by position without bounds checks.
- In the **grid layout** the eight pad rows are sections, not tracks, so the query
  reports the eight leftmost track columns instead and sets `layout` to `grid`. `y` is
  then the column index and `c` is absent.
- In the **arranger** each entry is an output; `c` is absent and the state bits reflect
  the output's arrangement mute/solo state.
- In **clip view** or any other UI, `rows` is still present but every entry is
  `{"y": n, "t": "none"}`, and `ui` says why. The page holds its last good picture and
  greys it out rather than blanking.

### 4.3 Implementation notes

- Add a `view` branch to the dispatch in `smSysex::handleNextSysEx` next to `ping`,
  calling a new `smSysex::sendView(cable, reader)`.
- Build the reply with the existing `JsonSerializer` (`startReply`,
  `writeOpeningTag("^view", ...)`, `writeAttribute`, `writeArrayStart("rows", ...)`,
  `sendMsg`) exactly as the file handlers do.
- Row resolution goes through `SessionView::getViewQueryRow`, which covers both session
  layouts (`getClipOnScreen` returns nothing in the grid layout unless a pad is held).
  Name derivation is a small local helper mirroring the name half of
  `View::drawOutputNameFromDetails`; sharing that code properly would mean splitting the
  display work out of it, which is a bigger change than this query justifies.
- `gen` is written last so it can be an FNV-1a hash of the payload in front of it. That
  needs no hooks anywhere else in the firmware and changes exactly when the reply's
  content changes.
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
src/deluge/gui/views/session_view.cpp getViewQueryRow, the row lookup it uses
contrib/rows_sidecar/index.html       single-file page
contrib/rows_sidecar/README.md        setup and calibration notes
contrib/rows_sidecar/serve.py         local HTTP server, for loading the page on a phone
contrib/rows_sidecar/bridge/          CircuitPython bridge scripts (deferred)
.claude/skills/deluge-flash/hw_view.py  command-line probe for the query
```

## 10. Open questions

- Grid layout: tracks along the top edge (what is implemented) or the clips of one
  selected track down the side? Undecided until the grid layout is actually used
  with the display.
- Where should the output display-name derivation live so both the OLED code and the
  `view` handler share it without duplicating `drawOutputNameFromDetails`? For now the
  handler has its own small copy.
- Does the Web MIDI Browser app on iOS pass a ~700 byte SysEx intact? The 740-byte cap
  makes this likely, but it is untested.

Settled:

- `gen` is a hash of the payload, not a change counter (section 4.3).

## 11. Milestones

1. ~~Firmware `view` query, verified from a desktop browser.~~ Done; `hw_view.py` is the
   command-line probe.
2. ~~Page over Web MIDI with calibration.~~ Done.
3. Wired iPhone evaluation. ← here
4. Decision point: build a bridge or stop.
5. Bridge, push mode, and extensions as warranted.
