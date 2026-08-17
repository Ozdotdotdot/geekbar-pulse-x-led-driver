# Geekbar Pulse X LED Driver

![demo](media/demo.gif)

Full-length video with audio: [`media/demo.mp4`](media/demo.mp4)
(the gif above is a trimmed, silent 15s preview -- generated with
`scripts/demo.py`, see [Demo](#demo) below).

Reverse-engineered driver for the LED light-guide film salvaged from a
Geekbar Pulse X disposable vape. The film is driven by a `PLS916H` LED
matrix controller (8-bit dimming, 144 individually addressable channels)
over a modified 2-wire SPI-like protocol, controlled here by an ESP32 over
a simple host-driven serial command set.

Protocol reverse-engineering credit:
- https://gist.github.com/The5thZone/fcb294b999651d99634e3481fc5ee4ed
- https://github.com/schlae/vapere

## This project's contribution: automated physical LED mapping

The prior work above got a logic analyzer on the film's clock/data lines
and worked out the `PLS916H` command structure -- that's what makes it
possible to address any of the 144 LEDs individually at all. What was
still missing was knowing *which* of those 144 channel indices lights up
*where* on the physical film -- without that, you can turn LEDs on and off,
but you can't build a deliberate animation that traces a specific
constellation line or fills a specific digit segment.

Getting that mapping by eye (photograph each index, squint, guess
coordinates) doesn't scale to 144 channels. Instead, `map_leds.py` drives
it end to end with a webcam pointed straight down at the film in a dark
room:

1. Step through indices `0..143` one at a time, sending the single-LED-on
   command (`L <index>`) for each.
2. On every index, grab a frame and threshold it for the single brightest
   blob -- that blob's centroid is that index's physical `(x, y)` position.
3. Repeat for all 144, and write out `led_map.json`: index -> pixel
   centroid.

144 photos later, that point cloud gets overlaid on a reference photo of
the actual film (`led_map_on_photo.png`) and hand-annotated
(`reference_map_annotated.png`) to identify the named groups -- the two
constellations, the digit clusters, the ring, the X icon, the stray
background stars -- that `include/led_groups.h` and `internal/leds/groups.go`
are built from. That's what turns "144 independently addressable dimmable
channels" into "a ring that can progress-fill clockwise" and "a digit
slot that can render `0-9` in 7-segment" -- the mapping is the missing
piece between the reverse-engineered protocol and anything that looks
intentional on screen.

See [LED index -> physical position mapping](#led-index---physical-position-mapping)
below for how to run it yourself.

## Wiring

Flex tab pins -> classic ESP32-WROOM-32 dev board:

| Flex tab pin | ESP32 pin | Notes |
|---|---|---|
| `G` | GND | |
| `C` (clock) | IO18 | |
| `D` (data) | IO23 | |
| `V` | 3.3V for bring-up, or direct battery (3.0-4.2V) for full brightness | On some boards the `V` test point isn't wired to the chip's VCC pin -- check continuity, jumper to the nearby capacitor (e.g. `C2`) if needed |

Avoid the pins silkscreened `CLK`/`SD0-3`/`CMD` on typical ESP32 dev
boards -- those are wired to the module's onboard SPI flash, not general
purpose I/O.

## Firmware (`src/main.cpp`)

PlatformIO project, `env:esp32dev`. Build/upload:

```
pio run -t upload --upload-port /dev/ttyUSB0
```

Named LED groups (constellations, digit slots, ring, stray stars) live in
`include/led_groups.h`, derived from `led_map.json` -- see [LED index ->
physical position mapping](#led-index---physical-position-mapping) below.

## Demo

`scripts/demo.py` drives the board through a curated sequence covering
everything above at once -- constellations tracing in, a bounce with the
background stars twinkling underneath, the ring's progress-fill and
loading-sweep, a digit readout, and a finish blink. It's what produced
the video/gif at the top of this README.

```
.venv/bin/python3 scripts/demo.py --port /dev/ttyUSB0
```

## Computer integration (`timerled/`)

`timerled` is a small Go daemon + CLI that turns the panel into an always-on
desk fixture reacting to the machine it's plugged into, instead of a script
you run by hand. Only one process can hold the serial port at a time, so the
design mirrors `mpd`/`mpc`: `timerled daemon` is a long-running process that
owns the port and runs the state machine; every other subcommand is a thin
client that talks to the running daemon over a Unix socket.

**States**, highest priority first:

- **Off** -- forced manually with `timerled off`, persists across sleep/wake
  until `timerled normal`.
- **Pomodoro** -- started with `timerled pomodoro <duration>` (Go's
  `time.ParseDuration`, e.g. `timerled pomodoro 1h20m`). All decorations off,
  single-purpose: the ring drains from full as time runs out, the clock
  digits show remaining time (`HH:MM` above an hour left, `MM:SS` under),
  finishes with two hard blinks across every LED.
- **Music** -- entered automatically whenever `mpc status` reports
  `[playing]`. Constellations paint themselves anew (the `B` trace-and-breathe
  reveal) on every new track, stray stars twinkle in the background, the ring
  tracks song progress, the clock digits keep ticking alongside all of it. On
  pause the decorations clear and the center X icon lights instead; after 5
  idle minutes it falls back to Regular.
- **Regular** -- the default/idle state: constellations built and held,
  background twinkle, clock digits at brightness 160, with the 5x
  bounce-and-settle animation (`Y`) replaying on every hour.

Sleep and wake are handled by subscribing directly to logind's
`PrepareForSleep` D-Bus signal (no root scripts needed) -- the panel blanks
before suspend and redraws after resume. Since the ESP32 is USB-powered off
the same machine, resume also reopens the serial port rather than trusting
the old handle, since the board very likely reset when USB power dropped.

**Setup:**

```
cd timerled
go build -o ~/.local/bin/timerled ./cmd/timerled
cp timerled.service ~/.config/systemd/user/
systemctl --user enable --now timerled.service
```

**CLI:**

| Command | Effect |
|---|---|
| `timerled daemon` | run the daemon in the foreground (what the systemd unit calls) |
| `timerled pomodoro <duration>` | start a countdown, e.g. `timerled pomodoro 25m` |
| `timerled cancel` | cancel an active pomodoro |
| `timerled status` | print the current mode, and time remaining if a pomodoro is active |
| `timerled off` | force all LEDs off |
| `timerled normal` | clear a manual off / cancel a pomodoro, resume automatic regular/music state |

`internal/leds/groups.go` mirrors the digit segment maps and the X icon
indices from `led_groups.h`/`main.cpp` -- keep them in sync if the physical
mapping ever changes.

### Web dashboard

The daemon can optionally serve a small dashboard for controlling the panel
from a browser -- brightness sliders (stray stars, constellations, clock
digits), one-shot animation triggers (reveal / bounce / ring sweep / blink),
pomodoro start/cancel, and off/resume-automatic. It talks to the daemon's
main loop the same way the CLI does (through the same request channel), so
it's just another thin client, not a second thing driving the serial port.

Enable it by setting `TIMERLED_HTTP_ADDR` before starting the daemon (unset
= dashboard disabled):

```
TIMERLED_HTTP_ADDR=:8420 timerled daemon
```

or uncomment the `Environment=TIMERLED_HTTP_ADDR=...` line in
`timerled.service`. `:8420` binds all interfaces, so it's reachable from any
device on the LAN at `http://<host-ip>:8420` -- there's no authentication,
so only enable it on a network you trust.

Brightness changes apply live (an on-device fade, not waiting for the next
mode transition); animation triggers are fire-and-forget and don't change
the automatic Regular/Music/Pomodoro state, so the next tick keeps
rendering normally on top of them.

## Serial protocol

115200 baud, one command per line, newline-terminated. The firmware
prints `OK` after handling a command, so a host script can wait for
acknowledgement before moving on. Full protocol reference is also kept as
a comment block directly above `loop()` in `src/main.cpp`.

**Direct control**

| Command | Effect |
|---|---|
| `L <index>` | clear all, light only `<index>` (0-143) at full brightness |
| `F <b> <i1> <i2> ...` | clear all, light every listed index at brightness `<b>` (0-255) |
| `G <b> <group>` | clear all, light a named group at brightness `<b>` |
| `C` | clear all |
| `A` | light all 144 LEDs at full brightness |

**Path animations** (for `constellation_left`, `constellation_right`, or
`both_constellations` -- see [Groups](#groups) below)

| Command | Effect |
|---|---|
| `P <steps> <ms> <path...>` | on-device crossfade along an arbitrary path of indices, one-shot |
| `X <n> <steps> <ms> <path...>` | ping-pong crossfade along a path, forward+backward, repeated `<n>` times |
| `W <steps> <ms> <peakB> <group>` | wavefront sweep root->tips, one-shot, peaking at `<peakB>` |
| `R <steps> <ms> <peakB> <group>` | reveal and hold: same sweep, but each depth stays lit instead of fading out, ending fully glowing |
| `Y <bounces> <decay> <steps> <ms> <peakB> <group>` | wavefront bounces root<->tip `<bounces>` times, losing `<decay>` brightness per round trip; the final return-to-root leg holds each depth lit as it passes instead of fading, ending fully lit at `<peakB>` |
| `Z <steps> <ms> <peakB> <group>` | independent pulse-bounce simulation: each branch bounces on its own, junctions fork, colliding pulses reflect instead of forking, decays and stops on its own |
| `B <steps> <ms> <flashDownMs> <flashUpMs> <peakB> <group>` | trace and pulse: reveal root->tip holding as it goes, breathe the fully-lit tree down to 0 and back up twice (eased, not an instant cut), sweep root->tip extinguishing depths one at a time, then sweep back tip->root holding as it relights -- ends fully lit |

All brightness ramps in the path animations use an eased sine curve
(`easeInOutSine` in `main.cpp`), not a linear fade, so they read as a
breathing motion rather than a mechanical one.

**Twinkle**

| Command | Effect |
|---|---|
| `T <ms> <frameMs> <minB> <maxB> <group>` | independent random twinkle per LED for `<ms>` total duration (`constellation_left`, `constellation_right`, `stray_stars`, or `all`) |
| `N <ms> <frameMs> <minB> <maxB> <staticB> <group>` | `stray_stars` twinkle while `<group>` (a constellation, or `both_constellations`) stays lit solidly at `<staticB>` |
| `K <minB> <maxB> <group>` | arm a background twinkle (`stray_stars` or `all`) that keeps flickering underneath whatever `W`/`R`/`Y`/`Z`/`B` animation runs next, and keeps animating on its own once idle |
| `K off` | disarm the background twinkle |
| `U <i1> <b1> <i2> <b2> ...` | non-destructive pixel set: writes each `(index, brightness)` pair without clearing anything else already lit -- lets a host compose several independent layers (a held constellation, clock digits, the X icon) into one frame without one erasing another |

**Ring** (the 14-LED ring around the center X icon)

| Command | Effect |
|---|---|
| `I <subSteps> <ms> <brightness>` | progress-fill, clockwise from LED 30, single pass, each LED eases in and stays lit -- good for a literal countdown/progress indicator |
| `S <ms> <brightness> <decayPercent> <laps>` | radar/second-hand sweep: a bright LED with a decaying tail, going around `<laps>` times -- reads as a "working" / loading indicator |
| `J <filledCount> <brightness>` | instant, non-blocking ring progress set (0-14 filled) -- unlike `I`, this doesn't block the ESP32, so a host-side timer/clock can drive its own pace over long durations and stay responsive to other commands in between |

**Digits** (the two 3-character "100%" style clusters, repurposed as four
7-segment digit slots)

| Command | Effect |
|---|---|
| `D <brightness> <topA> <topB> <bottomA> <bottomB>` | render a digit 0-9 (or `X` for blank) in each of the four slots at once, e.g. as a clock face reading `topA topB : bottomA bottomB` |

**Attention**

| Command | Effect |
|---|---|
| `H <times> <onMs> <offMs> <group>` | hard step blink (no easing, full on/off cut) `<times>` times, `<group>` is `all` or `ring` -- e.g. signaling a timer has finished |

### Groups

Named groups referenced by the commands above (defined in
`include/led_groups.h`):

- `constellation_left`, `constellation_right` -- the two branching
  constellation graphics
- `both_constellations` -- both, animated in perfect depth-sync as if one
  tree (the shorter right side simply finishes first and holds)
- `stray_stars` -- the 45 genuinely unconnected background stars (not
  part of any constellation line, digit, icon, or the ring)
- `all` -- all 144 LEDs
- `ring` -- the 14-LED ring around the center X icon (`H` only)
- `x_icon` -- the 4-arm X icon in the middle of the ring (`G` only; use `U`
  to turn it on/off without clearing the rest of the frame)

## Known layout notes

- **Digit clusters**: two clusters, each a 3-character "100%" style
  readout repurposed as two independent 7-segment digits plus a leading
  2-segment "1" (unused by the `D` command). The bottom cluster's indices
  are exactly the top cluster's indices `+ 40` -- confirmed independently
  via `led_map.json` centroid geometry, not just assumed from the offset.
  - `topA` = indices `0-6` (root `7`/`15` unused), `topB` = `8-14`
  - `bottomA` = indices `40-46`, `bottomB` = `48-54`
  - Segment order within each slot: top, upper-left, upper-right, middle,
    lower-left, lower-right, bottom
- **Ring**: 14 LEDs (`30-39`, `56-59`) around the center X icon. Plain
  ascending index order is already the correct physical clockwise order
  starting near the top -- confirmed via centroid angle from
  `led_map.json` (LED 30 sits at ~12 degrees off vertical, angle
  increases monotonically clockwise through LED 59 at ~347 degrees).
- **X icon**: 4 arms of 3 LEDs each (`16-27`), in the middle of the ring --
  confirmed via `led_map.json` centroids against the user's annotated
  reference photo. Indices `28, 29` sit nearby in image space but are
  actually part of the separate lightning-bolt icon, not this one.
- **Icons**: a lightning bolt (LEDs `74, 76`) above the top digit
  cluster, and a "juice drop" icon (LEDs `60, 61, 112, 113`) below the
  bottom cluster -- small, tightly-packed, individually addressable
  clusters, not part of any digit or the ring.
- **Multi-LED-per-index dots**: indices `128` and `138` each drive two or
  three physically separate LEDs wired together to the same output --
  they can be turned on/off/dimmed together but not addressed
  independently.
- The film is a genuine light-guide: a lit LED's light visibly bleeds
  along the printed constellation lines to neighboring points, not just
  at its own position.

## LED index -> physical position mapping

`map_leds.py` automates building an `index -> pixel coordinate` map by
stepping through every LED one at a time and photographing it with a
webcam, thresholding for the brightest blob per frame.

```
python3 -m venv .venv
.venv/bin/pip install opencv-python-headless numpy pyserial pillow
.venv/bin/python3 map_leds.py --port /dev/ttyUSB0 --camera 0
```

Best run in as dark a room as possible -- ambient light and the film's
own phosphorescent ink (which glows for a while after exposure to bright
light) both add false positives to the blob detection.

Outputs:
- `led_map.json` -- index -> `[[x, y], ...]` pixel centroids (most
  indices map to one LED; a few genuinely drive two or three at once)
- `led_map_overlay.png` -- point-cloud schematic
- `led_map_on_photo.png` -- same points overlaid on a reference photo
- `captures/` -- the raw photo captured for each index

`scripts/generate_bw_map.py` re-renders `led_map.json` as a white-
background, black-ink, printable map (`led_map_dots_bw.png`) -- run it
any time after `led_map.json` changes:

```
.venv/bin/python3 scripts/generate_bw_map.py
```

### Reference images

Several versions of the same map, useful for different purposes:

- `reference_map_annotated.png` -- hand-traced directly on a photo of the
  actual film (constellation lines drawn in, every index labeled) --
  the most legible starting point if you're orienting a physical board
  against this repo
- `led_map_on_photo.png` -- automated points overlaid on a reference
  photo
- `led_map_dots_bw.png` -- white background, black ink, printable

## Known limitations

- The mapping data reflects one specific salvaged board (`BD0053_V05`
  silkscreen); other Geekbar Pulse X revisions may differ.
- A handful of indices in `led_map.json` were captured under imperfect
  conditions and may be off by a few pixels -- good enough for the named
  groups in `led_groups.h`, but treat it as a solid starting point rather
  than a guaranteed-perfect ground truth.
