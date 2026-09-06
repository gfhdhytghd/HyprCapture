"""Optional real PipeWire/WebRTC AEC test with simulated acoustic echo."""
import os, tempfile, pathlib, subprocess as sp, time, json, array, math, sys
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
env=dict(os.environ,LC_ALL='C.UTF-8',PULSE_SERVER='unix:'+str(root/'native'))
helper=str(pathlib.Path(sys.argv[1]).resolve())
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
 for i in range(12*48000):
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
 for name,enabled in [('raw','0'),('aec','1')]:
  folder=root/name;folder.mkdir(mode=0o700)
  p=sp.Popen([helper,'--sound-capture','microphone',out,source,str(folder),'manual','0','0',enabled],env=env,stdin=sp.PIPE,stdout=open(root/(name+'.events'),'w'),stderr=open(root/(name+'.stderr'),'w'));children.append(p);captures.append((name,p))
 time.sleep(1)
 for name,device in [('far',out),('near',mic)]:
  children.append(sp.Popen(['paplay','--device='+device,str(root/(name+'.wav'))],env=env))
 time.sleep(12.5)
 for name,p in captures:p.stdin.close();p.wait(timeout=4);print(name,(root/(name+'.events')).read_text(),flush=True)
 def values(name):
  a=array.array('f');a.frombytes((root/name/'microphone.f32').read_bytes());return a[::2]
 def energy(x,a,b):
  y=x[int(a*48000):int(b*48000)];return math.sqrt(sum(v*v for v in y)/max(1,len(y)))
 raw=values('raw');aec=values('aec')
 print('RMS',[(a,b,energy(raw,a,b),energy(aec,a,b)) for a,b in [(4,6),(7.2,9),(11,12)]],flush=True)
 assert '"aec":"active"' in (root/'aec.events').read_text(),'AEC must load'
 assert energy(aec,4,6)<energy(raw,4,6)*.25,'far-end echo suppression must exceed 12dB'
 assert energy(aec,7.2,9)>.01,'near-end voice must survive double talk'
 assert energy(aec,11,12)<energy(raw,11,12)*.25,'echo stays suppressed after speech'
 assert before==(cmd('pactl','get-default-sink'),cmd('pactl','get-default-source')),'defaults must not change'
 assert not any('hyprcapture-aec-' in d['name'] for d in json.loads(cmd('pactl','-f','json','list','sources'))),'AEC nodes must clean up'
 # Losing the reference must be visible and preserve microphone recording.
 folder=root/'fallback';folder.mkdir(mode=0o700)
 p=sp.Popen([helper,'--sound-capture','microphone',out,source,str(folder),'manual','0','0','1'],env=env,stdin=sp.PIPE,stdout=open(root/'fallback.events','w'),stderr=open(root/'fallback.stderr','w'));children.append(p)
 for _ in range(50):
  if '"aec":"active"' in (root/'fallback.events').read_text():break
  time.sleep(.05)
 assert '"aec":"active"' in (root/'fallback.events').read_text()
 cmd('pactl','unload-module',output_module)
 time.sleep(.5)
 player=sp.Popen(['paplay','--device='+mic,str(root/'near.wav')],env=env);children.append(player)
 time.sleep(2)
 p.stdin.close();p.wait(timeout=4);player.terminate();player.wait(timeout=4)
 assert '"aec":"unavailable"' in (root/'fallback.events').read_text(),'reference loss must report fallback'
 assert energy(values('fallback'),1,2)>.01,'original microphone survives AEC failure'
 assert not any('hyprcapture-aec-' in d['name'] for d in json.loads(cmd('pactl','-f','json','list','sources'))),'failed AEC nodes clean up'
 print('PASS PipeWire WebRTC monitor mode: echo suppression, double talk, reference loss, defaults and cleanup',flush=True)
finally:
 for child in children:
  if child.poll() is None:
   child.terminate()
   try: child.wait(timeout=4)
   except sp.TimeoutExpired: child.kill(); child.wait()
 if server.poll() is None:server.terminate();server.wait(timeout=4)
