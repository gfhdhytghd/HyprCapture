"""Exercise the real helper/FFmpeg boundary with deterministic private PCM tracks."""
import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

helper = str(Path(sys.argv[1]).resolve())
if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
    sys.exit(77)

def run(*args, **kwargs):
    return subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs).stdout

def samples(path):
    values = array.array('f')
    values.frombytes(run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0', '-f', 'f32le', '-ac', '1', '-ar', '48000', '-'))
    return values

def rms(values):
    return math.sqrt(sum(v*v for v in values) / max(1, len(values)))

with tempfile.TemporaryDirectory(prefix='hyprcapture-audio-test-') as root:
    root = Path(root)
    for fmt in ('mp4', 'mov', 'webm', 'mkv'):
        folder = root / fmt
        folder.mkdir(mode=0o700)
        session = folder / 'recovery'
        session.mkdir(mode=0o700)
        video = folder / ('video.' + fmt)
        codec = 'libvpx-vp9' if fmt == 'webm' else 'libx264'
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=black:s=64x64:r=10:d=2', '-c:v', codec, str(video))
        before = run('ffmpeg', '-v', 'error', '-i', str(video), '-map', '0:v', '-c', 'copy', '-f', 'hash', '-')
        (session / 'session.json').write_text(json.dumps({'originUs': 1000000, 'mode': 'mix'}))
        # A pulse at session t=.75 becomes video t=.25 after trimming .5s.
        # Microphone stops at 1s; the rest of the system track must survive.
        for name, duration, frequency in [('system.f32', 2.5, 440), ('microphone.f32', 1, 880)]:
            values = array.array('f')
            for i in range(int(duration * 48000)):
                t = i / 48000
                value = .3 * math.sin(2 * math.pi * frequency * t) if .75 <= t < 1.5 else 0
                values.extend((value, value))
            (session / name).write_bytes(values.tobytes())
        run(helper, '--sound-finalize', str(session), str(video), '1500000', fmt)
        assert not session.exists(), 'successful mux cleans recovery files'
        doc = json.loads(run('ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(video)))
        assert len([s for s in doc['streams'] if s['codec_type'] == 'audio']) == 1
        after = run('ffmpeg', '-v', 'error', '-i', str(video), '-map', '0:v', '-c', 'copy', '-f', 'hash', '-')
        assert before == after, 'video bitstream must not be reencoded'
        audio = samples(video)
        assert abs(len(audio) / 48000 - 2) < .05
        assert rms(audio[:8000]) < .01, 'leading silence preserved'
        assert rms(audio[15000:20000]) > .05, 'mixed pulse present at correct offset'
        assert rms(audio[30000:40000]) > .04, 'surviving source continues after mic EOF'
        assert rms(audio[75000:85000]) < .01, 'tail padded with silence'
        run('ffmpeg', '-v', 'error', '-i', str(video), '-f', 'null', '-')
    # An error must leave the original artifact and recovery audio intact.
    session = root / 'failure'; session.mkdir(mode=0o700)
    (session / 'session.json').write_text(json.dumps({'originUs': 1000000, 'mode': 'system'}))
    (session / 'system.f32').write_bytes(b'\0' * 800)
    invalid = root / 'broken.mp4'; invalid.write_bytes(b'original incomplete video')
    before = hashlib.sha256(invalid.read_bytes()).digest()
    result = subprocess.run([helper, '--sound-finalize', str(session), str(invalid), '1000000', 'mp4'], capture_output=True)
    assert result.returncode != 0
    assert hashlib.sha256(invalid.read_bytes()).digest() == before
    assert (session / 'system.f32').exists()
print('Audio mux: four containers, synchronization, one mixed track, source loss and recovery passed')

# Compare frequency components, rather than total RMS, to distinguish speech boost
# from background ducking through the real encoder and finalizer.
def component(values, frequency, start, end):
    lo, hi = int(start * 48000), int(end * 48000)
    segment = values[lo:hi]
    re = sum(v * math.cos(2 * math.pi * frequency * (lo+i) / 48000) for i, v in enumerate(segment))
    im = sum(v * math.sin(2 * math.pi * frequency * (lo+i) / 48000) for i, v in enumerate(segment))
    return 2 * math.hypot(re, im) / len(segment)

with tempfile.TemporaryDirectory(prefix='hyprcapture-mix-test-') as root:
    root = Path(root)
    source = root / 'source.mp4'
    run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'color=c=black:s=64x64:r=10:d=8', '-c:v', 'libx264', str(source))
    def render(preset, system_gain=0, mic_gain=0, mic_present=True, noise=False):
        folder = Path(tempfile.mkdtemp(dir=root)); session = folder / 'audio'; session.mkdir(mode=0o700)
        video = folder / 'video.mp4'; shutil.copyfile(source, video)
        (session / 'session.json').write_text(json.dumps({'originUs': 1000000, 'mode': 'mix', 'mixPreset': preset,
                                                        'systemGain': system_gain, 'micGain': mic_gain}))
        for name, frequency, amplitude in [('system.f32', 440, .6), ('microphone.f32', 880, .025)]:
            if name.startswith('microphone') and not mic_present: continue
            data = array.array('f')
            for i in range(8 * 48000):
                t = i / 48000
                value = amplitude * math.sin(2 * math.pi * frequency * t)
                if name.startswith('microphone') and not 2 <= t < 5: value = .001 * math.sin(2 * math.pi * frequency * t) if noise else 0
                data.extend((value, value))
            (session / name).write_bytes(data.tobytes())
        run(helper, '--sound-finalize', str(session), str(video), '1000000', 'mp4')
        return samples(video)
    manual = render('manual')
    boosted = render('manual', system_gain=-6, mic_gain=12)
    muted = render('manual', system_gain=-61, mic_gain=-61)
    auto = render('auto-balance')
    voice = render('voice-priority')
    mute_mic = render('voice-priority', mic_gain=-61)
    missing_mic = render('voice-priority', mic_present=False)
    noisy = render('voice-priority', noise=True)
    assert component(noisy, 880, 7, 7.8) < .002, 'quiet background must not be amplified into speech'
    m = component(manual, 880, 3, 4)
    assert .022 * math.sqrt(2) < m < .028 * math.sqrt(2), ('manual unity', m)
    assert 3.8 < component(boosted, 880, 3, 4) / m < 4.2, 'manual +12dB gain reaches recording'
    assert .47 < component(boosted, 440, 3, 4) / component(manual, 440, 3, 4) < .53, 'manual -6dB gain reaches recording'
    assert rms(muted) < .00001, 'mute must remain silent through encoding'
    assert component(auto, 880, 3, 4) > m * 3, 'automatic brings quiet voice up'
    assert component(voice, 440, 3, 4) < component(auto, 440, 3, 4) * .65, 'voice ducks system sound'
    assert component(voice, 440, 7, 7.8) > component(voice, 440, 3, 4) * 1.5, 'background recovers after voice stops'
    assert abs(component(mute_mic, 440, 3, 4) / component(missing_mic, 440, 3, 4) - 1) < .05, 'muted microphone must not duck'
    print('Mixing: manual dB, mute, quiet speech boost, ducking, recovery and missing microphone passed')
