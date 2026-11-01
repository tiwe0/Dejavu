#!/usr/bin/env python3
"""Manual end-to-end MCP test against a real Android target.

Usage:
    ADB=... ADB_SERIAL=... TARGET_PROCESS=com.android.settings \
        python3 tests/mcp_device_test.py

This installs a JIT hello plugin, exercises the MCP tool surface, and cleans up
the allowlist/device files. It is intentionally not wired into CI because it
needs an authorized rooted device.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROCESS = os.environ.get("TARGET_PROCESS", "com.android.settings")
JIT = ROOT / "plugins" / "hello" / "hello_jit.c"


def request(request_id: int, method: str, params: dict | None = None) -> dict:
    message: dict = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        message["params"] = params
    return message


def main() -> int:
    if not JIT.is_file():
        print(f"missing JIT example: {JIT}", file=sys.stderr)
        return 2
    requests = [
        request(1, "initialize", {"protocolVersion": "2025-06-18", "capabilities": {}}),
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        request(2, "tools/list"),
        request(3, "tools/call", {"name": "dejavu_state", "arguments": {}}),
        request(
            4,
            "tools/call",
            {"name": "dejavu_exec", "arguments": {"code": "return 21*2"}},
        ),
        request(
            5,
            "tools/call",
            {"name": "dejavu_plugin_install", "arguments": {"source": str(JIT)}},
        ),
        request(6, "tools/call", {"name": "dejavu_plugin_list", "arguments": {}}),
        request(
            7,
            "tools/call",
            {
                "name": "dejavu_plugin_call",
                "arguments": {
                    "plugin": "hellojit",
                    "function": "add",
                    "arguments": [1, 2],
                },
            },
        ),
        request(
            8,
            "tools/call",
            {
                "name": "dejavu_plugin_jit",
                "arguments": {
                    "path": f"/data/user_de/0/{PROCESS}/files/hello_jit.c"
                },
            },
        ),
        request(
            9,
            "tools/call",
            {"name": "dejavu_plugin_unload", "arguments": {"name": "hellojit"}},
        ),
        request(
            10,
            "tools/call",
            {"name": "dejavu_plugin_allow", "arguments": {"clear": True}},
        ),
        request(11, "tools/call", {"name": "dejavu_targets", "arguments": {}}),
        request(12, "tools/call", {"name": "dejavu_logs_tail", "arguments": {"lines": 5}}),
        request(
            13,
            "tools/call",
            {"name": "dejavu_cli", "arguments": {"arguments": ["status"]}},
        ),
    ]
    payload = "".join(json.dumps(item) + "\n" for item in requests)
    server = subprocess.Popen(
        [sys.executable, str(ROOT / "dejavu"), "mcp", "-p", PROCESS],
        cwd=str(ROOT),
        env=os.environ.copy(),
        text=True,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        assert server.stdin is not None
        stdout, stderr = server.communicate(payload, timeout=600)
    finally:
        if server.poll() is None:
            server.kill()
    if stderr.strip():
        print("SERVER STDERR:", stderr.strip()[:2000])
    responses: dict[int, dict] = {}
    for line in stdout.splitlines():
        if line.strip():
            message = json.loads(line)
            responses[message.get("id")] = message

    def text(request_id: int) -> str:
        result = responses[request_id].get("result", {})
        return "\n".join(part.get("text", "") for part in result.get("content", []))

    failures: list[str] = []

    def check(condition: bool, label: str, detail: str = "") -> None:
        if condition:
            print(f"PASS: {label}")
        else:
            print(f"FAIL: {label}: {detail}")
            failures.append(label)

    check(responses[1]["result"]["protocolVersion"] == "2025-06-18", "initialize protocol")
    tools = responses[2]["result"]["tools"]
    check(len(tools) == 21, "tools/list count", str(len(tools)))
    try:
        state = json.loads(text(3))
        check(state.get("process") == PROCESS, "state process", str(state.get("process")))
        check("status" in state and "plugins" in state, "state sections")
    except json.JSONDecodeError as error:
        check(False, "state JSON", f"{error}: {text(3)[:200]}")
    check(text(4).strip() == "42", "exec returns 42", text(4)[:200])
    install_text = text(5)
    check(
        "installed hello_jit.c" in install_text and "hellojit" in install_text,
        "plugin install",
        install_text[:300],
    )
    list_text = text(6)
    check(
        "hellojit" in list_text and "jit=yes" in list_text,
        "plugin list after install",
        list_text[:300],
    )
    check(text(7).strip() == "3", "plugin call add(1,2)=3", text(7)[:200])
    check("already loaded" in text(8), "duplicate JIT rejected", text(8)[:300])
    check("hellojit unloaded" in text(9), "plugin unload", text(9)[:200])
    check("cleared" in text(10), "plugin allow clear", text(10)[:200])
    check(PROCESS in text(11), "targets list", text(11)[:300])
    check(len(text(12).splitlines()) <= 5, "logs tail limit", text(12)[:300])
    check("repository:" in text(13), "cli escape hatch", text(13)[:200])
    if stderr.strip():
        failures.append("server stderr non-empty")
    print(f"RESULT: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
