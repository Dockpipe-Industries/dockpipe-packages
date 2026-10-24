"""Admit exact task-extracted Linux package libraries and copyright files."""
import hashlib
import json
import os
from pathlib import Path
import re


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def document(path):
    if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= 1 << 20:
        raise ValueError('Linux prefix receipt must be a bounded regular JSON file')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('Duplicate Linux prefix receipt field')
            result[key] = value
        return result
    return json.loads(path.read_bytes(), object_pairs_hook=unique)


class LinuxPrefix:
    def __init__(self, path):
        value = document(path)
        if (not isinstance(value, dict) or set(value) != {'schema', 'prefix', 'packages', 'libraries', 'licenses'}
                or type(value['schema']) is not int or value['schema'] != 1):
            raise ValueError('Unsupported explicit Linux prefix receipt')
        if not isinstance(value['prefix'], str):
            raise ValueError('Linux prefix path must be a string')
        self.root = Path(value['prefix'])
        if not self.root.is_absolute() or not self.root.is_dir() or any(p.is_symlink() for p in (self.root, *self.root.parents)):
            raise ValueError('Linux prefix must be an explicit contained regular directory')
        packages = value['packages']
        if not isinstance(packages, list) or not 0 < len(packages) <= 64:
            raise ValueError('Linux package inventory exceeds its capacity')
        self.packages = {}
        for package in packages:
            if (not isinstance(package, dict) or set(package) != {'id', 'package', 'version', 'archive_sha256'}
                    or not isinstance(package['id'], str) or not re.fullmatch(r'[A-Za-z0-9_.+-]{1,128}', package['id'])
                    or not isinstance(package['package'], str) or not re.fullmatch(r'[a-z0-9][a-z0-9.+-]{0,127}(?::amd64)?', package['package'])
                    or not isinstance(package['version'], str) or not 0 < len(package['version']) <= 128
                    or not isinstance(package['archive_sha256'], str) or not re.fullmatch('[0-9a-f]{64}', package['archive_sha256'])
                    or package['id'] in self.packages):
                raise ValueError('Linux package identity/hash is invalid or duplicate')
            self.packages[package['id']] = package
        self.licenses = self.records(value['licenses'], 256)
        self.libraries = self.records(value['libraries'], 64)
        if len({path.name for path in self.libraries}) != len(self.libraries):
            raise ValueError('Linux library basenames collide')

    def records(self, values, maximum):
        if not isinstance(values, list) or not 0 < len(values) <= maximum:
            raise ValueError('Linux prefix file inventory exceeds its capacity')
        records = {}
        seen = set()
        for value in values:
            if not isinstance(value, dict) or set(value) != {'path', 'sha256', 'size_bytes', 'package', 'version', 'archive_id'}:
                raise ValueError('Linux file receipt has missing or unknown fields')
            name = value['path']
            if (not isinstance(name, str) or not name or Path(name).is_absolute() or '\\' in name
                    or ':' in name or '..' in Path(name).parts or Path(name).as_posix() != name
                    or name.casefold() in seen):
                raise ValueError('Linux receipt path escapes or collides')
            seen.add(name.casefold())
            if not isinstance(value['archive_id'], str):
                raise ValueError('Linux file archive identity must be a string')
            package = self.packages.get(value['archive_id'])
            if package is None or any(value[field] != package[field] for field in ('package', 'version')):
                raise ValueError('Linux library/license owner differs from its exact archive')
            path = self.root / name
            if (any(p.is_symlink() for p in (path, *path.parents)) or not path.is_file()
                    or type(value['size_bytes']) is not int or not 0 < value['size_bytes'] <= 64 << 20
                    or path.stat().st_size != value['size_bytes']
                    or not isinstance(value['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', value['sha256'])
                    or checksum(path) != value['sha256']):
                raise ValueError('Linux library/license bytes differ from the public package receipt')
            records[path] = value
        return records

    def package_license(self, path):
        if path not in self.libraries:
            raise ValueError('Dependency is outside the declared Linux prefix; no host fallback')
        record = self.libraries[path]
        # Recheck bytes at the consumer boundary rather than accepting path-only identity.
        if (any(item.is_symlink() for item in (path, *path.parents)) or not path.is_file()
                or path.stat().st_size != record['size_bytes'] or checksum(path) != record['sha256']):
            raise ValueError('Declared Linux library changed after admission')
        candidates = [(name, entry) for name, entry in self.licenses.items()
                      if entry['archive_id'] == record['archive_id']]
        if len(candidates) != 1:
            raise ValueError('Linux library requires exactly one package copyright record')
        source, license_record = candidates[0]
        if (any(item.is_symlink() for item in (source, *source.parents)) or not source.is_file()
                or source.stat().st_size != license_record['size_bytes']
                or checksum(source) != license_record['sha256']):
            raise ValueError('Linux copyright changed after admission')
        return record['package'], record['version'], source

    def closure(self, dependencies):
        for name, path in dependencies.items():
            if path not in self.libraries or name != path.name:
                raise ValueError('GIO resolved outside its declared Linux prefix')
            self.package_license(path)
        return dependencies


def selected(environment=None):
    environment = os.environ if environment is None else environment
    mode = environment.get('DOCKPIPE_VM_LINUX_DEPENDENCY_MODE', 'host-dpkg')
    receipt = environment.get('DOCKPIPE_VM_LINUX_PREFIX_RECEIPT', '')
    if mode == 'host-dpkg' and not receipt:
        return None
    if mode != 'prefix' or not receipt or not Path(receipt).is_absolute():
        raise ValueError('Select explicit host-dpkg or a complete Linux prefix receipt; no fallback')
    return LinuxPrefix(Path(receipt))
