# Geekbar Pulse X LED Driver

Reverse-engineered driver for the LED light-guide film salvaged from a
Geekbar Pulse X disposable vape. The film is driven by a `PLS916H` LED
matrix controller (8-bit dimming, up to 144 channels) over a modified
2-wire SPI-like protocol.

Protocol reverse-engineering credit:
- https://gist.github.com/The5thZone/fcb294b999651d99634e3481fc5ee4ed
- https://github.com/schlae/vapere

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

Host-controlled serial protocol (115200 baud, newline-terminated):

- `L <index>` -- clear all, light only `<index>` (0-143) at full brightness
- `F <i1> <i2> ...` -- clear all, light every listed index
- `C` -- clear all
- `A` -- light all LEDs

## LED index -> physical position mapping (`map_leds.py`)

Automates building an `index -> pixel coordinate` map by stepping through
every LED one at a time and photographing it with a webcam, thresholding
for the brightest blob per frame.

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
  indices map to one LED; a few genuinely drive two at once)
- `led_map_overlay.png` -- point-cloud schematic
- `led_map_on_photo.png` -- same points overlaid on a reference photo
- `led_map_dots_transparent.png` -- dots only, transparent background,
  for manually overlaying on your own photo
- `captures/` -- the raw photo captured for each index

## Known layout notes

- Indices 0-15 form a three-digit seven-segment-style display (the
  original device's battery percentage readout, e.g. "100").
  - Indices `7, 15` are a 2-segment "1" (leading digit).
  - Indices `0,1,2,3,4,5,6` are one full 7-segment digit: top=0,
    upper-left=5, upper-right=1, middle=6, lower-left=4, lower-right=2,
    bottom=3.
  - Indices `8,9,10,11,12,13,14` are the other 7-segment digit: top=8,
    upper-left=13, upper-right=9, middle=14, lower-left=12,
    lower-right=10, bottom=11.
- The film is a genuine light-guide: a lit LED's light visibly bleeds
  along the printed constellation lines to neighboring points, not just
  at its own position.
