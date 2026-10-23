#!/usr/bin/env python3
"""Draft native hosted provenance emitter around the existing Dockpipe producer."""
import argparse
import getpass
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import prepare_candidate as candidate

COMPONENT = Path('packages/vm/guest-tools')


def capture(arguments, maximum=262144):
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        subprocess.run([str(value) for value in arguments], check=True,
                       stdout=output, stderr=errors, timeout=30)
        output.seek(0)
        errors.seek(0)
        data = output.read(maximum + 1)
        diagnostic = errors.read(maximum + 1)
    if len(data) > maximum or len(diagnostic) > maximum:
        raise ValueError('Source/tool observation exceeds its metadata bound')
    return data


def hosted_identity(environment):
    if (environment.get('GITHUB_ACTIONS') != 'true' or environment.get('RUNNER_OS') != 'Windows'
            or environment.get('RUNNER_ARCH') != 'X64'
            or environment.get('GITHUB_REPOSITORY') != candidate.REPOSITORY):
        raise ValueError('This emitter requires an actual owning hosted Windows x64 workflow')
    run = environment.get('GITHUB_RUN_ID', '')
    attempt = environment.get('GITHUB_RUN_ATTEMPT', '')
    workflow_sha = environment.get('GITHUB_WORKFLOW_SHA', '')
    if (not re.fullmatch(r'[0-9]{1,18}', run) or not 1 <= int(run) < 2 ** 63
            or not re.fullmatch(r'[0-9]{1,3}', attempt) or not 1 <= int(attempt) <= 100
            or not candidate.source_value(workflow_sha)):
        raise ValueError('Actual hosted workflow identity is incomplete')
    return {'repository': candidate.REPOSITORY, 'run_id': int(run),
            'run_attempt': int(attempt), 'workflow_sha': workflow_sha}


def source_snapshot(repository, git, source_sha):
    if not candidate.source_value(source_sha) or not repository.is_absolute() or repository.is_symlink():
        raise ValueError('Select an explicit exact source checkout')
    prefix = [git, '--no-optional-locks', '-C', repository]
    observed_root = Path(capture(prefix + ['rev-parse', '--show-toplevel']).decode().strip())
    if not observed_root.samefile(repository):
        raise ValueError('Selected directory is not its own checkout root')
    if capture(prefix + ['rev-parse', 'HEAD']).decode().strip() != source_sha:
        raise ValueError('Actual checkout differs from the requested artifact source commit')
    remote = capture(prefix + ['remote', 'get-url', 'origin']).decode().strip()
    if remote.removesuffix('.git') != 'https://github.com/' + candidate.REPOSITORY:
        raise ValueError('Artifact source must come from its explicit public owning repository')
    if capture(prefix + ['status', '--porcelain=v1', '--untracked-files=all']):
        raise ValueError('A committed clean source checkout is required')
    names = capture(prefix + ['ls-files', '-z', '--', COMPONENT.as_posix()]).decode().split('\0')
    names = [name for name in names if name]
    if not 0 < len(names) <= 512:
        raise ValueError('Source component exceeds its file bound')
    actual = {path.relative_to(repository).as_posix() for path in (repository / COMPONENT).rglob('*')
              if path.is_file() and '__pycache__' not in path.parts}
    if actual != set(names):
        raise ValueError('Source component contains ignored/generated or missing material inputs')
    inventory = []
    for name in [*names, 'LICENSE']:
        path = repository / name
        data = candidate.regular_bytes(path, 1 << 20)
        committed = capture(prefix + ['show', source_sha + ':' + name], maximum=1 << 20)
        if data != committed:
            raise ValueError('Material source bytes differ from the exact commit; disable checkout conversion')
        if name != 'LICENSE':
            inventory.append({'path': path.relative_to(repository / COMPONENT).as_posix(),
                              'sha256': candidate.digest(data)})
    inventory.sort(key=lambda item: tuple(part.casefold() for part in Path(item['path']).parts))
    return inventory


def ctest_outcomes(path):
    data = candidate.regular_bytes(path, 128 << 10)
    root = ET.fromstring(data)
    cases = list(root.iter('testcase'))
    expected = {'guest-tools-status', 'guest-tools-clipboard'}
    if len(cases) != 2 or {case.get('name') for case in cases} != expected:
        raise ValueError('Native CTest output must contain exactly both Guest Tools tests')
    for case in cases:
        if (case.get('status', 'run') != 'run'
                or any(child.tag in {'failure', 'error', 'skipped'} for child in case)):
            raise ValueError('Native Guest Tools tests failed or were skipped')
    for suite in root.iter('testsuite'):
        if any(int(suite.get(name, '0')) != 0 for name in ('failures', 'errors', 'skipped', 'disabled')):
            raise ValueError('Native Guest Tools suite contains unsuccessful outcomes')
    return {name: 'passed' for name in sorted(expected)}


def actual_tools(qt):
    kit = qt / '6.10.3/mingw_64'
    tools = qt / 'Tools'
    observations = {
        'qt_version': capture([kit / 'bin/qmake.exe', '-query', 'QT_VERSION']).decode().strip(),
        'compiler_id': 'GNU',
        'compiler_version': capture([tools / 'mingw1310_64/bin/g++.exe', '-dumpfullversion']).decode().strip(),
        'cmake_version': capture([tools / 'CMake_64/bin/cmake.exe', '--version']).decode().splitlines()[0].removeprefix('cmake version '),
        'ninja_version': capture([tools / 'Ninja/ninja.exe', '--version']).decode().strip(),
    }
    if observations != candidate.TOOL_VERSIONS:
        raise ValueError('Actual native tool versions differ from the qualified Guest Tools kit')
    return observations


def build(repository, source_sha, git, binary, cli_sha, qt, output):
    if platform.system() != 'Windows' or platform.machine().casefold() not in {'amd64', 'x86_64'}:
        raise ValueError('Native Windows amd64 execution is required')
    workflow = hosted_identity(os.environ)
    if (not git.is_absolute() or not git.is_file() or not binary.is_absolute()
            or not binary.is_file() or not candidate.hash_value(cli_sha)
            or candidate.digest(candidate.regular_bytes(binary, 64 << 20)) != cli_sha):
        raise ValueError('Select an explicit public checksum-pinned Dockpipe and native Git executable')
    inventory = source_snapshot(repository, git, source_sha)
    if not qt.is_absolute() or qt.is_symlink() or not qt.is_dir():
        raise ValueError('Select the explicit locked Qt/toolchain prefix')
    tools = actual_tools(qt)
    if (not output.is_absolute() or output.exists() or output.is_symlink()
            or output.resolve().is_relative_to(repository.resolve())
            or output.resolve().is_relative_to(qt.resolve())):
        raise ValueError('Use a fresh work directory outside source and toolchain')
    output.mkdir(parents=True)
    native = output / 'native'
    environment = dict(os.environ, DOCKPIPE_GUEST_TOOLS_QT=str(qt),
                       DOCKPIPE_GUEST_TOOLS_OUTPUT=str(native))
    environment['PATH'] = os.pathsep.join((str(qt / 'Tools/mingw1310_64/bin'),
                                               str(qt / '6.10.3/mingw_64/bin'),
                                               environment.get('PATH', '')))
    # The selected workflow has one package-owned host producer and no resolver,
    # runtime, model or package dependency to compile for this artifact lane.
    subprocess.run([str(binary), '--no-compile-deps', '--workdir', str(repository),
                    '--workflow-file', str(repository / COMPONENT / 'workflows/build/config.yml'), '--'],
                   env=environment, check=True, timeout=900)
    results = output / 'native-ctest.xml'
    subprocess.run([str(qt / 'Tools/CMake_64/bin/ctest.exe'), '--test-dir', str(native / 'build'),
                    '--output-on-failure', '--no-tests=error', '--output-junit', str(results)],
                   env=environment, check=True, timeout=120)
    tests = ctest_outcomes(results)
    powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
    integrity_output = output / 'installer-integrity'
    subprocess.run([str(powershell), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                    '-File', str(repository / COMPONENT / 'tests/InstallerIntegrity.Tests.ps1'),
                    '-BundlePath', str(native / 'bundle'), '-InteractiveUserName', getpass.getuser(),
                    '-OutputDirectory', str(integrity_output)], env=environment, check=True, timeout=120)
    integrity, _ = candidate.document(integrity_output / 'result.json')
    if (integrity.get('schema') != 1 or integrity.get('platform') != candidate.PLATFORM
            or integrity.get('installer_parse') != 'passed'
            or integrity.get('corrupt_executable_rejected') is not True
            or integrity.get('ui_task_unchanged') is not True):
        raise ValueError('Actual native installer integrity proof failed')
    if source_snapshot(repository, git, source_sha) != inventory:
        raise ValueError('Source changed during native production')
    inputs = output / 'inputs'
    inputs.mkdir()
    for role in ('guest_tools', 'guest_descriptor'):
        shutil.copyfile(native / candidate.NAMES[role], inputs / candidate.NAMES[role])
    descriptor, descriptor_bytes = candidate.document(inputs / candidate.NAMES['guest_descriptor'])
    canonical_inventory, inventory_sha = candidate.source_inventory(inventory)
    if (descriptor.get('source_inventory_sha256') != inventory_sha
            or integrity.get('inventory_sha256') != descriptor.get('inventory_sha256')):
        raise ValueError('Actual native source and installer proof differ from the produced delivery')
    (inputs / candidate.NAMES['guest_source_inventory']).write_text(
        json.dumps(canonical_inventory, separators=(',', ':')))
    lock = repository / COMPONENT / 'tooling/toolchain.lock.json'
    shutil.copyfile(lock, inputs / candidate.NAMES['guest_toolchain_lock'])
    archive = candidate.regular_bytes(inputs / candidate.NAMES['guest_tools'], 256 << 20)
    provenance = {
        'schema': 1, 'product': 'DockpipeGuestToolsNativeBuild',
        'source_repository': candidate.REPOSITORY, 'source_sha': source_sha,
        'source_state': 'committed-snapshot', 'native_host': candidate.PLATFORM,
        'qualification_origin': 'github-actions-native-windows',
        'source_inventory_sha256': inventory_sha,
        'archive_sha256': candidate.digest(archive), 'descriptor_sha256': candidate.digest(descriptor_bytes),
        'toolchain_lock_sha256': candidate.digest(lock.read_bytes()), **tools,
        'checks': candidate.CHECKS, 'ctest_tests': tests,
        'installer_integrity': {key: integrity[key] for key in
                                ('installer_parse', 'corrupt_executable_rejected', 'ui_task_unchanged')},
        'workflow': workflow,
    }
    (inputs / candidate.NAMES['guest_native_provenance']).write_text(json.dumps(provenance, indent=2) + '\n')
    candidate.prepare(inputs, output / 'prepared', source_sha)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--git', type=Path, required=True)
    parser.add_argument('--dockpipe', type=Path, required=True)
    parser.add_argument('--dockpipe-sha256', required=True)
    parser.add_argument('--qt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    build(arguments.source, arguments.source_sha, arguments.git, arguments.dockpipe,
          arguments.dockpipe_sha256, arguments.qt, arguments.output)


if __name__ == '__main__':
    main()
