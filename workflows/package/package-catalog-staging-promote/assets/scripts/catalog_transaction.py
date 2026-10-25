"""Immutable object publication and one conditional optional-catalog pointer change.

Providers must expose actual object-store ETags, never CDN ETags. A failed or
uncertain mutation ends this invocation. A new invocation reconciles by reading
all objects before attempting any conditional mutation.
"""
from dataclasses import dataclass
import hashlib
import time

from catalog_plan import relative_path


class PublicationStopped(RuntimeError):
    pass


@dataclass(frozen=True)
class StoredObject:
    sha256: str
    size_bytes: int
    etag: str
    cache_control: str = ''


def fingerprint(payload):
    return hashlib.sha256(payload).hexdigest()


class CatalogTransaction:
    def __init__(self, provider, public_reader, consumer_validator, checkpoint,
                 clock=time.monotonic, deadline_seconds=2700):
        self.provider = provider
        self.public_reader = public_reader
        self.consumer_validator = consumer_validator
        self.checkpoint = checkpoint
        self.clock = clock
        self.deadline = clock() + deadline_seconds

    def record(self, stage, **fields):
        if self.clock() >= self.deadline:
            raise PublicationStopped('Publication deadline exceeded')
        self.checkpoint({'stage': stage, **fields})

    @staticmethod
    def matches(actual, expected):
        return (actual is not None and expected is not None and actual.sha256 == expected['sha256']
                and actual.size_bytes == expected['size_bytes'])

    def public_matches(self, key, expected):
        actual = self.public_reader(key, expected['size_bytes'])
        if not self.matches(actual, expected):
            raise PublicationStopped('Public object readback differs: ' + key)

    def stage_objects(self, plan, baseline, payload_source):
        prefix = relative_path(plan['prefix']) + '/'
        pointer_key = plan['pointer_key']
        if not prefix.startswith('packages/catalog/candidates/') or pointer_key != 'packages/latest.json':
            raise PublicationStopped('Destination authority is invalid')
        keys = [relative_path(item['key']) for item in plan['objects']]
        if len(set(keys)) != len(keys) or any(not key.startswith(prefix) for key in keys):
            raise PublicationStopped('Immutable object authority is invalid')
        pointer = plan['pointer_bytes']
        desired = {'sha256': fingerprint(pointer), 'size_bytes': len(pointer)}
        self.record('begin', product_source_sha=plan['product_source_sha'],
                    orchestration_source_sha=plan['orchestration_source_sha'])
        current = self.provider.read(pointer_key, 1048576)
        already_promoted = self.matches(current, desired)
        if not already_promoted:
            if not self.matches(current, baseline) and not (current is None and baseline is None):
                raise PublicationStopped('Admin pointer no longer matches reviewed baseline')
            if current is not None and not current.etag:
                raise PublicationStopped('Admin pointer has no usable ETag')
            public = self.public_reader(pointer_key, 1048576)
            if not self.matches(public, baseline) and not (public is None and baseline is None):
                raise PublicationStopped('Public and admin baseline pointer disagree')
        # Reconcile immutable objects even if a prior invocation changed the pointer.
        for item in plan['objects']:
            self.record('inspect-object', key=item['key'])
            existing = self.provider.read(item['key'], item['size_bytes'])
            if existing is not None and not self.matches(existing, item):
                raise PublicationStopped('Immutable object collision: ' + item['key'])
            if existing is None:
                source = payload_source(item)
                self.record('before-create', key=item['key'], sha256=item['sha256'])
                # Provider verifies source size/hash and sends If-None-Match: *.
                # Any exception, including uncertain transport, stops without retry.
                self.provider.create(item['key'], source, item)
                actual = self.provider.read(item['key'], item['size_bytes'])
                if not self.matches(actual, item):
                    raise PublicationStopped('Immutable create readback differs: ' + item['key'])
            self.public_matches(item['key'], item)
            self.record('object-verified', key=item['key'], sha256=item['sha256'])
        return {'schema': 1, 'pointer_sha256': desired['sha256'], 'object_inventory_sha256': fingerprint_inventory(plan['objects']),
                'pointer_before': None if current is None else {
                    'sha256': current.sha256, 'size_bytes': current.size_bytes, 'etag': current.etag},
                'already_promoted': already_promoted}

    def finish(self, plan, baseline, staged):
        if (staged.get('object_inventory_sha256') != fingerprint_inventory(plan['objects'])
                or staged.get('pointer_sha256') != fingerprint(plan['pointer_bytes'])):
            raise PublicationStopped('Staged object inventory differs')
        pointer_key = plan['pointer_key']
        if pointer_key != 'packages/latest.json':
            raise PublicationStopped('Mutable destination authority is invalid')
        pointer = plan['pointer_bytes']
        desired = {'sha256': fingerprint(pointer), 'size_bytes': len(pointer)}
        current = self.provider.read(pointer_key, 1048576)
        already_promoted = self.matches(current, desired)
        if not already_promoted:
            prior = staged.get('pointer_before')
            if (not self.matches(current, baseline) and not (current is None and baseline is None)):
                raise PublicationStopped('Admin pointer changed during native consumer jobs')
            if (current is None) != (prior is None):
                raise PublicationStopped('Admin pointer presence changed during native consumer jobs')
            if current is not None and (not self.matches(current, prior) or current.etag != prior.get('etag')):
                raise PublicationStopped('Admin pointer ETag changed during native consumer jobs')
            public = self.public_reader(pointer_key, 1048576)
            if not self.matches(public, baseline) and not (public is None and baseline is None):
                raise PublicationStopped('Public baseline pointer changed during native consumer jobs')
        for item in plan['objects']:
            self.record('recheck-object', key=item['key'])
            if not self.matches(self.provider.read(item['key'], item['size_bytes']), item):
                raise PublicationStopped('Staged immutable object disappeared or changed')
            self.public_matches(item['key'], item)
        # The final CI job admits only artifacts from its required native consumer jobs.
        # The callback validates their actual CLI identities, job/run binding and inventories.
        receipt = self.consumer_validator(plan)
        wrapper = next(item for item in plan['objects'] if item['role'] == 'release-manifest')
        if (receipt.get('wrapper_sha256') != wrapper['sha256']
                or receipt.get('object_inventory_sha256') != fingerprint_inventory(plan['objects'])
                or receipt.get('exit_code') != 0
                or receipt.get('source_independent') is not True):
            raise PublicationStopped('Actual consumer receipt is not bound to this publication')
        self.record('consumer-verified', receipt_sha256=fingerprint_receipt(receipt))
        if not already_promoted:
            self.record('before-pointer-cas', baseline_sha256=None if baseline is None else baseline['sha256'])
            # ETag comes exclusively from the admin GET above. No check-then-PUT.
            self.provider.compare_and_swap(pointer_key, pointer,
                                           None if current is None else current.etag)
        actual = self.provider.read(pointer_key, 1048576)
        if not self.matches(actual, desired) or actual.cache_control != 'no-cache':
            raise PublicationStopped('Admin pointer final readback or cache policy differs')
        self.public_matches(pointer_key, desired)
        self.record('complete', pointer_sha256=desired['sha256'], resumed=already_promoted)
        return {'pointer_sha256': desired['sha256'], 'consumer_receipt': receipt,
                'resumed': already_promoted}


    def publish(self, plan, baseline, payload_source):
        staged = self.stage_objects(plan, baseline, payload_source)
        return self.finish(plan, baseline, staged)


def fingerprint_inventory(objects):
    import json
    admitted = [{key: item[key] for key in ['key', 'sha256', 'size_bytes']} for item in objects]
    return fingerprint(json.dumps(admitted, sort_keys=True, separators=(',', ':')).encode())


def fingerprint_receipt(receipt):
    import json
    return fingerprint(json.dumps(receipt, sort_keys=True, separators=(',', ':')).encode())
