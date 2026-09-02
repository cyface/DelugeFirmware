# Rows sidecar

A phone stood beside the Deluge's pad grid, showing the name, type, colour and play
state of the eight rows currently on screen — the things the OLED will only tell you
one at a time, when you press a pad.

`index.html` is the whole thing: one file, no build step. It polls the firmware's
`view` SysEx query over Web MIDI at 4 Hz and repaints when the reply changes.

The design is in [`docs/dev/rows_sidecar_app.md`](../../docs/dev/rows_sidecar_app.md).

## What you need

- Firmware with the `view` query (`smSysex::sendView`, in `src/deluge/storage/smsysex.cpp`).
- A browser with Web MIDI **and** SysEx permission. Chrome or Edge on the desktop;
  on iOS, Safari has no Web MIDI at all, so use an app that provides it
  (*Web MIDI Browser*). Firefox does not implement Web MIDI.

## Desktop first

```bash
python3 contrib/rows_sidecar/serve.py        # prints the URLs
```

Open `http://localhost:8080/` in Chrome and allow MIDI when it asks. (`localhost` is a
secure context, so the permission prompt appears; a bare `http://192.168.…` URL is not,
and Chrome will refuse Web MIDI there — that only matters for the desktop, see below.)

**Chrome 152 on macOS drops all SysEx** in both directions: ports enumerate, permission is
granted, and not one message gets through, while any other MIDI client on the same machine
works. It is the UMP MIDI backend Chrome switched to; disabling it fixes it:

```bash
open -na "Google Chrome" --args --disable-features=MidiMacUmp
```

Verified here on 152.0.7977.76. If a plain Chrome shows *no reply from the Deluge* while
`hw_view.py` prints rows happily, this is why.

The status line at the bottom shows the port state and which Deluge UI is on screen. If
it says *no reply*, the firmware is probably older than the `view` query — check with:

```bash
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/hw_view.py'
DBT_NO_SYNC=1 ./dbt exec 'python3 .claude/skills/deluge-flash/hw_view.py --watch'
```

## On the phone

1. Connect the phone to the Deluge's **device** port with a USB-C to USB-B cable. iOS is
   a class-compliant USB MIDI host and the Deluge is self-powered, so nothing else is
   needed.
2. Put the Mac and the phone on the same Wi-Fi, run `serve.py`, and open the
   `http://<mac-ip>:8080/` URL it prints in a Web MIDI capable browser app.
3. Grant SysEx access when the app asks.
4. Set Auto-Lock to Never for the session — a plain `http` page cannot hold a wake lock.

The browser app's own toolbar is the thing most likely to spoil the alignment: eight rows
at 12.7 mm need about 100 mm of screen, and a toolbar can take the bottom row's worth. If
the app has a full-screen mode, use it. Otherwise either prop the phone up about a
centimetre so its usable screen covers the grid, or use **Fit to screen** in calibration
and accept a slightly compressed pitch.

If the browser app refuses Web MIDI over plain `http` (it is not a secure context), save
`index.html` to the phone and open it from Files instead; everything works from a
`file://` URL too.

## Calibration

Tap **calibrate** at the bottom right, or press and hold anywhere for about a second.

| Control | What it does |
|---|---|
| MIDI port | Which Deluge port to talk to. Port 3 is the SysEx port and is the default. |
| Row pitch | Height of one row. The Deluge's pads are **12.7 mm** apart (measured), which is the default; get the span of all eight right rather than judging one row. |
| Top offset | Slides all eight rows down to meet the top pad row. Depends entirely on how the phone is propped — 70 px was right for one setup. |
| Fit to screen | Gives up exact pad-for-pad pitch and spreads the eight rows over the height that is actually visible. Use it when a browser app's toolbar eats the bottom row: the top and bottom rows still line up, which is most of the value. The line above it says whether the rows currently fit. |
| Text size | Scales the type without changing the row pitch. |
| Pixels per mm | Only used to show the pitch in millimetres. Set it so the striped bar measures 100 mm against a ruler, then the pitch readout is trustworthy. |
| Rotate 180° | For mounting the phone upside down, so the cable points away from you. Rotates the whole page, text included — an iPhone will not rotate into upside-down portrait on its own. |
| Flip top/bottom | Reverses the row order only, leaving the text alone. For when the rows read the wrong way round but the phone is the right way up. |

Values are kept in `localStorage` on that phone.

## What the rows mean

- The colour bar is a colour actually on that row of pads: the middle note row's colour
  for an instrument clip, the clip's own for an audio clip, and the track hue the grid
  layout paints in grid layout. Section colour would be no use — a song living in one
  section would give you eight identical bars.
- A green dot is playing, dim is stopped, yellow is soloing, a blinking dot is armed to
  launch or stop, red is armed to record.
- Empty rows are a dash.
- In grid layout or the arranger the entries are not pad rows; the subtitle says which
  column or output each one is, and the status line names the layout.
- When the Deluge is in a clip, a menu or the browser, the last song view is held on
  screen and greyed out, and the status line says which UI it is now showing.
