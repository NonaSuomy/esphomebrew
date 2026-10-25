#!/usr/bin/env python3
"""Compiled RV32 video/audio test with the installed SVP Relay AviSender (real ffmpeg)."""
import argparse,asyncio,importlib,json,os,sys,tempfile,types
from pathlib import Path
from PIL import Image
async def run(a,tmp):
 package=types.ModuleType('svp_media_test');package.__path__=[str(a.relay/'custom_components/svp_relay')];sys.modules[package.__name__]=package
 module=importlib.import_module('svp_media_test.svp_core.avi')
 clip=tmp/'test.mkv'
 make=await asyncio.create_subprocess_exec('ffmpeg','-hide_banner','-loglevel','error','-f','lavfi','-i','color=c=red:s=320x192:r=2:d=8','-f','lavfi','-i','sine=frequency=440:sample_rate=48000:duration=8','-c:v','ffv1','-c:a','pcm_s16le','-y',str(clip))
 assert await make.wait()==0
 out=a.out;out.mkdir(parents=True,exist_ok=True);sd=tmp/'sd';sd.mkdir()
 events=out/'input.json';events.write_text(json.dumps([{'at':0.3,'type':'touch','x':500,'y':22,'down':True},{'at':0.5,'type':'touch','x':500,'y':22,'down':False}]))
 env=os.environ.copy();env['PYTHONPATH']=str(a.workspace/'pax-tools/desktop');env['SDL_AUDIODRIVER']='dummy'
 proc=await asyncio.create_subprocess_exec(str(a.workspace/'pax-tools/python-desktop/bin/python'),str(a.workspace/'pax-tools/desktop/host.py'),'emulated','--pax',str(a.pax),'--headless','--seconds','7','--sd-root',str(sd),'--input-script',str(events),'--report',str(out/'report.json'),'--screenshot',str(out/'screen.ppm'),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE,env=env)
 panel=types.SimpleNamespace(name='PAX AVI test',address='127.0.0.1',port=5000,tap_to_stop=False,sample_rate='device')
 sender=module.AviSender('ffmpeg',panel,module.AviJob(str(clip),'Test tone/video',fps=5,quality=15,audio_codec='auto'),lambda:None)
 try:
  await asyncio.sleep(1.5);await sender.start()
  assert sender._link.picture.upright==(512,300),sender._link.picture
  assert sender._link.rate==16000 and sender._link.channels==1
  stdout,stderr=await asyncio.wait_for(proc.communicate(),50);(out/'host.log').write_bytes(stdout+stderr)
  assert proc.returncode==0,stderr.decode()[-3000:]
  report=json.loads((out/'report.json').read_text());assert report['error'] is None,report
  assert report['audio_frames']>10000 and report['audio']['nonzero_blocks']>0,report
  image=Image.open(out/'screen.ppm');pixel=image.getpixel((image.width//2,image.height//2));assert pixel[0]>200 and pixel[1]<40,pixel
  image.save(out/'screen.png')
  print(json.dumps({'sender':'installed SVP Relay AviSender + ffmpeg','mode':'compiled RV32 app','frames':report['frames'],'audio_frames':report['audio_frames'],'nonzero_audio_blocks':report['audio']['nonzero_blocks'],'sample_rate':report['audio']['sample_rate'],'pixel_rgb':pixel},indent=2))
 finally:
  await sender.stop()
  if proc.returncode is None:proc.kill();await proc.wait()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True);p.add_argument('--relay',type=Path,required=True);p.add_argument('--pax',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='pax-avi-') as tmp:asyncio.run(run(a,Path(tmp)))
if __name__=='__main__':main()
