"""Admit the complete relocated bottle metadata graph without ambient lookup."""
import hashlib
import os
from pathlib import Path
import shlex
import subprocess

MAX_METADATA_BYTES = 65536


def observe(pkgconfig, arguments, environment):
    result = subprocess.run([str(pkgconfig), *arguments], env=environment, capture_output=True,
                            text=True, timeout=15)
    if len(result.stdout) > MAX_METADATA_BYTES or len(result.stderr) > MAX_METADATA_BYTES:
        raise ValueError('Private pkg-config observation exceeds its bound')
    if result.returncode:
        raise ValueError('Private pkg-config graph failed: ' + result.stderr.strip())
    return result.stdout.strip()


def admit_gio_graph(pkgconfig, environment, cellar):
    cellar = Path(cellar).resolve(strict=True)
    executable = Path(pkgconfig)
    if not executable.is_absolute() or not executable.is_file():
        raise ValueError('Select the absolute private pkg-config executable')
    if environment.get('PKG_CONFIG_PATH') != '' or environment.get('PKG_CONFIG_SYSROOT_DIR') != '':
        raise ValueError('Private pkg-config graph must disable ambient paths and sysroot')
    directories = environment.get('PKG_CONFIG_LIBDIR', '').split(os.pathsep)
    if not 1 <= len(directories) <= 16 or len(set(directories)) != len(directories):
        raise ValueError('Private pkg-config directories are missing or duplicated')
    metadata = []
    for directory in directories:
        path = Path(directory)
        if not path.is_absolute() or not path.resolve().is_relative_to(cellar):
            raise ValueError('Private pkg-config directory escapes the admitted Cellar')
        # Packages without .pc files are valid runtime dependencies. Missing
        # required graph nodes are rejected by the real pkg-config invocation.
        if not path.exists():
            continue
        for source in sorted(path.glob('*.pc')):
            if source.is_symlink() or not source.is_file() or not source.resolve().is_relative_to(cellar):
                raise ValueError('Private pkg-config metadata escapes its admitted owner')
            if not 0 < source.stat().st_size <= MAX_METADATA_BYTES or len(metadata) >= 128:
                raise ValueError('Private pkg-config metadata exceeds its bound')
            with source.open('rb') as stream:
                data = stream.read(MAX_METADATA_BYTES + 1)
            if len(data) > MAX_METADATA_BYTES or b'\0' in data:
                raise ValueError('Private pkg-config metadata is malformed')
            content = data.decode('utf-8')
            if '@@HOMEBREW' in content or '/opt/homebrew' in content:
                raise ValueError('Private pkg-config metadata retains an ambient bottle prefix')
            metadata.append({'path': source.relative_to(cellar).as_posix(),
                             'sha256': hashlib.sha256(data).hexdigest(), 'size_bytes': len(data)})
    version = observe(executable, ['--modversion', 'gio-unix-2.0'], environment)
    if version != '2.90.1':
        raise ValueError('Private GIO pkg-config version differs from pinned2.90.1')
    flags = observe(executable, ['--cflags', '--libs', 'gio-unix-2.0'], environment)
    for flag in shlex.split(flags):
        if flag.startswith(('-I', '-L')):
            path = Path(flag[2:])
            if not path.is_absolute() or not path.is_dir() or not path.resolve().is_relative_to(cellar):
                raise ValueError('Private GIO flags escape the admitted Cellar: ' + flag)
    return {'module': 'gio-unix-2.0', 'version': version,
            'flags_sha256': hashlib.sha256(flags.encode()).hexdigest(), 'metadata': metadata}
