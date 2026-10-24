"""Authenticated acquisition precedes offline native builds without changing source sums."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import catalog_go_dependencies as dependencies


class DependencyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / 'release').mkdir()
        (self.root / 'release/dependencies.lock.json').write_text(json.dumps({
            'dockpipe_go_sdk': {'manifest_sha256': 'a' * 64}}))
        (self.root / 'go.mod').write_text('module fixture\nrequire dockpipe v0.0.0\n')
        (self.root / 'go.sum').write_text('fixture v1 h1:existing\n')

    def unpack(self, metadata, archive, expected, output):
        output.mkdir()
        (output / 'go.sum').write_text('sdk-fixture v1 h1:existing\n')
        return {'verified': True}

    def test_target_mismatch_stops_before_sdk_or_network(self):
        with patch.object(dependencies.subprocess, 'check_output', return_value='linux\namd64\n'), \
                patch.object(dependencies.go_candidate, 'unpack_sdk') as unpack, \
                patch.object(dependencies.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'target differs'):
                dependencies.prime(self.root, self.root / 'inputs', {}, 'darwin-arm64')
            unpack.assert_not_called()
            run.assert_not_called()

    def test_native_target_and_authenticated_sum_graph_precede_acquisition(self):
        original = {name: (self.root / name).read_bytes() for name in ['go.mod', 'go.sum']}
        with patch.object(dependencies.subprocess, 'check_output', return_value='darwin\narm64\n'), \
                patch.object(dependencies.go_candidate, 'unpack_sdk', side_effect=self.unpack), \
                patch.object(dependencies.go_candidate, 'validate_sdk_pin') as validate, \
                patch.object(dependencies.subprocess, 'run') as run:
            dependencies.prime(self.root, self.root / 'inputs', {'GOOS': 'darwin', 'GOARCH': 'arm64'}, 'darwin-arm64')
            validate.assert_called_once()
            calls = run.call_args_list
            self.assertEqual([call.args[0][1] for call in calls], ['mod', 'mod', 'list'])
            self.assertEqual(calls[-1].args[0][-3:], ['-deps', '-test', './...'])
            for call in calls:
                self.assertEqual(call.kwargs['env']['GOOS'], 'darwin')
                self.assertEqual(call.kwargs['env']['GOARCH'], 'arm64')
                self.assertEqual(call.kwargs['env']['GOTOOLCHAIN'], 'local')
                self.assertEqual(call.kwargs['timeout'], 900)
        self.assertEqual(original, {name: (self.root / name).read_bytes() for name in original})

    def test_missing_source_or_sdk_checksum_is_rejected(self):
        def acquire(arguments, **kwargs):
            if arguments[1] == 'list':
                modfile = next(argument.split('=', 1)[1] for argument in arguments if argument.startswith('-modfile='))
                with Path(modfile).with_suffix('.sum').open('a') as output:
                    output.write('unreviewed v1 h1:new\n')
        with patch.object(dependencies.go_candidate, 'unpack_sdk', side_effect=self.unpack), \
                patch.object(dependencies.go_candidate, 'validate_sdk_pin'), \
                patch.object(dependencies.subprocess, 'run', side_effect=acquire):
            with self.assertRaisesRegex(ValueError, 'checksums absent'):
                dependencies.prime(self.root, self.root / 'inputs', {})


if __name__ == '__main__':
    unittest.main()
