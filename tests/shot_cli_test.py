#!/usr/bin/env python3
"""Black-box CLI tests with isolated hyprctl/compositor boundary, never host IPC."""
import base64
import json
import os
from pathlib import Path
import pty
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=')


def fake():
    expression = sys.argv[2]
    log = Path(os.environ['SHOT_LOG'])
    with log.open('a') as f:
        f.write(expression + '\n')
    if '.shot_cancel(' in expression:
        return
    case = os.environ['SHOT_CASE']
    if case == 'no-plugin':
        print('attempt to index a nil value')  # eval can return errors with exit 0
        return
    if case in ('locked', 'busy'):
        print('session locked' if case == 'locked' else 'another capture session already active')
        sys.exit(1)
    if case == 'trigger-hang':
        time.sleep(20)
        return
    path = json.loads(expression.split('shot(', 1)[1].split(',', 1)[0])
    # Keep helper lifetime independent of the triggering hyprctl process.
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'peer', path],
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, close_fds=True)
    # Model the synchronous connection acknowledgement of the plugin.
    deadline = time.monotonic() + 3
    while not Path(str(log) + '.connected').exists() and time.monotonic() < deadline:
        time.sleep(.005)


def peer(path):
    case = os.environ['SHOT_CASE']
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as channel:
        channel.connect(path)
        Path(os.environ['SHOT_LOG'] + '.connected').touch()
        if case in ('wait', 'signal', 'slow'):
            if case == 'slow':
                channel.sendall(struct.pack('>BQ', 0, len(PNG)) + PNG[:10])
            channel.settimeout(5)
            message = channel.recv(32)
            Path(os.environ['SHOT_LOG'] + '.cancelled').write_bytes(message)
            return
        if case == 'crash':
            return
        if case == 'truncated':
            channel.sendall(struct.pack('>BQ', 0, len(PNG)) + PNG[:16])
            return
        if case == 'error':
            data = b'helper encode failure\nsecond line'
            channel.sendall(struct.pack('>BQ', 2, len(data)) + data)
            return
        if case == 'cancel':
            channel.sendall(struct.pack('>BQ', 1, 0))
            return
        data = b'invalid' if case == 'invalid' else PNG
        frame = struct.pack('>BQ', 0, len(data)) + data
        # Split the protocol header and payload to exercise stream framing.
        for byte in frame:
            channel.sendall(bytes([byte]))


def main():
    binary = str(Path(sys.argv[1]).resolve())
    runtime = Path('/dev/shm') / f'hyprcapture-{os.getuid()}'
    with tempfile.TemporaryDirectory(prefix='shot-cli-', dir=Path(binary).parent) as directory:
        root = Path(directory)
        stub = root / 'hyprctl'
        stub.write_text(f'#!{sys.executable}\nimport runpy,sys\nsys.argv[0]={str(Path(__file__).resolve())!r}\nrunpy.run_path(sys.argv[0],run_name="__main__")\n')
        stub.chmod(0o700)
        env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'],
                   XDG_RUNTIME_DIR=str(root), HYPRLAND_INSTANCE_SIGNATURE='shot-test',
                   DBUS_SESSION_BUS_ADDRESS=f'unix:path={root}/absent-bus')
        sequence = 0
        def environment(case):
            nonlocal sequence
            sequence += 1
            log = root / f'log-{sequence}'
            return dict(env, SHOT_CASE=case, SHOT_LOG=str(log)), log
        def run(case, args=(), code=0, image=PNG):
            before = set(runtime.glob('shot-??????')) if runtime.exists() else set()
            current, log = environment(case)
            result = subprocess.run([binary, *args], env=current, capture_output=True, timeout=5)
            assert result.returncode == code, (case, result.returncode, result.stderr)
            assert result.stdout == image, (case, result.stdout)
            if code == 2 or code == 64:
                assert result.stderr.count(b'\n') == 1, result.stderr
            assert set(runtime.glob('shot-??????')) == before, ('socket/temp leak', case)
            return log
        for args in [('bad',), ('region', 'window'), ('-o',), ('--timeout',),
                     ('--timeout', '0'), ('--timeout', '-1'), ('--timeout', 'nan'),
                     ('--timeout', 'inf'), ('--timeout', '1x'), ('--unknown',),
                     ('-o', ''), ('--editor', '--editor'), ('--timeout', '1e9999')]:
            run('success', args, 64, b'')
        master, slave = pty.openpty()
        current, _ = environment('success')
        try:
            result = subprocess.run([binary], env=current, stdout=slave, stderr=subprocess.PIPE, timeout=3)
            assert result.returncode == 64 and b'terminal' in result.stderr
        finally:
            os.close(master); os.close(slave)
        for mode in ['region', 'fullscreen', 'window']:
            log = run('success', (mode, '--editor'))
            assert f'"{mode}",true)' in log.read_text()
        run('success')
        for case, code in [('cancel', 1), ('crash', 2), ('truncated', 2), ('no-plugin', 2),
                           ('error', 2), ('invalid', 2), ('locked', 2), ('busy', 2)]:
            run(case, code=code, image=b'')
        target = root / 'a file.png'
        target.write_bytes(b'original')
        for case in ['cancel', 'crash', 'truncated']:
            run(case, ('-o', str(target)), 1 if case == 'cancel' else 2, b'')
            assert target.read_bytes() == b'original'
        run('success', ('-o', str(target)), image=b'')
        assert target.read_bytes() == PNG and target.stat().st_mode & 0o777 == 0o600
        run('success', ('-o', str(root / 'missing' / 'out.png')), 2, b'')
        run('success', ('-o', str(root)), 2, b'')
        assert not list(root.glob('.hyprcapture-shot-*'))
        for case in ['wait', 'slow', 'trigger-hang']:
            log = run(case, ('--timeout', '.15'), 2, b'')
            assert 'shot_cancel(' in log.read_text()
            if case != 'trigger-hang':
                end = time.monotonic() + 2
                while not Path(str(log) + '.cancelled').exists() and time.monotonic() < end:
                    time.sleep(.01)
                assert Path(str(log) + '.cancelled').read_bytes() == b'cancel'
        for sig, code in [(signal.SIGINT, 1), (signal.SIGTERM, 2)]:
            current, log = environment('signal')
            process = subprocess.Popen([binary], env=current, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            end = time.monotonic() + 3
            while not Path(str(log) + '.connected').exists() and time.monotonic() < end:
                time.sleep(.005)
            process.send_signal(sig)
            out, err = process.communicate(timeout=3)
            assert process.returncode == code and out == b'', (process.returncode, err)
            assert 'shot_cancel(' in log.read_text()
        # Missing executable, without ever consulting the live compositor.
        current, _ = environment('success'); current['PATH'] = str(root / 'missing')
        result = subprocess.run([binary], env=current, capture_output=True, timeout=3)
        assert result.returncode == 2 and result.stdout == b''
        print('shot CLI: 38 scenarios passed (parsing, TTY, framing, cancellation, signals, atomic output, cleanup)')


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == 'eval':
        fake()
    elif len(sys.argv) > 1 and sys.argv[1] == 'peer':
        peer(sys.argv[2])
    else:
        main()
