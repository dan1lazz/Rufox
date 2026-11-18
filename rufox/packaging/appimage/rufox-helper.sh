#!/bin/sh
# pkexec sanitises LD_LIBRARY_PATH. Resolve the bundled helper relative to this
# wrapper so the privileged process loads exactly the libraries in the AppImage.
set -eu
APPDIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export APPDIR
export PATH="$APPDIR/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export LD_LIBRARY_PATH="$APPDIR/usr/lib"
export XDG_DATA_DIRS="$APPDIR/usr/share:/usr/local/share:/usr/share"
exec "$APPDIR/usr/libexec/rufox-helper.bin" "$@"
