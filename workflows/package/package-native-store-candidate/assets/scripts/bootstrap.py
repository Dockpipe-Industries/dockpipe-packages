"""Acquire only the locked bootstrap CLI, then enter the owning Dockpipe workflow."""
import os
import shutil
from pathlib import Path
import subprocess
import native_store
from candidate import location


if __name__ == '__main__':
    lock = location('DOCKPIPE_NATIVE_LOCK')
    target = os.environ['DOCKPIPE_NATIVE_PLATFORM']
    document, profile = native_store.load_lock(lock, target)
    source_archive = location('DOCKPIPE_NATIVE_SOURCE_ARCHIVE')
    source_pin = profile['inputs']['source']
    if (source_archive.is_symlink() or not source_archive.is_file()
            or source_archive.stat().st_size != source_pin['size_bytes']
            or native_store.sha256(source_archive) != source_pin['sha256']):
        raise ValueError('Shared CI source archive differs from the effective source descriptor')
    cache = location('DOCKPIPE_NATIVE_CACHE')
    cache.mkdir(parents=True, exist_ok=True)
    cached_source = cache / source_pin['sha256']
    if not cached_source.exists():
        shutil.copyfile(source_archive, cached_source)
    root = location('DOCKPIPE_NATIVE_WORK').with_name('native-cli-bootstrap')
    root.mkdir(exist_ok=False)
    asset = profile['inputs']['cli']
    cached = native_store.acquire(asset, location('DOCKPIPE_NATIVE_CACHE'),
                                  os.environ.get('DOCKPIPE_NATIVE_FETCH') == '1')
    installed = native_store.extract(asset, cached, root / 'cli')
    binary = installed / ('dockpipe.exe' if target.startswith('windows-') else 'dockpipe')
    if binary.is_symlink() or not binary.is_file():
        raise ValueError('Locked bootstrap archive lacks the native CLI')
    binary.chmod(0o755)
    environment = dict(os.environ, DOCKPIPE_BIN=str(binary),
                       DOCKPIPE_HOST_PYTHON_BIN=os.path.abspath(os.sys.executable))
    repository = Path(__file__).resolve().parents[5]
    workflow = Path(__file__).resolve().parents[2] / 'config.yml'
    subprocess.run([str(binary), '--no-compile-deps', '--no-vault', '--no-op-inject',
                    '--workdir', str(repository), '--workflow-file', str(workflow)],
                   env=environment, check=True, timeout=3600)
