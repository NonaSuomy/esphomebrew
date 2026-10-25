import argparse,asyncio,json,os,tempfile
from pathlib import Path
async def main():
 root=args.workspace.resolve();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True);seen=[]
 async def serve(r,w):
  h=await r.readuntil(b'\r\n\r\n');length=int(next(x.split(b':')[1] for x in h.split(b'\r\n') if x.lower().startswith(b'content-length:')));b=json.loads(await r.readexactly(length));path=h.split()[1].decode();seen.append((path,b))
  if path=='/browse':
   if b['id']=='':items=[dict(title='Media folder',id='media-source://media_source',type='',dir=True,play=False)]
   else:items=[dict(title='Test music',id='media-source://media_source/local/test.mp3',type='audio/mpeg',dir=False,play=True)]
   data=dict(ok=True,title='HA test media',total=len(items),offset=0,items=items)
  else:data=dict(ok=True)
  body=json.dumps(data).encode();w.write(f'HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\n\r\n'.encode()+body);await w.drain();w.close();await w.wait_closed()
 server=await asyncio.start_server(serve,'127.0.0.1',5001)
 with tempfile.TemporaryDirectory() as temp:
  sd=Path(temp);(sd/'media').mkdir();(sd/'media/Test.flac').write_bytes(b'test')
  events=[]
  def touch(t,x,y):events.extend([dict(at=t,type='touch',x=x,y=y,down=True),dict(at=t+.15,type='touch',x=x,y=y,down=False)])
  touch(.3,300,22) # home MEDIA (switches to 512x300)
  touch(1.5,100,108) # folder
  touch(2.5,100,108) # music
  touch(3.5,40,260) # pause
  touch(4.5,110,57) # SD
  touch(5.5,100,108) # SD music
  (out/'events.json').write_text(json.dumps(events))
  env={**os.environ,'PYTHONPATH':str(root/'pax-tools/desktop'),'SDL_AUDIODRIVER':'dummy'}
  p=await asyncio.create_subprocess_exec(str(root/'pax-tools/python-desktop/bin/python'),str(root/'pax-tools/desktop/host.py'),'emulated','--pax',str(args.pax.resolve()),'--headless','--seconds','7','--sd-root',str(sd),'--input-script',str(out/'events.json'),'--report',str(out/'report.json'),'--screenshot',str(out/'screen.png'),stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE,env=env)
  stdout,stderr=await asyncio.wait_for(p.communicate(),60);(out/'host.log').write_bytes(stdout+stderr);assert p.returncode==0,stderr.decode()[-3000:]
  report=json.loads((out/'report.json').read_text());assert report['error'] is None,report
  assert [x[0] for x in seen]==['/browse','/browse','/play','/control','/play'],seen
  assert seen[2][1]['sd'] is False and seen[3][1]['command']=='pause' and seen[4][1]['sd'] is True,seen
  assert seen[4][1]['id']=='/sd/media/Test.flac',seen
  print(json.dumps(dict(requests=seen,frames=report['frames'],error=report['error']),indent=2))
 server.close();await server.wait_closed()
if __name__ == '__main__':
 parser=argparse.ArgumentParser(description='Compiled RV32 media browser: HA browse/play/pause and SD selection with a loopback mock bridge.');parser.add_argument('--workspace',type=Path,required=True);parser.add_argument('--pax',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args();asyncio.run(main())
