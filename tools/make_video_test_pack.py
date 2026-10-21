#!/usr/bin/env python3
"""Make matched PAX codec/container/audio tests locally; never uploads files."""
from __future__ import annotations
import argparse,concurrent.futures,hashlib,json,subprocess
from pathlib import Path

VIDEO = [
 ('h264','mp4',['-c:v','libx264','-preset','veryfast','-profile:v','baseline','-level:v','3.0','-crf','26','-bf','0'],['-c:a','aac','-b:a','64k']),
 ('mpeg4','avi',['-c:v','mpeg4','-q:v','5','-bf','0'],['-c:a','libmp3lame','-b:a','64k']),
 ('mjpeg','avi',['-c:v','mjpeg','-q:v','5','-pix_fmt','yuvj420p'],['-c:a','pcm_s16le']),
 ('vp8','webm',['-c:v','libvpx','-deadline','realtime','-cpu-used','8','-b:v','400k'],['-c:a','libvorbis','-q:a','3']),
 ('msmpeg4v2','avi',['-c:v','msmpeg4v2','-q:v','5'],['-c:a','libmp3lame','-b:a','64k']),
 ('theora','ogv',['-c:v','libtheora','-q:v','5'],['-c:a','libvorbis','-q:a','3']),
 ('hevc','mp4',['-c:v','libx265','-preset','ultrafast','-crf','30','-x265-params','pools=2:frame-threads=1:bframes=0:ref=1:log-level=error','-tag:v','hvc1'],['-c:a','aac','-b:a','64k']),
 ('vp9','webm',['-c:v','libvpx-vp9','-deadline','realtime','-cpu-used','8','-row-mt','1','-b:v','0','-crf','35'],['-c:a','libopus','-b:a','64k']),
 ('av1','mkv',['-c:v','libaom-av1','-cpu-used','8','-usage','realtime','-lag-in-frames','0','-g','48','-b:v','400k'],['-c:a','libopus','-b:a','64k']),
]
AUDIO = [('aac','m4a',['-c:a','aac','-b:a','64k']),('mp3','mp3',['-c:a','libmp3lame','-b:a','64k']),
 ('opus','opus',['-c:a','libopus','-b:a','64k']),('vorbis','ogg',['-c:a','libvorbis','-q:a','3'])]
AUDIO += [(codec,'mov' if codec=='pcm_s16be' else 'wav',['-c:a',codec]) for codec in
          ['pcm_s16le','pcm_s16be','pcm_u8','pcm_s24le','pcm_s32le','pcm_f32le','pcm_alaw','pcm_mulaw']]
EXPERIMENTAL={'hevc','vp9','av1'}

def run(command,log):
 result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 log.parent.mkdir(parents=True,exist_ok=True);log.write_text(result.stdout)
 if result.returncode:raise RuntimeError(f'Command failed; see {log}\n'+result.stdout[-1200:])

def describe(path,root,kind,expected):
 result=subprocess.run(['ffprobe','-v','error','-show_entries','stream=codec_type,codec_name,width,height,pix_fmt,r_frame_rate,sample_rate,channels:format=duration','-of','json',str(path)],check=True,capture_output=True,text=True)
 info=json.loads(result.stdout)
 if not info.get('streams'):raise RuntimeError('No streams: '+str(path))
 codecs={s['codec_name'] for s in info['streams']}
 if expected not in codecs:raise RuntimeError(f'{path}: expected {expected}, found {codecs}')
 with path.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
 return dict(file=path.relative_to(root).as_posix(),kind=kind,experimental=expected in EXPERIMENTAL,size=path.stat().st_size,sha256=digest,**info)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path,required=True);p.add_argument('--out',type=Path,required=True)
 p.add_argument('--start',type=float,default=60);p.add_argument('--duration',type=float,default=15);p.add_argument('--resolutions',nargs='+',default=['320x180','640x360']);p.add_argument('--fps',type=int,default=24);p.add_argument('--jobs',type=int,default=2);p.add_argument('--codecs',nargs='+',choices=[v[0] for v in VIDEO]);p.add_argument('--skip-extras',action='store_true');a=p.parse_args()
 if not a.input.is_file() or a.duration<=0 or a.start<0 or a.fps<=0:p.error('Valid input, start, duration and FPS required')
 root=a.out.resolve();root.mkdir(parents=True,exist_ok=True);base=root/'videos/pax-tests';logs=root/'encode-logs';records=[]
 source_args=['-ss',str(a.start),'-i',str(a.input.resolve()),'-t',str(a.duration)]
 def encode(spec):
  name,ext,vflags,aflags,size=spec;w,h=map(int,size.split('x'))
  if min(w,h)<32 or w%2 or h%2:raise ValueError('Use even dimensions at least 32 pixels')
  path=base/size/f'{name}_{size}_{a.fps}fps.{ext}';path.parent.mkdir(parents=True,exist_ok=True)
  command=['ffmpeg','-hide_banner','-loglevel','warning','-y',*source_args,'-map','0:v:0','-map','0:a:0','-vf',f'scale={w}:{h}:flags=lanczos,setsar=1,fps={a.fps}','-pix_fmt','yuv420p','-threads','2',*vflags,*aflags,'-ar','48000' if 'libopus' in aflags else '32000','-ac','2','-map_metadata','-1',str(path)]
  print('Encoding',path.relative_to(root),flush=True);run(command,logs/(path.stem+'.log'))
  run(['ffmpeg','-v','error','-threads','2','-i',str(path),'-f','null','-'],logs/(path.stem+'-decode.log'))
  return describe(path,root,'video',name)
 specs=[(name,ext,v,audio,size) for size in a.resolutions for name,ext,v,audio in VIDEO if not a.codecs or name in a.codecs]
 with concurrent.futures.ThreadPoolExecutor(max_workers=max(1,min(a.jobs,4))) as pool:
  for record in pool.map(encode,specs):records.append(record)
 if not a.skip_extras:
  for codec,ext,flags in AUDIO:
   path=base/'audio'/f'audio_{codec}.{ext}';path.parent.mkdir(parents=True,exist_ok=True)
   run(['ffmpeg','-hide_banner','-loglevel','warning','-y',*source_args,'-vn',*flags,'-ar','48000' if 'libopus' in flags else '32000','-ac','2','-map_metadata','-1',str(path)],logs/(path.stem+'.log'))
   run(['ffmpeg','-v','error','-i',str(path),'-f','null','-'],logs/(path.stem+'-decode.log'));records.append(describe(path,root,'audio',codec));print('Encoded',path.relative_to(root),flush=True)
  # Rewrap identical H.264/AAC streams to isolate demuxer/container behavior.
  baseline=base/a.resolutions[0]/f'h264_{a.resolutions[0]}_{a.fps}fps.mp4'
  if baseline.exists():
   for ext,flags in [('mov',[]),('3gp',[]),('mkv',[]),('ts',[]),('mts',['-f','mpegts']),('m2ts',['-f','mpegts','-mpegts_m2ts_mode','1'])]:
    path=base/'containers'/('h264_aac.'+ext);path.parent.mkdir(parents=True,exist_ok=True)
    run(['ffmpeg','-hide_banner','-loglevel','warning','-y','-i',str(baseline),'-map','0','-c','copy','-map_metadata','-1',*flags,str(path)],logs/('container-'+ext+'.log'))
    run(['ffmpeg','-v','error','-i',str(path),'-f','null','-'],logs/('container-'+ext+'-decode.log'));records.append(describe(path,root,'container','h264'))
  # A flash and short tone share each whole-second boundary for A/V checks.
  path=base/'sync/flash_beep.mp4';path.parent.mkdir(parents=True,exist_ok=True)
  run(['ffmpeg','-hide_banner','-loglevel','warning','-y','-f','lavfi','-i',f'testsrc2=size=320x180:rate={a.fps}:duration={a.duration}','-f','lavfi','-i',f'aevalsrc=if(lt(mod(t\\,1)\\,0.05)\\,0.5*sin(2*PI*1000*t)\\,0):s=32000:d={a.duration}','-vf',"drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='lt(mod(t,1),0.05)'",'-c:v','libx264','-preset','veryfast','-profile:v','baseline','-pix_fmt','yuv420p','-threads','2','-c:a','aac','-b:a','64k','-ac','2',str(path)],logs/'sync.log')
  run(['ffmpeg','-v','error','-i',str(path),'-f','null','-'],logs/'sync-decode.log');records.append(describe(path,root,'av-sync','h264'))
 version=subprocess.check_output(['ffmpeg','-version'],text=True).splitlines()[0]
 manifest=dict(source=str(a.input.resolve()),start=a.start,duration=a.duration,fps=a.fps,encoder=version,files=records,validation='All encoded files fully decoded on this computer; no P4 playback claims')
 (root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 (root/'SHA256SUMS').write_text(''.join(r['sha256']+'  '+r['file']+'\n' for r in records))
 lines=['# PAX Video Player test pack','','Copy `videos/` and `roms/` (if supplied) to the SD card root. Open Video Player, then `pax-tests/`. No files are uploaded by this generator.','',f'All movie tests use the same {a.duration:g}-second segment, starting {a.start:g} seconds into Big Buck Bunny, at {a.fps} fps.','', 'Start at 320×180, then repeat at 640×360. HEVC, VP9 and AV1 are experimental; successful decoding does not promise real-time playback.','', 'For each clip check picture, sound, pause/resume, seek forward/back, return to the list and FPS/dropped frames. Use `sync/flash_beep.mp4` to compare the white flash with its short beep every second. Keep the player’s audio offset at zero for the initial comparison.','', 'The player supports 8-bit YUV output. These clips use 8-bit formats; this pack does not claim 10/12-bit or HDR support.','', 'Files were fully decoded on the computer. Actual P4 performance still needs hardware testing.','', '| File | Streams | Experimental |','|---|---|---|']
 for r in records:lines.append('| '+r['file']+' | '+', '.join(s['codec_name'] for s in r['streams'])+' | '+('Yes' if r['experimental'] else '')+' |')
 (root/'README.md').write_text('\n'.join(lines)+'\n')
 print(f'COMPLETE: {len(records)} validated files, {sum(r["size"] for r in records):,} bytes, in {root}',flush=True)
if __name__=='__main__':main()
