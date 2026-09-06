"""Opt-in integration test. Only virtual sources/sinks; no physical microphone or playback.
Requires a validated cache and explicit HYPRCAPTURE_AEC_MODEL_DIR/TFLITE_LIBRARY.
AEC_TEST_* switches are consumed by this test only, never production code.
"""
import os, tempfile, pathlib, subprocess as sp, time, json, array, math
root=pathlib.Path(tempfile.mkdtemp(prefix='hyprcapture-sound-live-'))
print(root,flush=True)
(root/'pulse.conf').write_text('''context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
 { name = libpipewire-module-protocol-native }
 { name = libpipewire-module-client-node }
 { name = libpipewire-module-adapter }
 { name = libpipewire-module-metadata }
 { name = libpipewire-module-protocol-pulse args = { server.address = [ "unix:'''+str(root)+'''/native" ] } }
]
pulse.cmd = []
''')
log=open(root/'pulse.log','w')
server=sp.Popen(['pipewire-pulse','-c',str(root/'pulse.conf')],stdout=log,stderr=log)
env=dict(os.environ,HYPRCAPTURE_TIMING='1',PULSE_SERVER='unix:'+str(root/'native'))
helper=os.environ.get('HYPRCAPTURE_TEST_HELPER',str(pathlib.Path('build-codex/hyprcapture-ui').resolve()))
duration=int(os.environ.get('HYPRCAPTURE_AEC_TEST_SECONDS','12'))
fault=os.environ.get('HYPRCAPTURE_AEC_TEST_FAULT','')
transport_test=bool(os.environ.get('HYPRCAPTURE_AEC_TEST_TRANSPORT'))
children=[]
def cmd(*args): return sp.check_output(args,env=env,text=True).strip()
try:
 for _ in range(100):
  if (root/'native').exists(): break
  if server.poll() is not None: raise RuntimeError((root/'pulse.log').read_text())
  time.sleep(.05)
 suffix=str(os.getpid()); out='hc_sound_'+suffix; mic='hc_mic_'+suffix; source='hc_input_'+suffix
 output_module=cmd('pactl','load-module','module-null-sink','sink_name='+out,'sink_properties=device.description=HyprCapture_Test_Output')
 mic_module=cmd('pactl','load-module','module-null-sink','sink_name='+mic,'sink_properties=device.description=HyprCapture_Test_Mic_Bus')
 source_module=cmd('pactl','load-module','module-remap-source','master='+mic+'.monitor','source_name='+source,'source_properties=device.description=HyprCapture_Test_Microphone')
 time.sleep(.5)
 print('Private test sources ready',flush=True)
 import random,wave
 before=(cmd('pactl','get-default-sink'),cmd('pactl','get-default-source'))
 random.seed(314159);ref=[];smooth=0
 for i in range(duration*48000):
  smooth=.6*smooth+.4*random.uniform(-.3,.3);ref.append(smooth)
 for name in ('far','near'):
  data=array.array('h')
  for i,v in enumerate(ref):
   t=i/48000
   if name=='near':
    v=(.5*ref[i-3120] if i>=3120 else 0)+(.15*ref[i-5280] if i>=5280 else 0)
    if 6<t<9:v+=.08*math.sin(2*math.pi*900*t)*(0.6+.4*math.sin(2*math.pi*4*t))
   sample=int(max(-1,min(1,v))*32767);data.extend((sample,sample))
  with wave.open(str(root/(name+'.wav')),'wb') as f:f.setparams((2,2,48000,0,'NONE',''));f.writeframes(data.tobytes())
 captures=[]
 transport=None
 if transport_test:
  transport=sp.Popen([str(pathlib.Path(helper).with_name('hyprcapture-aec')),'--serve',source,out,'256','cpu','hyprcapture-aec-transport-'+suffix],env=dict(env,HYPRCAPTURE_TFLITE_LIBRARY=str(pathlib.Path(helper).with_name('libhyprcapture-fake-tflite.so'))),stdin=sp.PIPE,stdout=sp.PIPE,stderr=open(root/'transport.log','w'))
  children.append(transport)
  assert '"aec":"active"' in transport.stdout.readline().decode()
  time.sleep(.5)
 for name,enabled in [('raw','0'),('aec','1')]:
  folder=root/name;folder.mkdir(mode=0o700)
  capture_source='hyprcapture-aec-transport-'+suffix if transport_test and name=='aec' else source
  p=sp.Popen([helper,'--sound-capture','mix',out,capture_source,str(folder),'manual','0','0','0' if transport_test else enabled,'cpu'],env=env,stdin=sp.PIPE,stdout=open(root/(name+'.events'),'w'),stderr=open(root/(name+'.stderr'),'w'));children.append(p);captures.append((name,p))
 time.sleep(2)
 for name,device in [('far',out),('near',mic)]:
  children.append(sp.Popen(['paplay','--device='+device,str(root/(name+'.wav'))],env=env))
 if os.environ.get('HYPRCAPTURE_AEC_TEST_ENCODER'):
  children.append(sp.Popen(['ffmpeg','-v','error','-re','-f','lavfi','-i','testsrc2=size=3840x2160:rate=30','-t',str(duration),'-c:v','libx264','-preset','veryfast','-threads','4','-f','null','-'],stdout=sp.DEVNULL,stderr=open(root/'encoder.log','w')))
 if fault:
  time.sleep(2)
  worker=cmd('pgrep','-P',str(captures[1][1].pid),'-x','hyprcapture-aec')
  if fault == 'parent': os.kill(captures[1][1].pid,9)
  elif fault == 'device': cmd('pactl','unload-module',source_module)
  elif fault == 'output': cmd('pactl','unload-module',output_module)
  else: os.kill(int(worker),9)
  time.sleep(duration-1.5)
 else: time.sleep(duration+.5)
 for name,p in captures:p.stdin.close();p.wait(timeout=4);print(name,(root/(name+'.events')).read_text(),(root/(name+'.stderr')).read_text()[-2000:],flush=True)
 def values(name):
  a=array.array('f');a.frombytes((root/name/'microphone.f32').read_bytes());return a[::2]
 def energy(x,a,b):
  y=x[int(a*48000):int(b*48000)];return math.sqrt(sum(v*v for v in y)/max(1,len(y)))
 raw=values('raw');aec=values('aec')
 print('RMS',[(a,b,energy(raw,a,b),energy(aec,a,b)) for a,b in [(4,6),(7.2,9),(11,12)]],flush=True)
 if transport:
  transport.stdin.close();transport.wait(timeout=3)
 else: assert '"aec":"active"' in (root/'aec.events').read_text(),'AEC must load'
 if fault and fault != 'parent': assert '"aec":"unavailable"' in (root/'aec.events').read_text(),'worker exit must fall back'
 elif not fault: assert '"aec":"unavailable"' not in (root/'aec.events').read_text(),'AEC must remain active throughout recording'
 assert all(math.isfinite(v) for v in aec),'finite audio'
 if fault not in ('parent','device'): assert energy(aec,7.2,9)>0,'nonempty processed audio'


 if fault not in ('device','output'): assert before==(cmd('pactl','get-default-sink'),cmd('pactl','get-default-source')),'defaults must not change'
 assert not any('hyprcapture-aec-' in d['name'] for d in json.loads(cmd('pactl','-f','json','list','sources'))),'AEC nodes must clean up'
 if transport_test:
  import numpy as np
  a=np.asarray(raw);b=np.asarray(aec)
  origins=[json.loads((root/p/'session.json').read_text())['originUs'] for p in ['raw','aec']]
  shift=round((origins[1]-origins[0])*48000/1e6)
  if shift>=0:a=a[shift:]
  else:b=b[-shift:]
  for lo,hi in [(3,6),(9,12)]:
   x=a[lo*48000:hi*48000];y=b[lo*48000:hi*48000];n=1<<(len(x)*2-1).bit_length()
   correlation=np.fft.irfft(np.fft.rfft(y,n)*np.conj(np.fft.rfft(x,n)),n)
   lag=int(np.argmax(correlation[:12000]));residual=lag/48-80
   assert abs(residual)<20,('transport delay outside 20 ms timestamp budget',residual)
   print('transport residual after 80 ms compensation:',residual,'ms',flush=True)
 print('PASS native DTLN PipeWire graph: audio, defaults and cleanup',flush=True)
finally:
 for child in children:
  if child.poll() is None:
   child.terminate()
   try: child.wait(timeout=3)
   except sp.TimeoutExpired: child.kill(); child.wait()
 if server.poll() is None:server.terminate();server.wait(timeout=4)
