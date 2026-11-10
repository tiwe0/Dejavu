#!/usr/bin/env bash
# Fetch libevent development headers and static libraries without root.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/libevent"
mkdir -p "$DEST"
cd "$DEST"
rm -f libevent-dev*.deb
apt-get download libevent-dev
DEB=$(ls -1 libevent-dev*.deb | head -1)
dpkg-deb -x "$DEB" "$DEST"
echo "libevent extracted under $DEST/usr"
echo "build with: make LIBEVENT_PREFIX=$DEST/usr"