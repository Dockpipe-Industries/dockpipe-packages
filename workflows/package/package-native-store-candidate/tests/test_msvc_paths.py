"""Exercise the actual filesystem admission helper under PowerShell strict mode."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import test_native_store as fixtures


class MicrosoftPathTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('pwsh'), 'PowerShell is required for native helper execution')
    def test_effective_command_requires_selected_first_despite_later_duplicates(self):
        script = (fixtures.SCRIPTS / 'prepare_msvc.ps1').read_text()
        function = script[script.index('function Assert-SelectedCommand'):script.index('function Assert-MicrosoftBinary')]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            selected = root / 'selected'
            other = root / 'other'
            selected.mkdir()
            other.mkdir()
            for directory in [selected, other]:
                shutil.copy2(sys.executable, directory / 'fixture-tool.exe')
            for first, second, accepted in [(selected, other, True), (other, selected, False)]:
                environment = dict(os.environ, EXPECTED_TOOL=str(selected / 'fixture-tool.exe'),
                                   PATH=os.pathsep.join([str(first), str(second), os.environ.get('PATH', '')]))
                body = "Set-StrictMode -Version Latest\n$ErrorActionPreference = 'Stop'\n" + function + r'''
$matches = @(Get-Command fixture-tool.exe -CommandType Application)
if ($matches.Count -lt 2) {
    throw 'The duplicate-PATH fixture did not produce multiple applications.'
}
Assert-SelectedCommand 'fixture-tool.exe' $env:EXPECTED_TOOL
Write-Output 'effective-command-pass'
'''
                result = subprocess.run([shutil.which('pwsh'), '-NoProfile', '-NonInteractive', '-Command', body],
                                        env=environment, capture_output=True, text=True, timeout=20)
                with self.subTest(selected_first=accepted):
                    if accepted:
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(result.stdout.strip(), 'effective-command-pass')
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn('resolves outside the selected toolset or SDK', result.stderr)

    @unittest.skipUnless(shutil.which('pwsh'), 'PowerShell is required for native helper execution')
    def test_raw_parent_objects_preserve_regular_file_and_reparse_checks(self):
        script = (fixtures.SCRIPTS / 'prepare_msvc.ps1').read_text()
        function = script[script.index('function Get-ContainedFile'):script.index('function Assert-MicrosoftBinary')]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / 'installation' / 'tools'
            nested.mkdir(parents=True)
            tool = nested / 'compiler.exe'
            tool.write_bytes(b'fixture')
            link = root / 'installation' / 'linked.exe'
            link.symlink_to(tool)
            environment = dict(os.environ, FIXTURE_ROOT=str(root / 'installation'), FIXTURE_FILE=str(tool),
                               FIXTURE_DIRECTORY=str(nested), FIXTURE_LINK=str(link))
            body = r'''Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
''' + function + r'''
$admitted = Get-ContainedFile $env:FIXTURE_FILE $env:FIXTURE_ROOT
if ($admitted.FullName -ne $env:FIXTURE_FILE) {
    throw 'Expected the exact regular file.'
}
foreach ($candidate in @($env:FIXTURE_DIRECTORY, $env:FIXTURE_LINK)) {
    $rejected = $false
    try {
        Get-ContainedFile $candidate $env:FIXTURE_ROOT | Out-Null
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw 'Directory or reparse point was admitted.'
    }
}
Write-Output 'filesystem-admission-pass'
'''
            result = subprocess.run([shutil.which('pwsh'), '-NoProfile', '-NonInteractive', '-Command', body],
                                    env=environment, capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), 'filesystem-admission-pass')


if __name__ == '__main__':
    unittest.main()
