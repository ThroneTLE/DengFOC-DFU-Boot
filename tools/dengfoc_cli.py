"""Send commands to DengFOC firmware (selftest / foc_demo) over its USB virtual COM port.

Usage: python dengfoc_cli.py [--port COM36] [--timeout 5] info adc pull enc
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports


def find_port():
    for p in list_ports.comports():
        if p.vid == 0x0483 and p.pid == 0x5740:
            return p.device
    return None


def run(ser, cmd, timeout):
    ser.reset_input_buffer()
    ser.write((cmd + "\r").encode())
    buf = b""
    t_end = time.time() + timeout
    try:
        while time.time() < t_end:
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                buf += chunk
                if buf.endswith(b"> "):
                    break
    except serial.SerialException:
        # `dfu` reboots the board, which drops the port mid-read.
        buf += b"\n(port closed by device)"
    return buf.decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()

    # Replies can contain stray binary scope frames; never let printing crash.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    port = args.port or find_port()
    if not port:
        sys.exit("self-test COM port (0483:5740) not found")
    with serial.Serial(port, 115200, timeout=0.1) as ser:
        time.sleep(0.2)
        for cmd in args.commands:
            print(f"===== {cmd} ({port})")
            print(run(ser, cmd, args.timeout).rstrip("> ").rstrip())


if __name__ == "__main__":
    main()
