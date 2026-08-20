#!/usr/bin/env python3
"""
Pulls the last mic recording off the watch over serial and saves it as a
real, playable .wav file on this computer.

Usage:
    python3 tools/dump_wav.py [output.wav]

Requires the watch to already be running the mic-test firmware (src/main.cpp)
and to have recorded at least once (press the BOOT button on the watch first).
"""
import base64
import sys
import time

import serial
import serial.tools.list_ports

PORT_HINT = "usbmodem"
BAUD = 115200


def find_port():
    ports = [p.device for p in serial.tools.list_ports.comports() if PORT_HINT in p.device]
    if not ports:
        sys.exit("No matching serial port found. Is the watch plugged in?")
    return ports[0]


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "recording.wav"
    port = find_port()
    print(f"Connecting to {port}...")
    ser = serial.Serial(port, BAUD, timeout=1)
    time.sleep(0.3)
    ser.reset_input_buffer()

    print("Requesting last recording (sending 'd')...")
    ser.write(b"d\n")

    b64_lines = []
    collecting = False
    deadline = time.time() + 15
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(line if not collecting else ".", end="", flush=True) if False else None
        if line == "---WAV-BEGIN---":
            collecting = True
            print("Receiving...")
            continue
        if line == "---WAV-END---":
            print("Done.")
            break
        if line == "No recording yet — press BOOT first.":
            sys.exit("No recording on the watch yet — press BOOT on the watch, then rerun this script.")
        if collecting:
            b64_lines.append(line)

    ser.close()

    if not b64_lines:
        sys.exit("Timed out without receiving any data.")

    wav_bytes = base64.b64decode("".join(b64_lines))
    with open(out_path, "wb") as f:
        f.write(wav_bytes)
    print(f"Saved {out_path} ({len(wav_bytes)} bytes)")


if __name__ == "__main__":
    main()
