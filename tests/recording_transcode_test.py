"""Run the real APNG helper against successful and failed conversions."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from session_lock_fixture import SessionLockFixture

helper = str(Path(sys.argv[1]).resolve())
ffmpeg = shutil.which('ffmpeg')
if not ffmpeg:
    sys.exit(77)

with SessionLockFixture() as lock, tempfile.TemporaryDirectory(prefix='hc-transcode-') as folder:
    root = Path(folder)
    source = root / 'source.mkv'
    output = root / 'output.apng'
    env = dict(lock.env, QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='',
               QT_WAYLAND_SHELL_INTEGRATION='', QT_STYLE_OVERRIDE='Fusion', QT_LOGGING_TO_CONSOLE='1')
    env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + str(root / 'no-bus')
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'QT_IM_MODULE'):
        env.pop(key, None)
    args = [helper, '--recording-transcode-input', str(source),
            '--recording-transcode-output', str(output), '--record-save-dir', str(root),
            '--thumbnail', '0', '--clipboard', '0']

    def run(prefix=(), success=False):
        result = subprocess.run([*prefix, *args], env=env, capture_output=True, timeout=15)
        error = result.stderr.decode(errors='replace')
        assert (result.returncode == 0) == success, (result.returncode, error)
        if not success:
            assert source.exists(), 'failure must retain the original recording'
            assert str(source) in error and 'retained' in error, error
            assert not output.exists(), 'remove partial conversion output'
        return result

    subprocess.run([ffmpeg, '-v', 'error', '-f', 'lavfi', '-i',
                    'testsrc2=s=64x64:r=10:d=0.3', '-c:v', 'ffv1', str(source)], check=True)
    original = source.read_bytes()
    source.chmod(0o600)
    blocked = root / 'blocked'
    blocked.mkdir(mode=0o500)
    args[args.index('--recording-transcode-output') + 1] = str(blocked / 'output.apng')
    try:
        result = subprocess.run(args, env=env, capture_output=True, timeout=15)
        assert result.returncode != 0, result.stderr
        assert source.read_bytes() == original
        assert str(source).encode() in result.stderr, (result.returncode, result.stdout, result.stderr, source.stat(), root.resolve())
        assert not (blocked / 'output.apng').exists()
    finally:
        blocked.chmod(0o700)
    args[args.index('--recording-transcode-output') + 1] = str(output)

    # Encoding/decoding failure with a trusted, regular input file.
    source.write_bytes(b'not a recording')
    run()
    assert source.read_bytes() == b'not a recording'
    source.write_bytes(original)

    # Replace ffmpeg only inside a private mount namespace. This exercises
    # QProcess::waitForStarted failure and a zero exit without output.
    bwrap = shutil.which('bwrap')
    if bwrap and subprocess.run([bwrap, '--ro-bind', '/', '/', 'true'],
                                capture_output=True).returncode == 0:
        fake = root / 'ffmpeg'
        prefix = [bwrap, '--ro-bind', '/', '/', '--bind', str(root), str(root),
                  '--ro-bind', str(fake), str(Path(ffmpeg).resolve())]
        for program in ('#!/nonexistent-hc-interpreter\n', '#!/bin/sh\nexit 0\n'):
            fake.write_text(program)
            fake.chmod(0o700)
            run(prefix)
            assert source.read_bytes() == original
    else:
        print('SKIP subcases: mount namespace unavailable for process startup/empty-output checks')

    run(success=True)
    assert not source.exists(), 'successful conversion consumes the MKV'
    assert output.is_file() and output.stat().st_size > 0
    subprocess.run([ffmpeg, '-v', 'error', '-i', str(output), '-f', 'null', '-'], check=True)

# The compositor branch cannot be invoked without loading a live plugin.
# Guard its recovery contract alongside the real helper integration coverage.
recording = (Path(__file__).resolve().parents[1] / 'src/plugin/recording.cpp').read_text()
branch = recording.split('void startApngTranscodeFromFinishedRecording(', 1)[1].split('\nvoid ', 1)[0]
assert 'remove(recording.encoderOutputPath' not in branch
assert 'recording retained at: ' in branch
print('PASS: failed APNG conversion retains MKV and reports recovery; success consumes it')
