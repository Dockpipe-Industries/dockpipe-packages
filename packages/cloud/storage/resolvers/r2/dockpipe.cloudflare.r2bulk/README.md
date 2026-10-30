# Manifest bulk upload to R2

`dockpipe.cloudflare.r2bulk` 0.1.0 is a separate package-owned workflow. It uploads
existing files to explicit immutable object keys without creating archives. The
existing `dockpipe.cloudflare.r2upload` archive workflow is unchanged. This source
version is a local candidate until separately qualified and published; it is not
a claimed released catalog pin.

The three host stages validate a plan, transfer with one persistent S3 client,
and verify current HEAD responses. During transfer all payloads must verify before
objects marked `publish_last: true` enter the upload queue. A completion marker
means the manifest's payloads passed these checks; it does not assert a restore
or reconstruction test.

## Dependency and consumer contract

Use Python 3.10+ on Linux or macOS. Prepare an explicit virtual environment and
install the accompanying `requirements.lock` with `pip install --require-hashes
-r requirements.lock`. Set `R2_BULK_PYTHON` to that environment's absolute Python executable. The workflow
never installs dependencies or resolves credentials itself. The SDK uses standard
AWS credentials/profile configuration; consumers own secret references. It does
not invoke AWS CLI, Wrangler, Dockpipe release upload or Terraform per object.

Pass `R2_BUCKET`, an HTTPS origin in `R2_ENDPOINT_URL`, `R2_BULK_MANIFEST`, and
`R2_BULK_STATE` through the consumer's packaged workflow call. Use one stable state
path for the exact manifest and destination. Example manifest:

```json
{
  "schema": 1,
  "objects": [
    {
      "source": "objects/existing-content.bin",
      "key": "objects/immutable-content-key",
      "size": 3,
      "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "publish_last": false
    }
  ]
}
```

The example SHA256 corresponds to the three bytes `abc`. Relative sources resolve
against the manifest directory; absolute sources are also accepted. Relative
parent traversal, symlink paths, unsafe keys, duplicate destination keys and
source/state collisions fail before provider operations. Sources must remain
immutable for the invocation. A source stat change during upload fails the run.
State is bound to exact canonical source mappings, destination and trusted receipts;
changing those inputs requires a different state path. Duplicate sources with
different keys are permitted; mapping and CAS deduplication belong to the consumer.

## Resume and verification

Missing remote objects are locally SHA256 checked before upload. The same pass
calculates MD5 part digests and the expected R2 ETag. After upload, HEAD must match
size, SHA256 metadata and that ETag. Single-part and configured multipart shapes
are both checked. The ETag provides transport/content identity for this R2 path;
SHA256 metadata alone is a declaration rather than an independently computed
server SHA256. ETag is not a general S3 cryptographic SHA256 guarantee. See the
[R2 upload ETag contract](https://developers.cloudflare.com/r2/objects/upload-objects/)
and [R2 S3 API compatibility](https://developers.cloudflare.com/r2/api/s3/api/).

Existing objects are never deliberately overwritten: mismatched size, metadata,
or recorded ETag fails closed. Existing matching SHA256 metadata and size are reconciled against locally
computed SHA256 and ETag before reuse when no package journal exists. A prior package journal
with a verified local SHA256/ETag also requires the current ETag to match. HEAD
checks are repeated on resume; the journal alone cannot suppress remote checks.
The object namespace must have one coordinated writer: HEAD followed by managed
multipart upload is not an atomic conditional create against concurrent writers.

For objects without SHA256 metadata, a consumer may explicitly supply
`R2_BULK_TRUSTED_RECEIPTS` referring to a JSON file:

```json
{
  "schema": 1,
  "endpoint": "https://example.r2.cloudflarestorage.com",
  "bucket": "example-bucket",
  "objects": [
    {
      "key": "objects/immutable-content-key",
      "size": 3,
      "sha256": "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "verification": "full-get-sha256",
      "etag": "optional exact previously verified ETag"
    }
  ]
}
```

The adapter must derive these records from completed full-download SHA256 checks;
this workflow does not create that evidence. Endpoint, bucket, key, size and SHA256
must match exactly. If an ETag was recorded it must match the current HEAD too.
Without a recorded ETag, trusted historical bytes plus current size remain weaker
than current independent content verification; the receipt label preserves that
limitation. Unknown objects with no usable metadata or receipt require explicit
reconciliation outside this workflow. No ordinary upload stage downloads objects.

Each completed object is recorded using an atomic replace, file fsync and directory
fsync. A nonblocking advisory lock prevents concurrent processes sharing state.
An interruption preserves completed records; a completed PUT whose response was
lost can be recovered by its current HEAD metadata. Managed multipart transfer
retries are bounded and normally abort failed uploads. Hard termination can leave
incomplete multipart uploads, so bucket lifecycle cleanup remains the operator's
responsibility. No remote deletion, bucket mutation or cleanup runs here.

## Resource bounds and status

Defaults are four object workers, one multipart worker per object, 16 MiB parts,
and at most four concurrent transfer requests. `R2_BULK_OBJECT_WORKERS` accepts
1–16, `R2_BULK_MULTIPART_WORKERS` accepts 1–8, and their product must not exceed
`R2_BULK_MAX_REQUESTS` (default 4, explicit maximum 16). `R2_BULK_PART_MIB` accepts
8–256. Inputs requiring more than 10,000 configured parts are rejected so the SDK
cannot silently change part sizes and invalidate the expected ETag calculation.
Filename-based managed uploads stream parts from disk; local hash buffers are
bounded by workers and part size. These bounds do not claim a measured RSS ceiling.

`assets/scripts/r2-bulk.py status` prints the durable phase and per-object receipts.
`preflight`, `transfer` and `verify` can be run separately with the same environment.
The workflow runs all three in order; a failed stage returns nonzero. The final
`verified-head` summary reports object/byte totals and `full_get_performed: false`.

## Offline checks

With the pinned SDK Python selected:

```sh
python -B -m unittest discover -s packages/cloud/storage/tests -p test_r2_bulk.py -v
```

The suite checks fake-remote missing/existing/mismatch, corrupted ETags, trusted
receipts, interruption/resume, marker ordering, concurrency, local corruption,
path safety, state binding and locking. Real pinned boto3 plus botocore Stubber
also exercises single-part and multipart request shapes and checksum settings
without network. SDK tests explicitly skip when that dependency is absent; a skip
is not SDK qualification. No live provider or native macOS proof is implied.
