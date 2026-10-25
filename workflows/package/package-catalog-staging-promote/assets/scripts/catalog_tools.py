"""Task-private tool extraction from exact public input pins; no system install."""
import importlib.util
import os
from pathlib import Path
import re
import shutil
import stat
import tarfile
import time
import urllib.request
import zipfile

from catalog_plan import relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped


def owning_acquirer():
    source = Path(__file__).resolve().parents[3] / 'package-native-store-candidate/assets/scripts/toolchain_acquire.py'
    specification = importlib.util.spec_from_file_location('catalog_official_input_acquirer', source)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module.acquire


def extract_cli(archive, pin, destination):
    destination.mkdir(mode=0o700, parents=True)
    expected_name = 'dockpipe.exe' if pin['platform'] == 'windows-amd64' else 'dockpipe'
    target = destination / expected_name
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            members = source.infolist()
            if len(members) != 1 or members[0].filename != expected_name or members[0].is_dir():
                raise PublicationStopped('Public CLI ZIP must contain exactly its single executable')
            mode = members[0].external_attr >> 16
            if stat.S_ISLNK(mode) or members[0].file_size > 128 * 1024 * 1024:
                raise PublicationStopped('Public CLI ZIP executable is unsafe or unbounded')
            with source.open(members[0]) as stream, target.open('xb') as output:
                shutil.copyfileobj(stream, output, 1024 * 1024)
    else:
        with tarfile.open(archive, 'r:gz') as source:
            member = source.next()
            if (member is None or member.name != expected_name or not member.isfile()
                    or member.size > 128 * 1024 * 1024):
                raise PublicationStopped('Public CLI tar must contain exactly its single regular executable')
            with source.extractfile(member) as stream, target.open('xb') as output:
                shutil.copyfileobj(stream, output, 1024 * 1024)
            if source.next() is not None:
                raise PublicationStopped('Public CLI tar contains an unexpected additional member')
    digest, size = file_digest(target, 128 * 1024 * 1024)
    if digest != pin['executable_sha256'] or size != pin['executable_size_bytes']:
        raise PublicationStopped('Public CLI executable differs from admitted inner byte inventory')
    expected_magic = {'linux-amd64': b'\x7fELF', 'windows-amd64': b'MZ',
                      'darwin-arm64': b'\xcf\xfa\xed\xfe'}[pin['platform']]
    with target.open('rb') as stream:
        if stream.read(len(expected_magic)) != expected_magic:
            raise PublicationStopped('Public CLI executable format differs from its native platform')
    target.chmod(0o755)
    return target


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, new_url):
        raise PublicationStopped('Official AWS tool input unexpectedly redirected')


def prepare_aws(pin, destination):
    if not re.fullmatch(r'https://awscli\.amazonaws\.com/awscli-exe-linux-x86_64-[0-9]+\.[0-9]+\.[0-9]+\.zip', pin['url']):
        raise PublicationStopped('AWS CLI needs an exact versioned official distribution URL')
    if not 0 < pin['size_bytes'] <= 128 * 1024 * 1024:
        raise PublicationStopped('AWS CLI distribution exceeds input capacity')
    destination.mkdir(mode=0o700)
    archive = destination / 'official.zip'
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    deadline = time.monotonic() + 125
    with opener.open(pin['url'], timeout=30) as response, archive.open('xb') as output:
        remaining = pin['size_bytes']
        while remaining:
            if time.monotonic() >= deadline:
                raise PublicationStopped('Official AWS CLI input exceeded its total deadline')
            block = response.read1(min(1024 * 1024, remaining))
            if not block:
                raise PublicationStopped('Official AWS CLI distribution is truncated')
            output.write(block)
            remaining -= len(block)
        if response.read(1):
            raise PublicationStopped('Official AWS CLI distribution exceeds its pinned size')
    digest, size = file_digest(archive, pin['size_bytes'])
    if digest != pin['sha256'] or size != pin['size_bytes']:
        raise PublicationStopped('Official AWS CLI distribution differs from exact source-bound pin')
    count = 0
    total = 0
    with zipfile.ZipFile(archive) as source:
        names = set()
        for member in source.infolist():
            count += 1
            total += member.file_size
            name = member.filename.rstrip('/')
            relative_path(name)
            mode = member.external_attr >> 16
            if (count > 100000 or total > 2 * 1024 * 1024 * 1024 or name in names
                    or not (name.startswith('aws/') or (name == 'aws' and member.is_dir()))
                    or stat.S_ISLNK(mode)
                    or (stat.S_IFMT(mode) not in {0, stat.S_IFREG, stat.S_IFDIR})):
                raise PublicationStopped('AWS CLI distribution contains unsafe or unbounded members')
            names.add(name)
            target = destination / name
            if member.is_dir():
                target.mkdir(mode=0o700, parents=True, exist_ok=True)
                continue
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with source.open(member) as stream, target.open('xb') as output:
                shutil.copyfileobj(stream, output, 1024 * 1024)
            target.chmod(0o755 if mode & 0o111 else 0o644)
    executable = destination / 'aws/dist/aws'
    if not executable.is_file() or executable.is_symlink():
        raise PublicationStopped('Official AWS CLI executable is absent')
    return executable, file_digest(executable, 256 * 1024 * 1024)[0]
