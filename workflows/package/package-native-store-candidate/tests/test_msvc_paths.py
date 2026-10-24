"""Exercise the actual filesystem admission helper under PowerShell strict mode."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import test_native_store as fixtures


class MicrosoftPathTests(unittest.TestCase):
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
