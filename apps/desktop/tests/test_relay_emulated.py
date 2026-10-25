#!/usr/bin/env python3
"""Test the compiled RV32 receiver using the installed SVP Relay's sender code.
Needs --workspace, --pax, --relay (the integration root); optional --host for a staged emulator.
Runs a mock authenticated HA endpoint, not the user's Home Assistant.
"""
import argparse
import ast
import io
import asyncio
import importlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
from PIL import Image

async def run(args, temporary):
    # Import the real transport/caster without executing HA integration setup.
    package=types.ModuleType('svp_test_source')
    package.__path__=[str(args.relay/'custom_components/svp_relay')]
    sys.modules[package.__name__]=package
    link_module=importlib.import_module('svp_test_source.svp_core.link')
    wire=importlib.import_module('svp_test_source.svp_core.udisp')
    # Execute the installed encoder function unchanged, without importing its
    # optional HA/browser dependency chain (aiohttp/Chromium are not needed here).
    tree=ast.parse((args.relay/'custom_components/svp_relay/desktop/caster.py').read_text())
    nodes=[node for node in tree.body if
           isinstance(node,ast.FunctionDef) and node.name=='desktop_jpeg' or
           isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='_TURNS' for t in node.targets)]
    encoder={'io':io,'JPEG_QUALITIES':wire.JPEG_QUALITIES}
    exec(compile(ast.Module(body=nodes,type_ignores=[]),'installed desktop_jpeg','exec'),encoder)
    caster=types.SimpleNamespace(desktop_jpeg=encoder['desktop_jpeg'])
    seen={'text':[], 'keys':[], 'mouse':[], 'touch':[], 'echo':[], 'stats':[], 'requests':[]}
    errors=[]
    link=None

    async def request(reader,writer):
        nonlocal link
        try:
            head=await reader.readuntil(b'\r\n\r\n')
            headers={}
            for line in head.decode().split('\r\n')[1:]:
                if ':' in line:
                    name,value=line.split(':',1);headers[name.lower()]=value.strip()
            assert head.startswith(b'POST /api/svp_relay/desktop HTTP/1.1\r\n')
            assert headers['authorization']=='Bearer TEST_ONLY_NOT_A_REAL_TOKEN'
            data=json.loads(await reader.readexactly(int(headers['content-length'])))
            assert data=={'port':5000,'action':'start','kind':'vnc','host':'computer.test','server_port':5901,'password':''},data
            seen['requests'].append(data)
            panel=types.SimpleNamespace(name='PAX desktop',address='127.0.0.1',port=5000,tap_to_stop=False,sample_rate='device')
            link=link_module.PanelLink(panel,lambda _:None,
                on_key=lambda *values:seen['keys'].append(values),
                on_text=lambda value:seen['text'].append(value),
                on_pointer=lambda *values:seen['mouse'].append(values),
                on_touch=lambda values:seen['touch'].append(values),
                on_echo=lambda value:seen['echo'].append(value),
                on_stats=lambda value:seen['stats'].append(value))
            await link.open()
            assert link.picture.turns==0 and link.still==1048576
            assert link.picture.width==1024 and link.picture.height==600
            width,height=link.picture.upright
            image=Image.new('RGB',(width,height))
            image.paste((240,15,10),(0,0,width//2,height))
            image.paste((10,200,40),(width//2,0,width,height))
            jpeg,quality=caster.desktop_jpeg(image,(width,height),link.still,85,85,0)
            link.writer.write(wire.UDISP_HEADER.pack(0,0x21,0,1,0,0,0,0))
            link.writer.write(wire.UDISP_HEADER.pack(0,0x20,0,1,0,0,0,0))
            link.writer.write(wire.UDISP_HEADER.pack(0,0x22,0,321,0,0,0,0))
            # Deliberately fragment a real desktop_jpeg() packet header.
            packet=wire.UDISP_HEADER.pack(0,3,0,0,0,width,height,len(jpeg)<<10)+jpeg
            for offset in range(0,len(packet),173):
                link.writer.write(packet[offset:offset+173]);await link.writer.drain()
            cursor=bytes([255,255,255,255])*4
            link.writer.write(wire.UDISP_HEADER.pack(0,0x23,0,0,0,2,2,len(cursor)<<10)+cursor)
            link.writer.write(wire.UDISP_HEADER.pack(0,0x24,0,600,300,0,0,0))
            link.writer.write(wire.HEARTBEAT)
            await link.writer.drain()
            writer.write(b'HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{"ok":true}')
            await writer.drain()
        except Exception as error:
            errors.append(repr(error))
            writer.write(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n')
            await writer.drain()
        finally:
            writer.close()

    api=await asyncio.start_server(request,'127.0.0.1',0)
    port=api.sockets[0].getsockname()[1]
    out=args.out or temporary/'out';out.mkdir(parents=True,exist_ok=True)
    sd=temporary/'sd';folder=sd/'roms/desktop';folder.mkdir(parents=True)
    (folder/'relay.conf').write_text(f'http://127.0.0.1:{port}\nTEST_ONLY_NOT_A_REAL_TOKEN\n')
    events=out/'input.json'
    events.write_text(json.dumps([
      {'at':0.4,'type':'touch','x':940,'y':22,'down':True},
      {'at':0.6,'type':'touch','x':940,'y':22,'down':False},
      {'at':4,'type':'key','key':ord('a'),'down':True},
      {'at':4.1,'type':'key','key':ord('a'),'down':False},
      {'at':4.3,'type':'mouse','x':500,'y':300,'buttons':1},
      {'at':4.4,'type':'mouse','x':500,'y':300,'buttons':0},
      {'at':4.7,'type':'touch','x':250,'y':220,'down':True},
      {'at':4.8,'type':'touch','x':250,'y':220,'down':False},
    ]))
    env=os.environ.copy();env['PYTHONPATH']=str(args.workspace/'pax-tools/desktop')+os.pathsep+env.get('PYTHONPATH','')
    env['SDL_AUDIODRIVER']='dummy'
    host=args.host or args.workspace/'pax-tools/desktop/host.py'
    process=await asyncio.create_subprocess_exec(
      str(args.workspace/'pax-tools/python-desktop/bin/python'),str(host),'emulated',
      '--pax',str(args.pax),'--headless','--seconds','8','--sd-root',str(sd),
      '--app-arg','computer.test:5901','--input-script',str(events),
      '--report',str(out/'report.json'),'--screenshot',str(out/'screen.ppm'),
      stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE,env=env)
    try:
        stdout,stderr=await asyncio.wait_for(process.communicate(),40)
        (out/'host.log').write_bytes(stdout+stderr)
        assert process.returncode==0,stderr.decode()[-3000:]
        report=json.loads((out/'report.json').read_text())
        assert report['error'] is None,report
        assert not errors,errors
        assert len(seen['requests'])==1,seen
        assert 321 in seen['echo'],seen
        assert any(key[5]==ord('a') and key[4] for key in seen['keys']),seen
        assert 'a' in seen['text'],seen
        assert seen['mouse'] and any(points for points in seen['touch']),seen
        assert seen['stats'],seen
        image=Image.open(out/'screen.ppm')
        left,right=image.getpixel((100,100)),image.getpixel((800,100))
        assert left[0]>200 and left[1]<30 and right[1]>170 and right[0]<30,(left,right)
        print(json.dumps({'sender':'installed SVP Relay desktop_jpeg + PanelLink',
            'mode':'compiled RV32 app via mock authenticated HA API',
            'frames':report['frames'],'keys':len(seen['keys']),'mouse':len(seen['mouse']),
            'touch':len(seen['touch']),'stats':len(seen['stats']),'left_rgb':left,'right_rgb':right},indent=2))
    finally:
        if process.returncode is None:process.kill();await process.wait()
        if link:link.close()
        api.close();await api.wait_closed()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace',type=Path,required=True);parser.add_argument('--pax',type=Path,required=True)
    parser.add_argument('--relay',type=Path,required=True);parser.add_argument('--host',type=Path);parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pax-relay-emulated-') as directory:
        asyncio.run(run(args,Path(directory)))
if __name__=='__main__':main()
