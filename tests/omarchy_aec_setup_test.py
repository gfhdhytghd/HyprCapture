"""Optional AEC failure and offline installation cannot hold the core setup lock."""
import fcntl
import pathlib
import subprocess
import tempfile
import time

repo = pathlib.Path(__file__).resolve().parents[1]


def run(body, root):
    return subprocess.run(
        ['bash', '-c', 'set -euo pipefail; DIR=$1; CACHE=$2; source "$DIR/omarchy/plugin.conf"; ' + body,
         'test', str(repo), str(root)], capture_output=True, text=True, timeout=3,
    )


with tempfile.TemporaryDirectory(prefix='hyprcapture-optional-aec-') as folder:
    root = pathlib.Path(folder)
    stage = root / 'stage'
    stage.mkdir()
    fakebin = root / 'bin'
    fakebin.mkdir()
    cmake = fakebin / 'cmake'
    cmake.write_text('#!/bin/sh\necho simulated-AEC-build-failure >&2\nexit 1\n')
    cmake.chmod(0o755)
    result = run('PATH="$CACHE/bin:$PATH"; stage_extras "$CACHE/stage"; '
                 '[[ ${#FILES[@]} == 2 && ${#TARGETS[@]} == 2 ]]', root)
    assert result.returncode == 0, result.stderr
    assert 'AEC components pending' in result.stderr
    assert (stage / 'hyprcapture-install-aec').exists()
    assert (stage / 'hyprcapture-aec-sources.json').read_bytes() == (repo / 'nix/aec-sources.json').read_bytes()
    assert (stage / 'hyprcapture-prepare-aec-sources.cmake').exists()
    assert not (stage / 'hyprcapture-aec').exists()

    worker = stage / 'hyprcapture-aec'
    worker.write_text('#!/bin/sh\nexit 0\n')
    worker.chmod(0o755)
    installer = stage / 'hyprcapture-install-aec'
    installer.write_text('#!/bin/bash\necho started >> "$1/started"\n'
                         'while [[ ! -f $1/release ]]; do sleep .02; done\n'
                         'echo simulated-offline-install-failure >&2\n'
                         'touch "$1/finished"\nexit 1\n')
    installer.chmod(0o755)
    try:
        body = 'exec 9>"$CACHE/.lock"; flock 9; setup_extras "$CACHE/stage"'
        result = run(body, root)
        assert result.returncode == 0, result.stderr
        deadline = time.monotonic() + 2
        while not (stage / 'started').exists() and time.monotonic() < deadline:
            time.sleep(.01)
        assert (stage / 'started').exists(), 'background installer did not start'
        assert not (stage / 'finished').exists(), 'installer must still be waiting offline'
        with (root / '.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = run(body, root)
        assert result.returncode == 0, result.stderr
        time.sleep(.1)
        assert (stage / 'started').read_text().count('started') == 1, 'duplicate installation'
    finally:
        (stage / 'release').touch()
        deadline = time.monotonic() + 2
        while not (stage / 'finished').exists() and time.monotonic() < deadline:
            time.sleep(.01)
    assert (stage / 'finished').exists(), 'failed installer did not finish'
    assert 'simulated-offline-install-failure' in (root / 'aec-install.log').read_text()
print('PASS: optional native build failure, offline background failure, setup lock release and single installer')
