#!/usr/bin/env python3
"""Create an original Scratch 3 smoke-test project; no downloaded game assets."""
import json,zipfile,hashlib,io,wave,math,struct,shutil
from pathlib import Path
root=Path(__file__).resolve().parent.parent
out=root/'data';out.mkdir(exist_ok=True)
assets={}
def asset(data,ext,name,cx,cy):
 key=hashlib.md5(data).hexdigest();assets[key+'.'+ext]=data
 return dict(assetId=key,name=name,md5ext=key+'.'+ext,dataFormat=ext,bitmapResolution=1,rotationCenterX=cx,rotationCenterY=cy)
stage=asset(b'<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360"><rect width="480" height="360" fill="#182838"/><path d="M0 280H480" stroke="#80a0b0" stroke-width="2"/></svg>','svg','Backdrop',240,180)
shape=asset(b'<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48"><rect x="4" y="4" width="40" height="40" rx="8" fill="#ffb020"/><circle cx="17" cy="19" r="4"/><circle cx="31" cy="19" r="4"/><path d="M15 32H33" stroke="#203040" stroke-width="3"/></svg>','svg','PAX Sprite',24,24)
# Hand-encoded 16x16 RGBA PNG (stdlib only).
import zlib
pixels=b''.join(b'\0'+bytes([32,192,255,255])*16 for _ in range(16))
def chunk(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',16,16,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(pixels))+chunk(b'IEND',b'')
bitmap=asset(png,'png','Bitmap',8,8)
f=io.BytesIO()
with wave.open(f,'wb') as w:
 w.setnchannels(1);w.setsampwidth(2);w.setframerate(22050);w.writeframes(b''.join(struct.pack('<h',int(6000*math.sin(i*440*2*math.pi/22050))) for i in range(11025)))
wav=f.getvalue();key=hashlib.md5(wav).hexdigest();assets[key+'.wav']=wav
sound=dict(assetId=key,name='beep',md5ext=key+'.wav',dataFormat='wav',rate=22050,sampleCount=11025)
def block(op,nxt=None,parent=None,inputs=None,fields=None,top=False):return dict(opcode=op,next=nxt,parent=parent,inputs=inputs or {},fields=fields or {},shadow=False,topLevel=top,**(dict(x=30,y=30) if top else {}))
blocks={
 'flag':block('event_whenflagclicked','set',top=True),
 'set':block('data_setvariableto','say','flag',{'VALUE':[1,[4,'0']]},{'VARIABLE':['ticks','ticks']}),
 'say':block('looks_say','sound','set',{'MESSAGE':[1,[10,'Hello PAX!']]}),
 'sound':block('sound_play','pen','say',{'SOUND_MENU':[1,'soundmenu']}),
 'soundmenu':dict(opcode='sound_sounds_menu',next=None,parent='sound',inputs={},fields={'SOUND_MENU':['beep',None]},shadow=True,topLevel=False),
 'pen':block('pen_penDown','forever','sound'),
 'forever':block('control_forever',None,'pen',{'SUBSTACK':[2,'tick']}),
 'tick':block('data_changevariableby','move','forever',{'VALUE':[1,[4,'1']]},{'VARIABLE':['ticks','ticks']}),
 'move':block('motion_movesteps','bounce','tick',{'STEPS':[1,[4,'3']]}),
 'bounce':block('motion_ifonedgebounce',None,'move'),
}
base=dict(variables={},lists={},broadcasts={},blocks={},comments={},currentCostume=0,sounds=[],volume=100)
targets=[dict(base,isStage=True,name='Stage',costumes=[stage],tempo=60,videoTransparency=50,videoState='on'),dict(base,isStage=False,name='PAX Sprite',costumes=[shape],variables={'ticks':['ticks',0]},sounds=[sound],blocks=blocks,visible=True,x=-120,y=0,size=100,direction=90,draggable=False,rotationStyle='all around',layerOrder=1),dict(base,isStage=False,name='Bitmap',costumes=[bitmap],visible=True,x=150,y=-70,size=100,direction=90,rotationStyle='all around',layerOrder=2)]
project=dict(targets=targets,monitors=[dict(id='ticks',mode='default',opcode='data_variable',params={'VARIABLE':'ticks'},spriteName='PAX Sprite',value=0,width=0,height=0,x=10,y=10,visible=True,sliderMin=0,sliderMax=100,isDiscrete=True)],extensions=['pen'],meta={'semver':'3.0.0','vm':'0.2.0','agent':'PAX authored smoke test'})
with zipfile.ZipFile(out/'PAX Demo.sb3','w',compression=zipfile.ZIP_DEFLATED) as z:
 info=zipfile.ZipInfo('project.json',(2026,10,13,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,json.dumps(project));
 for name,data in assets.items():
  info=zipfile.ZipInfo(name,(2026,10,13,0,0,0));info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,data)
sd=Path('/tmp/pax-scratch-test-sd');(sd/'projects').mkdir(parents=True,exist_ok=True);(sd/'settings').mkdir(exist_ok=True)
shutil.copy2(out/'PAX Demo.sb3',sd/'projects/')
shutil.copytree(root/'core/gfx',sd/'romfs/gfx',dirs_exist_ok=True)
# Stream ZIPs from SD instead of keeping the compressed archive in RAM.
(sd/'projects/PAX Demo.sb3.json').write_text(json.dumps({'settings':{'sb3InRam':False}}))
print(out/'PAX Demo.sb3')
