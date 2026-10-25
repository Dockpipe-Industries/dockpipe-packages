# Reviewed promotion request

A request is an owning source JSON file below `release/`, selected by its exact
SHA256. There is intentionally no executable example with missing candidate or
runtime pins. The only permitted self-reference slot is
`promotion.orchestration_source_sha: null`; prepare binds it to its actual owning
staging workflow SHA. Every other input must exist and be independently reviewed.

The root fields are exact; unknown or missing root fields stop preparation:

| Field | Required content |
| --- | --- |
| `schema` | `1` |
| `promotion` | Product source40-SHA and exact native tag; null orchestration slot; staging channel and public origin; explicit core exclusion; eight owned identities; six-platform policy with all48 promote/retain/retire decisions and reasons |
| `candidate` | Genuine public native release: source/tag/release ID; successful owning run ID/attempt; exact seven published assets; three native outer archive pins; exact public raw source lock and dependencies-lock pins |
| `baseline_manifests` | Six exact public staging store manifest URL/SHA256/size triplets |
| `baseline_pointer` | Exact reviewed public/admin optional latest SHA256/size, or admitted absence; the actual S3 ETag is obtained at execution |
| `cli` | LinuxAMD64, WindowsAMD64 and MacARM64 exact public CLI archive URL/SHA256/size, version and inner executable SHA256/size |
| `destination` | Literal reference names `endpoint_reference: R2_ENDPOINT_URL` and `bucket_reference: DOCKPIPE_RELEASE_BUCKET`; `target_sha256` of `catalog_plan.json_bytes({endpoint, bucket})` using actual operator values |
| `aws_cli_archive` | Exact `archive` triplet from the source-bound official AWS CLI descriptor |
| `named_workflow_qualification` | Path below `release/catalog-qualification/`, SHA256 and size of the reviewed public evidence projection |
| `runtime` | Independent public runtime pointer/manifest pins and source/version; exact `cli_manifest` public CLI producer metadata; three platform package-owned `vmimage_run_sha256` values |

All file pins use `url`, `sha256`, `size_bytes`; native outer pins also bind
`manifest_sha256`. Candidate asset records additionally preserve actual immutable
GitHub asset IDs/names and native run identity, as admitted by
`catalog_candidate_gate.admit_candidate`. The native descriptor's artifact pins
must match those actual release assets. No handcrafted descriptor replaces
successful actual owning native jobs.

The named-workflow projection has schema1, `product_source_sha`, and three
`platforms`. Each binds `cli_sha256`, all eight `package_archives` SHA256 values,
`effective_vmimage_run_sha256`, empty `failed_checks`/`skipped_checks`, and bounded
`raw_receipts` references (`role`, `sha256`, `size_bytes`). This supplementary
review projection does not replace the required actual producer and three native
consumer jobs. It must describe the actual VM1.3.7 cohort and admitted public runtime/CLI inputs.

The runtime pin resolves only `/packages/runtime/latest.json`, which the catalog
publisher never writes. Empty or absent runtime package stores are valid. The
public CLI manifest must bind all five producer archives to its exact source,
release, executable inventories and owning checks. Each selected native CLI must
also match the product source lock. The SDK dependency lock binds the actual
published source digest, commit, archive and manifest; no sibling SDK is accepted.
Effective VM adapter bytes come from the hash-admitted `resolver/qemu` archive in
the actual native product, never a runtime source/core archive.

The named projection additionally requires `isolated_cli_acceptance`, a
`path`/`sha256`/`size_bytes` reference beneath `release/catalog-qualification/`.
That reviewed raw Linux capsule receipt binds the actual public `cli_sha256`,
`cli_version`, and `cli_source_sha`, `network: none`, and
`source_or_sibling_mounts: false`. Its checks include `typed offline init`,
`execute real host starter`, `three SDK resource languages`, and
`real Python SDK explicit CLI without PATH CLI`. A nested `command_receipt` uses
the same path/hash/size contract and contains the actual commands: nonempty
`argv` arrays and integer `exit: 0`, including init, named example execution and
all three SDK path languages. Never generate this projection from a local-only
binary or insert anticipated public hashes. Keep host paths and credentials out
of committed public proof; execution outputs belong in workflow artifact scopes.

Each actual native consumer independently runs init, the host starter, all three
SDK resource paths and the Python SDK with the selected CLI. Its receipt binds
command-output and helper hashes to that binary and trusted current-run job.
These sparse-helper checks establish source independence and do not claim OS
filesystem isolation or network denial. They supplement the reviewed strict
Linux capsule and all existing native product/provider gates.

The destination's actual values remain operator configuration, not committed
source or printed metadata. The digest binds the reviewed target without granting
arbitrary endpoint authority. The adapter still independently restricts endpoint,
bucket and object keys. Missing secret references are a hard stop; no profile,
ambient credentials or GitHub environment is created or inherited.

Preparation emits a source-bound public-data bundle and its complete hash
inventory. Native jobs receive that exact current-run artifact and never inherit
R2 credentials. Upload/promote accept only the trusted prepare hash, and promote
also verifies actual current-run native job statuses and exact receipt inventories.
A later reconciliation must reuse exact source/request inputs and first read all
existing objects; an unknown write outcome is not permission to retry a write.
