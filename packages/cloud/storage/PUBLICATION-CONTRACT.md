# R2 workflow package dependencies

`cloud.storage` is an authoring family, not an installable store package. Its
legacy `resolvers/r2/` directory groups workflows and does not define an `r2`
resolver profile. Likewise `secrets` is a family; consumers install its actual
`onepassword` resolver when they need that provider.

The R2 publication implementation already belongs to the independently published
`dorkpipe` resolver. Workflows use its actual logical export,
`scripts/dorkpipe/r2-publish.sh`. They do not require a phantom `dockpipe` or `r2`
resolver, and this catalog does not duplicate that implementation. Generic
`dockpipe sdk` and `dockpipe scope` calls use the selected CLI/runtime contract,
not an optional resolver named `dockpipe`.

The complete default-mode dependency graph is:

| Installable unit | Required units |
| --- | --- |
| `secretstore-r2-publish-test` 0.6.2 | `onepassword`, `dorkpipe`, `dockpipe.cloudflare.r2infra` |
| `dockpipe.cloudflare.r2infra` 0.6.4 | `dockpipe.terraform.core` 0.6.3 |
| `dockpipe.cloudflare.r2upload` 0.6.4 | `dorkpipe` (Terraform remains explicitly skipped by this upload-only workflow) |

The three repaired default-mode workflow artifacts must be delivered together by
one verified candidate: secretstore 0.6.2, R2 infrastructure 0.6.4 and Terraform
0.6.3. Name-only dependency metadata is not a substitute for the candidate's
exact artifact/version/hash admission. The old Terraform 0.6.2 helper can swallow
failed commands because Bash ignores `set -e` inside an OR-list's guarded subshell.
Version 0.6.3 deliberately propagates required command failures before later
operations. No historical archive or public pin is overwritten.

For the compatibility proof, the existing pinned DorkPipe input was read back at:

- URL: `https://packages.staging.dockpipe.com/packages/standalone/dorkpipe/candidates/0.6.5-staging.cc31cf0aa848/dorkpipe-packages-0.6.5-linux-amd64.tar.gz`
- Archive: 19,418,995 bytes, SHA256 `e00a93057b3e1afedfd13d01c951f16c261ddbd4ea677584ff34b347d5140955`.
- Manifest SHA256: `464aec661eed0eea756599d08a6e98a4261fffe7d53a5c2aff28fb38d74c2438`.
- Selected `dorkpipe` 0.6.5 resolver: 19,245,748 bytes, SHA256 `6d063a887c3ed27864b24b55de5baff778e249153a7467fbc1d6902bb989c2ae`.
- Exported R2 helper: 8,468 bytes, SHA256 `969067f0aa827fb80dfb5d6d5f819ab253d7106a1009d8275e037c788aad5b5b`.

The baseline's real `onepassword` 0.6.1 artifact is 1,238,747 bytes, SHA256
`6787c453852dba13b6c983f6f1928d3de931bd537c4087ff66c195c0649d86d3`.
These are explicit independently owned inputs, not source fallbacks or family
aliases. Acquisition and immutable publication belong to the candidate producer.
The existing separately pinned compiled runtime-core bootstrap remains required.

## Nonpublishing checks

Select an absolute public CLI in `DOCKPIPE_BIN`. Select a task-private
`DOCKPIPE_GLOBAL_ROOT` populated with the exact declared public resolver/core
inputs and the three freshly compiled repair workflows. Then run:

```sh
python3 -B -m unittest discover -s packages/cloud/storage/tests -v
python3 -B -m unittest discover -s packages/terraform/resolvers/terraform-core/tests -v
```

The first suite executes real source and installed workflows in isolated workspaces
and resolves the actual published helper. Its `op`, Wrangler and Terraform
executables are nonsecret fixtures: it checks environment-output merging, both
upload-only and default Terraform branches, cleanup, and provider failure
propagation without network or provider changes. The second suite checks required
Terraform operations, workspace fallback and failed-directory handling even when
the caller suppresses Bash errexit. These are Linux compatibility checks; they do
not claim real Cloudflare provisioning, credentials, Flatpak installation, native
Windows/macOS execution or native VM package qualification.

The source-only overlay preserves all unrelated baseline archives byte-for-byte.
The public 0.6.25 compiler refreshes tar member timestamps, so recompiling an
unchanged source payload can change its archive SHA. A payload comparison is
useful diagnostic evidence; it never authorizes replacement of an existing
immutable filename. A complete rebuilt native catalog still requires its own
native producer, dependency closure and installed acceptance gates.

## Explicit immutable-object bulk workflow

The separate `dockpipe.cloudflare.r2bulk` 0.1.0 source candidate owns its own
manifest-based S3 transfer helper, pinned Python SDK lock and offline contracts.
It does not replace the published archive helper or change any existing workflow
identity. See [the bulk workflow contract](resolvers/r2/dockpipe.cloudflare.r2bulk/README.md)
for immutable source/key mappings, bounded workers, durable resume, trusted remote
receipts and completion-marker ordering. Its SHA256/ETag and HEAD checks do not
claim a full download or reconstruction test. Source validation and local archive
compilation do not authorize package publication or invent a released consumer pin.
