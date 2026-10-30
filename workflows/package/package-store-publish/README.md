# package-store-publish

Select `DOCKPIPE_BIN` as the absolute path to the pinned installed Dockpipe CLI.
Run this maintainer workflow from the package repository.

The workflow builds this host's package store with `catalog.build`, installs the
exact released `dockpipe.cloudflare.r2bulk` package selected by
`release/r2bulk.lock.json`, and prepares a bounded bulk upload manifest. The lock
binds the immutable official release manifest URL and SHA-256, package version, archive SHA-256
and SDK requirements SHA-256. Bootstrap installs the hash-locked SDK in an artifacts
scope virtual environment and checks its exact dependency versions. It can install
the declared dependency in skip or dry-run mode; it makes no R2 API calls.

Immutable payload archives and their checksum files retain their object keys and
Content-Type values. The nested bulk workflow verifies their current remote
identity and headers before the final step publishes JSON catalogs and their
checksum/signature companions in filename order through `dockpipe release upload`.
The metadata gate checks the exact manifest identity and requires `verified-head`;
a failed payload stage cannot publish mutable metadata. Metadata files are checked
for local changes before any metadata upload. No publication transaction across
multiple metadata objects is claimed.

This local workflow builds a single host's store. Release pipelines build separate
stores per OS/CPU pair because resolvers contain native helpers. Do not replace a
cross-platform catalog with another host's store.

## Modes and credentials

`R2_PUBLISH_DRY_RUN=1` is the default. Preparation validates the local artifact
selection and destination; bulk dry run creates no SDK client, uses no credentials
and performs no provider operations. Metadata publication is skipped. Dry run does
not prove remote identity or successful publication.

`PACKAGE_RELEASE_SKIP_UPLOAD=1` keeps the build step but requires no artifact
selection or destination for publication preparation. All bulk stages skip before
SDK/configuration loading and metadata publication skips without reading a state
receipt. The workflow still requires its released dependency lock and bootstrap.

The existing outer flags accept `1` or `true` to enable a mode. Preparation emits
strict `true`/`false` values for the nested package's skip and dry-run contract.
AWS CLI remains a declared dependency for real ordered metadata publication.
The workflow uses `vault: environment`; supply bucket-scoped upload credentials
and the non-secret `R2_BUCKET` and HTTPS `R2_ENDPOINT_URL` (or account ID) explicitly.
No broad account-management credentials or Terraform-state credentials are needed.

```bash
# Default dry run; no publication credentials are required.
"$DOCKPIPE_BIN" --workflow-file workflows/package/package-store-publish/config.yml --

# Real publication with an explicitly selected secret environment.
"$DOCKPIPE_BIN" --workflow-file workflows/package/package-store-publish/config.yml --secret-environment packages-production --var R2_PUBLISH_DRY_RUN=0 --

# Build and skip all publication stages.
"$DOCKPIPE_BIN" --workflow-file workflows/package/package-store-publish/config.yml --var PACKAGE_RELEASE_SKIP_UPLOAD=1 --
```

Provision bucket/domain infrastructure with `package-store-infra` separately.
The former `upload-release-artifacts.sh` remains a legacy direct invocation helper;
this workflow no longer invokes its per-file payload upload loop.

## Offline checks

```bash
python3 -B -m unittest discover -s workflows/package/package-store-publish/tests -v
```

The suite covers immutable/metadata selection, content types, skip/dry-run provider
isolation, exact payload proof and metadata ordering. Native Windows execution and
live provider publication require separate validation.

Bootstrap downloads only verified public release assets, then materializes the
workflow into the explicitly declared `.toolchains/r2bulk/packages` store. The
store is configured in `dockpipe.config.json` before the workflow starts, so nested
resolution sees the pinned package. Cached file digests are checked before reuse.
This preserves the CLI catalog client's redirect policy and never uses a sibling
checkout as a dependency.

The consumer runner is pinned to released CLI `0.6.33`, including archive hashes
for every existing target. Older `0.6.25` and `0.6.29` runners do not resolve this
declared nested package store. Package compilation remains separately qualified
with `0.6.25`; installed-consumer execution uses the newer pinned runner.
