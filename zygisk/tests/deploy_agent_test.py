#!/usr/bin/env python3
"""Run the deploy-agent remote transaction against a host filesystem."""

import fcntl
import json
import mmap
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "deploy-agent.sh"
TARGETS = ("lib/arm64-v8a/libdejavu.so", "lib/arm64-v8a/libdejavu_agent.so", "config/init.lua")


class DeployAgentTest(unittest.TestCase):
    def run_transaction(self, fail_install=False, fail_restore=False, fail_cleanup=False, retained_backup=False, held_lock=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            module, uploads, binaries = root / "module", root / "uploads", root / "bin"
            uploads.mkdir()
            binaries.mkdir()
            originals = []
            mappings = []
            lock = None
            try:
                for index, relative in enumerate(TARGETS):
                    target = module / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(f"old-{index}".encode())
                    target.chmod(0o750 if index < 2 else 0o640)
                    link = root / f"mapped-{index}"
                    os.link(target, link)
                    originals.append((target, link, target.stat().st_ino, stat.S_IMODE(target.stat().st_mode)))
                    with target.open("rb") as handle:
                        mappings.append(mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ))
                if held_lock:
                    lock = (module / ".deploy.lock").open("a")
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                (binaries / "flock").write_text(f"#!{sys.executable}\n" + '''
import fcntl
import sys
try:
    fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    sys.exit(1)
''')
                (binaries / "flock").chmod(0o755)
                for name, content in (("libdejavu.so", b"new-core"),
                                      ("libdejavu_agent.so", b"new-agent"),
                                      ("dejavu-init.lua", b"new-init")):
                    (uploads / f"{name}.123").write_bytes(content)
                # Intercept actual shell operations, including the unsafe old cp
                # implementation, so the same mid-transaction failure is exercised.
                for command in ("cp", "mv"):
                    wrapper = binaries / command
                    wrapper.write_text("#!/bin/sh\n" + '''
source=$1
if [ "$source" = -f ]; then source=$2; fi
case "$source" in
  */.deploy.123/lib/arm64-v8a/libdejavu_agent.so)
    if [ "$FAIL_INSTALL" = 1 ]; then exit 73; fi ;;
  */.deploy-backup.123/libdejavu.so)
    if [ "$FAIL_RESTORE" = 1 ]; then exit 74; fi ;;
esac
''' + f"exec {shlex.quote(shutil.which(command))} \"$@\"\n")
                    wrapper.chmod(0o755)
                (binaries / "rm").write_text("#!/bin/sh\n" + '''
for argument do
  case "$argument" in
    */.deploy.123) if [ "$FAIL_CLEANUP" = 1 ]; then exit 75; fi ;;
  esac
done
''' + f"exec {shlex.quote(shutil.which('rm'))} \"$@\"\n")
                (binaries / "rm").chmod(0o755)
                sibling = module / ".deploy.other"
                sibling.mkdir()
                (sibling / "keep").write_text("unrelated")
                (binaries / "chcon").write_text("#!/bin/sh\nexit 0\n")
                (binaries / "chcon").chmod(0o755)
                if retained_backup:
                    (module / ".deploy-backup.123").mkdir()
                    (module / ".deploy-backup.123/keep").write_bytes(b"last recovery copy")
                source = SCRIPT.read_text().split("<<'DEPLOY_SCRIPT'\n", 1)[1].split("\nDEPLOY_SCRIPT", 1)[0]
                source = source.replace("/data/adb/modules/dejavu_zygisk", str(module))
                source = source.replace("/data/local/tmp", str(uploads)).replace("__SUFFIX__", "123")
                result = subprocess.run(["sh"], input=source, text=True, capture_output=True,
                                        timeout=5, env={**os.environ, "PATH": f"{binaries}:{os.environ['PATH']}",
                                                        "FAIL_INSTALL": str(int(fail_install)),
                                                        "FAIL_RESTORE": str(int(fail_restore)),
                                                        "FAIL_CLEANUP": str(int(fail_cleanup))})
                self.assertEqual(result.returncode,
                                 1 if retained_backup or held_lock else 73 if fail_install else 0,
                                 result.stderr)
                for index, (target, link, inode, mode) in enumerate(originals):
                    self.assertEqual(link.read_bytes(), f"old-{index}".encode())
                    self.assertEqual(mappings[index][:], f"old-{index}".encode())
                    if (fail_install or retained_backup or held_lock) and not (fail_restore and index == 0):
                        self.assertEqual(target.read_bytes(), link.read_bytes())
                        self.assertEqual(target.stat().st_ino, inode)
                        self.assertEqual(stat.S_IMODE(target.stat().st_mode), mode)
                if retained_backup:
                    self.assertEqual((module / ".deploy-backup.123/keep").read_bytes(),
                                     b"last recovery copy")
                elif fail_restore:
                    saved = module / ".deploy-backup.123/libdejavu.so"
                    self.assertTrue(saved.is_file(), result.stderr)
                    self.assertEqual(saved.read_bytes(), b"old-0")
                    self.assertIn("rollback", result.stderr)
                else:
                    self.assertFalse((module / ".deploy-backup.123").exists())
                self.assertEqual((module / ".deploy.123").exists(), fail_cleanup)
                self.assertEqual((sibling / "keep").read_text(), "unrelated")
                self.assertTrue((module / ".deploy.lock").is_file())
                if lock is not None:
                    self.assertEqual((module / ".deploy.lock").stat().st_ino,
                                     os.fstat(lock.fileno()).st_ino)
                if fail_cleanup:
                    self.assertIn("staging cleanup failed", result.stderr)
                if not fail_install and not retained_backup and not held_lock:
                    for relative, content in zip(TARGETS, (b"new-core", b"new-agent", b"new-init")):
                        self.assertEqual((module / relative).read_bytes(), content)
            finally:
                for mapping in mappings:
                    mapping.close()
                if lock is not None:
                    lock.close()

    def test_concurrent_deploy_lock_rejects_without_mutating_targets(self):
        self.run_transaction(held_lock=True)

    def test_retained_backup_is_never_reused_or_cleaned(self):
        self.run_transaction(retained_backup=True)

    def test_install_keeps_running_mappings_intact(self):
        self.run_transaction()

    def test_middle_replacement_failure_restores_original_inodes(self):
        self.run_transaction(fail_install=True)

    def test_cleanup_failure_preserves_primary_error_and_backup(self):
        self.run_transaction(fail_install=True, fail_restore=True, fail_cleanup=True)

    def test_failed_rollback_retains_backup_and_primary_error(self):
        self.run_transaction(fail_install=True, fail_restore=True)


class DeployWrapperTest(unittest.TestCase):
    def test_normal_wrapper_substitutes_script_and_preserves_shell_exit_status(self):
        for shell_status in (0, 37):
            with self.subTest(shell_status=shell_status), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                script = root / "zygisk/scripts/deploy-agent.sh"
                script.parent.mkdir(parents=True)
                shutil.copyfile(SCRIPT, script)
                for name in ("agent", "core", "init", "hookx"):
                    (root / name).write_text(name)
                fake_adb = root / "adb"
                fake_adb.write_text(f"#!{sys.executable}\n" + r'''
import json
import os
from pathlib import Path
import re
import sys
import time
args = sys.argv[1:]
log = os.open(os.environ["FAKE_LOG"], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
os.write(log, (json.dumps(args) + "\n").encode())
os.close(log)
if args[0] == "push":
    if args[2].endswith(".sh"):
        source = Path(args[1]).read_text()
        assert "__SUFFIX__" not in source
        suffix = re.search(r"dejavu-deploy\.([0-9]+)\.sh$", args[2]).group(1)
        assert re.search(r"^suffix=" + suffix + r"$", source, re.M)
        assert "mv -f" in source
        Path(os.environ["CAPTURED_SCRIPT"]).write_text(source)
    time.sleep(0.02)
elif args[0] == "shell":
    assert Path(os.environ["CAPTURED_SCRIPT"]).is_file()
    sys.exit(int(os.environ["SHELL_STATUS"]))
else:
    sys.exit(99)
''')
                fake_adb.chmod(0o755)
                env = {**os.environ, "ADB": str(fake_adb), "ADB_SERIAL": "", "NO_RESTART": "1",
                       "AGENT_LIB": str(root / "agent"), "CORE_LIB": str(root / "core"),
                       "INIT_LUA": str(root / "init"), "HOOKX_LUA": str(root / "hookx"),
                       "FAKE_LOG": str(root / "log"), "CAPTURED_SCRIPT": str(root / "captured"),
                       "SHELL_STATUS": str(shell_status)}
                count = 2 if shell_status == 0 else 1
                processes = [subprocess.Popen(["bash", str(script)], text=True,
                                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                             for _ in range(count)]
                try:
                    for process in processes:
                        _, stderr = process.communicate(timeout=10)
                        self.assertEqual(process.returncode, shell_status, stderr)
                finally:
                    for process in processes:
                        if process.poll() is None:
                            process.kill()
                            process.communicate()
                calls = [json.loads(line) for line in (root / "log").read_text().splitlines()]
                self.assertEqual(sum(call[0] == "push" for call in calls), 4 * count)
                self.assertEqual(sum(call[0] == "shell" for call in calls), count)
                script_uploads = [call for call in calls if call[0] == "push"
                                  and call[2].endswith(".sh")]
                suffixes = [Path(call[2]).name.split(".")[1] for call in script_uploads]
                self.assertEqual(len(set(suffixes)), count)
                for suffix in suffixes:
                    self.assertTrue(suffix.isdecimal())
                    self.assertGreater(int(suffix), 2 ** 64)
                    self.assertLess(int(suffix), 2 ** 128)
                    self.assertNotIn(int(suffix), [process.pid for process in processes])
                    self.assertEqual(sum(call[0] == "push" and suffix in call[2]
                                         for call in calls), 4)
                self.assertEqual(list((root / "out/tooling").iterdir()), [])
                temporary_sources = [Path(call[1]).name for call in calls if call[0] == "push"
                                     and "out/tooling" in call[1]]
                self.assertEqual(len(temporary_sources), 2 * count)
                self.assertEqual(len(set(temporary_sources)), 2 * count)
                self.assertTrue(all("XXXXXX" not in name for name in temporary_sources))


if __name__ == "__main__":
    unittest.main()
