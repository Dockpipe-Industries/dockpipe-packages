# Focused R2 bulk workflow release

This producer releases only `dockpipe.cloudflare.r2bulk` version `0.1.1` from
`packages/cloud/storage/resolvers/r2/dockpipe.cloudflare.r2bulk`. Its catalog
contains one workflow. It does not replace the global catalog or its latest
pointer, and does not write R2.

Commit the package and producer together, then select the exact clean source
commit with `R2BULK_SOURCE_COMMIT`. Set absolute `DOCKPIPE_BIN` to the released
DockPipe 0.6.25 executable with SHA-256
`fc73040456eb229b13b05ed59632d6b30d4ad9419b22afd3bcd4babe85d8f183`.
Set absolute `R2_BULK_PYTHON` to the hash-locked SDK virtual environment.
The output defaults to `dockpipe scope artifacts r2bulk-release-0.1.1`;
`R2BULK_RELEASE_OUTPUT` may select another absolute, initially empty directory.
Generated output must remain ignored or outside the source checkout.

Run the authored entrypoint:

```bash
"$DOCKPIPE_BIN" --workflow-file workflows/package/package-r2bulk-release/config.yml
```

The build exports tracked package bytes from the admitted commit, compiles in an
isolated project and runs `package build store --only workflows`. It records a
source inventory, provenance, the package archive and the one-package manifest.
The manifest omits the producing host platform because the archive contains
portable Python and shell assets; it contains no native executable.

Qualification extracts that archive and runs all 27 package tests with exact SDK
versions and no skips. Four tests mock Windows locking and durability boundaries;
they do not claim native Windows proof. Native macOS proof remains absent. The
released CLI also runs all three dry-run stages and all three skip stages using
Python with site packages disabled and credentials removed. Logs stay in the
output scope; the attached qualification receipt names these limits.

Publication defaults to dry run. For the authorized release invocation, set
`R2BULK_RELEASE_EXECUTE=true`. Before publication, the release owner must publish
the exact source tag using the repository's runtime-owned Git tag-publish action.
The publisher verifies that remote tag resolves to the admitted commit, checks
the qualified asset inventory, refuses an existing release, and creates a draft
with `--verify-tag`,
uploads without clobber, then publishes with `--latest=false`. The tag is
`dockpipe.cloudflare.r2bulk-v0.1.1` in
`Dockpipe-Industries/dockpipe-packages`. Every public asset is fetched with
unauthenticated HTTPS curl and compared by SHA-256 before the completion receipt
is written. A failed readback leaves a published release requiring review; a
partial draft or a mismatched tag is never silently overwritten or resumed.

After public readback succeeds, consumers may populate their schema-1 release
lock with `package`, `version`, immutable `manifest_url`, `archive_sha256` and
`requirements_lock_sha256` from these release assets. Actual installed-consumer
qualification remains a separate acceptance step.

Focused helper checks:

```bash
python3 -B -m unittest discover -s workflows/package/package-r2bulk-release/tests -v
"$DOCKPIPE_BIN" workflow validate workflows/package/package-r2bulk-release/config.yml
```
