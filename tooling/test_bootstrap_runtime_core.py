import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch


specification = importlib.util.spec_from_file_location(
    'runtime_core', Path(__file__).with_name('bootstrap_runtime_core.py'))
runtime_core = importlib.util.module_from_spec(specification)
specification.loader.exec_module(runtime_core)


class RuntimeCoreInstallationTests(unittest.TestCase):
    def fixture(self, root, members=None):
        archive = root / 'input.tar.gz'
        with tarfile.open(archive, 'w:gz') as bundle:
            for name, content, kind in members or [
                    ('core/package.yml', b'name: dockpipe.core\nkind: core\n', tarfile.REGTYPE),
                    ('core/runtimes/host/profile', b'host', tarfile.REGTYPE)]:
                entry = tarfile.TarInfo(name)
                entry.type = kind
                entry.size = len(content) if kind == tarfile.REGTYPE else 0
                bundle.addfile(entry, io.BytesIO(content))
        pin = {
            'version': '0.6.25',
            'release': 'v0.6.25-staging.39554459680.1.2a984e78d9da',
            'url': ('https://github.com/Dockpipe-Industries/dockpipe/releases/download/'
                    'v0.6.25-staging.39554459680.1.2a984e78d9da/dockpipe-core-0.6.25.tar.gz'),
            'size_bytes': archive.stat().st_size,
            'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        }
        (root / 'release').mkdir(exist_ok=True)
        lock = root / 'release/dependencies.lock.json'
        lock.write_text(json.dumps({'schema': 1, 'dockpipe_runtime_core': pin}))
        return archive, pin, lock

    def test_verified_archive_is_installed_without_extracting_runtime_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, pin, _ = self.fixture(root)
            output = root / 'task global'
            environment = root / 'github.env'
            with patch.object(runtime_core, 'ROOT', root), patch.dict(
                    runtime_core.os.environ, {'GITHUB_ENV': str(environment)}):
                runtime_core.install(output, archive)
            files = [path.relative_to(output).as_posix() for path in output.rglob('*') if path.is_file()]
            self.assertEqual(files, ['packages/core/dockpipe-core-0.6.25.tar.gz'])
            self.assertEqual(hashlib.sha256((output / files[0]).read_bytes()).hexdigest(), pin['sha256'])
            self.assertEqual(environment.read_text(), 'DOCKPIPE_GLOBAL_ROOT=' + str(output) + '\n')

    def test_wrong_hash_leaves_no_partial_global_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, pin, lock = self.fixture(root)
            pin['sha256'] = '0' * 64
            lock.write_text(json.dumps({'schema': 1, 'dockpipe_runtime_core': pin}))
            output = root / 'global'
            with patch.object(runtime_core, 'ROOT', root), self.assertRaisesRegex(ValueError, 'checksum'):
                runtime_core.install(output, archive)
            self.assertFalse(output.exists())

    def test_existing_global_root_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, _, _ = self.fixture(root)
            output = root / 'global'
            output.mkdir()
            (output / 'owned.txt').write_text('preserve')
            with patch.object(runtime_core, 'ROOT', root), self.assertRaisesRegex(ValueError, 'fresh'):
                runtime_core.install(output, archive)
            self.assertEqual((output / 'owned.txt').read_text(), 'preserve')

    def test_mutable_or_other_owner_urls_fail_before_network(self):
        for mutation in ('owner', 'tag', 'platform', 'query'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                _, pin, lock = self.fixture(root)
                if mutation == 'owner':
                    pin['url'] = pin['url'].replace('/dockpipe/', '/other/')
                elif mutation == 'tag':
                    pin['release'] = 'latest'
                elif mutation == 'platform':
                    pin['url'] = pin['url'].replace('dockpipe-core-', 'dockpipe-')
                else:
                    pin['url'] += '?download=1'
                lock.write_text(json.dumps({'schema': 1, 'dockpipe_runtime_core': pin}))
                with patch.object(runtime_core, 'ROOT', root), patch.object(
                        runtime_core.subprocess, 'run') as run, self.assertRaises(ValueError):
                    runtime_core.install(root / 'global')
                run.assert_not_called()

    def test_unsafe_or_incomplete_archives_fail_without_installation(self):
        for members in [
                [('core/../escape', b'x', tarfile.REGTYPE)],
                [('core/link', b'', tarfile.SYMTYPE)],
                [('core/package.yml', b'x', tarfile.REGTYPE)],
                [('core/package.yml', b'x', tarfile.REGTYPE),
                 ('core/package.yml', b'y', tarfile.REGTYPE)]]:
            with self.subTest(members=members), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                archive, _, _ = self.fixture(root, members)
                with patch.object(runtime_core, 'ROOT', root), self.assertRaises(ValueError):
                    runtime_core.install(root / 'global', archive)
                self.assertFalse((root / 'global').exists())

    def test_expanded_archive_capacity_is_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, pin, _ = self.fixture(root)
            with patch.object(runtime_core, 'MAX_EXPANDED_BYTES', 1), self.assertRaisesRegex(
                    ValueError, 'expanded'):
                runtime_core.verify(archive, pin)


if __name__ == '__main__':
    unittest.main()
