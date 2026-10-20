"""(Re)build the "DengFOC 电机" page and command-panel group in a running Synex.

Needs Synex open with "AI 外部助手" enabled. Idempotent: clears the page and the
"DengFOC 电机" command group, then recreates them.

  python -X utf8 build_dengfoc_page.py [--dry-run]

Firmware telemetry (JustFloat, 14 ch): 0 目标, 1 位置_deg, 2 速度_rad_s, 3 速度参考,
4 Uq_V, 5 电角度_rad, 6 母线_V, 7 中断_us, 8 模式, 9 故障, 10 MOS温度_C, 11 已校准,
12 旋钮档位, 13 旋钮模式.
"""
import argparse
import json
import queue
import subprocess
import sys
import threading

EXE = r"E:/User/Desk/Synex_v3.0.5/mcp/SynexMcpServer.exe"
PAGE = "DengFOC 电机"
KNOB_PAGE = "力反馈旋钮"
GROUP = "DengFOC 电机"
NL = chr(92) + "n"   # Synex keeps escapes as text and expands them when sending

MODE_LABELS = "停止;校准中;开环;力矩;速度;位置;旋钮"
FAULT_LABELS = "正常;DRV故障;编码器;母线电压;过温;DRV配置"


def enum_cfg(labels):
    n = len(labels.split(";"))
    return {"enumDisplayEnabled": True, "enumRangeMode": True, "enumLabelsText": labels,
            "enumValuesText": ";".join(str(i) for i in range(n)),
            "enumRangesText": ";".join(f"{i - 0.5}~{i + 0.5}" for i in range(n))}


def g(col, row, w, h):
    return {"col": col, "row": row, "colSpan": w, "rowSpan": h}


def numeric(title, ch, col, row, w, h, unit="", dec=2, extra=None):
    cfg = {"boundChannelIndex": ch, "customTitleText": title, "useBoundChannelName": False,
           "unit": unit, "valueTextDecimals": dec, "textPixelSize": 18, "splitRatio": 0.42,
           "enumDisplayEnabled": False}
    cfg.update(extra or {})
    return {"type": "Numeric", "title": title, "geometry": g(col, row, w, h), "config": cfg}


def gauge(title, ch, lo, hi, unit, col, row, w, h, dec=1):
    return {"type": "Gauge", "title": title, "geometry": g(col, row, w, h),
            "config": {"boundChannelIndex": ch, "customTitleText": title, "useBoundChannelName": False,
                       "minimumValue": lo, "maximumValue": hi, "unit": unit, "valueTextDecimals": dec}}


def slider(title, tmpl, lo, hi, step, val, col, row, w=24, h=7):
    return {"type": "SerialCmd", "title": title, "geometry": g(col, row, w, h),
            "config": {"displayName": title, "asciiContent": tmpl + NL, "dispatchMode": "serialAscii",
                       "hexMode": False, "minimum": lo, "maximum": hi, "singleStep": step, "value": val,
                       "resendCount": 0}}


def plot(title, chans, col, row, w, h):
    return {"type": "Plot", "title": title, "geometry": g(col, row, w, h),
            "config": {"selectedChannels": chans, "dynamicYRangeEnabled": True, "topWidgetVisible": True}}


def selector(title, tmpl, labels, values, col, row, w, h=6):
    return {"type": "ModeSelector", "title": title, "geometry": g(col, row, w, h),
            "config": {"displayName": title, "asciiContent": tmpl + NL, "dispatchMode": "serialAscii",
                       "hexMode": False, "modeTexts": ";".join(labels), "modeValues": ";".join(map(str, values)),
                       "resendOnSameState": True, "resendCount": 0}}


TELEMETRY = {"type": "Toggle", "title": "遥测数据流", "geometry": g(0, 0, 12, 6),
             "config": {"displayName": "遥测数据流", "asciiContent": "scope %d" + NL, "dispatchMode": "serialAscii",
                        "hexMode": False, "stateTexts": "开启;停止", "stateValues": "1;0", "checked": False,
                        "resendCount": 0}}
ACTIONS = ("操作", "act %d", ["停止", "校准", "清故障", "归零"], [0, 1, 2, 3])

MOTOR_TILES = [
    # left: state, actions, numeric parameters
    TELEMETRY,
    numeric("运行模式", 8, 12, 0, 12, 6, extra=enum_cfg(MODE_LABELS)),
    selector(*ACTIONS, 0, 6, 24),
    numeric("故障", 9, 0, 12, 12, 4, extra=enum_cfg(FAULT_LABELS)),
    numeric("校准", 11, 12, 12, 12, 4, extra=enum_cfg("未校准;已校准")),
    slider("速度目标 rad/s", "vel %.2f", -30, 30, 0.5, 0, 0, 16),
    slider("位置目标 deg", "pos %.1f", -720, 720, 5, 0, 0, 23),
    slider("力矩 Uq V", "tq %.2f", -3, 3, 0.05, 0, 0, 30),
    slider("开环转速 rad/s", "open %.2f", -20, 20, 0.5, 0, 0, 37),
    slider("电压上限 V", "ulim %.2f", 0, 5, 0.1, 2, 0, 44),
    slider("速度环 P", "vp %.3f", 0, 1, 0.01, 0.1, 0, 51, w=12, h=6),
    slider("速度环 I", "vi %.2f", 0, 20, 0.1, 2, 12, 51, w=12, h=6),
    slider("位置环 P", "ap %.1f", 0, 100, 1, 20, 0, 57, w=12),
    slider("速度上限 rad/s", "vlim %.1f", 0, 100, 1, 30, 12, 57, w=12),
    # right top: gauges and readouts
    gauge("转速", 2, -30, 30, "rad/s", 24, 0, 12, 10),
    gauge("母线电压", 6, 0, 20, "V", 36, 0, 12, 10, dec=2),
    gauge("MOS温度", 10, 0, 100, "°C", 48, 0, 12, 10),
    numeric("位置", 1, 60, 0, 18, 5, "°", 1),
    numeric("Uq", 4, 78, 0, 18, 5, "V", 2),
    numeric("目标", 0, 60, 5, 18, 5, "", 2),
    numeric("中断耗时", 7, 78, 5, 18, 5, "µs", 0),
    # right: waveforms
    plot("位置 deg（目标 / 实际）", [0, 1], 24, 10, 36, 27),
    plot("速度 rad/s（实际 / 参考）", [2, 3], 60, 10, 36, 27),
    plot("Uq V", [4], 24, 37, 36, 27),
    plot("电角度 rad", [5], 60, 37, 36, 27),
]

# Knob presets: firmware `knob <n>` number, button label, button row. Keep in step with knob.c.
KNOB = [
    (2, "细棘轮", 0), (3, "粗棘轮", 0), (32, "8档吸附", 0), (26, "16档", 0), (25, "四向菜单", 0),
    (24, "菜单", 0), (34, "缺口档", 0), (22, "咔嗒", 0), (30, "SurfaceDial", 0), (29, "色环", 0),
    (4, "限位0-10", 1), (10, "多圈限位", 1), (11, "精调0-255", 1), (12, "粗调强档", 1), (13, "粗调弱档", 1),
    (28, "调光0-100", 1), (27, "空调0-14", 1), (31, "窗帘0-20", 1), (14, "磁吸点", 1), (33, "90°窗口", 1),
    (5, "开关", 2), (16, "三档开关", 2), (6, "回中", 2), (15, "回中带档", 2), (35, "分段弹簧", 2),
    (41, "阻尼弹簧", 2), (19, "单向棘轮", 2), (17, "阻尼限位", 2), (38, "恒力回位", 2),
    (1, "自由", 3), (7, "阻尼", 3), (18, "摩擦", 3), (8, "飞轮", 3), (9, "惯性棘轮", 3), (39, "惯性列表", 3),
    (20, "自适应", 3),
    (21, "纹理", 4), (36, "方波纹理", 4), (37, "棘爪纹理", 4), (40, "震动", 4), (23, "自转", 4),
]
KNOB_ROWS = ["档位·菜单", "数值·限位", "开关·弹簧", "阻尼·惯性", "纹理·特效"]


def knob_tiles():
    names = {n: label for n, label, _ in KNOB}
    top = max(names)
    current = enum_cfg(";".join(["关"] + [names.get(i, str(i)) for i in range(1, top + 1)]))
    tiles = [
        TELEMETRY,
        selector(*ACTIONS, 12, 0, 30),
        numeric("当前模式", 13, 42, 0, 22, 6, extra=current),
        numeric("档位", 12, 64, 0, 12, 6, "", 0),
        numeric("故障", 9, 76, 0, 20, 6, extra=enum_cfg(FAULT_LABELS)),
    ]
    for r, title in enumerate(KNOB_ROWS):
        row = [(n, label) for n, label, k in KNOB if k == r]
        tiles.append(selector(title, "knob %d", [l for _, l in row], [n for n, _ in row], 0, 6 + 5 * r, 96, h=5))
    tiles += [
        slider("手感力度", "kstr %.2f", 0, 2, 0.05, 1, 0, 31, w=24, h=6),
        slider("电压上限 V", "ulim %.2f", 0, 5, 0.1, 2, 24, 31, w=24, h=6),
        numeric("转速", 2, 48, 31, 16, 6, "rad/s", 1),
        numeric("Uq", 4, 64, 31, 16, 6, "V", 2),
        numeric("飞轮转速", 3, 80, 31, 16, 6, "rad/s", 1),
        plot("位置 deg（格中心 / 实际）", [0, 1], 0, 37, 32, 27),
        plot("速度 rad/s（实际 / 虚拟飞轮）", [2, 3], 32, 37, 32, 27),
        plot("Uq V", [4], 64, 37, 32, 27),
    ]
    return tiles


PAGES = [(PAGE, MOTOR_TILES), (KNOB_PAGE, knob_tiles())]

COMMANDS = [
    ("校准", "cal"), ("停止", "stop"), ("清除故障", "clear"), ("位置归零", "zero"),
    ("遥测开启", "scope 1"), ("遥测停止", "scope 0"), ("进入烧录模式", "dfu"),
]


class Synex:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, bufsize=0)
        self.q = queue.Queue()
        threading.Thread(target=lambda: [self.q.put(x) for x in self.p.stdout], daemon=True).start()
        self.n = 0
        self._rpc("initialize", {"protocolVersion": "2025-03-26", "capabilities": {},
                                 "clientInfo": {"name": "build_dengfoc_page", "version": "1"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def _send(self, obj):
        self.p.stdin.write((json.dumps(obj, ensure_ascii=True) + "\n").encode())
        self.p.stdin.flush()

    def _rpc(self, method, params):
        self.n += 1
        self._send({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params})
        while True:
            msg = json.loads(self.q.get(timeout=60))
            if msg.get("id") == self.n:
                return msg

    def call(self, name, args):
        res = self._rpc("tools/call", {"name": name, "arguments": args})
        if "error" in res:
            raise RuntimeError(res["error"])
        return json.loads(res["result"]["content"][0]["text"])


def verify(sx, page_id, specs):
    """Read the page back and report any tile whose geometry or config differs from specs."""
    w = sx.call("get_plot_workspace", {"workspace": {"id": page_id}})
    tiles = w.get("workspace", w).get("tiles", [])
    bad = 0
    for t, spec in zip(tiles, specs):
        diff = {k: (v, t["geometry"].get(k)) for k, v in spec["geometry"].items() if t["geometry"].get(k) != v}
        diff.update({k: (v, t["config"].get(k)) for k, v in spec["config"].items() if t["config"].get(k) != v})
        if diff:
            bad += 1
            print("  mismatch", spec["title"], diff)
    print(f"verify: {len(tiles)} tiles, {bad} mismatched")


def build_page(sx, name, specs, dry_run):
    patch = "preview_plot_workspace_patch" if dry_run else "apply_plot_workspace_patch"
    layout = sx.call("get_plot_workspace_layout", {})
    page = next((w for w in layout["workspaces"] if w["name"] == name), None)
    if page is None:
        if dry_run:
            print(f"page '{name}': would be created with {len(specs)} tiles")
            return
        sx.call(patch, {"operations": [{"op": "addWorkspace", "name": name}]})
        layout = sx.call("get_plot_workspace_layout", {})
        page = next(w for w in layout["workspaces"] if w["name"] == name)
    ws = {"id": page["id"]}
    ops = [{"op": "deleteTile", "workspace": ws, "tile": {"id": t["id"]}} for t in page.get("tiles", [])]
    ops += [{"op": "addTile", "workspace": ws, "tile": {"type": t["type"], "title": t["title"]},
             "geometry": t["geometry"]} for t in specs]
    r = sx.call(patch, {"operations": ops})
    print(f"page '{name}': deleted {len(page.get('tiles', []))}, added {len(specs)} ->",
          "ok" if r.get("ok") else r.get("error"))
    if dry_run:
        return
    # addTile ignores geometry and config; the new tile id comes back as the op message
    ids = [o["message"] for o in r["appliedOperations"] if o["op"] == "addTile"]
    ops = []
    for i, t in zip(ids, specs):
        ops.append({"op": "updateTileGeometry", "workspace": ws, "tile": {"id": i}, "geometry": t["geometry"]})
        ops.append({"op": "updateTileConfig", "workspace": ws, "tile": {"id": i}, "patch": t["config"]})
    r = sx.call(patch, {"operations": ops})
    print(f"placed and configured {len(ids)} tiles ->", "ok" if r.get("ok") else r.get("error"))
    verify(sx, page["id"], specs)


def find_group(items, name):
    for it in items:
        if it.get("type") == "group" and it.get("name") == name:
            return it
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    sx = Synex()
    for name, specs in PAGES:
        build_page(sx, name, specs, args.dry_run)

    panel = sx.call("get_command_panel_json", {})["configuration"]["items"]
    old = find_group(panel, GROUP)
    ops = []
    if old is not None:
        ops.append({"op": "deleteNode", "id": old["id"], "nodeType": "group"})
    cmds = [{"type": "command", "id": f"cmd_dengfoc_{i}", "name": name,
             "content": {"mode": "abc", "abc": text + NL, "hex": "", "crcType": "none",
                         "crcRange": "before-placeholder"},
             "repeat": {"count": 0, "intervalMs": 100}, "skipInGroupLoop": False}
            for i, (name, text) in enumerate(COMMANDS)]
    ops.append({"op": "addGroup", "position": {"where": "root-start"},
                "group": {"type": "group", "id": "grp_dengfoc", "name": GROUP, "expanded": True,
                          "repeat": {"count": 0, "intervalMs": 100}, "items": cmds, "commands": cmds}})
    tool = "preview_command_panel_patch" if args.dry_run else "apply_command_panel_patch"
    r = sx.call(tool, {"operations": ops})
    print(f"command group '{GROUP}': {len(cmds)} commands ->", "ok" if r.get("ok") else r.get("error"))
    sx.p.terminate()


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
