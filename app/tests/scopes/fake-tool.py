#!/usr/bin/env python3
import fcntl
import hashlib
import os
from pathlib import Path
import sys
root = Path(os.environ['DEJAVU_SCOPE_SANDBOX']).resolve()
assert root.name.startswith('dejavu-scopes.')
assert (root / '.host-only').read_text() == 'scopes-host-only\n'
args = sys.argv[1:]
tool = Path(sys.argv[0]).name
if tool == 'flock':
    assert args == ['-n', '9']
    try:
        fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(1)
elif tool == 'sha256sum':
    print(hashlib.sha256(Path(args[0]).read_bytes()).hexdigest() + '  ' + args[0])
elif tool == 'chown':
    pass
elif tool == 'mv':
    marker = root / 'fail-rename'
    if marker.exists():
        marker.unlink()
        sys.exit('Injected rename failure')
    os.execv('/bin/mv', ['/bin/mv', *args])
else:
    sys.exit('Unexpected test tool')
