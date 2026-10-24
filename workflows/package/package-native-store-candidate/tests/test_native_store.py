import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parent.parent / 'assets/scripts'
sys.path.insert(0, str(SCRIPTS))
import native_store
import publication_plan


class NativeStoreTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.commit = 'a' * 40

    def asset(self, name='input.tar.gz', format='tar.gz'):
        return {'url': 'https://github.com/Dockpipe-Industries/dockpipe/releases/download/v1/' + name,
                'sha256': 'b' * 64, 'size_bytes': 10, 'format': format, 'root': ''}

    def lock(self):
        inputs = {role: self.asset() for role in native_store.ROLES}
        inputs['source']['url'] = 'https://github.com/' + native_store.REPOSITORY + '/archive/' + self.commit + '.tar.gz'
        return {'schema': 1, 'source_commit': self.commit, 'accepted_platforms': ['linux-amd64'], 'owned': [['resolver', 'qemu']],
                'profiles': {'linux-amd64': {'cli_version': '1.0.0-staging.cli.shaabc',
                                            'inputs': inputs, 'previous_artifacts': {}, 'guest_source_commit': self.commit}}}

    def save_lock(self, document):
        path = self.root / 'lock.json'
        path.write_text(json.dumps(document))
        return path

    def store(self, kind, name='qemu', content=b'compiled-package'):
        directory = self.root / (kind + '-store')
        directory.mkdir()
        filename = 'dockpipe-' + kind + '-' + name + '-2.0.0.tar.gz'
        (directory / filename).write_bytes(content)
        entry = {'name': name, 'version': '2.0.0', 'tarball': filename,
                 'size_bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest()}
        (directory / 'packages-store-manifest.json').write_text(json.dumps(
            {'schema': 1, 'platform': 'linux-amd64', 'packages': {kind + 's': [entry]}}))
        return directory, entry

    def test_complete_immutable_profile(self):
        with patch.dict(os.environ, {}, clear=True):
            lock, profile = native_store.load_lock(self.save_lock(self.lock()), 'linux-amd64')
        self.assertEqual(lock['source_commit'], self.commit)
        self.assertEqual(set(profile['inputs']), native_store.ROLES)

    def test_missing_guest_broker_qt_or_sdk_fails_before_build(self):
        for role in ('guest_tools', 'guest_descriptor', 'broker', 'qt', 'display_sdk'):
            with self.subTest(role=role):
                document = self.lock()
                del document['profiles']['linux-amd64']['inputs'][role]
                with self.assertRaises(ValueError):
                    native_store.load_lock(self.save_lock(document), 'linux-amd64')

    def test_no_unqualified_platform_claims(self):
        for target in ('linux-arm64', 'darwin-arm64', 'windows-arm64'):
            with self.subTest(target=target), self.assertRaises(ValueError):
                native_store.load_lock(self.save_lock(self.lock()), target)

    def test_mutable_or_private_urls_and_missing_hash_fail(self):
        for url in ('http://github.com/Example/Producer/releases/download/v1/a',
                    'https://github.com/Example/Producer/releases/latest/download/a',
                    'https://token@github.com/Example/Producer/releases/download/v1/a',
                    'https://github.com/Dockpipe-Industries/dockpipe/releases/download/v1/a?token=x'):
            asset = self.asset()
            asset['url'] = url
            with self.subTest(url=url), self.assertRaises(ValueError):
                native_store.validate_asset(asset)
        asset = self.asset()
        asset['sha256'] = None
        with self.assertRaises(ValueError):
            native_store.validate_asset(asset)

    def test_cache_bytes_must_match_public_lock(self):
        asset = self.asset()
        cache = self.root / 'cache'
        cache.mkdir()
        (cache / asset['sha256']).write_bytes(b'corrupt')
        with self.assertRaises(ValueError):
            native_store.acquire(asset, cache)
        with self.assertRaises(ValueError):
            native_store.acquire(asset, self.root / 'missing')

    def tar(self, names):
        path = self.root / 'input.tar.gz'
        with tarfile.open(path, 'w:gz') as archive:
            for name, link in names:
                entry = tarfile.TarInfo(name)
                if link:
                    entry.type = tarfile.SYMTYPE
                    entry.linkname = link
                    archive.addfile(entry)
                else:
                    entry.size = 3
                    entry.mode = 0o755
                    archive.addfile(entry, io.BytesIO(b'abc'))
        return path

    def test_regular_archive_extraction_preserves_executable(self):
        archive = self.tar([('kit/bin/tool', None)])
        asset = self.asset()
        asset['root'] = 'kit'
        root = native_store.extract(asset, archive, self.root / 'extract')
        self.assertEqual((root / 'bin/tool').read_bytes(), b'abc')
        if os.name != 'nt':
            self.assertTrue((root / 'bin/tool').stat().st_mode & 0o111)

    def test_archive_traversal_links_and_case_collisions_fail(self):
        for entries in ([('../escape', None)], [('link', '/outside')], [('A', None), ('a', None)]):
            with self.subTest(entries=entries):
                archive = self.tar(entries)
                destination = self.root / ('extract-' + str(len(list(self.root.iterdir()))))
                with self.assertRaises(ValueError):
                    native_store.extract(self.asset(), archive, destination)

    def test_exact_noncore_ownership_and_deterministic_flat_archive(self):
        directory, entry = self.store('resolver')
        flat = self.root / 'flat'
        manifest = native_store.combine({'resolver': directory}, [['resolver', 'qemu']], 'linux-amd64', flat, {})
        self.assertEqual(manifest['packages']['resolvers'], [entry])
        first, second = self.root / 'first.tar.gz', self.root / 'second.tar.gz'
        native_store.archive_flat(flat, first)
        native_store.archive_flat(flat, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        with tarfile.open(first) as archive:
            self.assertEqual(set(archive.getnames()), {'packages-store-manifest.json', entry['tarball']})

    def test_changed_immutable_name_requires_package_version_bump(self):
        directory, entry = self.store('resolver')
        with self.assertRaisesRegex(ValueError, 'new package version'):
            native_store.combine({'resolver': directory}, [['resolver', 'qemu']], 'linux-amd64',
                                 self.root / 'flat', {entry['tarball']: '0' * 64})

    def test_missing_owned_artifact_and_core_fail(self):
        directory, entry = self.store('resolver')
        with self.assertRaises(ValueError):
            native_store.combine({'resolver': directory}, [['resolver', 'missing']], 'linux-amd64', self.root / 'missing', {})
        path = directory / 'packages-store-manifest.json'
        document = json.loads(path.read_text())
        document['packages']['core'] = entry
        path.write_text(json.dumps(document))
        with self.assertRaises(ValueError):
            native_store.combine({'resolver': directory}, [['resolver', 'qemu']], 'linux-amd64', self.root / 'core', {})

    def test_runtime_core_requires_separately_bootstrapped_exact_archive(self):
        profile = self.lock()['profiles']['linux-amd64']
        data = b'verified-compiled-runtime-core'
        name = 'dockpipe-core-0.6.25.tar.gz'
        profile['inputs']['runtime_core'] = self.asset(name, 'file')
        profile['inputs']['runtime_core'].update(sha256=hashlib.sha256(data).hexdigest(), size_bytes=len(data))
        global_root = self.root / 'global'
        directory = global_root / 'packages/core'
        directory.mkdir(parents=True)
        archive = directory / name
        archive.write_bytes(data)
        self.assertEqual(native_store.require_runtime_core(profile, str(global_root)), global_root)
        archive.write_bytes(b'wrong')
        with self.assertRaises(ValueError):
            native_store.require_runtime_core(profile, str(global_root))

    def test_runtime_core_cannot_fall_back_to_global_default_or_competing_archive(self):
        profile = self.lock()['profiles']['linux-amd64']
        with self.assertRaises(ValueError):
            native_store.require_runtime_core(profile, '')
        data = b'core'
        profile['inputs']['runtime_core'] = self.asset('dockpipe-core-1.0.0.tar.gz', 'file')
        profile['inputs']['runtime_core'].update(sha256=hashlib.sha256(data).hexdigest(), size_bytes=len(data))
        global_root = self.root / 'global'
        directory = global_root / 'packages/core'
        directory.mkdir(parents=True)
        (directory / 'dockpipe-core-1.0.0.tar.gz').write_bytes(data)
        (directory / 'dockpipe-core-2.0.0.tar.gz').write_bytes(data)
        with self.assertRaises(ValueError):
            native_store.require_runtime_core(profile, str(global_root))

    def test_producer_orchestrates_owning_public_hook_and_noncore_store_exports(self):
        self.assert_producer_orchestration()

    def test_recipe_generator_selection_reaches_the_actual_owning_source_hook(self):
        self.assert_producer_orchestration({'CMAKE_GENERATOR': 'Ninja'})

    def test_declared_opengl_prefix_reaches_actual_owning_source_hook(self):
        self.assert_producer_orchestration({
            'CMAKE_GENERATOR': 'Ninja',
            'CMAKE_PREFIX_PATH': '/declared/native-inputs/gio/usr',
        })

    def assert_producer_orchestration(self, recipe_environment=None):
        document = self.lock()
        document['owned'] = [list(identity) for identity in sorted(native_store.VM_FAMILY)]
        profile = document['profiles']['linux-amd64']
        inputs = {}
        file_roles = {'broker_provenance', 'go_sdk_archive', 'go_sdk_manifest',
                      'guest_tools', 'guest_descriptor', 'guest_release_manifest'}
        for role in native_store.ROLES - {'runtime_core'}:
            path = self.root / ('input-' + role)
            if role in file_roles:
                path.write_bytes(b'input')
            else:
                path.mkdir()
            inputs[role] = path
        if recipe_environment is not None:
            source = Path(__file__).resolve().parents[4]
            recipes = json.loads((source / 'release/native-input-recipes.json').read_bytes())
            profile['native_inputs'] = recipes['linux-amd64']
            for role in native_store.native_inputs.DERIVED_ROLES:
                del profile['inputs'][role]
        inputs['guest_release_manifest'].write_text('{}')
        (inputs['cli'] / 'dockpipe').write_bytes(b'fixture CLI')
        source = inputs['source']
        (source / 'packages/vm').mkdir(parents=True)
        (source / 'packages/vm/package.yml').write_text('build:\n  source:\n    script: assets/scripts/build-source.py\n')
        (source / 'release').mkdir()
        sdk_hash = hashlib.sha256(b'input').hexdigest()
        (source / 'release/dependencies.lock.json').write_text(json.dumps({
            'dockpipe_go_sdk': {'archive_sha256': sdk_hash, 'manifest_sha256': sdk_hash},
            'dockpipe_runtime_core': profile['inputs']['runtime_core'],
        }))
        commands = []
        def invoke(arguments, repository, environment):
            commands.append([str(item) for item in arguments])
            self.assertEqual(environment['DOCKPIPE_DISPLAY_SDK'], 'display')
            for key, value in (recipe_environment or {}).items():
                self.assertEqual(environment[key], value)
            if arguments[1:4] != ['package', 'build', 'store']:
                return
            destination = Path(arguments[arguments.index('--out') + 1])
            destination.mkdir()
            group = arguments[arguments.index('--only') + 1]
            entries = []
            for kind, name in sorted(native_store.VM_FAMILY):
                if kind + 's' != group:
                    continue
                artifact_name = 'dockpipe-' + kind + '-' + name + '-1.3.6.tar.gz'
                (destination / artifact_name).write_bytes(b'compiled fixture')
                entries.append(dict(name=name, version='1.3.6', min_dockpipe_version='0.6.26-0',
                                    tarball=artifact_name, size_bytes=16,
                                    sha256=hashlib.sha256(b'compiled fixture').hexdigest()))
            (destination / 'packages-store-manifest.json').write_text(json.dumps(
                {'schema': 1, 'platform': 'linux-amd64', 'packages': {group: entries}}))
        extracted = [inputs[role] for role in profile['inputs'] if role != 'runtime_core']
        with (
            patch.dict(os.environ, {}, clear=True),
            patch.object(native_store.platform, 'system', return_value='Linux'),
            patch.object(native_store.platform, 'machine', return_value='x86_64'),
        ):
            with (
                patch.object(native_store, 'require_runtime_core', return_value=self.root / 'global'),
                patch.object(native_store, 'acquire', return_value=self.root / 'cached'),
                patch.object(native_store, 'extract', side_effect=extracted),
                patch.object(native_store, 'validate_guest_release'),
                patch.object(native_store.subprocess, 'check_output', return_value=profile['cli_version']),
                patch.object(native_store.native_inputs, 'prepare', return_value=(
                    {role: inputs[role] for role in native_store.native_inputs.DERIVED_ROLES},
                    recipe_environment or {})),
                patch.object(native_store, 'run', side_effect=invoke),
            ):
                output = self.root / 'produced'
                native_store.build(self.save_lock(document), 'linux-amd64', self.root / 'cache', self.root / 'work', output)
        self.assertTrue(commands[0][1].endswith('/tooling/catalog_go_dependencies.py'))
        self.assertEqual(commands[0][-2:], ['--target', 'linux-amd64'])
        self.assertEqual(commands[1][1:4], ['package', 'build', 'source'])
        self.assertIn('vm', commands[1])
        self.assertEqual({command[-1] for command in commands if command[1:4] == ['package', 'build', 'store']}, {'workflows', 'resolvers'})
        self.assertTrue(all(command[0].endswith('/dockpipe') for command in commands[1:]))
        planned = publication_plan.plan([output / 'candidate.json'], qualification='native')
        self.assertEqual(planned['owned'], document['owned'])

    def guest_release(self):
        profile = self.lock()['profiles']['linux-amd64']
        tag = 'guest-tools-candidate-' + self.commit
        prefix = 'https://github.com/' + native_store.REPOSITORY + '/releases/download/' + tag + '/'
        names = {'guest_tools': 'dockpipe-guest-tools-1.0.0-windows-amd64.zip',
                 'guest_descriptor': 'guest-tools-release.json',
                 'guest_source_inventory': 'guest-tools-source-inventory.json',
                 'guest_native_provenance': 'guest-tools-native-provenance.json',
                 'guest_toolchain_lock': 'guest-tools-toolchain.lock.json'}
        artifacts = {role: dict(name=name, url=prefix+name, sha256='b'*64, size_bytes=10)
                     for role, name in names.items()}
        for role in ('guest_tools', 'guest_descriptor'):
            profile['inputs'][role] = {key: artifacts[role][key] for key in ('url', 'sha256', 'size_bytes')}
        profile['inputs']['guest_release_manifest']['url'] = prefix + 'guest-tools-candidate-manifest.json'
        manifest = {'schema': 1, 'product': 'DockpipeGuestToolsCandidate', 'artifact_class': 'guest-tools',
                    'version': '1.0.0', 'platform': 'windows-amd64',
                    'source_repository': native_store.REPOSITORY, 'source_sha': self.commit,
                    'source_state': 'committed-snapshot', 'tag': tag,
                    'checks': ['native-windows-build', 'native-windows-ctest', 'installer-integrity'],
                    'toolchain': {'qt_version': '6.10.3', 'lock_sha256': 'b'*64, 'compiler_id': 'GNU',
                                  'compiler_version': '13.1.0', 'cmake_version': '3.30.5',
                                  'ninja_version': '1.12.1'}, 'artifacts': artifacts}
        return manifest, profile

    def test_guest_tools_requires_public_qualified_windows_manifest(self):
        manifest, profile = self.guest_release()
        native_store.validate_guest_release(manifest, profile)
        manifest['source_state'] = 'development'
        with self.assertRaises(ValueError):
            native_store.validate_guest_release(manifest, profile)

    def test_guest_archive_and_descriptor_must_match_qualification_pin(self):
        for role in ('guest_tools', 'guest_descriptor'):
            manifest, profile = self.guest_release()
            profile['inputs'][role]['sha256'] = 'c'*64
            with self.subTest(role=role), self.assertRaises(ValueError):
                native_store.validate_guest_release(manifest, profile)

    def test_guest_component_reuse_needs_independent_source_identity(self):
        manifest, profile = self.guest_release()
        profile['guest_source_commit'] = 'd'*40
        with self.assertRaises(ValueError):
            native_store.validate_guest_release(manifest, profile)

    def candidate(self):
        directory, entry = self.store('resolver')
        flat = self.root / 'flat'
        native_store.combine({'resolver': directory}, [['resolver', 'qemu']], 'linux-amd64', flat, {})
        output = self.root / 'output'
        output.mkdir()
        archive = output / 'dockpipe-catalog-linux-amd64.tar.gz'
        native_store.archive_flat(flat, archive)
        descriptor = {'schema': 1, 'source_commit': self.commit, 'owned': [['resolver', 'qemu']],
                      'platform': 'linux-amd64', 'repository': native_store.REPOSITORY,
                      'tag': 'catalog-native-candidate-' + self.commit, 'asset': archive.name,
                      'sha256': native_store.sha256(archive), 'size_bytes': archive.stat().st_size,
                      'manifest_sha256': native_store.sha256(flat / 'packages-store-manifest.json'),
                      'publication_mode': 'native', 'native_build': True, 'published': False}
        path = output / 'candidate.json'
        path.write_text(json.dumps(descriptor))
        return path, archive

    def test_publication_plan_binds_actual_platform_and_exact_core_lock_wire(self):
        candidate, archive = self.candidate()
        result = publication_plan.plan([candidate], qualification='native')
        self.assertEqual(result['tag'], 'catalog-native-candidate-' + self.commit)
        self.assertEqual(set(result['artifacts']), {'linux-amd64'})
        artifact = result['core_lock_overlay']['artifacts']['linux-amd64']
        self.assertEqual(artifact['sha256'], native_store.sha256(archive))
        self.assertTrue(artifact['url'].endswith('/' + archive.name))

    def test_publication_plan_rejects_corrupt_artifact_and_duplicate_platform(self):
        candidate, archive = self.candidate()
        with self.assertRaises(ValueError):
            publication_plan.plan([candidate, candidate], qualification='native')
        archive.write_bytes(b'changed')
        with self.assertRaises(ValueError):
            publication_plan.plan([candidate], qualification='native')


if __name__ == '__main__':
    unittest.main()
