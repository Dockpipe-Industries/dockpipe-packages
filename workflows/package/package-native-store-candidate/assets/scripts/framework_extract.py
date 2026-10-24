"""Stage regular files and contained framework aliases; never delegate filesystem extraction."""
from contextlib import contextmanager
import ctypes
import ctypes.util
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import platform
import stat
import tempfile
import time
import unicodedata

MAX_FILES = 60000
MAX_LINKS = 4096
MAX_BYTES = 2 << 30
MAX_PATH = 4096
MAX_LINK_HOPS = 64


@dataclass
class Member:
    name: str
    kind: str
    size: int
    mode: int
    target: str = ''


def name_parts(value):
    if (not isinstance(value, str) or not value or len(value.encode('utf-8')) > MAX_PATH
            or any(ord(character) < 32 for character in value) or '\\' in value or ':' in value):
        raise ValueError('Invalid framework archive path')
    path = PurePosixPath(value)
    if path.is_absolute() or path.as_posix() != value or any(part in {'.', '..'} for part in path.parts):
        raise ValueError('Noncanonical or escaping framework archive path')
    return path.parts


def link_destination(name, target):
    if (not target or len(target.encode('utf-8')) > MAX_PATH or target.startswith('/')
            or '\\' in target or ':' in target or any(ord(character) < 32 for character in target)):
        raise ValueError('Invalid framework alias target')
    parts = list(PurePosixPath(name).parent.parts)
    for part in target.split('/'):
        if part in {'', '.'}:
            continue
        if part == '..':
            if not parts:
                raise ValueError('Framework alias escapes the selected prefix')
            parts.pop()
        else:
            parts.append(part)
    return '/'.join(parts)


class FrameworkTree:
    def __init__(self, root, maximum_files=MAX_FILES, maximum_bytes=MAX_BYTES, cancelled=None):
        self.root = root
        self.maximum_files = maximum_files
        self.maximum_bytes = maximum_bytes
        self.cancelled = cancelled or (lambda: False)
        self.started = time.monotonic()
        self.entries = {'': 'directory'}
        self.names = {}
        self.links = {}
        self.count = 0
        self.expanded = 0

    def checkpoint(self):
        if self.cancelled() or time.monotonic() - self.started > 600:
            raise ValueError('Framework acquisition cancelled or deadline exceeded')

    def register(self, name, kind):
        self.checkpoint()
        parts = name_parts(name)
        for index in range(1, len(parts) + 1):
            current = '/'.join(parts[:index])
            current_kind = kind if index == len(parts) else 'directory'
            key = unicodedata.normalize('NFC', current).casefold()
            if key in self.names and self.names[key] != current:
                raise ValueError('Case or Unicode-normalized framework path collision')
            old = self.entries.get(current)
            if old is not None and not (old == current_kind == 'directory'):
                raise ValueError('Duplicate file, alias parent or framework entry collision')
            self.names[key] = current
            self.entries[current] = current_kind

    def add(self, member, read):
        self.count += 1
        if (self.count > self.maximum_files or type(member.size) is not int or member.size < 0
                or self.expanded + member.size > self.maximum_bytes):
            raise ValueError('Framework member or expanded byte capacity exceeded')
        if member.kind not in {'file', 'directory', 'symlink'}:
            raise ValueError('Framework hard links and special files are unsupported')
        self.register(member.name, member.kind)
        self.expanded += member.size
        destination = self.root.joinpath(*name_parts(member.name))
        if member.kind == 'directory':
            destination.mkdir(parents=True, exist_ok=True, mode=0o700)
            return
        if member.kind == 'symlink':
            if len(self.links) >= MAX_LINKS:
                raise ValueError('Framework alias capacity exceeded')
            self.links[member.name] = (member.target, link_destination(member.name, member.target))
            return
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        remaining = member.size
        with os.fdopen(descriptor, 'wb') as output:
            while remaining:
                self.checkpoint()
                block = read(min(1 << 20, remaining))
                if not block or len(block) > remaining:
                    raise ValueError('Framework member payload is truncated or oversized')
                output.write(block)
                remaining -= len(block)
            if read(1):
                raise ValueError('Framework payload exceeds its declared member size')
        destination.chmod(0o755 if member.mode & 0o111 else 0o644)

    def resolve(self, name):
        seen = set()
        current = name
        for _ in range(MAX_LINK_HOPS):
            if current in seen:
                raise ValueError('Framework alias cycle')
            seen.add(current)
            parts = PurePosixPath(current).parts
            for index in range(1, len(parts) + 1):
                prefix = '/'.join(parts[:index])
                if prefix in self.links:
                    base = self.links[prefix][1]
                    suffix = '/'.join(parts[index:])
                    current = '/'.join(part for part in (base, suffix) if part)
                    break
            else:
                if current not in self.entries or self.entries[current] == 'symlink':
                    raise ValueError('Dangling framework alias')
                return current
        raise ValueError('Framework alias resolution exceeds its hop bound')

    def finish(self):
        # Every alias graph is admitted before the first filesystem link exists.
        resolved = {}
        for name in self.links:
            self.checkpoint()
            target = self.resolve(name)
            if self.entries[target] == 'directory' and (not target or name.startswith(target + '/')):
                raise ValueError('Recursive framework directory alias')
            resolved[name] = target
        for name, (relative, _) in self.links.items():
            destination = self.root.joinpath(*PurePosixPath(name).parts)
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            destination.symlink_to(relative)
        canonical_root = self.root.resolve(strict=True)
        for name in resolved:
            target = (self.root / name).resolve(strict=True)
            if not target.is_relative_to(canonical_root):
                raise ValueError('Framework alias changed outside the staged prefix')
        return {'members': self.count, 'expanded_bytes': self.expanded, 'aliases': len(self.links)}


class LibArchive:
    """Read data only through OS libarchive; no archive_write_disk/extractall calls."""
    def __init__(self):
        library = '/usr/lib/libarchive.2.dylib' if platform.system() == 'Darwin' else ctypes.util.find_library('archive')
        if not library:
            raise ValueError('The selected OS libarchive reader is unavailable')
        self.api = ctypes.CDLL(library)
        pointer = ctypes.c_void_p
        definitions = {
            'archive_read_new': (pointer, []),
            'archive_read_support_filter_all': (ctypes.c_int, [pointer]),
            'archive_read_support_format_7zip': (ctypes.c_int, [pointer]),
            'archive_read_support_format_tar': (ctypes.c_int, [pointer]),
            'archive_read_open_filename': (ctypes.c_int, [pointer, ctypes.c_char_p, ctypes.c_size_t]),
            'archive_read_next_header': (ctypes.c_int, [pointer, ctypes.POINTER(pointer)]),
            'archive_entry_pathname': (ctypes.c_char_p, [pointer]),
            'archive_entry_symlink': (ctypes.c_char_p, [pointer]),
            'archive_entry_hardlink': (ctypes.c_char_p, [pointer]),
            'archive_entry_mode': (ctypes.c_uint, [pointer]),
            'archive_entry_size': (ctypes.c_longlong, [pointer]),
            'archive_read_data': (ctypes.c_ssize_t, [pointer, pointer, ctypes.c_size_t]),
            'archive_read_free': (ctypes.c_int, [pointer]),
            'archive_version_string': (ctypes.c_char_p, []),
        }
        for name, (result, arguments) in definitions.items():
            function = getattr(self.api, name)
            function.restype = result
            function.argtypes = arguments
        self.version = self.api.archive_version_string().decode('utf-8', errors='strict')

    @contextmanager
    def entries(self, path):
        reader = self.api.archive_read_new()
        if not reader:
            raise ValueError('Cannot allocate archive reader')
        try:
            if (self.api.archive_read_support_filter_all(reader) != 0
                    or self.api.archive_read_support_format_7zip(reader) != 0
                    or self.api.archive_read_support_format_tar(reader) != 0
                    or self.api.archive_read_open_filename(reader, os.fsencode(path), 1 << 20) != 0):
                raise ValueError('Cannot open the selected archive with the OS reader')
            def read(size):
                buffer = ctypes.create_string_buffer(size)
                amount = self.api.archive_read_data(reader, buffer, size)
                if amount < 0:
                    raise ValueError('Archive member decoder failed')
                return buffer.raw[:amount]
            def iterate():
                while True:
                    entry = ctypes.c_void_p()
                    status = self.api.archive_read_next_header(reader, ctypes.byref(entry))
                    if status == 1:
                        return
                    if status != 0 or not entry:
                        raise ValueError('Archive member metadata decoder failed')
                    raw_name = self.api.archive_entry_pathname(entry)
                    if not raw_name or len(raw_name) > MAX_PATH:
                        raise ValueError('Archive member path exceeds its bound')
                    name = raw_name.decode('utf-8', errors='strict')
                    mode = self.api.archive_entry_mode(entry)
                    link = self.api.archive_entry_symlink(entry)
                    hard = self.api.archive_entry_hardlink(entry)
                    kind = 'special'
                    if hard:
                        kind = 'hardlink'
                    elif link:
                        kind = 'symlink'
                    elif stat.S_ISDIR(mode):
                        kind = 'directory'
                    elif stat.S_ISREG(mode):
                        kind = 'file'
                    if kind == 'directory':
                        name = name.rstrip('/')
                    if link and len(link) > MAX_PATH:
                        raise ValueError('Archive alias target exceeds its bound')
                    target = link.decode('utf-8', errors='strict') if link else ''
                    yield Member(name, kind, self.api.archive_entry_size(entry), mode, target), read
            yield iterate()
        finally:
            self.api.archive_read_free(reader)


def extract_locked(archives, output, cancelled=None):
    """archives: exact {path,sha256,size_bytes}; output is a fresh prefix."""
    if output.exists() or output.is_symlink():
        raise ValueError('Framework output must be fresh')
    for entry in archives:
        path = entry['path']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != entry['size_bytes']:
            raise ValueError('Framework input differs from its public lock')
        checksum = hashlib.sha256()
        with path.open('rb') as stream:
            while block := stream.read(1 << 20):
                checksum.update(block)
        if checksum.hexdigest() != entry['sha256']:
            raise ValueError('Framework input digest differs from its public lock')
    reader = LibArchive()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.framework-stage-', dir=output.parent) as temporary:
        staging = Path(temporary) / 'prefix'
        staging.mkdir(mode=0o700)
        tree = FrameworkTree(staging, cancelled=cancelled)
        for entry in archives:
            with reader.entries(entry['path']) as members:
                for member, read in members:
                    tree.add(member, read)
        receipt = tree.finish()
        staging.rename(output)
    return dict(receipt, reader=reader.version, native_execution=False)
