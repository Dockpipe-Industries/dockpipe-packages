# Manifest bulk upload to R2

`dockpipe.cloudflare.r2bulk` 0.1.1 is a separate package-owned workflow. It uploads
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

Use Python 3.10+ on Linux, macOS or Windows with Bash available for the host stages. Prepare an explicit virtual environment and
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
      "publish_last": false,
      "content_type": "application/octet-stream",
      "cache_control": "public, max-age=31536000, immutable"
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

Optional `content_type` and `cache_control` fields set the object's HTTP
Content-Type and Cache-Control headers. Values must contain 1–1024 printable ASCII
characters and cannot be whitespace only. Explicit values become part of the plan
identity and are checked against current HEAD responses before reuse or completion,
including resume and trusted-receipt paths. Mismatches fail without overwrite.
When a field is omitted, the SDK default is retained and that header is not checked;
existing manifests and their state identities remain compatible. Adding or changing
an explicit header requires a new state file and cannot repair an existing immutable
object with different headers.

Bulk manifests contain immutable objects only. Consumers keep mutable catalogs,
latest pointers, release manifests and signature publication in their existing
ordered publisher, after this workflow succeeds. `publish_last` defers immutable
completion markers until payload verification; markers can run concurrently and
their order in the manifest does not define a mutable metadata transaction.

## Skip and dry run

`R2_BULK_SKIP` and `R2_BULK_DRY_RUN` accept only `true` or `false` and default to
`false`. Set `R2_BULK_SKIP=true` for an already-decided consumer skip: every stage
returns success with `phase: skipped` before validating destination, manifest or
SDK configuration. It performs no provider operations and writes no state. Skip
takes precedence over dry run. It does not create a verified receipt, and any
pre-existing state file remains unchanged.

Set `R2_BULK_DRY_RUN=true` to validate the manifest, local source paths and sizes,
resource bounds and state identity without loading the SDK or using credentials.
Each workflow stage records and reports `phase: dry-run` and
`provider_operations: false`. It does not hash source bytes, inspect remote objects
or claim transfer verification. It requires the same valid local inputs as
preflight. The flag is excluded from plan identity, so the same state path can be
used for a later real execution. A consumer must respect skip/dry-run mode when
checking its metadata-publication gate; only a successful real workflow produces
`phase: verified-head`. `R2_BULK_STATE` names a JSON file, not a directory.

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

Each completed object is recorded using atomic replace and file fsync. POSIX hosts
also fsync the state directory and report `durability: file-and-directory-fsync`.
Windows uses a one-byte nonblocking `msvcrt` advisory lock instead of `fcntl`, and
reports `durability: file-fsync-and-atomic-replace`: directory-fsync crash durability
is not claimed there. A nonblocking advisory lock prevents concurrent processes
sharing state.
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

## Offline qualification

The archive owns its complete test suite and `test.script` hook. Select a Python
with the exact locked dependencies and invoke the hook explicitly:

```sh
R2_BULK_PYTHON=/absolute/path/to/pinned-venv/bin/python bash /absolute/path/to/dockpipe.cloudflare.r2bulk/tests/run.sh
```

The hook fails on absent or mismatched locked dependencies before running tests.
It is safe to run from an extracted release archive with no repository or sibling
source checkout. In the configured producer repository the pinned Dockpipe CLI
also discovers the hook with `dockpipe package test --workdir /absolute/repository
--only dockpipe.cloudflare.r2bulk`; verify that it reports one executed package,
not a zero-count no-op. Package discovery requires the repository's configured
compile roots. A release producer can execute the archive-owned hook directly
to qualify the exported artifact.

Catalog test discovery remains available through
`python -B -m unittest discover -s packages/cloud/storage/tests -p test_r2_bulk.py -v`.
That direct unittest route permits SDK skips for lightweight development; skips
cannot qualify a release.

The suite checks fake-remote missing/existing/mismatch, corrupted ETags, trusted
receipts, interruption/resume, marker ordering, concurrency, local corruption,
path safety, state binding and locking. Real pinned boto3 plus botocore Stubber
exercises single-part and multipart request shapes, optional HTTP metadata,
checksum settings and provider errors without network. Skip and dry-run tests
prove that no SDK client is created and no verified phase is claimed. No live
provider, native macOS or native Windows proof is implied. Windows lock acquisition,
contention, release and weaker durability labels have mocked branch coverage;
actual Windows filesystem and interruption behavior still require native validation.
