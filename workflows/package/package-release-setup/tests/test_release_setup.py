#!/usr/bin/env python3
"""Verify credential minimization and preflight for GitHub release setup."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
from unittest.mock import patch
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'assets/scripts/configure-release.py'
spec = importlib.util.spec_from_file_location('release_setup', SCRIPT)
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class ReleaseSetupTests(unittest.TestCase):
    def setUp(self):
        self.environment = {
            'RELEASE_SETUP_REPOSITORY': 'example/standalone-package',
            'AWS_ACCESS_KEY_ID': 'fixture-upload-id',
            'AWS_SECRET_ACCESS_KEY': 'fixture-upload-secret',
            'R2_ENDPOINT_URL': 'https://example.invalid',
            'DOCKPIPE_RELEASE_BUCKET': 'example-staging',
            'R2_PREFIX': 'packages',
        }

    def test_upload_only_copies_two_credentials_and_never_reads_signing_key(self):
        output = io.StringIO()
        with patch.dict(os.environ, self.environment, clear=True), \
                patch.object(setup, 'run') as run, \
                patch.object(setup, 'normalize_signing_key') as normalize, \
                patch.object(setup, 'signing_fingerprint') as signing, \
                contextlib.redirect_stdout(output):
            setup.configure(True, 'release-staging', upload_only=True)
        names = [call.args[0][3] for call in run.call_args_list]
        self.assertEqual(names, ['AWS_ACCESS_KEY_ID', 'AWS_SECRET_ACCESS_KEY',
                                 'R2_ENDPOINT_URL', 'DOCKPIPE_RELEASE_BUCKET', 'R2_PREFIX'])
        for call in run.call_args_list[:2]:
            self.assertEqual(call.args[0][:3], ['gh', 'secret', 'set'])
            self.assertIn('--env', call.args[0])
            self.assertNotIn(call.kwargs['value'], call.args[0])
        normalize.assert_not_called()
        signing.assert_not_called()
        self.assertNotIn(self.environment['AWS_ACCESS_KEY_ID'], output.getvalue())
        self.assertNotIn(self.environment['AWS_SECRET_ACCESS_KEY'], output.getvalue())

    def test_missing_upload_binding_fails_before_any_github_operation(self):
        del self.environment['AWS_SECRET_ACCESS_KEY']
        with patch.dict(os.environ, self.environment, clear=True), patch.object(setup, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'Missing release settings'):
                setup.configure(True, 'release-staging', upload_only=True)
        run.assert_not_called()

    def test_upload_only_checks_do_not_mutate_github(self):
        with patch.dict(os.environ, self.environment, clear=True), \
                patch.object(setup, 'run') as run, contextlib.redirect_stdout(io.StringIO()):
            setup.configure(False, 'release-staging', upload_only=True)
        run.assert_not_called()

    def test_full_release_still_requires_and_copies_signing_material(self):
        self.environment['APT_SIGNING_KEY'] = 'fixture-signing-key'
        with patch.dict(os.environ, self.environment, clear=True), \
                patch.object(setup, 'run') as run, \
                patch.object(setup, 'normalize_signing_key', return_value='normalized-fixture-key'), \
                patch.object(setup, 'signing_fingerprint', return_value='fixture-fingerprint'), \
                contextlib.redirect_stdout(io.StringIO()):
            setup.configure(True, 'release-staging')
        names = [call.args[0][3] for call in run.call_args_list]
        self.assertEqual(names, ['AWS_ACCESS_KEY_ID', 'AWS_SECRET_ACCESS_KEY', 'APT_SIGNING_KEY',
                                 'R2_ENDPOINT_URL', 'DOCKPIPE_RELEASE_BUCKET', 'R2_PREFIX',
                                 'APT_SIGNING_FINGERPRINT'])

    def test_release_setup_copies_only_scoped_values_and_defaults_to_checks(self):
        values = {
            "RELEASE_SETUP_REPOSITORY": "example/project",
            "AWS_ACCESS_KEY_ID": "test-id",
            "AWS_SECRET_ACCESS_KEY": "test-key",
            "APT_SIGNING_KEY": "test-signing-key\nsecond-line",
            "R2_ENDPOINT_URL": "https://example.invalid",
            "DOCKPIPE_RELEASE_BUCKET": "example-packages",
            "R2_PREFIX": "packages",
            "CLOUDFLARE_API_TOKEN": "must-not-copy",
            "R2_STATE_SECRET_ACCESS_KEY": "must-not-copy",
        }
        with patch.dict(os.environ, values, clear=True), \
                patch.object(setup, "signing_fingerprint", return_value="A" * 40), \
                patch.object(setup, "run") as run:
            setup.configure(False)
            run.assert_not_called()
            setup.configure(True)
            secrets = [call for call in run.call_args_list if call.args[0][1] == "secret"]
            self.assertEqual({call.args[0][3] for call in secrets}, set(setup.SECRETS))
            for call in secrets:
                self.assertEqual(call.kwargs["value"], values[call.args[0][3]])
                self.assertNotIn(call.kwargs["value"], call.args[0])
            self.assertEqual(len(run.call_args_list), 7)
            run.reset_mock()
            with patch.dict(os.environ, {"RELEASE_SETUP_REPOSITORY": ""}):
                with self.assertRaises(ValueError):
                    setup.configure(True, "release-staging")
            run.assert_not_called()
            with patch.dict(os.environ, {"DOCKPIPE_RELEASE_BUCKET": "example-staging"}):
                setup.configure(True, "release-staging")
            self.assertEqual(len(run.call_args_list), 7)
            for call in run.call_args_list:
                self.assertIn("example/project", call.args[0])
                self.assertIn("release-staging", call.args[0])
                self.assertNotIn("release", call.args[0])

    def test_release_setup_repairs_only_valid_flattened_armor(self):
        armor = (
            "-----BEGIN PGP PRIVATE KEY BLOCK-----\n\n"
            "YWJjZA==\n"
            "=YWJj\n"
            "-----END PGP PRIVATE KEY BLOCK-----\n"
        )
        self.assertEqual(setup.normalize_signing_key(armor.replace("\n", " ")), armor)
        self.assertEqual(setup.normalize_signing_key(armor), armor.strip())
        invalid_values = (
            "filename.asc",
            armor.replace("\n", " ").replace("YWJjZA==", "bad!"),
        )
        for invalid in invalid_values:
            with self.assertRaises(ValueError):
                setup.normalize_signing_key(invalid)


if __name__ == '__main__':
    unittest.main()
