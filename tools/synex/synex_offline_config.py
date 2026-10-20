"""Edit plain-text keys of Synex config.ini while Synex is closed.

Covers what the Synex MCP bridge cannot reach: physical channel names, plot sample
time, buffer size, and wiping derived channels. Tile states (@Variant blobs) are not
touched here; change those live through the MCP bridge.
Refuses to run while Synex.exe is running, because Synex rewrites config.ini on exit.

Usage:
  python -X utf8 synex_offline_config.py dengfoc [--config PATH]
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

DEFAULT_CONFIG = r"E:/User/Desk/Synex_v3.0.5/config.ini"

DENGFOC_NAMES = ["目标", "位置_deg", "速度_rad_s", "速度参考", "Uq_V", "电角度_rad", "母线_V", "中断_us",
                 "模式", "故障", "MOS温度_C", "已校准", "旋钮档位", "旋钮模式"]


def set_channel_names(ini, names):
    """Write name0..N-1, drop any stale higher names, and set the channel count."""
    for i, n in enumerate(names):
        ini.set("ChannelNames", f"name{i}", n)
    for i in range(len(names), 256):
        ini.delete("ChannelNames", f"name{i}")
    ini.set("MainWindowSettings", "SerialChannelNum", str(len(names)))


def reset_plot_history(synex_dir):
    """Move the saved plot history aside: on startup Synex re-imports it and takes the
    channel count and names from it (plot_data.csv header in 4.x, plot_data.splot in 3.x)."""
    moved = []
    for name in ("plot_data.splot", "plot_data.csv"):
        src = os.path.join(synex_dir, name)
        if os.path.exists(src):
            dst = f"{src}.bak_{time.strftime('%Y%m%d_%H%M%S')}"
            os.replace(src, dst)
            moved.append(dst)
    return moved


def synex_running():
    # tasklist prints in the console code page (GBK here), so compare raw bytes
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Synex.exe"], capture_output=True).stdout
    return b"Synex.exe" in out


class Ini:
    """Minimal line-preserving editor for a QSettings IniFormat file."""

    def __init__(self, path):
        self.path = path
        raw = open(path, "rb").read()
        self.newline = "\r\n" if b"\r\n" in raw else "\n"
        self.lines = raw.decode("utf-8").split(self.newline)

    def _section(self, name):
        start = None
        for i, ln in enumerate(self.lines):
            if ln == f"[{name}]":
                start = i
            elif start is not None and ln.startswith("["):
                return start, i
        if start is None:
            return None
        return start, len(self.lines)

    def set(self, section, key, value):
        rng = self._section(section)
        if rng is None:
            self.lines += ["", f"[{section}]", f"{key}={value}"]
            return
        start, end = rng
        for i in range(start + 1, end):
            if self.lines[i].split("=", 1)[0] == key:
                self.lines[i] = f"{key}={value}"
                return
        insert_at = end
        while insert_at > start + 1 and self.lines[insert_at - 1] == "":
            insert_at -= 1
        self.lines.insert(insert_at, f"{key}={value}")

    def delete(self, section, key):
        rng = self._section(section)
        if rng is None:
            return False
        start, end = rng
        for i in range(start + 1, end):
            if self.lines[i].split("=", 1)[0] == key:
                del self.lines[i]
                return True
        return False

    def keep_only(self, section, keys):
        """Delete every key in section except `keys`; returns how many lines were removed."""
        rng = self._section(section)
        if rng is None:
            return 0
        start, end = rng
        kept = [ln for ln in self.lines[start + 1:end]
                if ln == "" or ln.split("=", 1)[0] in keys]
        removed = (end - start - 1) - len(kept)
        self.lines[start + 1:end] = kept
        return removed

    def save(self):
        backup = f"{self.path}.bak_offline_{time.strftime('%Y%m%d_%H%M%S')}"
        shutil.copy2(self.path, backup)
        open(self.path, "wb").write(self.newline.join(self.lines).encode("utf-8"))
        return backup


def cmd_dengfoc(path):
    ini = Ini(path)
    set_channel_names(ini, DENGFOC_NAMES)
    removed = ini.keep_only("DerivedChannels", {"FormatVersion"})
    ini.set("MainWindowSettings", "PlotSampleTimeMs", "0.5")   # firmware streams at 2 kHz
    ini.set("MainWindowSettings", "SerialbufferSize", "40000")  # 20 s at 2 kHz
    backup = ini.save()
    history = reset_plot_history(os.path.dirname(os.path.abspath(path)))
    print("backup:", backup)
    print("plot history moved to:", history)
    print("channel names:", DENGFOC_NAMES)
    print("derived channel lines removed:", removed)
    print("PlotSampleTimeMs=0.5, SerialbufferSize=40000")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["dengfoc"])
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    args = ap.parse_args()
    if synex_running():
        sys.exit("Synex is running: close it first, otherwise it overwrites config.ini on exit")
    if args.action == "dengfoc":
        cmd_dengfoc(args.config)


if __name__ == "__main__":
    main()
