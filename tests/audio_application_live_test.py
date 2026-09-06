"""Optional real PipeWire integration test; leaves host defaults unchanged."""
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
env=dict(os.environ,PULSE_SERVER='unix:'+str(root/'native'))
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
 time.sleep(.5)
 import select
 for freq in (440,660,880,1000):
  sp.run(['ffmpeg','-v','error','-f','lavfi','-i',f'sine=frequency={freq}:duration='+('8' if freq==1000 else '2'),'-ar','48000','-ac','2',str(root/(str(freq)+'.wav'))],check=True)
 controller_code="""import os, subprocess as sp, time, sys
root,device=sys.argv[1:]
time.sleep(1.2)
a=sp.Popen(['paplay','--device='+device,root+'/440.wav'])
b=sp.Popen(['paplay','--device='+device,root+'/660.wav'])
a.wait();b.wait();time.sleep(.6)
sp.run(['paplay','--device='+device,root+'/880.wav'])
time.sleep(2)
"""
 controller=sp.Popen(['python3','-c',controller_code,str(root),out],env=env);children.append(controller)
 folder=root/'application';folder.mkdir(mode=0o700)
 capture=sp.Popen([helper,'--sound-capture','system','pid:'+str(controller.pid),'default',str(folder),'manual','0','0'],env=env,stdin=sp.PIPE,stdout=open(root/'capture.events','w'));children.append(capture)
 meter=sp.Popen([helper,'--sound-meter','system','pid:'+str(controller.pid),'default'],env=env,stdin=sp.PIPE,stdout=sp.PIPE,text=True,bufsize=1);children.append(meter)
 noise=sp.Popen(['paplay','--device='+out,str(root/'1000.wav')],env=env);children.append(noise)
 levels=[];start=time.monotonic()
 while time.monotonic()-start<7.5:
  if select.select([meter.stdout],[],[],.1)[0]:
   obj=json.loads(meter.stdout.readline())
   if 'levels' in obj: levels.append((time.monotonic()-start,obj['levels'].get('System',{})))
 capture.stdin.close();capture.wait(timeout=4);meter.stdin.close();meter.wait(timeout=4)
 print('events',(root/'capture.events').read_text(),flush=True)
 data=array.array('f');data.frombytes((folder/'system.f32').read_bytes());mono=data[::2]
 def amplitude(freq,a,b):
  results=[]
  for j in range(max(1,int((b-a)/.1))):
   values=mono[int((a+j*.1)*48000):int((a+(j+1)*.1)*48000)]
   re=sum(v*math.cos(2*math.pi*freq*i/48000) for i,v in enumerate(values));im=sum(v*math.sin(2*math.pi*freq*i/48000) for i,v in enumerate(values))
   results.append(2*math.hypot(re,im)/max(1,len(values)))
  return sum(results)/len(results)
 print('duration',len(mono)/48000,'components',[(f,amplitude(f,1.8,2.8)) for f in (440,660,1000)],'late',amplitude(880,4.5,5.2),flush=True)
 assert amplitude(440,1.8,2.8)>.06 and amplitude(660,1.8,2.8)>.06,'must capture both application streams'
 assert amplitude(1000,1.8,2.8)<.002,'must exclude other applications on same sink'
 assert amplitude(880,4.5,5.2)>.06,'must discover new playback stream after silence'
 assert max(x.get('peak',0) for t,x in levels if t<.8)==0,'waiting app must never fall back to desktop'
 assert max(x.get('peak',0) for t,x in levels if 1.8<t<2.8)>.12,'live meter must sum multiple streams'
 video=root/'application.mp4'
 sp.run(['ffmpeg','-v','error','-f','lavfi','-i','color=s=64x64:r=10:d=7.5','-c:v','libx264',str(video)],check=True)
 origin=str(int(json.loads((folder/'session.json').read_text())['originUs']))
 sp.run([helper,'--sound-finalize',str(folder),str(video),origin,'mp4'],env=env,check=True)
 sp.run(['ffmpeg','-v','error','-i',str(video),'-f','null','-'],check=True)
 assert not folder.exists(),'successful application mux removes recovery data'
 print('PASS application isolation, child-process matching, multiple streams, late stream and live sum',flush=True)
finally:
 for child in children:
  if child.poll() is None:
   child.terminate()
   try: child.wait(timeout=4)
   except sp.TimeoutExpired: child.kill(); child.wait()
 if server.poll() is None:server.terminate();server.wait(timeout=4)
