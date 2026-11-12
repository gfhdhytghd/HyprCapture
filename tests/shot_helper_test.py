#!/usr/bin/env python3
"""Real helper: result transport and cancellation during frozen-session startup."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
from session_lock_fixture import SessionLockFixture


def main():
    helper = str(Path(sys.argv[1]).resolve())
    runtime = Path('/dev/shm') / f'hyprcapture-{os.getuid()}'
    runtime.mkdir(mode=0o700, exist_ok=True)
    with SessionLockFixture() as lock, tempfile.TemporaryDirectory(prefix='shot-helper-', dir=runtime) as directory:
        root = Path(directory)
        env = dict(lock.env, QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(root / 'config'),
                   XDG_CACHE_HOME=str(root / 'cache'), DBUS_SESSION_BUS_ADDRESS=f'unix:path={root}/no-bus', HYPRCAPTURE_TIMING_FILE=str(root / 'timing.log'))
        for case in ['success', 'cancel', 'disconnect', 'pending-cancel', 'cancel-shown', 'locked', 'crash']:
            (root / 'timing.log').write_text('')
            artifact = root / f'{case}.rgba'
            artifact.write_bytes(bytes([17, 29, 53, 255]) * 4096)
            artifact.chmod(0o600)
            metadata = root / f'{case}.json'
            metadata.write_text(json.dumps({
                'id': 'shot-helper-test', 'defaults': {'mode': 'fullscreen', 'save': True,
                'saveDir': str(root / 'save'), 'clipboard': True, 'showThumbnail': True,
                'screenshotNotification': True, 'rememberSettings': True, 'confirmBeforeCapture': True},
                'monitors': [{'name': 'fixture', 'geometry': {'x': 0, 'y': 0, 'width': 64, 'height': 64},
                    'scale': 1, 'transform': 0, 'focused': True, 'artifactPath': str(artifact), 'artifactWidth': 64,
                    'artifactHeight': 64, 'artifactTopDown': True}], 'windows': []}))
            metadata.chmod(0o600)
            startup, child_startup = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            result, child_result = socket.socketpair()
            result.settimeout(5)
            args = [helper, '--shot-result-stdout', '--session-json-stdin']
            if case == 'success': args.append('--quick')
            process = subprocess.Popen(args, stdin=child_startup, stdout=child_result, stderr=subprocess.PIPE, env=env)
            child_startup.close(); child_result.close()
            try:
                startup.send(os.fsencode(metadata))
                if case == 'pending-cancel': startup.send(b'cancel')
                if case == 'cancel': result.sendall(b'cancel')
                if case == 'disconnect': result.close()
                if case == 'cancel-shown':
                    deadline = time.monotonic() + 4
                    while 'ui.show_end' not in (root / 'timing.log').read_text() and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(.01)
                    assert 'ui.show_end' in (root / 'timing.log').read_text(), 'overlay did not open'
                    result.sendall(b'cancel')
                if case in ['locked', 'crash']:
                    deadline = time.monotonic() + 4
                    while (metadata.exists() or artifact.exists()) and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(.01)
                    if case == 'locked': lock.lock()
                    else: process.kill()
                data = b''
                if case != 'disconnect':
                    while True:
                        try:
                            part = result.recv(65536)
                        except ConnectionResetError:
                            assert case in ['cancel', 'cancel-shown', 'crash'], case
                            break
                        except TimeoutError:
                            raise AssertionError((case, process.poll(), (root / 'timing.log').read_text()))
                        if not part: break
                        data += part
                code = process.wait(timeout=5)
                assert code == (-9 if case == 'crash' else 0), (case, code, process.stderr.read().decode(), (root / 'timing.log').read_text() if (root / 'timing.log').exists() else '', data[:100])
                if case == 'success':
                    assert len(data) > 9 and data[0] == 0
                    assert struct.unpack('>Q', data[1:9])[0] == len(data) - 9
                    assert data[9:17] == b'\x89PNG\r\n\x1a\n'
                    assert not (root / 'save').exists(), 'shot saved to save_dir'
                    assert not (root / 'config/hyprcapture/last-settings.ini').exists(), 'shot wrote settings'
                elif case in ['pending-cancel', 'cancel-shown']:
                    assert data == struct.pack('>BQ', 1, 0), (case, data)
                elif case == 'locked':
                    assert not data or (data[0] == 2 and b'capture session closed' in data), (case, data[:30])
                else:
                    assert not data, (case, data[:30])
                if case in ['cancel', 'disconnect', 'pending-cancel']:
                    assert 'ui.show_begin' not in (root / 'timing.log').read_text(), 'cancelled startup mapped an overlay'
                assert not metadata.exists() and not artifact.exists(), 'frozen capture artifacts leaked'
            finally:
                result.close(); startup.close()
                if process.poll() is None: process.kill(); process.wait(timeout=5)
                if case == 'locked':
                    lock.locked = False
        print('shot helper: 7 real-helper scenarios passed')


if __name__ == '__main__':
    main()
