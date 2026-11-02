#!/usr/bin/env python3
"""Host-only su/tool shim. All /data paths map to one marked temp directory."""

import base64
import json
import os
from pathlib import Path
import re
import sys
import tempfile


def fail(message):
    print("importer host sandbox: " + message, file=sys.stderr)
    raise SystemExit(90)


root_text = os.environ.get("DEJAVU_IMPORTER_SANDBOX", "")
if not root_text:
    fail("missing sandbox")
root = Path(root_text).resolve()
temporary = Path(tempfile.gettempdir()).resolve()
if (
    not root.is_dir()
    or root.parent != temporary
    or not root.name.startswith("dejavu-importer-regression.")
    or not re.fullmatch(r"/[A-Za-z0-9_./-]+", str(root))
    or (root / ".host-only").read_text() != "dejavu-importer-host-only\n"
):
    fail("sandbox must be a marked, isolated temp directory")

mapped = root / "mapped-data"
tool = Path(sys.argv[0]).name
arguments = sys.argv[1:]


def audit(name, values):
    with (root / "audit.jsonl").open("a") as output:
        output.write(json.dumps({"tool": name, "args": values}) + "\n")


def contained(path):
    # A symlink within the sandbox is permitted here so that the *production*
    # importer, rather than the shim, must reject stored symlink resources.
    candidate = Path(path).resolve()
    if candidate != mapped and mapped not in candidate.parents:
        fail("tool path escaped the mapped data tree")


if tool == "su":
    if arguments[:1] == ["-M"]:
        arguments = arguments[1:]
    if len(arguments) != 2 or arguments[0] != "-c":
        fail("only the production su -c contract is supported")
    command = arguments[1]
    prefix = re.compile(r"(?<![A-Za-z0-9_.-])/data(?=/|[\s'\";]|$)")
    rewritten, count = prefix.subn(lambda _: str(mapped), command)
    if count == 0 or prefix.search(rewritten):
        fail("expected a fixed /data prefix and a fully mapped command")
    audit("su", [rewritten])
    os.execv("/bin/sh", ["/bin/sh", "-c", rewritten])
elif tool == "chown":
    if len(arguments) != 2 or arguments[0] != "0:0":
        fail("unexpected ownership command")
    contained(arguments[1])
    audit("chown-skipped", arguments)
elif tool == "base64":
    if len(arguments) != 1:
        fail("expected Android base64's one-file encoding form")
    contained(arguments[0])
    audit("base64", arguments)
    encoded = base64.b64encode(Path(arguments[0]).read_bytes())
    for offset in range(0, len(encoded), 76):
        sys.stdout.buffer.write(encoded[offset : offset + 76] + b"\n")
elif tool == "mv":
    if len(arguments) != 2:
        fail("unexpected move command")
    for argument in arguments:
        contained(argument)
    audit("mv", arguments)
    marker = root / "fail-next-commit"
    if Path(arguments[0]).name.startswith(".import-") and marker.exists():
        marker.unlink()
        print("injected staging commit failure", file=sys.stderr)
        raise SystemExit(61)
    os.execv("/bin/mv", ["/bin/mv", *arguments])
else:
    fail("unexpected shim name")
