"""Exact upstream license texts for native dependency bottles that omit them."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'dependencies'


def pinned_license_files(name, version):
    lock = ROOT / 'runtime-licenses.lock.json'
    if lock.stat().st_size > 65536:
        raise ValueError('Native license lock exceeds its bound')
    document = json.loads(lock.read_text())
    if document.get('schema') != 1 or len(document.get('dependencies', [])) > 32:
        raise ValueError('Unsupported native license lock')
    profile = next((entry for entry in document['dependencies']
                    if entry['name'] == name and entry['version'] == version), None)
    if profile is None:
        raise ValueError('No pinned upstream license profile: ' + name + ' ' + version)
    entries = profile['files']
    if not 1 <= len(entries) <= 128:
        raise ValueError('Native license inventory exceeds its bound')
    files = []
    total = 0
    for entry in entries:
        relative = Path(entry['path'])
        path = ROOT / relative
        if relative.is_absolute() or '..' in relative.parts or path.is_symlink() or \
                not path.resolve().is_relative_to(ROOT.resolve()) or not path.is_file():
            raise ValueError('Invalid pinned native license path')
        total += path.stat().st_size
        if total > 16 * 1024 * 1024:
            raise ValueError('Pinned native licenses exceed 16 MiB')
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Pinned native license digest mismatch')
        files.append(path)
    provenance = {key: profile[key] for key in
                  ('name', 'version', 'source_url', 'source_sha256', 'source_size_bytes')}
    return files, provenance
