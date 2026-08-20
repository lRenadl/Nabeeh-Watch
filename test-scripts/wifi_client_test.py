#!/usr/bin/env python3
"""
Records live microphone audio streamed from the watch over Wi-Fi
(wifi_test/main.cpp) and saves it as a real, playable .wav file here.

The watch only streams while it's in "streaming" mode, toggled by sending
'r' (start) / 's' (stop) over its USB Serial connection — this script does
that for you automatically, timed to the recording length you ask for.

Setup:
    1. Flash the watch:  pio run -e wifi_test -t upload
    2. Find its Wi-Fi IP from the serial monitor (e.g. "Watch IP address:
       192.168.100.201") and its USB serial port with: pio device list
    3. Make sure this computer is on the SAME Wi-Fi network as the watch.
    4. Run (uses the PlatformIO-bundled Python, which already has pyserial —
       plain `python3` likely doesn't):
           ~/.platformio/penv/bin/python3 test-scripts/wifi_client_test.py \
               <watch-ip> <serial-port> [seconds] [output.wav]

       Example:
           ~/.platformio/penv/bin/python3 test-scripts/wifi_client_test.py \
               192.168.100.201 /dev/cu.usbmodem1301 5 my_recording.wav

       Recording length defaults to 5 seconds, output to recording.wav in
       the current directory, if you don't pass them.
"""
import serial
import socket
import struct
import sys
import time

TCP_PORT = 3333
SAMPLE_RATE = 16000
BITS_PER_SAMPLE = 16
CHANNELS = 1


def write_wav_header(f, data_len):
    byte_rate = SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE // 8
    block_align = CHANNELS * BITS_PER_SAMPLE // 8
    f.write(b"RIFF")
    f.write(struct.pack("<I", 36 + data_len))
    f.write(b"WAVE")
    f.write(b"fmt ")
    f.write(struct.pack("<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE, byte_rate, block_align, BITS_PER_SAMPLE))
    f.write(b"data")
    f.write(struct.pack("<I", data_len))


def main():
    if len(sys.argv) < 3:
        sys.exit("Usage: wifi_client_test.py <watch-ip> <serial-port> [seconds=5] [output.wav]")
    host = sys.argv[1]
    port_name = sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    out_path = sys.argv[4] if len(sys.argv) > 4 else "recording.wav"

    print(f"Opening serial port {port_name}...")
    ser = serial.Serial(port_name, 115200, timeout=1)
    time.sleep(0.3)
    ser.reset_input_buffer()

    print(f"Connecting to watch at {host}:{TCP_PORT}...")
    sock = socket.create_connection((host, TCP_PORT), timeout=10)

    print("Starting stream ('r')...")
    ser.write(b"r")

    pcm = bytearray()
    sock.settimeout(1.0)
    start = time.time()
    print(f"Recording for {seconds}s...")
    while time.time() - start < seconds:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            print("Connection closed by watch.")
            break
        pcm.extend(chunk)

    print("Stopping stream ('s')...")
    ser.write(b"s")

    sock.close()
    ser.close()

    with open(out_path, "wb") as f:
        write_wav_header(f, len(pcm))
        f.write(pcm)

    duration = len(pcm) / (SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8)
    print(f"Saved {out_path}: {len(pcm)} bytes (~{duration:.1f}s of audio)")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
