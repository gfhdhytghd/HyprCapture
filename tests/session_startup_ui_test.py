#!/usr/bin/env python3
"""Exercise the actual helper's prelaunch success and fail-closed paths."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from session_lock_fixture import SessionLockFixture


def main():
    helper = str(Path(sys.argv[1]).resolve())
    runtime = Path('/dev/shm') / f'hyprcapture-{os.getuid()}'
    runtime.mkdir(mode=0o700, exist_ok=True)
    assert runtime.stat().st_uid == os.getuid() and runtime.stat().st_mode & 0o777 == 0o700
    with SessionLockFixture() as lock, tempfile.TemporaryDirectory(prefix='startup-ui-test-', dir=runtime) as directory:
        root = Path(directory)
        env = dict(lock.env, QT_QPA_PLATFORM='offscreen',
                   XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'),
                   DBUS_SESSION_BUS_ADDRESS=f'unix:path={root}/no-session-bus')
        log = root / 'timing.log'
        env['HYPRCAPTURE_TIMING_FILE'] = str(log)
        cases = ['closed', 'relative', 'oversized', 'untrusted', 'invalid-json', 'valid', 'valid-cancel', 'valid-lock', 'valid-pending-cancel']
        for case in cases:
            log.write_text('')
            server, client = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            process = subprocess.Popen([helper, '--session-json-stdin'], stdin=client,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
            client.close()
            try:
                if case == 'relative':
                    server.send(b'relative/session.json')
                elif case == 'oversized':
                    server.send(b'/' * 4097)
                elif case == 'untrusted':
                    server.send(b'/etc/passwd')
                elif case == 'invalid-json' or case.startswith('valid'):
                    metadata = root / f'{case}.json'
                    artifact = root / 'monitor.rgba'
                    if case.startswith('valid'):
                        artifact.write_bytes(bytes([17, 29, 53, 255]) * (64 * 64))
                        artifact.chmod(0o600)
                        metadata.write_text(json.dumps({
                            'id': 'startup-ui-test', 'defaults': {'save': False, 'clipboard': False},
                            'monitors': [{'name': 'fixture', 'geometry': {'x': 0, 'y': 0, 'width': 64, 'height': 64},
                                          'scale': 1, 'transform': 0, 'focused': True,
                                          'artifactPath': str(artifact), 'artifactWidth': 64,
                                          'artifactHeight': 64, 'artifactTopDown': True}], 'windows': []}))
                    else:
                        metadata.write_text('{}')
                    metadata.chmod(0o600)
                    server.send(os.fsencode(metadata))
                    if case == 'valid-pending-cancel':
                        server.send(b'cancel')
                if case not in ('valid-cancel', 'valid-lock'):
                    server.close()
                if case == 'valid-pending-cancel':
                    assert process.wait(timeout=5) == 0, 'pending startup cancellation must survive a new unlocked handshake'
                    assert 'ui.show_begin' not in log.read_text(), 'cancelled startup constructed an overlay'
                elif case.startswith('valid'):
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline and process.poll() is None and 'ui.show_end' not in log.read_text():
                        time.sleep(0.01)
                    assert process.poll() is None, process.stderr.read().decode()
                    assert 'ui.show_end' in log.read_text(), 'valid metadata did not construct overlay'
                    assert not metadata.exists() and not artifact.exists(), 'session files were not consumed'
                    if case == 'valid-cancel':
                        server.send(b'cancel')
                        assert process.wait(timeout=5) == 0, 'cancel did not close the actual helper cleanly'
                    elif case == 'valid-lock':
                        lock.lock()
                        lock.locked = False  # Fast unlock must not resume this helper.
                        assert process.wait(timeout=5) == 0, 'lock did not close the overlay cleanly'
                    else:
                        process.terminate()
                        process.wait(timeout=5)
                else:
                    assert process.wait(timeout=5) == 1, (case, process.stderr.read().decode())
                    assert 'ui.show_begin' not in log.read_text(), 'failure fell through to a new capture'
            finally:
                server.close()
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
        print('startup UI: frozen session, graceful cancel, closed control peer and five failure paths passed')


if __name__ == '__main__':
    main()
