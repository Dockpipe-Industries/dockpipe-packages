#!/usr/bin/env python3
"""Bounded manifest-to-S3 transfers. No archive creation or download verification."""
import concurrent.futures
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import threading
from urllib.parse import urlsplit


class BulkError(Exception):
    """An expected input, remote identity or transfer failure."""


def canonical_hash(value):
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def read_json(path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def safe_path(value, base):
    path = Path(value)
    if ".." in path.parts:
        raise BulkError("Parent path traversal is not allowed")
    if not path.is_absolute():
        path = base / path
    path = path.absolute()
    if path.resolve() != path:
        raise BulkError(f"Symlink paths are not allowed: {path}")
    return path


def bounded_integer(environment, name, default, minimum, maximum):
    value = int(environment.get(name, str(default)))
    if not minimum <= value <= maximum:
        raise BulkError(f"{name} must be between {minimum} and {maximum}")
    return value


class BulkUpload:
    def __init__(self, environment):
        self.endpoint = environment.get("R2_ENDPOINT_URL", "").rstrip("/")
        endpoint = urlsplit(self.endpoint)
        if (endpoint.scheme != "https" or not endpoint.hostname or endpoint.username
                or endpoint.password or endpoint.path or endpoint.query or endpoint.fragment):
            raise BulkError("R2_ENDPOINT_URL must be an HTTPS origin without credentials")
        self.bucket = environment.get("R2_BUCKET", "")
        if not re.fullmatch(r"[a-z0-9][a-z0-9.-]{1,61}[a-z0-9]", self.bucket):
            raise BulkError("R2_BUCKET must be a valid bucket name")
        self.workers = bounded_integer(environment, "R2_BULK_OBJECT_WORKERS", 4, 1, 16)
        self.multipart_workers = bounded_integer(environment, "R2_BULK_MULTIPART_WORKERS", 1, 1, 8)
        self.part_mib = bounded_integer(environment, "R2_BULK_PART_MIB", 16, 8, 256)
        request_limit = bounded_integer(environment, "R2_BULK_MAX_REQUESTS", 4, 1, 16)
        if self.workers * self.multipart_workers > request_limit:
            raise BulkError("Object workers times multipart workers exceeds R2_BULK_MAX_REQUESTS")
        manifest_value = environment.get("R2_BULK_MANIFEST", "")
        state_value = environment.get("R2_BULK_STATE", "")
        if not manifest_value or not state_value:
            raise BulkError("Set R2_BULK_MANIFEST and R2_BULK_STATE")
        self.manifest_path = safe_path(manifest_value, Path.cwd())
        self.state_path = safe_path(state_value, Path.cwd())
        manifest = read_json(self.manifest_path)
        if manifest.get("schema") != 1 or not isinstance(manifest.get("objects"), list):
            raise BulkError("Manifest requires schema 1 and an objects list")
        self.objects = []
        keys = set()
        for entry in manifest["objects"]:
            key = entry.get("key", "")
            if (not isinstance(key, str) or not key or len(key.encode()) > 1024
                    or key.startswith("/") or "\\" in key or any(ord(c) < 32 for c in key)
                    or any(part in ("", ".", "..") for part in key.split("/"))):
                raise BulkError("Object keys must be nonempty safe relative keys")
            if key in keys:
                raise BulkError(f"Duplicate object key: {key}")
            keys.add(key)
            size = entry.get("size")
            digest = entry.get("sha256", "")
            if type(size) is not int or size < 0 or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise BulkError(f"Invalid size or SHA256: {key}")
            if size > self.part_mib * 1024 * 1024 * 10000:
                raise BulkError("Object exceeds configured 10000-part limit; choose larger parts explicitly")
            source = safe_path(entry["source"], self.manifest_path.parent)
            if not source.is_file() or source.stat().st_size != size:
                raise BulkError(f"Missing source or size mismatch: {key}")
            if self.state_path in (source, self.manifest_path):
                raise BulkError("State must not overwrite sources or the manifest")
            publish_last = entry.get("publish_last", False)
            if type(publish_last) is not bool:
                raise BulkError("publish_last must be a boolean")
            self.objects.append({"source": str(source), "key": key, "size": size,
                                 "sha256": digest, "publish_last": publish_last})
        if self.state_path == self.manifest_path:
            raise BulkError("State must not overwrite the manifest")
        self.trusted = {}
        receipts_value = environment.get("R2_BULK_TRUSTED_RECEIPTS", "")
        if receipts_value:
            receipts_path = safe_path(receipts_value, Path.cwd())
            if self.state_path == receipts_path:
                raise BulkError("State must not overwrite trusted receipts")
            receipts = read_json(receipts_path)
            if (receipts.get("schema") != 1 or receipts.get("endpoint") != self.endpoint
                    or receipts.get("bucket") != self.bucket):
                raise BulkError("Trusted receipts must bind the exact endpoint and bucket")
            for receipt in receipts.get("objects", []):
                key = receipt.get("key")
                if receipt.get("verification") != "full-get-sha256":
                    raise BulkError("Trusted receipts require explicit full-get-sha256 verification")
                if key in self.trusted:
                    raise BulkError("Duplicate trusted receipt key")
                self.trusted[key] = receipt
        self.identity = canonical_hash({"endpoint": self.endpoint, "bucket": self.bucket,
                                        "objects": self.objects, "trusted": self.trusted})
        self.mutex = threading.Lock()
        self.stop = threading.Event()
        self.state = {"schema": 1, "identity": self.identity, "endpoint": self.endpoint,
                      "bucket": self.bucket, "objects": {}, "phase": "planned"}

    @contextlib.contextmanager
    def locked(self):
        self.state_path.parent.mkdir(parents=True, exist_ok=True)
        lock_path = safe_path(str(self.state_path) + ".lock", Path.cwd())
        with lock_path.open("a+") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise BulkError("Another bulk process owns this state") from error
            if self.state_path.exists():
                state = read_json(self.state_path)
                if state.get("schema") != 1 or state.get("identity") != self.identity:
                    raise BulkError("State belongs to another manifest or destination")
                self.state = state
            yield

    def save(self):
        descriptor, name = tempfile.mkstemp(prefix=".r2-bulk-", dir=self.state_path.parent)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(self.state, stream, sort_keys=True, indent=2)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(name, self.state_path)
            directory = os.open(self.state_path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    def record(self, entry, verification, head):
        with self.mutex:
            self.state["objects"][entry["key"]] = {
                "size": entry["size"], "sha256": entry["sha256"],
                "verification": verification, "etag": head.get("ETag"),
            }
            self.save()
        print(json.dumps({"key": entry["key"], "status": verification}), flush=True)

    def head(self, client, entry):
        try:
            return client.head_object(Bucket=self.bucket, Key=entry["key"])
        except Exception as error:
            response = getattr(error, "response", {})
            code = response.get("Error", {}).get("Code")
            if str(code) in ("404", "NoSuchKey", "NotFound"):
                return None
            raise

    def remote_verification(self, entry, head):
        if head["ContentLength"] != entry["size"]:
            raise BulkError(f"Remote size mismatch; refusing overwrite: {entry['key']}")
        etag = head.get("ETag", "").strip('"')
        previous = self.state["objects"].get(entry["key"], {})
        if (previous.get("verification") == "head-etag-and-local-sha256"
                and previous.get("sha256") == entry["sha256"]
                and previous.get("size") == entry["size"]):
            if etag != previous.get("etag", "").strip('"'):
                raise BulkError(f"Remote ETag changed from durable receipt: {entry['key']}")
            return "head-etag-and-local-sha256"
        digest = head.get("Metadata", {}).get("sha256")
        if digest is not None:
            if digest != entry["sha256"]:
                raise BulkError(f"Remote SHA metadata mismatch: {entry['key']}")
            return "head-size-and-sha256-metadata"
        receipt = self.trusted.get(entry["key"], {})
        if receipt.get("size") == entry["size"] and receipt.get("sha256") == entry["sha256"]:
            if receipt.get("etag") and etag != receipt["etag"].strip('"'):
                raise BulkError(f"Remote ETag changed from trusted receipt: {entry['key']}")
            return "head-size-and-trusted-full-get-receipt"
        raise BulkError(f"Remote object has no matching SHA metadata or trusted receipt: {entry['key']}")

    def hash_source(self, stream):
        before = os.fstat(stream.fileno())
        sha256 = hashlib.sha256()
        part_digests = []
        while True:
            block = stream.read(self.part_mib * 1024 * 1024)
            if not block:
                break
            sha256.update(block)
            part_digests.append(hashlib.md5(block, usedforsecurity=False).digest())
        digest = sha256.hexdigest()
        if before.st_size < self.part_mib * 1024 * 1024:
            expected_etag = (part_digests[0].hex() if part_digests
                             else hashlib.md5(b"", usedforsecurity=False).hexdigest())
        else:
            expected_etag = hashlib.md5(b"".join(part_digests), usedforsecurity=False).hexdigest()
            expected_etag += f"-{len(part_digests)}"
        return digest, expected_etag

    def reconcile_remote(self, entry, head):
        verification = self.remote_verification(entry, head)
        if verification == "head-size-and-sha256-metadata":
            # Metadata alone must not turn a previously rejected ETag into a
            # successful resume. Reconcile it against the actual local bytes.
            with Path(entry["source"]).open("rb") as stream:
                digest, expected_etag = self.hash_source(stream)
            if digest != entry["sha256"] or head.get("ETag", "").strip('"') != expected_etag:
                raise BulkError(f"Existing object does not match local content: {entry['key']}")
            verification = "head-etag-and-local-sha256"
        return verification

    def transfer_one(self, client, config, entry):
        if self.stop.is_set():
            return
        head = self.head(client, entry)
        if head is not None:
            verification = self.reconcile_remote(entry, head)
            self.record(entry, verification, head)
            return
        source = Path(entry["source"])
        with source.open("rb") as stream:
            before = os.fstat(stream.fileno())
            digest, expected_etag = self.hash_source(stream)
            if before.st_size != entry["size"] or digest != entry["sha256"]:
                raise BulkError(f"Local SHA256 mismatch: {entry['key']}")
            stream.seek(0)
            if self.stop.is_set():
                return
            # Filename uploads stream seekable parts from disk instead of buffering
            # file-object multipart chunks in memory. Sources must remain immutable.
            client.upload_file(str(source), self.bucket, entry["key"],
                                  ExtraArgs={"Metadata": {"sha256": digest}}, Config=config)
            after = os.fstat(stream.fileno())
        current = source.stat()
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (
                after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns) or (
                before.st_ino, before.st_mtime_ns, before.st_ctime_ns) != (
                current.st_ino, current.st_mtime_ns, current.st_ctime_ns):
            raise BulkError(f"Source changed during upload: {entry['key']}")
        head = self.head(client, entry)
        if head is None:
            raise BulkError(f"Uploaded object is missing: {entry['key']}")
        if head.get("ETag", "").strip('"') != expected_etag:
            raise BulkError(f"Uploaded ETag differs from locally hashed bytes: {entry['key']}")
        self.remote_verification(entry, head)
        self.record(entry, "head-etag-and-local-sha256", head)

    def run_bounded(self, entries, action):
        # Bound submitted work as well as active multipart transfers.
        iterator = iter(entries)
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.workers) as pool:
            pending = set()
            for entry in iterator:
                pending.add(pool.submit(action, entry))
                if len(pending) == self.workers:
                    break
            try:
                while pending:
                    done, pending = concurrent.futures.wait(
                        pending, return_when=concurrent.futures.FIRST_COMPLETED)
                    for future in done:
                        future.result()
                    for _ in done:
                        entry = next(iterator, None)
                        if entry is not None:
                            pending.add(pool.submit(action, entry))
            except BaseException:
                self.stop.set()
                for future in pending:
                    future.cancel()
                raise

    def verify_one(self, client, entry):
        if self.stop.is_set():
            return
        head = self.head(client, entry)
        if head is None:
            raise BulkError(f"Missing remote object: {entry['key']}")
        self.record(entry, self.reconcile_remote(entry, head), head)

    def transfer(self, client, config):
        self.state["phase"] = "transferring"
        self.save()
        payloads = [entry for entry in self.objects if not entry["publish_last"]]
        markers = [entry for entry in self.objects if entry["publish_last"]]
        self.run_bounded(payloads, lambda entry: self.transfer_one(client, config, entry))
        self.run_bounded(payloads, lambda entry: self.verify_one(client, entry))
        self.state["phase"] = "payloads-verified-head"
        self.save()
        # Completion markers never enter the queue before all payloads verify.
        self.run_bounded(markers, lambda entry: self.transfer_one(client, config, entry))
        self.state["phase"] = "transferred"
        self.save()

    def verify(self, client):
        self.run_bounded(self.objects, lambda entry: self.verify_one(client, entry))
        self.state["phase"] = "verified-head"
        self.save()
        print(json.dumps({"phase": "verified-head", "objects": len(self.objects),
                          "bytes": sum(entry["size"] for entry in self.objects),
                          "full_get_performed": False}), flush=True)


def sdk(upload):
    try:
        import boto3
        from boto3.s3.transfer import TransferConfig
        from botocore.config import Config
    except ImportError as error:
        raise BulkError("Install the package's pinned requirements.lock in the selected Python environment") from error
    if boto3.__version__ != "1.40.6":
        raise BulkError("Expected pinned boto3 1.40.6")
    client = boto3.client(
        "s3", endpoint_url=upload.endpoint, region_name="auto",
        config=Config(max_pool_connections=upload.workers * upload.multipart_workers,
                      connect_timeout=10, read_timeout=60,
                      retries={"mode": "standard", "total_max_attempts": 3},
                      request_checksum_calculation="when_required",
                      response_checksum_validation="when_required"))
    config = TransferConfig(multipart_threshold=upload.part_mib * 1024 * 1024,
                            multipart_chunksize=upload.part_mib * 1024 * 1024,
                            max_concurrency=upload.multipart_workers,
                            preferred_transfer_client="classic")
    return client, config


def main():
    try:
        phase = sys.argv[1] if len(sys.argv) == 2 else ""
        if phase not in ("preflight", "transfer", "verify", "status"):
            raise BulkError("Usage: r2-bulk.py preflight|transfer|verify|status")
        upload = BulkUpload(os.environ)
        with upload.locked():
            if phase == "status":
                print(json.dumps(upload.state, sort_keys=True))
            elif phase == "preflight":
                upload.save()
                print(json.dumps({"phase": "planned", "objects": len(upload.objects),
                                  "bytes": sum(entry["size"] for entry in upload.objects),
                                  "object_workers": upload.workers,
                                  "multipart_workers": upload.multipart_workers,
                                  "identity": upload.identity}))
            else:
                client, config = sdk(upload)
                try:
                    if phase == "transfer":
                        upload.transfer(client, config)
                    else:
                        upload.verify(client)
                finally:
                    client.close()
        return 0
    except KeyboardInterrupt:
        print("Bulk transfer interrupted; rerun with the same manifest and state", file=sys.stderr)
        return 130
    except Exception as error:
        # SDK exceptions may include signed URLs or credentials; print only safe types.
        message = str(error) if isinstance(error, BulkError) else type(error).__name__
        print(f"r2bulk: {message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
