"""Real NUT -> FFmpeg -> sound-finalize regression for sparse capture timestamps."""
import array
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

fixture, helper = sys.argv[1:3]
if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
    sys.exit(77)

def run(*args):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout

input_args = run(fixture, '--input-args').decode().splitlines()
output_args = run(fixture, '--output-args').decode().splitlines()
expected = [0, .033333, .2, .75, 1.5, 1.983334]

def probe(path):
    return json.loads(run('ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_packets', '-show_format', '-of', 'json', str(path)))

def rms(xs):
    return math.sqrt(sum(x*x for x in xs) / max(1, len(xs)))

with tempfile.TemporaryDirectory(prefix='hyprcapture-recording-pts-') as folder:
    root = Path(folder)
    for fmt, codec, pixfmt in [('mp4', 'libx264', 'yuv420p'), ('mov', 'libx264', 'yuv420p'),
                                ('mkv', 'ffv1', 'rgba'), ('webm', 'libvpx-vp9', 'yuva420p')]:
        path = root / ('video.' + fmt)
        source = subprocess.Popen([fixture], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        encoded = subprocess.run(['ffmpeg', '-v', 'error', *input_args, '-c:v', codec, '-pix_fmt', pixfmt,
                                  *output_args, str(path)], stdin=source.stdout, capture_output=True)
        source.stdout.close()
        source_error = source.stderr.read()
        assert source.wait() == 0, source_error.decode()
        assert encoded.returncode == 0, encoded.stderr.decode()
        doc = probe(path)
        packets = doc['packets']
        assert len(packets) == 6, 'five captures plus one terminal sample, not 120 CFR copies'
        for packet, pts in zip(packets, expected):
            assert abs(float(packet['pts_time']) - pts) <= .001, (fmt, packet, pts)
        assert abs(float(doc['format']['duration']) - 2) < .002, (fmt, doc['format']['duration'])
        # Verify frame data survives the long timestamp gaps, in the same order.
        rgb = run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v:0', '-fps_mode', 'passthrough',
                  '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-')
        size = 64 * 64 * 3
        assert len(rgb) == size * 6
        for i, red in enumerate([20, 60, 100, 140, 180, 180]):
            assert abs(rgb[i * size] - red) < 12, (fmt, i, rgb[i*size])
        if fmt != 'mp4':
            continue
        # Audio origin precedes video by .5s. A pulse at audio t=1.25 must
        # coincide with video t=.75 even though only three earlier frames exist.
        before = run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v', '-c', 'copy', '-f', 'hash', '-')
        recovery = root / 'sound'
        recovery.mkdir(mode=0o700)
        (recovery / 'session.json').write_text(json.dumps({'originUs': 1000000, 'mode': 'system'}))
        values = array.array('f')
        for i in range(3 * 48000):
            t = i / 48000
            sample = .4 * math.sin(2 * math.pi * 440 * t) if 1.25 <= t < 1.5 else 0
            values.extend((sample, sample))
        (recovery / 'system.f32').write_bytes(values.tobytes())
        run(helper, '--sound-finalize', str(recovery), str(path), '1500000', fmt)
        after = run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:v', '-c', 'copy', '-f', 'hash', '-')
        assert before == after, 'audio finalization must retain timestamped video packets'
        final = probe(path)
        assert len(final['packets']) == 6 and abs(float(final['format']['duration']) - 2) < .05
        for packet, pts in zip(final['packets'], expected):
            assert abs(float(packet['pts_time']) - pts) <= .001, 'sound mux shifted video timestamps'
        sound = array.array('f')
        sound.frombytes(run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0', '-ac', '1', '-ar', '48000', '-f', 'f32le', '-'))
        assert abs(len(sound) / 48000 - 2) < .05
        assert rms(sound[24000:30000]) < .01
        assert rms(sound[38000:44000]) > .1
        assert rms(sound[55000:65000]) < .01
