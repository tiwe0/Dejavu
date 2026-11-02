#!/usr/bin/env python3
"""Non-privileged tools used only by the native-library host test."""
import hashlib
import base64
import fcntl
import json
import os
from pathlib import Path
import re
import sys
import tempfile

root = Path(os.environ['DEJAVU_NATIVE_SANDBOX']).resolve()
assert root.parent == Path(tempfile.gettempdir()).resolve()
assert root.name.startswith('dejavu-native-regression.')
assert (root / '.host-only').read_text() == 'native-host-only\n'
tool = Path(sys.argv[0]).name
args = sys.argv[1:]
if tool == 'flock':
    assert args == ['-n', '9']
    try:
        fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit(1)
elif tool in ('chown', 'chcon', 'restorecon'):
    assert str(root / 'mapped-data') in args[-1]
    marker = root / 'change-slot-on-copy'
    if tool == 'chcon' and args[-1].endswith('/new-plugin') and marker.exists():
        marker.unlink()
        path = root / 'properties.json'
        props = json.loads(path.read_text())
        props['debug.dejavu.plugins.allowed.1'] = '/data/concurrently-installed.so'
        path.write_text(json.dumps(props))
elif tool == 'sha256sum':
    assert len(args) == 1 and str(root / 'mapped-data') in args[0]
    print(hashlib.sha256(Path(args[0]).read_bytes()).hexdigest() + '  ' + args[0])
elif tool == 'base64':
    assert len(args) == 1 and str(root / 'mapped-data') in args[0]
    print(base64.b64encode(Path(args[0]).read_bytes()).decode('ascii'))
elif tool == 'mv':
    assert len(args) == 2 and all(str(root / 'mapped-data') in arg for arg in args)
    marker = root / 'fail-next-import-commit'
    if Path(args[0]).name.startswith('.native-import-') and marker.exists():
        marker.unlink()
        raise SystemExit('Injected import commit failure')
    os.execv('/bin/mv', ['/bin/mv', *args])
elif tool in ('getprop', 'setprop'):
    assert re.fullmatch(r'debug\.dejavu\.plugins\.(?:allowed|sha256|skill\.sha256)\.\d+', args[0])
    # Every trust read/write must happen while the separate device shell holds
    # the shared lock, including rollback after an injected property failure.
    with (root / 'mapped-data/adb/dejavu/.plugin-trust.lock').open('a') as shared_lock:
        try:
            fcntl.flock(shared_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            raise SystemExit('Trust property operation occurred without the shared lock')
    path = root / 'properties.json'
    props = json.loads(path.read_text()) if path.exists() else {}
    if tool == 'getprop':
        assert len(args) == 1
        print(props.get(args[0], ''))
    else:
        assert len(args) == 2
        marker = root / 'fail-next-hash-write'
        if args[0] == 'debug.dejavu.plugins.sha256.1' and marker.exists():
            marker.unlink()
            raise SystemExit('Injected property update failure')
        props[args[0]] = args[1]
        path.write_text(json.dumps(props))
else:
    raise SystemExit('Unexpected test tool')
