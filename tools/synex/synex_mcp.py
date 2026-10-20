"""Minimal MCP stdio client: start a server exe, initialize, then list tools or call one.

Usage:
  python mcp_stdio.py <server.exe> list
  python mcp_stdio.py <server.exe> call <tool> '<json args>' [<tool> '<json args>' ...]
"""
import json
import subprocess
import sys
import threading
import queue


def main():
    exe = sys.argv[1]
    mode = sys.argv[2]
    proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, bufsize=0)
    q = queue.Queue()

    def reader():
        for line in proc.stdout:
            q.put(line)
    threading.Thread(target=reader, daemon=True).start()

    def send(obj):
        proc.stdin.write((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
        proc.stdin.flush()

    def wait(id_, timeout=15):
        while True:
            line = q.get(timeout=timeout)
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("id") == id_:
                return msg

    send({"jsonrpc": "2.0", "id": 1, "method": "initialize",
          "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                     "clientInfo": {"name": "claude-code-script", "version": "1.0"}}})
    init = wait(1)
    info = init.get("result", {}).get("serverInfo", {})
    print("server:", json.dumps(info, ensure_ascii=False))
    send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    if mode == "list":
        send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
        res = wait(2)
        for t in res.get("result", {}).get("tools", []):
            props = t.get("inputSchema", {}).get("properties", {})
            print(f"- {t['name']}: {t.get('description', '')[:160]}")
            if props:
                print("    args:", json.dumps({k: v.get("type", "?") for k, v in props.items()}, ensure_ascii=False))
    elif mode == "call":
        args = sys.argv[3:]
        rid = 10
        for i in range(0, len(args), 2):
            name = args[i]
            params = json.loads(args[i + 1]) if i + 1 < len(args) else {}
            send({"jsonrpc": "2.0", "id": rid, "method": "tools/call",
                  "params": {"name": name, "arguments": params}})
            res = wait(rid, timeout=30)
            print(f"=== {name} {json.dumps(params, ensure_ascii=False)}")
            if "error" in res:
                print("ERROR:", json.dumps(res["error"], ensure_ascii=False))
            for c in res.get("result", {}).get("content", []):
                print(c.get("text", c))
            rid += 1
    proc.stdin.close()
    proc.terminate()


if __name__ == "__main__":
    main()
