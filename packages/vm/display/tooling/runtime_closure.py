"""Native runtime dependency selection and relocation for package display modules."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import json

from tooling.pinned_licenses import pinned_license_files

WINDOWS_SYSTEM_DLLS = {
    'advapi32.dll', 'authz.dll', 'bcrypt.dll', 'cfgmgr32.dll', 'comctl32.dll',
    'comdlg32.dll', 'crypt32.dll', 'dbghelp.dll', 'dhcpcsvc.dll', 'dnsapi.dll',
    'dwmapi.dll', 'gdi32.dll', 'imm32.dll', 'iphlpapi.dll', 'kernel32.dll',
    'mpr.dll', 'msvcrt.dll', 'ncrypt.dll', 'netapi32.dll', 'normaliz.dll',
    'ntdll.dll', 'ole32.dll', 'oleaut32.dll', 'powrprof.dll', 'psapi.dll',
    'rpcrt4.dll', 'secur32.dll', 'setupapi.dll', 'shell32.dll', 'shlwapi.dll',
    'ucrtbase.dll', 'user32.dll', 'userenv.dll', 'usp10.dll', 'uxtheme.dll',
    'version.dll', 'wevtapi.dll', 'winhttp.dll', 'winmm.dll', 'winspool.drv',
    'ws2_32.dll', 'wtsapi32.dll',
}


# These redistributables belong to the exact MSVC/Qt host ABI. Their presence
# must be qualified by the host; never harvest machine-wide System32 binaries.
WINDOWS_HOST_CRT_DLLS = {
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll', 'vccorlib140.dll', 'concrt140.dll',
}


def capture(arguments):
    return subprocess.check_output([str(item) for item in arguments], text=True, timeout=30)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_licenses(source, destination, prefix):
    files = [path for path in source.rglob('*') if path.is_file()]
    if not files or len(files) > 256 or sum(path.stat().st_size for path in files) > 16 * 1024 * 1024:
        raise ValueError('Missing or out-of-bounds native dependency license inventory')
    for path in files:
        target = destination / prefix / path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)


def windows_closure(module, destination, gio, qt, inspector):
    if not inspector.is_file():
        raise ValueError('Windows runtime closure requires an explicit objdump or dumpbin executable')
    directories = [gio / 'bin', qt / 'bin']
    result = {}
    pending = [module]
    while pending:
        binary = pending.pop()
        if inspector.name.lower().startswith('dumpbin'):
            output = capture([inspector, '/DEPENDENTS', binary])
            names = re.findall(r'^\s+(\S+\.dll)\s*$', output, re.MULTILINE | re.IGNORECASE)
        else:
            output = capture([inspector, '-p', binary])
            names = re.findall(r'DLL Name:\s+(\S+)', output)
        for name in names:
            key = name.lower()
            if key in WINDOWS_SYSTEM_DLLS or key in WINDOWS_HOST_CRT_DLLS or key.startswith(('api-ms-win-', 'ext-ms-win-', 'qt6')):
                continue
            if re.match(r'^(?:msvcp|vcruntime|concrt|vccorlib)[0-9]', key):
                raise ValueError('Unqualified host CRT dependency: ' + name)
            if key in result:
                continue
            source = next((directory / name for directory in directories if (directory / name).is_file()), None)
            if source is None:
                raise ValueError('Unresolved native runtime DLL: ' + name)
            if len(result) >= 64:
                raise ValueError('Native runtime DLL closure exceeds bound')
            target = destination / name
            shutil.copyfile(source, target)
            result[key] = {'name': name, 'sha256': digest(target), 'source': 'selected-native-prefix'}
            pending.append(source)
    copy_licenses(gio / 'share/licenses', destination / 'licenses', 'native-prefix')
    return list(result.values())


def macho_dependencies(binary):
    return [line.strip().split(' (', 1)[0] for line in capture(['otool', '-L', binary]).splitlines()[1:]]


def macos_closure(module, destination, gio, brew, admitted_libraries=None):
    admitted_libraries = admitted_libraries or {}
    libraries = {}
    references = {}
    pending = [module]
    while pending:
        binary = pending.pop()
        for reference in macho_dependencies(binary):
            if reference.startswith(('/usr/lib/', '/System/Library/')) or '.framework/' in reference:
                continue
            source = Path(reference)
            if reference.startswith('@rpath/'):
                source = admitted_libraries.get(Path(reference).name, gio / 'lib' / Path(reference).name)
            elif reference.startswith('@loader_path/'):
                source = binary.parent / reference.removeprefix('@loader_path/')
            source = source.resolve()
            admitted = source in admitted_libraries.values()
            if not source.is_file() or (not admitted and not source.is_relative_to(brew.resolve())):
                raise ValueError('Unresolved or unadmitted Mach-O dependency: ' + Path(reference).name)
            name = source.name
            references[reference] = name
            if name in libraries:
                if libraries[name] != source:
                    raise ValueError('Mach-O dependency basename collision')
                continue
            if len(libraries) >= 64:
                raise ValueError('Mach-O dependency closure exceeds bound')
            libraries[name] = source
            pending.append(source)
    runtime = destination / 'lib'
    runtime.mkdir(exist_ok=True)
    for name, source in libraries.items():
        shutil.copyfile(source, runtime / name)
    records = []
    copied = [module] + [runtime / name for name in libraries]
    for binary in copied:
        for reference in macho_dependencies(binary):
            if reference in references:
                relative = '@loader_path/' + ('lib/' if binary == module else '') + references[reference]
                subprocess.run(['install_name_tool', '-change', reference, relative, str(binary)], check=True)
        if binary != module:
            subprocess.run(['install_name_tool', '-id', '@rpath/' + binary.name, str(binary)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', str(binary)], check=True)
    formulas = {}
    cellar = brew / 'Cellar'
    for source in libraries.values():
        if source in admitted_libraries.values():
            continue
        if not source.is_relative_to(cellar):
            raise ValueError('No admitted Homebrew dependency provenance')
        parts = source.relative_to(cellar).parts
        formulas[parts[0]] = cellar / parts[0] / parts[1]
    license_provenance = []
    for name, formula in formulas.items():
        candidates = list(formula.rglob('COPYING*')) + list(formula.rglob('LICENSE*'))
        files = [path for path in candidates if path.is_file()]
        if not files:
            files, provenance = pinned_license_files(name, formula.name.split('_', 1)[0])
            license_provenance.append(provenance)
        if len(files) > 128 or sum(path.stat().st_size for path in files) > 16 * 1024 * 1024:
            raise ValueError('Out-of-bounds dependency license text: ' + name)
        for index, path in enumerate(files):
            target = destination / 'licenses' / name / (str(index) + '-' + path.name)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    (destination / 'native-license-provenance.json').write_text(
        json.dumps({'schema': 1, 'upstream_sources': license_provenance}, indent=2) + '\n')
    for name in libraries:
        records.append({'name': name, 'sha256': digest(runtime / name), 'source': 'selected-homebrew-prefix'})
    return records
