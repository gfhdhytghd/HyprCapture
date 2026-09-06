"""Developer-only reference generator; not an installed runtime dependency.

DTLN inference follows breizhn/DTLN-aec run_aec.py at
9d24e128b4f409db18227b8babb343016625921f, MIT, Nils L. Westhausen.
Use tflite-runtime==2.14.0 and numpy==1.26.4. No personal audio is used.
"""
import math
import pathlib
import struct
import sys
import zlib
import numpy as np
from tflite_runtime.interpreter import Interpreter

root = pathlib.Path(__file__).resolve().parents[1]
models = pathlib.Path(sys.argv[1])
count = 160000
mic, ref = np.zeros(count, np.float32), np.zeros(count, np.float32)
seed, filtered = 314159, 0.0
for i in range(count):
    t = i / 16000
    seed = (1664525 * seed + 1013904223) & 0xffffffff
    filtered = .7 * filtered + .3 * ((seed >> 8) / 16777216 - .5)
    ref[i] = .3 * filtered + .08 * math.sin(2 * math.pi * 237 * t) if t < 3 or t >= 6 else 0
    echo = (.5 * float(ref[i-640]) if i >= 640 else 0) + (.15 * float(ref[i-1120]) if i >= 1120 else 0)
    voice = math.sin(2*math.pi*143*t) + .4*math.sin(2*math.pi*286*t) + .2*math.sin(2*math.pi*429*t)
    mic[i] = echo + (.065*voice*(.5+.5*math.sin(2*math.pi*3*t)) if t >= 3 else 0)

for size in (256, 512):
    interpreters = [Interpreter(model_path=str(models/f'dtln_aec_{size}_{p}.tflite'), num_threads=1) for p in (1, 2)]
    for i in interpreters: i.allocate_tensors()
    ins = [i.get_input_details() for i in interpreters]
    outs = [i.get_output_details() for i in interpreters]
    states = [np.zeros(d[1]['shape'], np.float32) for d in ins]
    buf, far, overlap = (np.zeros(512, np.float32) for _ in range(3))
    result, snapshots = [], []
    for frame in range(-3, count//128):
        buf[:-128], far[:-128] = buf[128:], far[128:]
        buf[-128:] = mic[frame*128:(frame+1)*128] if frame >= 0 else 0
        far[-128:] = ref[frame*128:(frame+1)*128] if frame >= 0 else 0
        spec = np.fft.rfft(buf).astype('complex64')
        values = [np.abs(spec).reshape(1, 1, -1), states[0], np.abs(np.fft.rfft(far).astype('complex64')).reshape(1, 1, -1)]
        for d, v in zip(ins[0], values): interpreters[0].set_tensor(d['index'], v)
        interpreters[0].invoke()
        mask = interpreters[0].get_tensor(outs[0][0]['index'])
        states[0] = interpreters[0].get_tensor(outs[0][1]['index'])
        estimated = np.fft.irfft(spec*mask).astype('float32').reshape(1, 1, 512)
        values = [estimated, states[1], far.reshape(1, 1, -1)]
        for d, v in zip(ins[1], values): interpreters[1].set_tensor(d['index'], v)
        interpreters[1].invoke()
        block = interpreters[1].get_tensor(outs[1][0]['index']).reshape(-1)
        states[1] = interpreters[1].get_tensor(outs[1][1]['index'])
        overlap[:-128] = overlap[128:]; overlap[-128:] = 0; overlap += block
        if frame >= 0: result.append(overlap[:128].copy())
        if frame >= 0: snapshots.extend(s.copy().reshape(-1) for s in states)
    raw = np.concatenate(result+snapshots).astype('<f4').tobytes()
    path = root/'resources/aec'/f'golden-{size}.qz'
    path.write_bytes(struct.pack('>I', len(raw))+zlib.compress(raw, 9))
    print(path, len(raw), flush=True)
