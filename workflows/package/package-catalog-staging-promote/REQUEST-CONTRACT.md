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
| `destination` | Literal reference names `endpoint_reference: CATALOG_STAGING_R2_ENDPOINT` and `bucket_reference: CATALOG_STAGING_R2_BUCKET`; `target_sha256` of `catalog_plan.json_bytes({endpoint, bucket})` using actual operator values |
| `aws_cli_archive` | Exact `archive` triplet from the source-bound official AWS CLI descriptor |
| `named_workflow_qualification` | Path below `release/catalog-qualification/`, SHA256 and size of the reviewed public evidence projection |
| `runtime` | Independent public runtime pointer and manifest pins, runtime source/version, exact producer compiled-core release pin, and three platform store/core pins with effective `vmimage-run.sh` SHA256 |

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
consumer jobs. It must describe the new VM1.3.7 cohort and real runtime0.6.28.

The runtime pin resolves only `/packages/runtime/latest.json`, which the catalog
publisher never writes. Its manifest must classify itself as runtime, and each
selected platform store must be core-only. Product source
`release/native-store.lock.json` and `release/dependencies.lock.json` member
`dockpipe_runtime_core` must bind the exact selected public CLI and producer core.
The effective script bytes must match both host-bundled and producer core archives.

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
