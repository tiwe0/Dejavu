#!/usr/bin/env python3
"""Host-side protocol tests for the Dejavu MCP server (no device required)."""

import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import dejavu_mcp  # noqa: E402


class FakeRunner:
    def __init__(self):
        self.calls = []

    def run(self, command, timeout=None):
        self.calls.append((list(command), timeout))
        if command == ["status"]:
            return "fake status"
        if command == ["targets", "list"]:
            return "fake targets"
        if command[:3] == ["plugin", "-p", "pkg"] and command[-1] == "list":
            return "fake plugins"
        if command[:5] == ["plugin", "-p", "pkg", "call", "demo"]:
            return "called"
        if command == ["exec", "-p", "pkg", "-e", "return 1"]:
            return "1"
        return "ok"


def main():
    runner = FakeRunner()
    server = dejavu_mcp.McpServer(runner, default_process="pkg")

    initialized = server.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"protocolVersion": "2024-11-05", "capabilities": {}},
        }
    )
    assert initialized["result"]["protocolVersion"] == "2024-11-05"
    assert initialized["result"]["serverInfo"]["name"] == "dejavu-mcp"

    listed = server.handle_request({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    names = {tool["name"] for tool in listed["result"]["tools"]}
    for expected in (
        "dejavu_status",
        "dejavu_state",
        "dejavu_exec",
        "dejavu_plugin_list",
        "dejavu_plugin_call",
        "dejavu_plugin_install",
        "dejavu_logs_tail",
    ):
        assert expected in names, expected
    assert server.handle_request(
        {"jsonrpc": "2.0", "method": "notifications/initialized"}
    ) is None

    status = server.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "dejavu_status", "arguments": {}},
        }
    )
    assert status["result"]["content"][0]["text"] == "fake status"
    assert status["result"]["isError"] is False

    executed = server.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "tools/call",
            "params": {
                "name": "dejavu_exec",
                "arguments": {"code": "return 1", "timeout": 5},
            },
        }
    )
    assert executed["result"]["content"][0]["text"] == "1"

    called = server.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 5,
            "method": "tools/call",
            "params": {
                "name": "dejavu_plugin_call",
                "arguments": {
                    "plugin": "demo",
                    "function": "add",
                    "arguments": [1, 2, "path", True, None],
                },
            },
        }
    )
    assert called["result"]["content"][0]["text"] == "called"
    command = runner.calls[-1][0]
    assert command[-10:] == [
        "--arg",
        "1",
        "--arg",
        "2",
        "--arg",
        "s:path",
        "--arg",
        "true",
        "--arg",
        "null",
    ], command

    missing = server.handle_request(
        {
            "jsonrpc": "2.0",
            "id": 6,
            "method": "tools/call",
            "params": {"name": "dejavu_does_not_exist", "arguments": {}},
        }
    )
    assert missing["result"]["isError"] is True

    unknown = server.handle_request({"jsonrpc": "2.0", "id": 7, "method": "no/such"})
    assert unknown["error"]["code"] == -32601

    # Exercise stdio framing with the same handler.
    stdin = io.StringIO(
        '{"jsonrpc":"2.0","id":8,"method":"ping"}\n'
        '{"jsonrpc":"2.0","method":"notifications/initialized"}\n'
    )
    stdout = io.StringIO()
    assert server.serve(stdin, stdout) == 0
    responses = [json.loads(line) for line in stdout.getvalue().splitlines() if line]
    assert len(responses) == 1 and responses[0]["result"] == {}

    print("OK: MCP protocol and tool schemas")


if __name__ == "__main__":
    main()
