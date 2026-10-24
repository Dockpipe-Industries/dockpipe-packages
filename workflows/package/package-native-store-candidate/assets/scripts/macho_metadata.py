"""Bounded read-only Mach-O architecture, deployment target and install-name inspection."""
from pathlib import Path
import struct

MAX_LOAD_COMMANDS = 8192
MAX_COMMAND_BYTES = 4 << 20


def version(value):
    return [value >> 16, (value >> 8) & 255, value & 255]


def inspect(path):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or not 32 <= path.stat().st_size <= 256 << 20:
        raise ValueError('Mach-O input must be a bounded regular file')
    size = path.stat().st_size
    with path.open('rb') as stream:
        magic = stream.read(4)
        slices = [(0, size)]
        fat = {b'\xca\xfe\xba\xbe': ('>', False), b'\xbe\xba\xfe\xca': ('<', False),
               b'\xca\xfe\xba\xbf': ('>', True), b'\xbf\xba\xfe\xca': ('<', True)}
        if magic in fat:
            order, large = fat[magic]
            count = struct.unpack(order + 'I', stream.read(4))[0]
            if not 1 <= count <= 8:
                raise ValueError('Mach-O slice count exceeds its bound')
            slices = []
            width = 32 if large else 20
            for _ in range(count):
                record = stream.read(width)
                if len(record) != width:
                    raise ValueError('Truncated Mach-O fat header')
                if large:
                    _, _, offset, length, _, _ = struct.unpack(order + 'IIQQII', record)
                else:
                    _, _, offset, length, _ = struct.unpack(order + 'IIIII', record)
                if offset < 8 + count * width or length < 32 or offset + length > size:
                    raise ValueError('Mach-O slice escapes its input')
                if any(offset < old + span and old < offset + length for old, span in slices):
                    raise ValueError('Mach-O slices overlap')
                slices.append((offset, length))
        results = []
        for offset, length in slices:
            stream.seek(offset)
            header = stream.read(32)
            if header[:4] != b'\xcf\xfa\xed\xfe':
                raise ValueError('Expected a little-endian 64-bit Mach-O slice')
            _, cpu, _, file_type, count, commands_size, _, _ = struct.unpack('<IIIIIIII', header)
            if count > MAX_LOAD_COMMANDS or commands_size > MAX_COMMAND_BYTES or 32 + commands_size > length:
                raise ValueError('Mach-O command table exceeds its bound')
            commands = stream.read(commands_size)
            cursor = 0
            minimum = None
            platform = None
            identity = None
            sdk = None
            dependencies = []
            for _ in range(count):
                if cursor + 8 > len(commands):
                    raise ValueError('Truncated Mach-O command')
                command, span = struct.unpack_from('<II', commands, cursor)
                if span < 8 or span % 4 or cursor + span > len(commands):
                    raise ValueError('Mach-O command size escapes its table')
                if command == 0x32:
                    if span < 24 or minimum is not None:
                        raise ValueError('Invalid duplicate Mach-O build version')
                    platform, encoded = struct.unpack_from('<II', commands, cursor + 8)
                    minimum = version(encoded)
                    sdk = version(struct.unpack_from('<I', commands, cursor + 16)[0])
                elif command == 0x24:
                    if span < 16 or minimum is not None:
                        raise ValueError('Invalid duplicate Mach-O deployment version')
                    platform = 1
                    minimum = version(struct.unpack_from('<I', commands, cursor + 8)[0])
                    sdk = version(struct.unpack_from('<I', commands, cursor + 12)[0])
                elif command in {0xC, 0xD, 0x80000018, 0x8000001F, 0x80000023}:
                    if span < 24:
                        raise ValueError('Truncated Mach-O dylib command')
                    name_offset = struct.unpack_from('<I', commands, cursor + 8)[0]
                    if not 24 <= name_offset < span:
                        raise ValueError('Invalid Mach-O install-name offset')
                    raw = commands[cursor + name_offset:cursor + span]
                    end = raw.find(b'\0')
                    if not 0 <= end <= 4096:
                        raise ValueError('Unbounded Mach-O install name')
                    name = raw[:end].decode('utf-8', errors='strict')
                    if any(ord(character) < 32 for character in name):
                        raise ValueError('Invalid Mach-O install-name text')
                    if command == 0xD:
                        identity = name
                    else:
                        dependencies.append(name)
                cursor += span
            if cursor != len(commands) or minimum is None or platform != 1:
                raise ValueError('Mach-O does not establish one bounded macOS deployment target')
            results.append({'architecture': {0x0100000C: 'arm64', 0x01000007: 'amd64'}.get(cpu, 'unsupported'),
                            'file_type': file_type, 'minimum_os': minimum, 'built_sdk': sdk, 'install_name': identity,
                            'dependencies': dependencies})
        return results
