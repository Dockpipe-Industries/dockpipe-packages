"""Seal a bounded public-data-only plan for transfer between trusted CI jobs."""
import copy
import hashlib
import json
import os
from pathlib import Path

from catalog_plan import json_bytes, relative_path
from catalog_s3 import file_digest
from catalog_transaction import PublicationStopped, fingerprint_inventory

MAX_PLAN_BYTES = 4 * 1024 * 1024


def write_exclusive(path, payload):
    with path.open('xb') as output:
        output.write(payload)
        output.flush()
        os.fsync(output.fileno())


def contained_file(root, relative):
    relative_path(relative)
    target = root / relative
    current = target
    while current != root:
        if current.is_symlink():
            raise PublicationStopped('Plan payload or ancestor is aliased')
        current = current.parent
    if not target.is_file():
        raise PublicationStopped('Plan payload is not a regular file')
    return target


def seal(plan, destination):
    root = Path(destination)
    if not root.is_absolute() or any(parent.is_symlink() for parent in root.parents):
        raise PublicationStopped('Plan output must be an absolute alias-free path')
    if root.exists():
        raise PublicationStopped('Plan output must be fresh; prior evidence is preserved')
    root.mkdir(mode=0o700)
    payload_root = root / 'payloads'
    payload_root.mkdir(mode=0o700)
    document = copy.deepcopy(plan)
    for item in document['objects']:
        payload = item.pop('bytes', None)
        if payload is None:
            continue
        if len(payload) != item['size_bytes'] or hashlib.sha256(payload).hexdigest() != item['sha256']:
            raise PublicationStopped('Generated object differs from planned digest')
        relative = 'payloads/' + item['sha256'] + '.json'
        target = root / relative
        if target.exists():
            if target.read_bytes() != payload:
                raise PublicationStopped('Generated payload identity collision')
        else:
            write_exclusive(target, payload)
        item['payload_file'] = relative
    pointer = document.pop('pointer_bytes')
    write_exclusive(root / 'pointer.json', pointer)
    document['pointer_file'] = 'pointer.json'
    document['pointer_sha256'] = hashlib.sha256(pointer).hexdigest()
    document['pointer_size_bytes'] = len(pointer)
    document['object_inventory_sha256'] = fingerprint_inventory(document['objects'])
    encoded = json_bytes(document)
    if len(encoded) > MAX_PLAN_BYTES:
        raise PublicationStopped('Sealed plan metadata capacity exceeded')
    write_exclusive(root / 'plan.json', encoded)
    return {'schema': 1, 'plan_sha256': hashlib.sha256(encoded).hexdigest(),
            'size_bytes': len(encoded), 'object_inventory_sha256': document['object_inventory_sha256']}


def load(directory, expected_sha256):
    root = Path(directory)
    if not root.is_absolute() or not root.is_dir() or root.is_symlink():
        raise PublicationStopped('Sealed plan root is unavailable or aliased')
    source = contained_file(root, 'plan.json')
    digest, _ = file_digest(source, MAX_PLAN_BYTES)
    if digest != expected_sha256:
        raise PublicationStopped('Sealed plan differs from admitted job output')
    document = json.loads(source.read_bytes())
    if document.get('object_inventory_sha256') != fingerprint_inventory(document['objects']):
        raise PublicationStopped('Sealed object inventory differs')
    for item in document['objects']:
        if 'payload_file' not in item:
            continue
        payload = contained_file(root, item['payload_file'])
        actual, size = file_digest(payload, item['size_bytes'])
        if actual != item['sha256'] or size != item['size_bytes']:
            raise PublicationStopped('Sealed generated object differs')
        item['bytes'] = payload.read_bytes()
    pointer = contained_file(root, document['pointer_file'])
    actual, size = file_digest(pointer, document['pointer_size_bytes'])
    if actual != document['pointer_sha256'] or size != document['pointer_size_bytes']:
        raise PublicationStopped('Sealed pointer bytes differ')
    document['pointer_bytes'] = pointer.read_bytes()
    return document
