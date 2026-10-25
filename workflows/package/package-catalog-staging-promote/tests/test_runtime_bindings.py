"""Bind self-contained runtime inputs and package-owned adapters without core stores."""
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'assets/scripts'))
from catalog_runtime_gate import admit_runtime, PLATFORMS, RELEASES
from catalog_transaction import PublicationStopped


class RuntimeBindingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.files = {}
        self.counter = 0
        self.sha = 'a' * 40
        self.version = '1.2.3-staging.cli.sha' + self.sha[:12]
        self.tag = 'cli-candidate-1.2.3-staging.' + self.sha
        self.script = b'#!/bin/sh\npackage-owned-adapter\n'
        self.script_hash = hashlib.sha256(self.script).hexdigest()
        self.pin = {'source_sha': 'd' * 40, 'version': '1.2.3',
                    'platforms': {platform: {'vmimage_run_sha256': self.script_hash} for platform in PLATFORMS}}
        self.pin['pointer'] = self.document('https://packages.staging.dockpipe.com/packages/runtime/latest.json',
                                           {'version': '1.2.3', 'manifest': 'packages/candidates/test/release-manifest.json'})
        self.pin['manifest'] = self.document('https://packages.staging.dockpipe.com/packages/candidates/test/release-manifest.json',
                                            {'artifact_class': 'runtime', 'source_sha': 'd' * 40,
                                             'version': '1.2.3', 'stores': {}})
        sdk_digest = 'b' * 64
        self.sdk = {'version': '0.1.0-source.' + sdk_digest[:16], 'source_commit': 'c' * 40,
                    'source_digest': sdk_digest, 'archive_sha256': 'e' * 64}
        sdk_prefix = RELEASES + 'dockpipe-go-sdk-' + self.sdk['version'] + '/dockpipe-go-sdk-' + sdk_digest[:16]
        sdk_archive = {'url': sdk_prefix + '.tar.gz', 'sha256': 'e' * 64, 'size_bytes': 100}
        sdk_manifest = self.document(sdk_prefix + '.json', {'schema': 1, 'module': 'dockpipe',
                                     'sdk_version': self.sdk['version'], 'source_commit': 'c' * 40,
                                     'source_digest': sdk_digest, 'sha256': 'e' * 64, 'bytes': 100})
        self.sdk['manifest_sha256'] = sdk_manifest['sha256']
        self.dependencies = {'schema': 1, 'dockpipe_go_sdk': self.sdk}
        artifacts = {}
        self.cli = {}
        self.source = {'profiles': {}}
        self.stores = {}
        for target in ['linux/amd64', 'linux/arm64', 'darwin/amd64', 'darwin/arm64', 'windows/amd64']:
            platform = target.replace('/', '-')
            suffix = '.zip' if target.startswith('windows/') else '.tar.gz'
            name = 'dockpipe_' + self.version + '_' + target.replace('/', '_') + suffix
            artifact = {'name': name, 'sha256': '1' * 64, 'size_bytes': 100,
                        'executable_sha256': '2' * 64, 'executable_size_bytes': 200}
            artifacts[target] = artifact
            if platform not in PLATFORMS:
                continue
            archive = {'url': RELEASES + self.tag + '/' + name, 'sha256': '1' * 64, 'size_bytes': 100}
            self.cli[platform] = {'version': self.version, 'archive': archive,
                                  'executable_sha256': '2' * 64, 'executable_size_bytes': 200}
            self.source['profiles'][platform] = {'cli_version': self.version,
                'inputs': {'cli': archive, 'go_sdk_archive': sdk_archive, 'go_sdk_manifest': sdk_manifest}}
            directory = self.root / platform
            directory.mkdir()
            package = directory / 'dockpipe-resolver-qemu-1.3.7.tar.gz'
            with tarfile.open(package, 'w:gz') as stream:
                member = tarfile.TarInfo('resolvers/qemu/assets/scripts/vmimage-run.sh')
                member.size = len(self.script)
                stream.addfile(member, io.BytesIO(self.script))
            entry = {'name': 'qemu', 'version': '1.3.7', 'tarball': package.name,
                     'sha256': hashlib.sha256(package.read_bytes()).hexdigest(), 'size_bytes': package.stat().st_size}
            manifest = {'schema': 1, 'platform': platform, 'packages': {'resolvers': [entry]}}
            path = directory / 'packages-store-manifest.json'
            path.write_text(json.dumps(manifest))
            self.stores[platform] = {'directory': directory, 'manifest_sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        checks = ['public-module-download', 'module-integrity', 'pure-go-five-platform-build',
                  'native-linux-version', 'native-python-package-source-hook', 'native-python-host-step']
        self.pin['cli_manifest'] = self.document(RELEASES + self.tag + '/cli-candidate.json',
            {'schema': 1, 'artifact_class': 'cli-only', 'source_repository': 'Dockpipe-Industries/dockpipe',
             'source_state': 'committed-snapshot', 'source_sha': self.sha,
             'version': self.version, 'tag': self.tag, 'checks': checks, 'artifacts': artifacts})

    def document(self, url, content):
        path = self.root / ('document-' + str(len(self.files)) + '.json')
        path.write_text(json.dumps(content))
        self.files[url] = path
        return {'url': url, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'size_bytes': path.stat().st_size}

    def admit(self):
        self.counter += 1
        def acquire(pin, *_arguments):
            return self.files[pin['url']]
        with patch('catalog_runtime_gate.acquire_baseline', side_effect=acquire):
            return admit_runtime(self.pin, self.source, self.dependencies, self.cli, None,
                                 self.root / ('ready-' + str(self.counter)), acquire, self.stores)

    def test_empty_runtime_store_is_valid_and_vm_adapter_belongs_to_actual_qemu_package(self):
        result = self.admit()
        self.assertEqual(result['cli_source_sha'], self.sha)
        for platform in PLATFORMS:
            self.assertEqual(result['platforms'][platform]['vmimage_run_sha256'], self.script_hash)
            self.assertNotIn('core_sha256', result['platforms'][platform])

    def test_old_core_role_or_mismatched_public_cli_cannot_be_silently_accepted(self):
        self.source['profiles']['linux-amd64']['inputs']['runtime_core'] = {}
        with self.assertRaises(PublicationStopped):
            self.admit()
        del self.source['profiles']['linux-amd64']['inputs']['runtime_core']
        self.cli['linux-amd64']['executable_sha256'] = '9' * 64
        with self.assertRaisesRegex(PublicationStopped, 'producer manifest'):
            self.admit()

    def test_sdk_source_or_manifest_identity_mismatch_rejects(self):
        self.dependencies['dockpipe_go_sdk']['source_commit'] = 'f' * 40
        with self.assertRaisesRegex(PublicationStopped, 'SDK manifest'):
            self.admit()

    def test_changed_package_manifest_or_effective_adapter_rejects(self):
        self.stores['linux-amd64']['manifest_sha256'] = '0' * 64
        with self.assertRaisesRegex(PublicationStopped, 'manifest differs'):
            self.admit()

    def test_reviewed_adapter_hash_cannot_replace_the_actual_package_bytes(self):
        self.pin['platforms']['linux-amd64']['vmimage_run_sha256'] = '0' * 64
        with self.assertRaisesRegex(PublicationStopped, 'actual|Actual'):
            self.admit()


if __name__ == '__main__':
    unittest.main()
