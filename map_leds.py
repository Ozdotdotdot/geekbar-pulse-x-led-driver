#!/usr/bin/env python3
"""
Automated LED index -> physical position mapper for the Geekbar Pulse X
PLS916H flex tab.

Requires the ESP32 running the "host-controlled scan mode" firmware in
src/main.cpp (protocol: "L <index>" lights one LED, prints "OK").

Usage:
    .venv/bin/python3 map_leds.py --port /dev/ttyUSB0 --camera 0

Do this in as dark a room as you can manage, with the camera on a tripod
(or just propped still) pointed straight at the flex tab, not moving
between shots. The darker the room, the cleaner the blob detection.

Output:
    led_map.json          -- index -> list of [x, y] pixel centroids
    led_map_overlay.png   -- composite photo with every LED position
                              plotted and labeled, i.e. a first-draft
                              schematic
    captures/NNN.jpg       -- raw photo taken for each index (for review /
                              re-processing if thresholds need tuning)
"""

import argparse
import json
import time
from pathlib import Path

import cv2
import numpy as np
import serial

NUM_LEDS = 144


def send_and_wait(ser: serial.Serial, cmd: str, timeout: float = 2.0) -> None:
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line == "OK":
            return
    raise TimeoutError(f"No OK received for command: {cmd!r}")


def grab_settled_frame(cap: cv2.VideoCapture, warmup: int = 3) -> np.ndarray:
    frame = None
    for _ in range(warmup):
        ok, frame = cap.read()
        if not ok:
            raise RuntimeError("Failed to read from camera")
    return frame


def find_blobs(frame_bgr: np.ndarray, brightness_thresh: int, min_area: int):
    gray = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY)
    _, mask = cv2.threshold(gray, brightness_thresh, 255, cv2.THRESH_BINARY)
    mask = cv2.dilate(mask, np.ones((3, 3), np.uint8), iterations=1)

    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    centroids = []
    for c in contours:
        area = cv2.contourArea(c)
        if area < min_area:
            continue
        M = cv2.moments(c)
        if M["m00"] == 0:
            continue
        cx = M["m10"] / M["m00"]
        cy = M["m01"] / M["m00"]
        centroids.append((cx, cy))
    return centroids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--brightness-thresh", type=int, default=200,
                     help="0-255 grayscale cutoff for 'this pixel is an LED'")
    ap.add_argument("--min-area", type=int, default=4,
                     help="minimum blob area in pixels to count as a real LED, not noise")
    ap.add_argument("--settle-ms", type=int, default=150,
                     help="delay after lighting an LED before the photo is taken")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--end", type=int, default=NUM_LEDS)
    args = ap.parse_args()

    out_dir = Path("captures")
    out_dir.mkdir(exist_ok=True)

    print(f"Opening serial {args.port} @ {args.baud}...")
    ser = serial.Serial(args.port, args.baud, timeout=1)
    time.sleep(2)  # let the ESP32 finish its post-flash reset
    ser.reset_input_buffer()

    print(f"Opening camera {args.camera}...")
    cap = cv2.VideoCapture(args.camera)
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    # Lock exposure/white-balance if the backend supports it, so brightness
    # thresholding stays consistent across all 144 shots instead of the
    # camera re-adjusting exposure every frame.
    cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 1)  # 1 = manual on many UVC drivers
    cap.set(cv2.CAP_PROP_AUTOFOCUS, 0)

    send_and_wait(ser, "C")  # all off, baseline

    mapping = {}
    composite = None

    for idx in range(args.start, args.end):
        send_and_wait(ser, f"L {idx}")
        time.sleep(args.settle_ms / 1000.0)

        frame = grab_settled_frame(cap)
        cv2.imwrite(str(out_dir / f"{idx:03d}.jpg"), frame)

        if composite is None:
            composite = np.zeros_like(frame)

        blobs = find_blobs(frame, args.brightness_thresh, args.min_area)
        mapping[idx] = [[round(x, 1), round(y, 1)] for x, y in blobs]

        tag = "!" if len(blobs) != 1 else " "
        print(f"[{idx:3d}/{NUM_LEDS-1}] blobs={len(blobs)} {tag} {mapping[idx]}")

        for (x, y) in blobs:
            cv2.circle(composite, (int(x), int(y)), 6, (0, 255, 255), 1)
            cv2.putText(composite, str(idx), (int(x) + 8, int(y) - 8),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.35, (0, 255, 255), 1, cv2.LINE_AA)

    send_and_wait(ser, "C")
    ser.close()
    cap.release()

    with open("led_map.json", "w") as f:
        json.dump(mapping, f, indent=2)

    if composite is not None:
        cv2.imwrite("led_map_overlay.png", composite)

    zero_hits = [i for i, v in mapping.items() if len(v) == 0]
    multi_hits = [i for i, v in mapping.items() if len(v) > 1]
    print("\nDone.")
    print(f"  led_map.json        -> {len(mapping)} entries")
    print(f"  led_map_overlay.png -> composite schematic")
    print(f"  {len(zero_hits)} indices detected 0 blobs (need threshold/exposure tuning): {zero_hits}")
    print(f"  {len(multi_hits)} indices detected >1 blob (paired/grouped LEDs): {multi_hits}")


if __name__ == "__main__":
    main()
