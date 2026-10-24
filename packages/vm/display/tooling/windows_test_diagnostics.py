"""Bounded static evidence for failed native tests; never retries or loads a DLL."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from tooling.runtime_closure import WINDOWS_SYSTEM_DLLS, WINDOWS_HOST_CRT_DLLS

TEST_NAMES = ('qmp-absolute-input-test.exe', 'native-windows-test.exe', 'frame-store-test.exe')


def file_record(path):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 64 << 20:
        raise ValueError('Diagnostic PE input is missing or unbounded')
    checksum = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            checksum.update(block)
        stream.seek(0)
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError('Diagnostic input is not a PE executable')
        offset = struct.unpack_from('<I', header, 0x3c)[0]
        if offset + 6 > path.stat().st_size:
            raise ValueError('Diagnostic PE header exceeds its file')
        stream.seek(offset)
        signature = stream.read(6)
        if signature[:4] != b'PE\0\0':
            raise ValueError('Diagnostic PE signature differs')
    return {'path': str(path), 'sha256': checksum.hexdigest(), 'size_bytes': path.stat().st_size,
            'machine': hex(struct.unpack_from('<H', signature, 4)[0])}


def diagnose_tests(build, gio, qt, environment):
    inspector = Path(environment.get('DOCKPIPE_VM_DISPLAY_INSPECTOR', ''))
    if not inspector.is_absolute() or not inspector.is_file():
        raise ValueError('Diagnostic inspection requires the selected native PE inspector')
    path_value = environment.get('PATH', '')
    if len(path_value) > 65536:
        raise ValueError('Diagnostic PATH exceeds its capacity')
    selected = []
    for name in TEST_NAMES:
        candidates = [path for path in (build / name, build / 'Release' / name) if path.is_file()]
        if len(candidates) != 1:
            raise ValueError('Diagnostic test executable is missing or ambiguous: ' + name)
        selected.append(candidates[0])
    directories = [selected[0].parent, gio / 'bin', qt / 'bin']
    graph = {}
    pending = list(selected)
    while pending:
        binary = pending.pop()
        key = str(binary)
        if key in graph:
            continue
        if len(graph) >= 96:
            raise ValueError('Diagnostic native dependency graph exceeds capacity')
        record = file_record(binary)
        result = subprocess.run([str(inspector), '/nologo', '/dependents', str(binary)],
                                capture_output=True, text=True, check=True, timeout=20)
        if len(result.stdout) + len(result.stderr) > 1 << 20:
            raise ValueError('Diagnostic PE import metadata exceeds capacity')
        imports = re.findall(r'^\s+(\S+\.dll)\s*$', result.stdout, re.MULTILINE | re.IGNORECASE)
        if len(imports) > 256:
            raise ValueError('Diagnostic PE import count exceeds capacity')
        record['imports'] = []
        graph[key] = record
        for name in imports:
            lower = name.lower()
            if Path(name).name != name:
                raise ValueError('Invalid native import basename')
            dependency = {'name': name}
            if lower in WINDOWS_SYSTEM_DLLS or lower.startswith(('api-ms-win-', 'ext-ms-win-')):
                dependency['classification'] = 'system-runtime-required'
            elif lower in WINDOWS_HOST_CRT_DLLS:
                dependency['classification'] = 'qualified-host-crt-required'
            else:
                matches = [directory / name for directory in directories if (directory / name).is_file()]
                if matches:
                    dependency.update(classification='selected-build-or-prefix', path=str(matches[0]))
                    pending.append(matches[0])
                else:
                    dependency['classification'] = 'unresolved-in-selected-prefixes'
            record['imports'].append(dependency)
    symbol_imports = {}
    for binary in selected[:2]:
        result = subprocess.run([str(inspector), '/nologo', '/imports', str(binary)],
                                capture_output=True, text=True, check=True, timeout=20)
        if len(result.stdout) + len(result.stderr) > 1 << 20:
            raise ValueError('Diagnostic PE symbol import metadata exceeds capacity')
        symbol_imports[binary.name] = result.stdout
    test_logs = []
    for binary in selected:
        log = build / (binary.name + '.qtest.txt')
        if not log.exists():
            continue
        if log.is_symlink() or not log.is_file() or log.stat().st_size > 1 << 20:
            raise ValueError('Native QtTest text is not a bounded regular file')
        test_logs.append({'name': log.name, 'size_bytes': log.stat().st_size,
                          'sha256': hashlib.sha256(log.read_bytes()).hexdigest()})
    evidence = {'schema': 1, 'effective_path': path_value, 'gio_bin': str(gio / 'bin'),
                'qt_bin': str(qt / 'bin'), 'selected_inspector': str(inspector),
                'prepared_crt_directory': environment.get('DOCKPIPE_NATIVE_CRT_DIR'),
                'prepared_redist_root': environment.get('VCTOOLSREDISTDIR') or environment.get('VCToolsRedistDir'),
                'selected_tests': [str(path) for path in selected], 'pe_graph': graph,
                'test_symbol_imports': symbol_imports, 'qt_test_logs': test_logs,
                'dll_execution_or_copy': False, 'test_retries': 0}
    data = json.dumps(evidence, indent=2)
    if len(data.encode()) > 4 << 20:
        raise ValueError('Diagnostic evidence exceeds capacity')
    print('Windows native test failure evidence:\n' + data, flush=True)
    destination_value = environment.get('DOCKPIPE_VM_BUILD_DIAGNOSTICS')
    if destination_value:
        destination = Path(destination_value)
        if not destination.is_absolute() or destination.exists():
            raise ValueError('Use a fresh explicit native-test evidence directory')
        destination.mkdir(parents=True)
        (destination / 'windows-test-evidence.json').write_text(data + '\n', encoding='utf-8')
        for path in selected:
            shutil.copyfile(path, destination / path.name)
        for record in test_logs:
            shutil.copyfile(build / record['name'], destination / record['name'])
    return evidence
