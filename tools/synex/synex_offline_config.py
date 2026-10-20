"""Edit plain-text keys of Synex config.ini while Synex is closed.

Covers what the Synex MCP bridge cannot reach or does not save: physical channel names,
plot sample time, buffer size, derived channels, and the plot x windows (inside the
@Variant tile states; every value is round-trip checked before anything is written).
Refuses to run while Synex.exe is running, because Synex rewrites config.ini on exit.

Usage:
  python -X utf8 synex_offline_config.py dengfoc [--config PATH]   (default: ../../Synex/config.ini)
  python -X utf8 synex_offline_config.py plotwindow [--seconds 20]  plot x windows only
  python -X utf8 synex_offline_config.py check                       round-trip test, read-only
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

# this project's own copy of Synex (tools/synex/install_synex.py); SYNEX_DIR overrides
SYNEX_DIR = os.environ.get("SYNEX_DIR", os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Synex")))
DEFAULT_CONFIG = os.path.join(SYNEX_DIR, "config.ini")

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


def synex_pids(folder=None):
    """PIDs of Synex.exe processes, only those started from `folder` if given."""
    ps = "Get-Process Synex -ErrorAction SilentlyContinue | ForEach-Object { \"$($_.Id)|$($_.Path)\" }"
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    want = os.path.normcase(os.path.abspath(os.path.join(folder, "Synex.exe"))) if folder else None
    pids = []
    for line in out.splitlines():
        pid, _, path = line.strip().partition("|")
        if pid and (want is None or os.path.normcase(os.path.abspath(path)) == want):
            pids.append(int(pid))
    return pids


def synex_running(folder=SYNEX_DIR):
    """Is the Synex that owns `folder`/config.ini running? (other copies do not matter)"""
    return bool(synex_pids(folder))


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


# --- tile states: QSettings "@Variant(...)" values -------------------------------
# Qt writes the QDataStream bytes of the variant with these escapes (qsettings.cpp,
# iniEscapedString): \0 \a \b \f \n \r \t \v \" \\, other bytes < 0x20 or >= 0x7f as
# \x<hex>, and after \0 or \x.. a following hex-digit character is escaped as well.
_ESC = {"0": 0, "a": 7, "b": 8, "f": 12, "n": 10, "r": 13, "t": 9, "v": 11, '"': 34, "\\": 92}
_ESC_OUT = {7: "\\a", 8: "\\b", 12: "\\f", 10: "\\n", 13: "\\r", 9: "\\t", 11: "\\v", 34: '\\"', 92: "\\\\"}
_HEX = set(b"0123456789abcdefABCDEF")


def variant_decode(s):
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\":
            n = s[i + 1]
            if n == "x":
                j = i + 2
                while j < len(s) and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(s[i + 2:j], 16))
                i = j
                continue
            out.append(_ESC[n])
            i += 2
            continue
        out.append(ord(c))
        i += 1
    return bytes(out)


def variant_encode(raw):
    out = []
    esc_next = False
    for b in raw:
        if esc_next and b in _HEX:
            out.append("\\x%x" % b)
            continue
        esc_next = False
        if b == 0:
            out.append("\\0")
            esc_next = True
        elif b in _ESC_OUT:
            out.append(_ESC_OUT[b])
        elif b < 0x20 or b >= 0x7f:
            out.append("\\x%x" % b)
            esc_next = True
        else:
            out.append(chr(b))
    return "".join(out)


def _split_variant(line):
    """'key=@Variant(...)' or 'key="@Variant(...)"' -> (key, body) or None."""
    key, _, val = line.partition("=")
    if val.startswith('"@Variant(') and val.endswith(')"'):
        return key, val[10:-2]
    if val.startswith("@Variant(") and val.endswith(")"):
        return key, val[9:-1]
    return None


def _join_variant(key, raw):
    body = variant_encode(raw)
    quoted = any(b in b";,=" for b in raw)   # Qt quotes values containing ; , =
    return f'{key}="@Variant({body})"' if quoted else f"{key}=@Variant({body})"


def check_variants(ini):
    """Decode and re-encode every @Variant line; returns (count, mismatching keys)."""
    n, bad = 0, []
    for ln in ini.lines:
        sv = _split_variant(ln)
        if sv:
            n += 1
            if _join_variant(sv[0], variant_decode(sv[1])) != ln:
                bad.append(sv[0])
    return n, bad


def _set_double(raw, key, value):
    k = key.encode("utf-16-be")
    i = raw.find(struct.pack(">I", len(k)) + k)
    if i < 0:
        return raw, False
    j = i + 4 + len(k)
    if struct.unpack(">I", raw[j:j + 4])[0] != 6:   # QMetaType::Double, value follows directly
        return raw, False
    return raw[:j + 4] + struct.pack(">d", value) + raw[j + 12:], True


def set_plot_windows(ini, lo, hi):
    """Put every Plot tile (itemType 0) and the shared global x range on [lo, hi] s.

    The real-time plot x axis is the position in the sample buffer, 0 .. SerialbufferSize x
    PlotSampleTimeMs; a window outside that stays blank. Changes made through the MCP bridge
    are not reliably saved, and the global range pulls all plots back, so fix it here."""
    n, bad = check_variants(ini)
    if bad:
        raise RuntimeError(f"@Variant round trip failed for {bad[:3]}: not touching tile states")
    plots = set()
    for ln in ini.lines:
        key, _, val = ln.partition("=")
        if key.endswith("\\itemType") and val.strip() == "0":
            plots.add(key[:-len("itemType")] + "itemState")
    changed = 0
    for idx, ln in enumerate(ini.lines):
        sv = _split_variant(ln)
        if sv and sv[0] in plots:
            raw = variant_decode(sv[1])
            raw, a = _set_double(raw, "xRangeLower", lo)
            raw, b = _set_double(raw, "xRangeUpper", hi)
            if a and b:
                ini.lines[idx] = _join_variant(sv[0], raw)
                changed += 1
    ini.set("PlotSettings", "globalXRangeValid", "true")
    ini.set("PlotSettings", "globalXRangeLower", repr(float(lo)))
    ini.set("PlotSettings", "globalXRangeUpper", repr(float(hi)))
    return changed, len(plots)


def cmd_dengfoc(path):
    ini = Ini(path)
    set_channel_names(ini, DENGFOC_NAMES)
    removed = ini.keep_only("DerivedChannels", {"FormatVersion"})
    ini.set("MainWindowSettings", "PlotSampleTimeMs", "0.5")   # firmware streams at 2 kHz
    ini.set("MainWindowSettings", "SerialbufferSize", "40000")  # 20 s at 2 kHz; keep X_SPAN_S in build_dengfoc_page.py in step
    changed, plots = set_plot_windows(ini, 0.0, 40000 * 0.5 / 1000)
    print(f"plot windows set to 0-20 s: {changed} of {plots} plot tiles")
    backup = ini.save()
    history = reset_plot_history(os.path.dirname(os.path.abspath(path)))
    print("backup:", backup)
    print("plot history moved to:", history)
    print("channel names:", DENGFOC_NAMES)
    print("derived channel lines removed:", removed)
    print("PlotSampleTimeMs=0.5, SerialbufferSize=40000")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=["dengfoc", "plotwindow", "check"])
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    ap.add_argument("--seconds", type=float, default=40000 * 0.5 / 1000, help="plotwindow: 0..this many s")
    args = ap.parse_args()
    if args.action == "check":   # read-only, fine while Synex runs
        n, bad = check_variants(Ini(args.config))
        print(f"{n} @Variant values, round trip {'OK' if not bad else 'FAILED for ' + ', '.join(bad)}")
        return
    if synex_running(os.path.dirname(os.path.abspath(args.config))):
        sys.exit("Synex is running: close it first, otherwise it overwrites config.ini on exit")
    if args.action == "dengfoc":
        cmd_dengfoc(args.config)
    elif args.action == "plotwindow":
        ini = Ini(args.config)
        changed, plots = set_plot_windows(ini, 0.0, args.seconds)
        print(f"plot windows set to 0-{args.seconds:g} s: {changed} of {plots} plot tiles; backup", ini.save())


if __name__ == "__main__":
    main()
