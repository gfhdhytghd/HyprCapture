#!/usr/bin/env bash
# Optional AEC failure must never fail an otherwise usable capture installation.
set -u
scriptdir=$(cd -- "$(dirname -- "$0")" && pwd)
default_bindir=$HOME/.local/bin
[[ $(basename -- "$0") == hyprcapture-install-aec ]] && default_bindir=$scriptdir
bindir=${1:-$default_bindir}
runtime=${XDG_DATA_HOME:-$HOME/.local/share}/hyprcapture/aec/runtime
if [[ ! -f "$runtime/libtensorflowlite_c.so" ]]; then
    builder="$scriptdir/build-aec-runtime.sh"
    [[ -x "$builder" ]] || builder="$scriptdir/hyprcapture-build-aec-runtime"
    if ! "$builder" "$runtime"; then
        echo 'HyprCapture AEC runtime pending; run hyprcapture-install-aec to retry.' >&2
    fi
fi
if ! "$bindir/hyprcapture-aec" --install; then
    echo 'HyprCapture AEC check pending; normal capture remains available.' >&2
fi
exit 0
