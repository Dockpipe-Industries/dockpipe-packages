#!/usr/bin/env python3
"""Exercise bridge lifecycle without reading a desktop clipboard (offscreen Qt)."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

binary = Path(os.environ['DOCKPIPE_VM_CLIPBOARD_BIN']).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix='dockpipe-clipboard-control-') as temporary:
    root = Path(temporary)
    runtime = root / 'runtime'
    runtime.mkdir(mode=0o700)
    failure = root / 'fail'
    manager = root / 'fixture-manager'
    manager.write_text('#!/usr/bin/python3\nimport json,sys\nfrom pathlib import Path\n'
                       f'failure=Path({str(failure)!r})\n'
                       'request=json.load(sys.stdin)\n'
                       'if failure.exists():\n print("invalid")\nelse:\n print(json.dumps({"schema":1,"success":True,"text_base64":""}))\n')
    manager.chmod(0o700)
    scope = root / 'state'
    scope.mkdir(mode=0o700)
    other_scope = root / 'other-state'
    other_scope.mkdir(mode=0o700)
    second_manager = root / 'second-manager'
    second_manager.write_bytes(manager.read_bytes())
    second_manager.chmod(0o700)
    environment = dict(os.environ, DOCKPIPE_VM_CLIPBOARD_SCOPE=str(scope), QT_QPA_PLATFORM='offscreen', XDG_RUNTIME_DIR=str(runtime), DISPLAY='', WAYLAND_DISPLAY='')
    def control(operation, executable=manager, state_scope=scope):
        child_environment = dict(environment, DOCKPIPE_VM_CLIPBOARD_SCOPE=str(state_scope))
        result = subprocess.run([str(binary), operation, '--resource', 'fixture', '--manager', str(executable)],
                                env=child_environment, capture_output=True, timeout=15, check=True)
        return json.loads(result.stdout)
    try:
        for invalid_scope in ('', 'relative-state'):
            invalid_environment = dict(environment, DOCKPIPE_VM_CLIPBOARD_SCOPE=invalid_scope)
            invalid = subprocess.run(
                [str(binary), '--status', '--resource', 'fixture', '--manager', str(manager)],
                env=invalid_environment, capture_output=True, timeout=15)
            assert invalid.returncode == 2
        assert control('--start')['state'] in ('active', 'unsupported-text')
        locks = list(runtime.glob('*.lock'))
        assert len(locks) == 1
        owner = locks[0].read_text().splitlines()[0]
        control('--start', second_manager)
        assert control('--status', second_manager)['state'] in ('active', 'unsupported-text')
        assert locks[0].read_text().splitlines()[0] == owner
        assert len(list(runtime.glob('*.lock'))) == 1
        assert control('--status', second_manager, other_scope)['state'] == 'stopped'
        control('--start', second_manager, other_scope)
        assert len(list(runtime.glob('*.lock'))) == 2
        control('--stop', second_manager, other_scope)
        for attempt in range(30):
            if len(list(runtime.glob('*.lock'))) == 1:
                break
            time.sleep(0.1)
        assert len(list(runtime.glob('*.lock'))) == 1
        assert locks[0].read_text().splitlines()[0] == owner
        failure.touch()
        for attempt in range(30):
            if control('--status')['state'] == 'paused':
                break
            time.sleep(0.1)
        else:
            raise AssertionError('transport failure did not pause synchronization')
        failure.unlink()
        assert control('--start')['state'] == 'active'
        assert control('--stop')['state'] == 'stopped'
        for attempt in range(30):
            if not locks[0].exists():
                break
            time.sleep(0.1)
        assert not locks[0].exists()
        print('PASS: private IPC, two manager paths share one state worker, separate scope isolation, disconnect pause, explicit resume and stop; offscreen clipboard only')
    finally:
        control('--stop')
        control('--stop', second_manager, other_scope)
