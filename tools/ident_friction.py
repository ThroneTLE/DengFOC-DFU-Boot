"""Measure the steady-state Uq needed to spin the motor at several speeds and
fit Uq = c0*sign(w) + c1*w for each direction (c0: friction, c1: back-EMF p*psi_f
+ viscous losses).

The knob's free-spinning presets cancel 80 % of this (`kcomp c0+ c0- c1`, c1 the
smaller of the two directions).
Needs the board calibrated (`cal`) and nothing else holding the COM port.

  python tools/ident_friction.py [--speeds 2,4,6,10,15,20,25] [--out data.csv] [--apply]
"""
import argparse
import csv
import os
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scope_capture import CHANNELS, find_port, parse  # noqa: E402

SETTLE_S = 1.0
HOLD_S = 1.6


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--speeds", default="2,4,6,10,15,20,25")
    ap.add_argument("--out")
    ap.add_argument("--apply", action="store_true", help="send the fitted `kcomp` to the board")
    args = ap.parse_args()
    speeds = [float(x) for x in args.speeds.split(",")]
    plan = speeds + [-v for v in speeds]
    port = args.port or find_port()
    if not port:
        sys.exit("DengFOC COM port (0483:5740) not found")

    frames = []
    with serial.Serial(port, 115200, timeout=0.01) as ser:
        ser.write(b"scope off\r")
        time.sleep(0.2)
        ser.reset_input_buffer()
        ser.write(b"s\r")
        time.sleep(0.3)
        status = ser.read(ser.in_waiting or 1).decode(errors="replace")
        if "cal=1" not in status:
            sys.exit("board not calibrated: run `cal` first (status: %s)" % status.strip())
        ser.write(b"scope\r")
        buf = b""
        t0 = time.time()
        for v in plan:
            ser.write(("vel %.2f\r" % v).encode())
            t_end = time.time() + HOLD_S
            while time.time() < t_end:
                buf += ser.read(ser.in_waiting or 1)
                buf, _ = parse(buf, len(CHANNELS), frames)
        ser.write(b"stop\r")
        time.sleep(0.1)
        ser.write(b"scope off\r")
        time.sleep(0.2)
        print(f"{len(frames)} frames in {time.time() - t0:.1f} s")

        idx = {c: i for i, c in enumerate(CHANNELS)}
        rows = []
        for v in plan:
            seg = [f for f in frames if abs(f[idx["tgt"]] - v) < 1e-3 and f[idx["mode"]] == 4]
            seg = seg[int(SETTLE_S * 2000):] if len(seg) > SETTLE_S * 2000 + 200 else seg[len(seg) // 2:]
            if not seg:
                continue
            w = sum(f[idx["vel"]] for f in seg) / len(seg)
            u = sum(f[idx["uq"]] for f in seg) / len(seg)
            rows.append((v, w, u, len(seg)))
        print(" target    speed     Uq      n")
        for v, w, u, n in rows:
            print(f"{v:7.1f} {w:8.2f} {u:7.3f} {n:6d}")

        # least squares on |Uq| = c0 + c1*|w|, each direction on its own
        fits = {}
        for sign, label in ((1, "forward"), (-1, "backward")):
            pts = [(abs(w), abs(u)) for _, w, u, _ in rows if w * sign > 0]
            n = len(pts)
            sx = sum(x for x, _ in pts)
            sy = sum(y for _, y in pts)
            c1 = (n * sum(x * y for x, y in pts) - sx * sy) / (n * sum(x * x for x, _ in pts) - sx * sx)
            c0 = (sy - c1 * sx) / n
            res = max(abs(y - c0 - c1 * x) for x, y in pts)
            fits[sign] = (c0, c1)
            print(f"{label:8s}: |Uq| = {c0:.4f} + {c1:.5f}*|w|   (max residual {res * 1000:.0f} mV)")
        c1 = min(fits[1][1], fits[-1][1])
        print("          back-EMF in Uq, theory p*psi_f = 0.0245 V/(rad/s); c1 also holds viscous losses")
        # friction moved by ~40 % between two runs (back-EMF did not): compensate half of it
        cmd = f"kcomp {fits[1][0] / 2:.4f} {fits[-1][0] / 2:.4f} {c1:.5f}"
        print("command:", cmd)
        if args.apply:
            ser.reset_input_buffer()
            ser.write((cmd + "\r").encode())
            time.sleep(0.3)
            print(ser.read(ser.in_waiting or 1).decode(errors="replace").strip())

    if args.out:
        with open(args.out, "w", newline="") as fh:
            wr = csv.writer(fh)
            wr.writerow(["target", "speed", "uq", "n"])
            wr.writerows(rows)
        print("saved", args.out)


if __name__ == "__main__":
    main()
