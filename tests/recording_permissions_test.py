"""Observe real UI transcode and audio mux files before their final chmod."""
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time

helper = str(Path(sys.argv[1]).resolve())
if not shutil.which('ffmpeg'):
    sys.exit(77)

def observe(args, outputs, env):
    seen = set()
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen(args, stdout=log, stderr=log, env=env, umask=0)
        try:
            deadline = time.monotonic() + 30
            while process.poll() is None:
                for path in outputs:
                    try:
                        info = path.stat()
                    except FileNotFoundError:
                        continue
                    assert stat.S_IMODE(info.st_mode) == 0o600, (path, oct(info.st_mode))
                    seen.add(path)
                assert time.monotonic() < deadline, 'helper timed out'
                time.sleep(.001)
            log.seek(0)
            assert process.returncode == 0, log.read().decode(errors='replace')
            assert seen == set(outputs), 'must observe growing output before helper exits'
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()

with tempfile.TemporaryDirectory(prefix='hyprcapture-recording-permissions-') as folder:
    root = Path(folder)
    root.chmod(0o755)
    source = root / 'source.mkv'
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                    'testsrc2=s=640x360:r=24:d=4', '-c:v', 'ffv1', str(source)], check=True)
    source.chmod(0o600)
    output = root / 'output.apng'
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_WAYLAND_SHELL_INTEGRATION='',
               QT_QPA_PLATFORMTHEME='', QT_STYLE_OVERRIDE='Fusion')
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS'):
        env.pop(key, None)
    observe([helper, '--recording-transcode-input', str(source), '--recording-transcode-output', str(output),
             '--record-save-dir', str(root), '--thumbnail', '0', '--clipboard', '0'], [output], env)
    assert stat.S_IMODE(output.stat().st_mode) == 0o600
    assert not source.exists(), 'transcode consumes intermediate recording'
    video = root / 'video.mp4'
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=s=64x64:r=10:d=20',
                    '-c:v', 'libx264', str(video)], check=True)
    session = root / 'audio'
    session.mkdir(mode=0o700)
    (session / 'session.json').write_text(json.dumps(dict(originUs=1000000, mode='system')))
    (session / 'system.f32').write_bytes(b'\0' * (48000 * 20 * 8))
    observe([helper, '--sound-finalize', str(session), str(video), '1000000', 'mp4'],
            [session / 'combined.mp4'], env)
    assert stat.S_IMODE(video.stat().st_mode) == 0o600
print('PASS: growing APNG and intermediate audio mux are 0600 with inherited umask zero')
