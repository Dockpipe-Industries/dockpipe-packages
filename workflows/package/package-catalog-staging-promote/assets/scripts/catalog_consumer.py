"""Execute the admitted public CLI in a fresh, source-independent consumer directory.

This is the secret-free native CI job boundary. It does not execute any package
workflow or boot a VM. The separate named-workflow qualification remains required.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from catalog_plan import entries
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped, fingerprint_inventory



SCRIPT_DIRECTORY = Path('workflows/package/package-catalog-staging-promote/assets/scripts')


def assert_source_independent(checkout_root):
    """Admit only the sparse helper checkout, excluding Git's inert metadata."""
    checkout_root = Path(checkout_root)
    if not checkout_root.is_absolute() or checkout_root.is_symlink() or not checkout_root.is_dir():
        raise PublicationStopped('Consumer requires its actual sparse helper checkout root')
    allowed_directories = {parent.as_posix() for parent in SCRIPT_DIRECTORY.parents
                           if parent != Path('.')}
    allowed_directories.add(SCRIPT_DIRECTORY.as_posix())
    pending = [checkout_root]
    files = []
    visited = 0
    while pending:
        directory = pending.pop()
        for path in directory.iterdir():
            visited += 1
            if visited > 128:
                raise PublicationStopped('Sparse consumer checkout exceeds helper inventory bound')
            relative = path.relative_to(checkout_root)
            if path.is_symlink():
                raise PublicationStopped('Sparse consumer checkout contains an alias')
            if relative == Path('.git'):
                continue
            if path.is_dir():
                if relative.as_posix() not in allowed_directories:
                    raise PublicationStopped('Consumer source tree is present: ' + relative.as_posix())
                pending.append(path)
            elif path.is_file() and relative.parent == SCRIPT_DIRECTORY and relative.suffix == '.py':
                digest, size = file_digest(path, 1048576)
                files.append({'path': relative.as_posix(), 'sha256': digest, 'size_bytes': size})
            else:
                raise PublicationStopped('Consumer checkout has a non-helper source file')
    if not files:
        raise PublicationStopped('Sparse consumer helper scripts are absent')
    return {'only_helper_scripts': True, 'git_metadata_excluded': True,
            'files': sorted(files, key=lambda item: item['path'])}


def run_cli(executable, arguments, root, environment, label):
    output = root / (label + '.stdout')
    errors = root / (label + '.stderr')
    deadline = time.monotonic() + 300
    with output.open('wb') as stdout, errors.open('wb') as stderr:
        process = subprocess.Popen([str(executable), *arguments],
                                   cwd=root, env=environment, stdin=subprocess.DEVNULL,
                                   stdout=stdout, stderr=stderr)
        while process.poll() is None:
            if (output.stat().st_size > 1048576 or errors.stat().st_size > 65536
                    or time.monotonic() >= deadline):
                process.kill()
                process.wait(timeout=5)
                raise PublicationStopped('Consumer execution exceeded its output or time bound')
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
    if output.stat().st_size > 1048576 or errors.stat().st_size > 65536:
        raise PublicationStopped('Consumer execution exceeded its output bound')
    if process.returncode != 0:
        raise PublicationStopped('Actual CLI consumer failed: ' + label)
    return output.read_bytes()


RUNTIME_CAPABILITY_CHECKS = [
    'init-without-source-dependencies', 'execute-host-starter',
    'sdk-shell-powershell-python-resources', 'python-sdk-explicit-cli',
]


def verify_runtime_capabilities(executable, cli_sha256, cli_version, root, environment):
    """Run real capabilities with private state; OS network denial is separate proof."""
    project = root / 'runtime-capability'
    project.mkdir(mode=0o700)
    selected_environment = dict(environment, DOCKPIPE_WORKDIR=str(project))
    outputs = []

    def invoke(command, arguments, label):
        content = run_cli(command, arguments, project, selected_environment, label)
        outputs.append({'role': label, 'sha256': hashlib.sha256(content).hexdigest(),
                        'size_bytes': len(content)})
        return content

    invoke(executable, ['init'], 'init')
    workflow = project / 'workflows/example/config.yml'
    if not workflow.is_file() or b'kind: host' not in workflow.read_bytes():
        raise PublicationStopped('Standalone init did not create the inert host starter')
    output = invoke(executable, ['--no-vault', '--no-op-inject', '--workdir', str(project),
                                 '--workflow', 'example'], 'execute-starter')
    if b'Hello from Dockpipe' not in output:
        raise PublicationStopped('Standalone host starter did not execute')
    helpers = {}
    for language in ('shell', 'powershell', 'python'):
        output = invoke(executable, ['sdk', 'path', '--language', language,
                                     '--workdir', str(project)], 'sdk-' + language)
        path = Path(output.decode().strip())
        if (not path.is_absolute() or path.is_symlink() or not path.is_file()
                or not path.resolve().is_relative_to(root.resolve())):
            raise PublicationStopped('SDK helper escaped the actual private consumer state')
        digest, size = file_digest(path, 1048576)
        helpers[language] = {'sha256': digest, 'size_bytes': size}
        if language == 'python':
            python_helper = path
    python_code = """import importlib.util
import json
import os
from pathlib import Path
import sys

specification = importlib.util.spec_from_file_location('selected_runtime_sdk', sys.argv[1])
module = importlib.util.module_from_spec(specification)
sys.modules[specification.name] = module
specification.loader.exec_module(module)
sdk = module.DockpipeSDK(os.getcwd())
if Path(sdk.dockpipe_bin).resolve() != Path(os.environ['DOCKPIPE_BIN']).resolve():
    raise RuntimeError('SDK selected a different executable')
scope = sdk.scope()
if not isinstance(scope, dict) or not scope:
    raise RuntimeError('SDK scope query failed')
print(json.dumps(scope, sort_keys=True))
"""
    invoke(Path(sys.executable), ['-c', python_code, str(python_helper)], 'python-sdk-scope')
    if (selected_environment.get('DOCKPIPE_GLOBAL_ROOT') and
            (Path(selected_environment['DOCKPIPE_GLOBAL_ROOT']) / 'packages/core').exists()):
        raise PublicationStopped('Standalone CLI unexpectedly materialized a core package')
    return {'cli_sha256': cli_sha256, 'cli_version': cli_version,
            'checks': RUNTIME_CAPABILITY_CHECKS, 'helpers': helpers, 'outputs': outputs,
            'network_denial_claimed': False}


def verify_consumer(cli, cli_sha256, cli_version, root, plan, platform, store_manifest, promoted, closures, source_checkout):
    source_admission = assert_source_independent(source_checkout)
    executable = Path(cli)
    root = Path(root)
    if (not executable.is_absolute() or executable.is_symlink()
            or file_digest(executable, 128 * 1024 * 1024)[0] != cli_sha256):
        raise PublicationStopped('Public CLI byte identity differs')
    if root.exists():
        raise PublicationStopped('Consumer directory must be fresh')
    root.mkdir(mode=0o700, parents=False)
    packages = root / 'packages'
    packages.mkdir(mode=0o700)
    environment = {'HOME': str(root), 'USERPROFILE': str(root),
                   'XDG_CONFIG_HOME': str(root / 'config'),
                   'XDG_STATE_HOME': str(root / 'state'),
                   'DOCKPIPE_SYSTEM_ROOT': str(root / 'system'),
                   'XDG_CACHE_HOME': str(root / 'cache'),
                   'XDG_DATA_HOME': str(root / 'data'),
                   'LOCALAPPDATA': str(root / 'local'), 'APPDATA': str(root / 'roaming'),
                   'DOCKPIPE_WORKDIR': str(root),
                   'DOCKPIPE_GLOBAL_ROOT': str(root / 'global'),
                   'DOCKPIPE_BIN': str(executable), 'NO_COLOR': '1'}
    if os.name == 'nt':
        system_root = os.environ.get('SystemRoot')
        if not system_root or not Path(system_root).is_absolute():
            raise PublicationStopped('Windows system root is unavailable')
        environment.update({'SystemRoot': system_root, 'WINDIR': system_root,
                            'PATH': str(Path(system_root) / 'System32'),
                            'PATHEXT': '.COM;.EXE;.BAT;.CMD',
                            'TEMP': str(root), 'TMP': str(root)})
    else:
        environment['PATH'] = '/usr/bin:/bin'
        environment['TMPDIR'] = str(root)
    observed_version = run_cli(executable, ['--version'], root, environment, 'version').decode().strip()
    if cli_version not in observed_version.split():
        raise PublicationStopped('Actual CLI version differs from admitted public release')
    capability = verify_runtime_capabilities(executable, cli_sha256, cli_version, root, environment)
    wrapper = next(item for item in plan['objects'] if item['role'] == 'release-manifest')
    remote = 'https://packages.staging.dockpipe.com/' + wrapper['key']
    observed = json.loads(run_cli(executable, ['package', 'catalog', '--remote', remote,
                                              '--include-hidden'], root, environment, 'catalog'))
    if observed.get('platform') != platform:
        raise PublicationStopped('Actual CLI selected a different native platform')
    expected = entries(store_manifest)
    actual = {}
    for entry in observed.get('packages', []):
        key = entry['kind'] + '/' + entry['name']
        if key in actual:
            raise PublicationStopped('Actual public catalog has duplicate identities')
        actual[key] = entry
    if set(actual) != set(expected):
        raise PublicationStopped('Actual public catalog identity set differs')
    for key, entry in expected.items():
        for field in ['version', 'sha256', 'size_bytes']:
            if actual[key].get(field) != entry[field]:
                raise PublicationStopped('Actual public catalog archive identity differs')
    installed_receipts = []
    for index, key in enumerate(sorted(promoted)):
        if key not in expected or key not in closures:
            raise PublicationStopped('Requested consumer install or declared closure is absent')
        case_root = root / ('case-' + str(index))
        case_root.mkdir(mode=0o700)
        case_environment = dict(environment)
        for variable, directory in [('HOME', 'home'), ('USERPROFILE', 'home'),
                                    ('XDG_CONFIG_HOME', 'config'), ('XDG_CACHE_HOME', 'cache'),
                                    ('XDG_DATA_HOME', 'data'), ('LOCALAPPDATA', 'local'),
                                    ('APPDATA', 'roaming'), ('DOCKPIPE_GLOBAL_ROOT', 'global')]:
            case_environment[variable] = str(case_root / directory)
        case_environment['DOCKPIPE_WORKDIR'] = str(case_root)
        kind, name = key.split('/', 1)
        installed = json.loads(run_cli(executable, ['package', 'install', '--remote', remote,
                                                    '--kind', kind, '--name', name,
                                                    '--sha256', expected[key]['sha256']],
                                       case_root, case_environment, 'install'))
        package_root = case_root / 'global' / 'packages'
        selected_path = package_root / (kind + 's') / expected[key]['tarball']
        if Path(installed.get('path', '')).resolve() != selected_path.resolve():
            raise PublicationStopped('Actual installation returned an unexpected path')
        installed_receipts.append(verify_installed_closure(package_root, key, closures[key], expected))
    if file_digest(executable, 128 * 1024 * 1024)[0] != cli_sha256:
        raise PublicationStopped('Public CLI changed during consumer proof')
    return {'schema': 1, 'platform': platform, 'cli_sha256': cli_sha256,
            'cli_version_output': observed_version, 'wrapper_sha256': wrapper['sha256'],
            'object_inventory_sha256': fingerprint_inventory(plan['objects']),
            'source_independent': source_admission['only_helper_scripts'],
            'runtime_capability': capability,
            'source_checkout_admission': source_admission,
            'exit_code': 0, 'installed': installed_receipts,
            'VM_operations': False, 'environment_keys': sorted(environment)}


def verify_installed_closure(package_root, requested, closure, expected):
    if requested not in closure or len(set(closure)) != len(closure):
        raise PublicationStopped('Exact requested dependency closure is invalid')
    wanted = {}
    for key in closure:
        if key not in expected:
            raise PublicationStopped('Required dependency is absent from selected store')
        kind = key.split('/', 1)[0]
        relative = kind + 's/' + expected[key]['tarball']
        wanted[relative] = expected[key]
    actual = {}
    if package_root.exists():
        for path in package_root.rglob('*'):
            if path.is_symlink():
                raise PublicationStopped('Installed store contains an alias')
            if path.is_file():
                actual[path.relative_to(package_root).as_posix()] = path
    if set(actual) != set(wanted):
        raise PublicationStopped('Actual fresh install dependency closure differs')
    for relative, path in actual.items():
        entry = wanted[relative]
        digest, size = file_digest(path, entry['size_bytes'])
        if digest != entry['sha256'] or size != entry['size_bytes']:
            raise PublicationStopped('Installed dependency archive bytes differ')
    return {'requested': requested, 'closure': sorted(closure),
            'archives': [{'relative_path': name, 'sha256': wanted[name]['sha256'],
                          'size_bytes': wanted[name]['size_bytes']} for name in sorted(wanted)]}
