#!/usr/bin/env python3
"""Runs a curated sequence across everything the firmware can do --
constellations, the ring, digits, and twinkle -- meant as a one-shot demo
of the whole system, not as an example of any single feature in
isolation. See README.md's serial protocol section for what each command
means individually.
"""
import argparse
import time

import serial


def cmd(ser: serial.Serial, line: str, wait: float = 20.0, hold: float = 0.0) -> None:
    ser.write((line + "\n").encode())
    start = time.time()
    while time.time() - start < wait:
        resp = ser.readline()
        if b"OK" in resp:
            break
    if hold:
        time.sleep(hold)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=20)
    time.sleep(2)  # let the ESP32 finish its post-connect reset
    ser.reset_input_buffer()

    cmd(ser, "C", hold=0.5)

    # constellations trace in together, breathing
    cmd(ser, "B 7 12 450 450 230 both_constellations", wait=15, hold=0.6)

    # arm background stars, bounce both constellations on top
    cmd(ser, "K 1 45 stray_stars")
    cmd(ser, "Y 5 35 6 9 220 both_constellations", wait=12, hold=0.8)

    # settle constellations solid, stars still twinkling
    cmd(ser, "G 180 both_constellations", hold=1.5)

    # ring progress fill (timer look)
    cmd(ser, "I 5 20 230", wait=8, hold=0.8)
    cmd(ser, "K off")
    cmd(ser, "C", hold=0.3)

    # ring loading sweep
    cmd(ser, "K 1 45 stray_stars")
    cmd(ser, "S 35 230 55 3", wait=10, hold=0.5)

    # digit readout
    cmd(ser, "D 220 1 2 3 4", hold=2.0)

    # grand finale
    cmd(ser, "K off")
    cmd(ser, "H 3 250 180 all", wait=10, hold=0.5)
    cmd(ser, "C")

    ser.close()
    print("demo sequence complete")


if __name__ == "__main__":
    main()
