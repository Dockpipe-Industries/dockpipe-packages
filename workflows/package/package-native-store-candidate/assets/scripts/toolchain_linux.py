"""Prepare an isolated tool prefix from exact recipe bytes; never install system packages."""
import argparse
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import zipfile

import toolchain_acquire as acquire

MAX_EXTRACTED = 768 << 20
MAX_FILES = 40000


def relative_name(name):
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or '\\' in name:
        raise ValueError('Archive path escapes its destination')
    return path


def extract(entry, cache, destination):
    archive_path = acquire.acquire(entry, cache)
    destination.mkdir(parents=True, exist_ok=False)
    root = entry['root']
    total = 0
    count = 0
    if entry['format'] == 'zip':
        with zipfile.ZipFile(archive_path) as archive:
            for item in archive.infolist():
                name = relative_name(item.filename)
                if item.is_dir() or item.file_size > MAX_EXTRACTED:
                    raise ValueError('Unexpected tool ZIP member')
                total += item.file_size
                count += 1
                if total > MAX_EXTRACTED or count > MAX_FILES:
                    raise ValueError('Archive exceeds extraction capacity')
                target = destination.joinpath(*name.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(item) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755)
        return
    with tarfile.open(archive_path) as archive:
        for item in archive:
            name = relative_name(item.name)
            if not name.parts or name.parts[0] != root:
                raise ValueError('Archive root differs from recipe')
            name = PurePosixPath(*name.parts[1:])
            if not name.parts:
                continue
            count += 1
            if item.size < 0:
                raise ValueError('Archive member has invalid size')
            total += item.size
            if total > MAX_EXTRACTED or count > MAX_FILES:
                raise ValueError('Archive exceeds extraction capacity')
            target = destination.joinpath(*name.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            if item.isdir():
                target.mkdir(exist_ok=True)
            elif item.isfile():
                with archive.extractfile(item) as source, target.open('xb') as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if item.mode & 0o111 else 0o644)
                os.utime(target, (item.mtime, item.mtime))
            else:
                raise ValueError('Unsupported tool archive member')


def prepare(recipe, cache, prefix):
    if prefix.exists():
        raise ValueError('Toolchain prefix must be fresh')
    prefix.mkdir(parents=True)
    entries = json.loads(recipe.read_text())
    for entry in entries:
        extract(entry, cache, prefix / entry['destination'])
        if 'license_asset' in entry:
            license_source = acquire.acquire(entry['license_asset'], cache)
            license_directory = prefix / 'licenses' / entry['id']
            license_directory.mkdir(parents=True)
            shutil.copyfile(license_source, license_directory / 'COPYING')
    binary_directory = prefix / 'bin'
    binary_directory.mkdir()
    for name, target in {'go': '../go/bin/go', 'gofmt': '../go/bin/gofmt',
                         'cmake': '../cmake/bin/cmake', 'ctest': '../cmake/bin/ctest',
                         'ninja': '../ninja/ninja'}.items():
        (binary_directory / name).symlink_to(target)
    compiler = prefix / 'compiler'
    compiler.mkdir()
    compiler_recipe = recipe.with_name('gcc-profile-acquisitions.json')
    for entry in json.loads(compiler_recipe.read_text()):
        package = acquire.acquire(entry, cache)
        subprocess.run(['dpkg-deb', '--extract', str(package), str(compiler)], check=True, timeout=30)
    for name in ('gcc', 'g++'):
        (binary_directory / name).symlink_to('../compiler/usr/bin/' + name + '-11')
    environment = dict(os.environ, CC=str(binary_directory / 'gcc'), CFLAGS='-O2', MAKEFLAGS='-j2')
    for name in ('GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LIBRARY_PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH'):
        environment.pop(name, None)
    source = prefix / 'pkgconf-source'
    with (prefix / 'pkgconf-build.log').open('w') as log:
        subprocess.run([str(source / 'configure'), '--prefix=' + str(prefix / 'pkgconf'),
                        '--disable-shared', '--enable-static', '--with-pkg-config-dir=',
                        '--with-system-libdir=', '--with-system-includedir='],
                       cwd=source, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        subprocess.run(['/usr/bin/make', '-j2'], cwd=source, env=environment,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        subprocess.run(['/usr/bin/make', 'install'], cwd=source, env=environment,
                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    (binary_directory / 'pkg-config').symlink_to('../pkgconf/bin/pkgconf')
    return prefix


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recipe', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--prefix', type=Path, required=True)
    arguments = parser.parse_args()
    prepare(arguments.recipe, arguments.cache, arguments.prefix)
