"""Record the DengFOC scope stream (VOFA+ JustFloat) to CSV.

Starts `scope <hz>` on the board, optionally sends motion commands while
recording, then stops the stream and reports the real frame rate.

Usage:
  python scope_capture.py --seconds 3 --out run.csv
  python scope_capture.py --seconds 4 --cmd "vel 5" --cmd "@2 vel -5" --cmd "@3.5 stop"

`--cmd "@t text"` sends `text` t seconds after recording starts (default t = 0).
"""
import argparse
import csv
import re
import struct
import sys
import time

import serial
from serial.tools import list_ports

CHANNELS = ["tgt", "pos_deg", "vel", "vel_ref", "uq", "theta_e", "vbus", "isr_us",
            "mode", "fault", "fet_c", "cal", "knob_pos", "knob_mode"]
TAIL = b"\x00\x00\x80\x7f"


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0x5740:
            return p.device
    return None


def parse(buf, nch, frames):
    """Pull complete frames out of buf; returns (rest, bad_resyncs)."""
    flen = 4 * nch + 4
    bad = 0
    while len(buf) >= flen:
        if buf[flen - 4:flen] == TAIL:
            frames.append(struct.unpack("<%df" % nch, buf[:flen - 4]))
            buf = buf[flen:]
            continue
        i = buf.find(TAIL)
        if i < 0:
            buf = buf[-3:]
            break
        buf = buf[i + 4:]
        bad += 1
    return buf, bad


def parse_cmd(spec):
    if spec.startswith("@"):
        t, _, text = spec[1:].partition(" ")
        return float(t), text
    return 0.0, spec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--hz", type=int, default=0, help="stream rate, 0 = control rate")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--cmd", action="append", default=[], help='"[@t] command", may repeat')
    ap.add_argument("--out", help="CSV file to write")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("DengFOC COM port (0483:5740) not found")
    cmds = sorted(parse_cmd(c) for c in args.cmd)

    frames = []
    bad = 0
    with serial.Serial(port, 115200, timeout=0.01) as ser:
        ser.write(b"scope off\r")
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(("scope %d\r" % args.hz if args.hz else "scope\r").encode())
        buf = b""
        t0 = time.time()
        while True:
            now = time.time() - t0
            while cmds and cmds[0][0] <= now:
                ser.write((cmds.pop(0)[1] + "\r").encode())
            if now >= args.seconds:
                break
            buf += ser.read(ser.in_waiting or 1)
            buf, b = parse(buf, len(CHANNELS), frames)
            bad += b
        elapsed = time.time() - t0
        ser.write(b"scope off\r")
        time.sleep(0.2)
        tail_bytes = ser.read(ser.in_waiting or 1)

    # The first resync skips the text banner printed before streaming starts.
    print(f"port {port}: {len(frames)} frames in {elapsed:.2f} s = {len(frames) / elapsed:.0f} frames/s, "
          f"resyncs {bad} (1 expected for the banner)")
    m = re.search(rb"frames dropped: (\d+)", tail_bytes)
    print("board-side dropped frames:", m.group(1).decode() if m else "?")
    if frames:
        isr = [f[7] for f in frames]
        print(f"isr_us min/avg/max = {min(isr):.0f}/{sum(isr) / len(isr):.0f}/{max(isr):.0f}")
    if args.out and frames:
        with open(args.out, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["t_s"] + CHANNELS)
            rate = len(frames) / elapsed
            for i, f in enumerate(frames):
                w.writerow([f"{i / rate:.6f}"] + [f"{v:.6g}" for v in f])
        print("saved", args.out)


if __name__ == "__main__":
    main()
