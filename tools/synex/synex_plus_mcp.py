"""synex-plus: MCP server (stdio) that extends Synex's own MCP server.

All tools of Synex's mcp/SynexMcpServer.exe are forwarded unchanged. Added tools
cover what that server cannot do:

  synex_status              is Synex running, where its config lives
  get_connection_settings   read Connection / display settings from config.ini
  offline_update_settings   channel names, serial port, protocol, plot sample time,
                            buffer size, wipe derived channels (Synex must be closed)
  get_tile_defaults         preview adding a tile and return its full default config,
                            so every style/option key of a tile type becomes visible
  remap_plot_channels       rewrite selectedChannels of all Plot tiles on a workspace

Run: python -X utf8 synex_plus_mcp.py   (SYNEX_DIR overrides the Synex folder)
"""
import json
import os
import queue
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from synex_offline_config import Ini, reset_plot_history, set_channel_names, synex_running  # noqa: E402

SYNEX_DIR = os.environ.get("SYNEX_DIR", os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Synex")))
UPSTREAM_EXE = os.path.join(SYNEX_DIR, "mcp", "SynexMcpServer.exe")
CONFIG = os.path.join(SYNEX_DIR, "config.ini")
PROTOCOL_VERSION = "2025-03-26"


class Upstream:
    """Persistent client for SynexMcpServer.exe over stdio."""

    def __init__(self):
        self.proc = None
        self.q = queue.Queue()
        self.next_id = 1
        self.tools = []
        self.lock = threading.Lock()

    def _reader(self, proc):
        for line in proc.stdout:
            self.q.put(line)

    def _send(self, obj):
        self.proc.stdin.write((json.dumps(obj, ensure_ascii=True) + "\n").encode())
        self.proc.stdin.flush()

    def ensure(self):
        if self.proc is not None and self.proc.poll() is None:
            return
        self.proc = subprocess.Popen([UPSTREAM_EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, bufsize=0)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, args=(self.proc,), daemon=True).start()
        self.request("initialize", {"protocolVersion": PROTOCOL_VERSION, "capabilities": {},
                                    "clientInfo": {"name": "synex-plus", "version": "0.1.0"}})
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        self.tools = self.request("tools/list", {}).get("result", {}).get("tools", [])

    def request(self, method, params, timeout=60):
        with self.lock:
            rid = self.next_id
            self.next_id += 1
            self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
            while True:
                msg = json.loads(self.q.get(timeout=timeout))
                if msg.get("id") == rid:
                    return msg

    def call(self, name, arguments):
        self.ensure()
        return self.request("tools/call", {"name": name, "arguments": arguments})

    def call_json(self, name, arguments):
        """Call an upstream tool and parse its first text content as JSON."""
        res = self.call(name, arguments)
        if "error" in res:
            raise RuntimeError(json.dumps(res["error"], ensure_ascii=False))
        content = res.get("result", {}).get("content", [])
        text = content[0].get("text", "{}") if content else "{}"
        return json.loads(text)


upstream = Upstream()


def text_result(obj, is_error=False):
    text = obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=False, indent=2)
    return {"content": [{"type": "text", "text": text}], "isError": is_error}


# ------------------------------------------------------------------ local tools

def t_synex_status(_args):
    discovery = os.path.join(os.environ.get("TEMP", ""), "Synex", "ai_bridge.json")
    bridge = None
    if os.path.exists(discovery):
        try:
            bridge = json.load(open(discovery, encoding="utf-8"))
        except (OSError, ValueError):
            bridge = "unreadable"
    return text_result({"synexRunning": synex_running(SYNEX_DIR), "synexDir": SYNEX_DIR, "config": CONFIG,
                        "upstreamServer": UPSTREAM_EXE, "upstreamExists": os.path.exists(UPSTREAM_EXE),
                        "bridgeDiscovery": bridge})


def _section(ini, name):
    rng = ini._section(name)
    if rng is None:
        return {}
    out = {}
    for ln in ini.lines[rng[0] + 1:rng[1]]:
        if "=" in ln:
            k, v = ln.split("=", 1)
            out[k] = v
    return out


def t_get_connection_settings(_args):
    ini = Ini(CONFIG)
    conn = _section(ini, "ConnectionSettings")
    keep = ["ConnectionType", "SelectedPortName", "SelectedPortText", "SelectedBaudRateText",
            "ProtocolType", "ProtocolName", "CustomProtocolName"]
    main = _section(ini, "MainWindowSettings")
    names = _section(ini, "ChannelNames")
    note = "values on disk; Synex keeps its own copy while running and saves it on exit"
    return text_result({"connection": {k: conn.get(k) for k in keep},
                        "display": {k: main.get(k) for k in ["SerialChannelNum", "SerialbufferSize",
                                                              "PlotSampleTimeMs", "PlotRefreshFps"]},
                        "channelNames": names, "note": note, "synexRunning": synex_running(SYNEX_DIR)})


def t_offline_update_settings(args):
    if synex_running(SYNEX_DIR):
        return text_result("Synex is running. Close it first: it rewrites config.ini on exit "
                           "and would discard these edits.", True)
    ini = Ini(CONFIG)
    done = []
    names = args.get("channelNames")
    if names:
        set_channel_names(ini, [str(n) for n in names])
        done.append(f"channelNames x{len(names)} (stale higher names removed)")
    simple = {"plotSampleTimeMs": ("MainWindowSettings", "PlotSampleTimeMs"),
              "serialBufferSize": ("MainWindowSettings", "SerialbufferSize"),
              "plotRefreshFps": ("MainWindowSettings", "PlotRefreshFps"),
              "serialPort": ("ConnectionSettings", "SelectedPortName"),
              "protocolType": ("ConnectionSettings", "ProtocolType")}
    for arg, (sec, key) in simple.items():
        if arg in args and args[arg] is not None:
            ini.set(sec, key, str(args[arg]))
            if arg == "protocolType":
                ini.set("ConnectionSettings", "ProtocolName", str(args[arg]))
            done.append(f"{key}={args[arg]}")
    if args.get("clearDerivedChannels"):
        n = ini.keep_only("DerivedChannels", {"FormatVersion"})
        done.append(f"derived channels cleared ({n} lines)")
    if not done and not args.get("resetPlotHistory"):
        return text_result("nothing to change", True)
    backup = ini.save() if done else None
    history = None
    if args.get("resetPlotHistory"):
        history = reset_plot_history(SYNEX_DIR)
        done.append("plot history moved aside")
    return text_result({"ok": True, "changed": done, "backup": backup, "plotHistoryBackup": history})


def t_get_tile_defaults(args):
    """Preview cannot return a new tile's config, so add a probe tile, read it, delete it."""
    ws = args.get("workspace") or {"current": True}
    data = upstream.call_json("get_plot_workspace", {"workspace": ws})
    w = data.get("workspace", data)
    before = {t["id"] for t in w.get("tiles", [])}
    op = {"op": "addTile", "workspace": {"id": w["id"]},
          "tile": {"type": args["type"], "title": args.get("title", "probe")},
          "geometry": args.get("geometry", {"col": 0, "row": 0, "colSpan": 16, "rowSpan": 16})}
    res = upstream.call_json("apply_plot_workspace_patch", {"operations": [op]})
    if not res.get("ok", False):
        return text_result(res, True)
    w = upstream.call_json("get_plot_workspace", {"workspace": {"id": w["id"]}}).get("workspace", {})
    new = [t for t in w.get("tiles", []) if t["id"] not in before]
    for t in new:
        upstream.call_json("apply_plot_workspace_patch", {"operations": [
            {"op": "deleteTile", "workspace": {"id": w["id"]}, "tile": {"id": t["id"]}}]})
    out = [{"type": t["type"], "itemType": t["itemType"], "minGeometry": t["geometry"],
            "config": {k: v for k, v in t["config"].items() if k != "hssWriteBinding"}} for t in new]
    return text_result({"ok": True, "defaults": out, "note": "probe tile added and removed again"})


def t_remap_plot_channels(args):
    ws = args["workspace"]
    mapping = {int(k): int(v) for k, v in args["mapping"].items()}
    data = upstream.call_json("get_plot_workspace", {"workspace": ws})
    w = data.get("workspace", data)
    ops = []
    changes = []
    for t in w.get("tiles", []):
        if t.get("type") != "Plot":
            continue
        old = [int(c) for c in (t.get("config") or {}).get("selectedChannels", [])]
        new = [mapping.get(c, c) for c in old]
        if new != old:
            ops.append({"op": "updateTileConfig", "workspace": {"id": w.get("id")},
                        "tile": {"id": t["id"]}, "patch": {"selectedChannels": new}})
            changes.append({"title": t.get("title"), "old": old, "new": new})
    if not ops:
        return text_result({"ok": True, "changes": []})
    tool = "preview_plot_workspace_patch" if args.get("dryRun") else "apply_plot_workspace_patch"
    res = upstream.call_json(tool, {"operations": ops})
    return text_result({"ok": res.get("ok", True), "changes": changes, "result": res})


WS_LOCATOR = {"type": "object", "description": "Workspace locator: id, index, name or current=true",
              "additionalProperties": True}

LOCAL_TOOLS = {
    "synex_status": (t_synex_status, "Is Synex running, its folder/config, and the AI bridge discovery file.",
                     {"type": "object", "properties": {}}),
    "get_connection_settings": (t_get_connection_settings,
                                "Read serial port, protocol, channel count/names, plot sample time and buffer "
                                "size from config.ini (works whether or not Synex is running).",
                                {"type": "object", "properties": {}}),
    "offline_update_settings": (t_offline_update_settings,
                                "Edit config.ini while Synex is CLOSED: physical channel names, serial port, "
                                "protocol type, plot sample time (ms per sample on the X axis), buffer size, "
                                "refresh fps, and clearing derived channels. Makes a backup first.",
                                {"type": "object", "properties": {
                                    "channelNames": {"type": "array", "items": {"type": "string"}},
                                    "serialPort": {"type": "string", "description": "e.g. COM36"},
                                    "protocolType": {"type": "string", "description": "e.g. JustFloat"},
                                    "plotSampleTimeMs": {"type": "number"},
                                    "serialBufferSize": {"type": "integer"},
                                    "plotRefreshFps": {"type": "integer"},
                                    "clearDerivedChannels": {"type": "boolean"},
                                    "resetPlotHistory": {"type": "boolean", "description":
                                                         "move plot_data.csv/.splot aside; Synex re-imports "
                                                         "it on startup and takes channel names from it"}}}),
    "get_tile_defaults": (t_get_tile_defaults,
                          "Return the full default config of a tile type (Plot, Numeric, Gauge, SerialCmd, "
                          "Toggle, ModeSelector, ...) by briefly adding a probe tile and removing it again. "
                          "addTile ignores config: add, then updateTileConfig with patch={...}. There is no Button tile (use ModeSelector with "
                          "resendOnSameState=true, or command-panel commands).",
                          {"type": "object", "properties": {
                              "type": {"type": "string"}, "title": {"type": "string"},
                              "workspace": WS_LOCATOR, "geometry": {"type": "object"}},
                           "required": ["type"]}),
    "remap_plot_channels": (t_remap_plot_channels,
                            "Rewrite selectedChannels of every Plot tile on one workspace using an "
                            "{old: new} channel index mapping. dryRun previews only.",
                            {"type": "object", "properties": {
                                "workspace": WS_LOCATOR,
                                "mapping": {"type": "object", "additionalProperties": {"type": "integer"}},
                                "dryRun": {"type": "boolean"}},
                             "required": ["workspace", "mapping"]}),
}


# ------------------------------------------------------------------ MCP server

def write(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=True) + "\n")
    sys.stdout.flush()


def handle(msg):
    method = msg.get("method")
    mid = msg.get("id")
    params = msg.get("params") or {}
    if method == "initialize":
        return {"protocolVersion": params.get("protocolVersion", PROTOCOL_VERSION),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "synex-plus", "version": "0.1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        tools = [{"name": n, "description": d, "inputSchema": s} for n, (_, d, s) in LOCAL_TOOLS.items()]
        try:
            upstream.ensure()
            tools += [t for t in upstream.tools if t["name"] not in LOCAL_TOOLS]
        except Exception as exc:  # upstream missing: still offer the local tools
            tools[0]["description"] += f" (upstream unavailable: {exc})"
        return {"tools": tools}
    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        if name in LOCAL_TOOLS:
            try:
                return LOCAL_TOOLS[name][0](args)
            except Exception as exc:
                return text_result(f"{type(exc).__name__}: {exc}", True)
        res = upstream.call(name, args)
        if "error" in res:
            raise RuntimeError(json.dumps(res["error"], ensure_ascii=False))
        return res.get("result", {})
    if mid is None:
        return None  # notification
    raise NotImplementedError(method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        mid = msg.get("id")
        try:
            result = handle(msg)
            if mid is not None:
                write({"jsonrpc": "2.0", "id": mid, "result": result})
        except Exception as exc:
            if mid is not None:
                write({"jsonrpc": "2.0", "id": mid, "error": {"code": -32000, "message": str(exc)}})


if __name__ == "__main__":
    main()
