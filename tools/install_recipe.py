"""Validated installation definitions shared by builds and storage preparation."""
import json
import re
from pathlib import Path
from urllib.parse import urlsplit

MAX_FILES = 512
MAX_MEDIA = 16

def storage_path(value, pattern=False):
    if not isinstance(value, str) or not value.isascii() or not 0 < len(value) <= 200:
        raise ValueError('Invalid storage path')
    if not value.startswith(('/sd/', '/usb0/')):
        raise ValueError('Storage path must be under /sd/ or /usb0/')
    parts = value.rstrip('/').split('/')[2:]
    if not parts or any(x in ('', '.', '..') or '\\' in x for x in parts):
        raise ValueError('Unsafe storage path')
    if any(any(c in x for c in '*?[]') for x in (parts[:-1] if pattern else parts)):
        raise ValueError('Patterns are only allowed in the last requirement component')
    return value

def relative(value):
    if not isinstance(value, str) or not value.isascii() or not 0 < len(value) < 192:
        raise ValueError('Invalid archive member')
    if value.startswith('/') or '\\' in value or any(x in ('', '.', '..') for x in value.split('/')):
        raise ValueError('Unsafe archive member')
    return value

def download(url, size, sha=None, archive_only=False):
    if not isinstance(url, str) or len(url) >= 512 or not url.isascii():
        raise ValueError('Invalid download URL')
    parsed = urlsplit(url)
    if parsed.scheme != 'https' or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise ValueError('Downloads require an HTTPS URL without credentials')
    if archive_only and not url.startswith('https://archive.org/download/'):
        raise ValueError('Version 1 requires an Archive member URL')
    if type(size) is not int or not 0 < size <= 512 * 1024 * 1024:
        raise ValueError('Invalid download size')
    if sha is not None and not re.fullmatch('[0-9a-f]{64}', sha):
        raise ValueError('Invalid SHA-256')

def validate(recipe):
    if not isinstance(recipe, dict) or recipe.get('version') not in (1, 2):
        raise ValueError('Unsupported installation recipe')
    storage_path(recipe.get('archive_dir'))
    media = recipe.get('media')
    if not isinstance(media, list) or len(media) > MAX_MEDIA or (recipe['version'] == 1 and not media):
        raise ValueError('Invalid media list')
    if recipe['version'] == 2:
        notes = recipe.get('instructions', '')
        if not isinstance(notes, str) or not notes.strip() or len(notes.encode()) >= 4096:
            raise ValueError('Version 2 requires bounded instructions')
        folders = recipe.get('folders', [])
        if not isinstance(folders, list) or len(folders) > 32:
            raise ValueError('Invalid folders')
        for folder in folders: storage_path(folder)
        for item in recipe.get('requires', []):
            storage_path(item.get('path'), pattern=True)
            for alternate in item.get('alternatives', []):storage_path(alternate, pattern=True)
    targets, count = set(), 0
    for group in media:
        fmt = group.get('format')
        if fmt not in ('zip-iso9660', 'zip-cue-bin', 'zip-files', 'zip-dos-floppy', 'download'):
            raise ValueError('Unsupported media format')
        if recipe['version'] == 1 and fmt not in ('zip-iso9660', 'zip-cue-bin'):
            raise ValueError('Version 1 media requires an ISO or CUE')
        if fmt not in ('download', 'zip-files', 'zip-dos-floppy'):
            if group.get('auto_entry'):
                if fmt != 'zip-iso9660' or 'entry' in group: raise ValueError('Invalid automatic ISO entry')
            else:
                relative(group.get('entry'))
                if not group['entry'].lower().endswith('.iso' if fmt == 'zip-iso9660' else '.cue'):
                    raise ValueError('Wrong image entry extension')
        if fmt == 'zip-files' or 'zip_name' in group:
            name = relative(group.get('zip_name'))
            if '/' in name or not name.lower().endswith('.zip'): raise ValueError('Invalid ZIP filename')
        if 'archive_url' in group:
            if fmt != 'zip-files': raise ValueError('Archive download needs zip-files format')
            download(group['archive_url'], group.get('archive_size'), group.get('archive_sha256'))
        files = group.get('files', [])
        if not isinstance(files, list) or not files: raise ValueError('Media has no file mappings')
        for item in files:
            relative(item.get('source'))
            target = storage_path(item.get('target'))
            if target.lower() in targets: raise ValueError('Duplicate recipe target')
            targets.add(target.lower()); count += 1
            if 'url' in item:
                download(item['url'], item.get('size'), item.get('sha256'), recipe['version'] == 1)
            if fmt == 'download' and 'url' not in item: raise ValueError('Download file has no URL')
            if 'sha256' in item and not re.fullmatch('[0-9a-f]{64}', item['sha256']):
                raise ValueError('Invalid member SHA-256')
            if 'size' in item and (type(item['size']) is not int or not 0 < item['size'] <= 512*1024*1024):
                raise ValueError('Invalid member size')
    if count > MAX_FILES: raise ValueError('Too many recipe mappings')
    if len(json.dumps(recipe).encode()) > 190*1024: raise ValueError('Recipe exceeds device JSON buffer')
    return recipe

def load(path):
    return validate(json.loads(Path(path).read_text()))
