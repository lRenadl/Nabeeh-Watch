#!/usr/bin/env python3
"""
Nabeeh — record labeled training clips straight from the watch's mic.

The audio you save here is exactly what the API receives in real use: the
watch's PDM mic -> DC-blocking filter -> x6 software gain -> PCM16 mono 16 kHz
over TCP port 3333 (see wifi_test/main.cpp).

IMPORTANT: the watch only trusts ONE paired device. The first time you run
this, the laptop has to pair, which replaces the phone's pairing:
    1. On the watch: Settings -> "إلغاء ارتباط الهاتف" (unpair the phone).
    2. Run this script — it pairs automatically and saves the key to
       tools/.recorder_key so later sessions reconnect without re-pairing.
    3. When you're done recording, unpair again on the watch and re-pair the
       phone from the app. (Next recording session: unpair, delete
       tools/.recorder_key, run again.)

Setup (once):
    python -m pip install cryptography

Usage:
    python tools/record_dataset.py                 (finds the watch as nabeeh-watch.local)
    python tools/record_dataset.py 192.168.1.23    (or give its IP if that fails)
    Options: --out D:/nabeeh_watch_audio (default)  --seconds 10

Then follow the prompts. Each take is saved as
    <out>/<class>/<class>__<session>__<subtype>__<NNN>.wav
and a row is appended to <out>/metadata.csv.
"""
import argparse
import csv
import datetime as dt
import hashlib
import hmac
import math
import os
import socket
import struct
import sys
import time

try:
    from cryptography.hazmat.primitives.asymmetric.x25519 import (
        X25519PrivateKey, X25519PublicKey)
    from cryptography.hazmat.primitives import serialization
except ImportError:
    sys.exit("Missing dependency. Run:  python -m pip install cryptography")

TCP_PORT = 3333
SAMPLE_RATE = 16000
BYTES_PER_SEC = SAMPLE_RATE * 2
PAIR_LABEL = b"nabeeh-pair-v1"
KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".recorder_key")

CLASSES = ["adhan", "baby_cry", "doorbell", "fire_alarm", "knock", "na", "quran_recitation"]
META_FIELDS = ["file", "class", "subtype", "session_id", "location", "distance",
               "placement", "sleeve", "background", "seconds", "peak", "rms_dbfs",
               "recorded_by", "timestamp", "notes"]


# ---------------------------------------------------------------- connection
def _read_line(sock, timeout=6.0):
    sock.settimeout(timeout)
    buf = bytearray()
    while True:
        c = sock.recv(1)
        if not c:
            raise ConnectionError("watch closed the connection during the handshake")
        if c in b"\r\n":
            if buf:
                return buf.decode(errors="replace")
            continue
        buf += c


def _raw(pub):
    return pub.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def connect(host):
    """Connects and runs the PAIR/AUTH handshake. Returns an authenticated socket."""
    try:
        sock = socket.create_connection((host, TCP_PORT), timeout=10)
    except OSError as e:
        sys.exit(f"Couldn't reach the watch at {host} ({e}).\n"
                 "Check the watch is on and on the same Wi-Fi as this laptop, or pass its IP:\n"
                 "    python tools/record_dataset.py <watch-ip>")
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    line = _read_line(sock)

    if line.startswith("PAIR,"):
        print("Watch is unpaired -> pairing this laptop...")
        watch_pub = bytes.fromhex(line[5:])
        priv = X25519PrivateKey.generate()
        my_pub = _raw(priv.public_key())
        shared = priv.exchange(X25519PublicKey.from_public_bytes(watch_pub))
        key = hashlib.sha256(PAIR_LABEL + shared + watch_pub + my_pub).digest()
        sock.sendall(f"PAIR,{my_pub.hex()}\n".encode())
        line = _read_line(sock)
        pairing = True
    else:
        if not os.path.exists(KEY_FILE):
            sock.close()
            sys.exit("The watch is paired with another device (probably the phone).\n"
                     "Unpair it on the watch (\"إلغاء ارتباط الهاتف\") and run this again.")
        with open(KEY_FILE, "rb") as f:
            key = f.read()
        pairing = False

    if not line.startswith("AUTH,"):
        sock.close()
        sys.exit(f"Unexpected handshake line from watch: {line!r}")
    nonce = bytes.fromhex(line[5:])
    mac = hmac.new(key, nonce, hashlib.sha256).hexdigest()
    sock.sendall(f"AUTH,{mac}\n".encode())
    reply = _read_line(sock)
    if reply != "AUTH,OK":
        sock.close()
        if not pairing and os.path.exists(KEY_FILE):
            os.remove(KEY_FILE)
        sys.exit("Watch rejected the key (it was re-paired to another device).\n"
                 "Unpair on the watch and run this again to re-pair the laptop.")
    if pairing:
        with open(KEY_FILE, "wb") as f:
            f.write(key)
        print(f"Paired. Key saved to {KEY_FILE}")
    print("Connected to watch.")
    return sock


# ------------------------------------------------------------------ audio
# The first few ms after 'r' restart the PDM mic and the DC filter, which
# produces a full-scale click (~samples 50-80) and a short DC decay. That is
# not real audio, so every take records STARTUP_TRIM_S extra and drops it.
STARTUP_TRIM_S = 0.3


def record(sock, seconds):
    sock.sendall(b"r")
    pcm = bytearray()
    trim = int(STARTUP_TRIM_S * SAMPLE_RATE) * 2
    target = int(seconds * BYTES_PER_SEC) + trim
    sock.settimeout(1.0)
    start = time.time()
    last_print = 0
    while len(pcm) < target:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            if time.time() - start > seconds + 5:
                print("\n  (no audio arriving — is the watch awake / mic working?)")
                break
            continue
        if not chunk:
            raise ConnectionError("watch disconnected while recording")
        pcm += chunk
        el = max(0.0, (len(pcm) - trim) / BYTES_PER_SEC)
        if el - last_print >= 1:
            last_print = el
            print(f"\r  recording... {el:4.1f}/{seconds:.0f}s  level {_bar(chunk)}", end="", flush=True)
    sock.sendall(b"s")
    # drain whatever was already in flight after 's'
    sock.settimeout(0.4)
    try:
        while sock.recv(4096):
            pass
    except socket.timeout:
        pass
    print()
    pcm = pcm[trim:target - (target % 2)]
    return bytes(pcm)


def _levels(pcm):
    n = len(pcm) // 2
    if n == 0:
        return 0.0, -120.0
    samples = struct.unpack(f"<{n}h", pcm[:n * 2])
    peak = max(abs(s) for s in samples) / 32768.0
    rms = math.sqrt(sum(s * s for s in samples) / n) / 32768.0
    return peak, (20 * math.log10(rms) if rms > 0 else -120.0)


def _bar(chunk):
    peak, _ = _levels(chunk)
    return "#" * int(peak * 20) + "." * (20 - int(peak * 20))


def save_wav(path, pcm):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, BYTES_PER_SEC, 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


# ---------------------------------------------------------------- prompts
def ask(prompt, default=""):
    v = input(f"{prompt}{f' [{default}]' if default else ''}: ").strip()
    return v or default


def slug(s):
    return "".join(c if c.isalnum() else "-" for c in s.lower()).strip("-") or "x"


def choose_class(default):
    print("  classes: " + "  ".join(f"{i+1}={c}" for i, c in enumerate(CLASSES)))
    while True:
        v = ask("  class (number or name, q=quit)", default)
        if v.lower() == "q":
            return None
        if v.isdigit() and 1 <= int(v) <= len(CLASSES):
            return CLASSES[int(v) - 1]
        if v in CLASSES:
            return v
        print("  not a class — try again")


def next_index(folder, stem):
    n = 1
    while os.path.exists(os.path.join(folder, f"{stem}__{n:03d}.wav")):
        n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="nabeeh-watch.local",
                    help="watch IP, or leave out to use its mDNS name nabeeh-watch.local")
    ap.add_argument("--out", default=r"D:\nabeeh_watch_audio", help="output folder")
    ap.add_argument("--seconds", type=float, default=10.0, help="default take length")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    meta_path = os.path.join(args.out, "metadata.csv")
    new_meta = not os.path.exists(meta_path)

    print("\n--- Session info (applies to every take until you quit) ---")
    recorded_by = ask("Your name", "ragad")
    location = ask("Location (e.g. home-riyadh-livingroom)", "home")
    session_id = f"{dt.date.today():%Y%m%d}-{slug(recorded_by)}-{slug(location)}"
    print(f"Session id: {session_id}")

    sock = connect(args.host)
    cls, subtype, distance, placement, sleeve, background = "na", "", "1m", "wrist", "no", "quiet"
    seconds = args.seconds

    with open(meta_path, "a", newline="", encoding="utf-8") as mf:
        writer = csv.DictWriter(mf, fieldnames=META_FIELDS)
        if new_meta:
            writer.writeheader()
        while True:
            print("\n--- New take (Enter keeps the value in [brackets]) ---")
            cls = choose_class(cls)
            if cls is None:
                break
            subtype = ask("  subtype (e.g. wooden-door, microwave-beep, mosque-window)", subtype)
            distance = ask("  distance to source", distance)
            placement = ask("  placement (wrist / table / pocket)", placement)
            sleeve = ask("  sleeve over watch? (yes/no)", sleeve)
            background = ask("  background (quiet / tv / ac / talking / street)", background)
            seconds = float(ask("  seconds", f"{seconds:g}"))
            notes = ask("  notes", "")

            input("  Press Enter, then make the sound...")
            try:
                pcm = record(sock, seconds)
            except (ConnectionError, OSError) as e:
                print(f"  connection lost ({e}) — reconnecting, redo this take.")
                sock = connect(args.host)
                continue

            peak, rms_db = _levels(pcm)
            warn = ""
            if peak >= 0.999:
                warn = "  ⚠ clipped — move the source further away"
            elif rms_db < -55:
                warn = "  ⚠ very quiet — the API's min-RMS gate may skip this"
            print(f"  got {len(pcm)/BYTES_PER_SEC:.1f}s  peak {peak:.2f}  rms {rms_db:.1f} dBFS{warn}")
            if ask("  keep it? (y/n)", "y").lower() != "y":
                continue

            folder = os.path.join(args.out, cls)
            os.makedirs(folder, exist_ok=True)
            stem = f"{cls}__{session_id}__{slug(subtype)}"
            fname = f"{stem}__{next_index(folder, stem):03d}.wav"
            save_wav(os.path.join(folder, fname), pcm)
            writer.writerow({
                "file": f"{cls}/{fname}", "class": cls, "subtype": subtype,
                "session_id": session_id, "location": location, "distance": distance,
                "placement": placement, "sleeve": sleeve, "background": background,
                "seconds": f"{len(pcm)/BYTES_PER_SEC:.2f}", "peak": f"{peak:.3f}",
                "rms_dbfs": f"{rms_db:.1f}", "recorded_by": recorded_by,
                "timestamp": dt.datetime.now().isoformat(timespec="seconds"), "notes": notes,
            })
            mf.flush()
            print(f"  saved {cls}/{fname}")

    sock.close()
    print(f"\nDone. Clips + metadata.csv are in {os.path.abspath(args.out)}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
