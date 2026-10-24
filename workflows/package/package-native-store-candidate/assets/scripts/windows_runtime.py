"""Admit only the source-bound desktop CRT from the prepared host toolchain."""
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil


def is_link_or_reparse(path):
    if path.is_symlink():
        return True
    try:
        attributes = os.stat(path, follow_symlinks=False).st_file_attributes
    except AttributeError:
        return False
    except FileNotFoundError:
        return False
    return bool(attributes & 0x400)


def validate_runtime(installation, receipt, expected):
    if not isinstance(receipt, dict) or set(receipt) != {'version', 'source_sha256', 'directory', 'files'}:
        raise ValueError('Invalid prepared desktop runtime receipt')
    expected_files = {record['name']: record for record in expected['files']}
    if (receipt['version'] != expected['version'] or receipt['source_sha256'] != expected['sha256']
            or receipt['directory'] != expected['directory'] or receipt['files'] != expected_files):
        raise ValueError('Prepared desktop runtime differs from the source-bound payload')
    relative = PurePosixPath(receipt['directory'])
    if relative.is_absolute() or '..' in relative.parts or '\\' in str(relative):
        raise ValueError('Desktop runtime path must be contained in the prepared installation')
    directory = Path(installation) / relative
    for name, record in expected_files.items():
        if name != Path(name).name or record['path'] != str(relative / name):
            raise ValueError('Invalid source-bound desktop runtime inventory')
        path = directory / name
        if (any(is_link_or_reparse(item) for item in (path, *path.parents)) or not path.is_file()
                or path.stat().st_size != record['size_bytes'] or path.stat().st_size > 16 << 20
                or hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']):
            raise ValueError('Prepared desktop runtime bytes differ: ' + name)
    return directory


def stage_probe_runtime(installation, receipt, expected, destination):
    """Recheck and stage exact CRT bytes for the disposable native ABI executable."""
    directory = validate_runtime(installation, receipt, expected)
    destination = Path(destination)
    for record in expected['files']:
        target = destination / record['name']
        if target.exists():
            raise ValueError('ABI probe runtime destination already exists')
        shutil.copyfile(directory / record['name'], target)
