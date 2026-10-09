#!/usr/bin/env python3
"""Prepare a mounted SD/USB volume from the app's canonical installation recipe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile
from install_recipe import load, storage_path

MODULES = Path(__file__).resolve().parents[1]

def destination(root, virtual):
    storage_path(virtual, pattern=True)
    relative = '/'.join(virtual.rstrip('/').split('/')[2:])
    target = root / relative
    # Refuse existing symlinks escaping the mounted storage root.
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError('Target leaves the storage volume: '+virtual)
    return target

def sha256(path):
    h=hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda:stream.read(1024*1024),b''):h.update(chunk)
    return h.hexdigest()

def verify(path, size=None, digest=None):
    if size and path.stat().st_size != size:raise ValueError('Size mismatch: '+str(path))
    if digest and sha256(path) != digest:raise ValueError('SHA-256 mismatch: '+str(path))

def obtain(cache, url, size, digest=None, offline=False):
    key=digest or hashlib.sha256(url.encode()).hexdigest()
    target=cache/key
    if target.exists():verify(target,size,digest);return target
    if offline:raise ValueError('Verified download is not in the cache: '+url)
    cache.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix='.download-',dir=cache)
    try:
        with os.fdopen(fd,'wb') as out,urllib.request.urlopen(url,timeout=45) as response:
            copied=0
            while chunk:=response.read(min(1024*1024,size-copied+1)):
                copied+=len(chunk)
                if copied>size:raise ValueError('Download exceeded its expected size')
                out.write(chunk)
        part=Path(name);verify(part,size,digest);part.replace(target)
    finally:
        Path(name).unlink(missing_ok=True)
    return target

def copy_new(stream, target, size=None, digest=None):
    if target.exists():return 'kept'
    target.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix='.pax-install-',dir=target.parent)
    try:
        with os.fdopen(fd,'wb') as out:
            written=0
            while chunk:=stream.read(1024*1024):
                written+=len(chunk)
                if written>(size or 512*1024*1024):raise ValueError('Output exceeded its expected size')
                out.write(chunk)
            out.flush();os.fsync(out.fileno())
        part=Path(name);verify(part,size,digest)
        # Hard-link commits without replacing a file created concurrently.
        # FAT volumes do not support hard links; exclusive-create is the fallback.
        try:os.link(part,target)
        except FileExistsError:return 'kept'
        except OSError:
            try:
                with target.open('xb') as out:
                    try:
                        with part.open('rb') as src:shutil.copyfileobj(src,out)
                        out.flush();os.fsync(out.fileno())
                    except BaseException:
                        target.unlink(missing_ok=True)
                        raise
            except FileExistsError:return 'kept'
        return 'installed'
    finally:Path(name).unlink(missing_ok=True)

def install_group(recipe, group, root, cache, offline=False):
    files=group['files']
    if group['format']=='download' or any(f.get('url') for f in files):
        for item in files:
            if not item.get('url'):continue
            target=destination(root,item['target'])
            if target.exists():print('kept',item['target']);continue
            source=obtain(cache,item['url'],item['size'],item.get('sha256'),offline)
            with source.open('rb') as stream:print(copy_new(stream,target,item['size'],item.get('sha256')),item['target'])
    elif group['format']=='zip-files':
        local=destination(root,recipe['archive_dir'])/group['zip_name']
        if local.exists():verify(local,group.get('archive_size'),group.get('archive_sha256'));source=local
        elif group.get('archive_url'):source=obtain(cache,group['archive_url'],group['archive_size'],group.get('archive_sha256'),offline)
        else:raise ValueError('Place the ZIP here: '+str(local))
        with zipfile.ZipFile(source) as archive:
            for item in files:
                target=destination(root,item['target'])
                if target.exists():print('kept',item['target']);continue
                info=archive.getinfo(item['source'])
                if info.is_dir() or info.file_size>512*1024*1024 or (info.external_attr>>16)&0o170000==0o120000:
                    raise ValueError('Unsafe ZIP member: '+item['source'])
                with archive.open(info) as stream:print(copy_new(stream,target,item.get('size'),item.get('sha256')),item['target'])
    else:raise ValueError('Use recipe.pax for this ISO/CUE extraction choice, or copy already extracted files.')

def check(recipe, root):
    rows=[]
    for item in recipe.get('requires',[]):
        present=False
        for virtual in [item['path'],*item.get('alternatives',[])]:
            target=destination(root,virtual)
            if any(c in target.name for c in '*?['):
                matches=list(target.parent.glob(target.name)) if target.parent.exists() else []
                found=any(p.is_file() and p.stat().st_size>0 for p in matches)
            elif target.is_dir():found=any(target.iterdir())
            else:found=target.is_file() and target.stat().st_size>0
            present=present or found
        rows.append({'path':item['path'],'status':'present' if present else 'optional' if item.get('optional') else 'missing','note':item.get('note','')})
    return rows

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action',choices=['list','prepare','check','install'])
    parser.add_argument('app',nargs='?')
    parser.add_argument('--root',type=Path,help='Mounted SD/USB volume root; required for changes')
    parser.add_argument('--choice',type=int,default=0,help='Zero-based media choice listed by list APP')
    parser.add_argument('--cache',type=Path,default=MODULES/'.cache/install-downloads')
    parser.add_argument('--offline',action='store_true')
    args=parser.parse_args();index=json.loads((MODULES/'install-recipes.json').read_text())
    if args.action=='list' and not args.app:
        for item in index:print(item['app'],item['mode'],str(item['download_choices'])+' download choices')
        return
    entry=next((x for x in index if x['app']==args.app),None)
    if not entry:parser.error('Unknown app; use list to see available recipes')
    recipe=load(MODULES/entry['path'])
    if args.action=='list':
        print(recipe['instructions'])
        for i,m in enumerate(recipe['media']):print(i,m.get('download_label',m.get('zip_name',m['format'])))
        return
    if not args.root or not args.root.is_dir():parser.error('--root must be an existing mounted storage directory')
    if args.action in ('prepare','install'):
        for virtual in recipe.get('folders',[]):destination(args.root,virtual).mkdir(parents=True,exist_ok=True)
        if args.action=='install':
            if not 0<=args.choice<len(recipe['media']):parser.error('No such download/extraction choice; see list APP for manual steps')
            install_group(recipe,recipe['media'][args.choice],args.root,args.cache,args.offline)
    rows=check(recipe,args.root)
    print(json.dumps({'app':args.app,'mode':recipe.get('mode'),'files':rows,'note':'Presence checks do not prove game compatibility; complete original data and host processing may still be required.'},indent=2))
    if args.action=='check' and any(x['status']=='missing' for x in rows):raise SystemExit(1)

if __name__=='__main__':main()
