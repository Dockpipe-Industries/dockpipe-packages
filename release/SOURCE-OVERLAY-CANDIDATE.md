# Source-only compatibility catalog candidate

This route repairs the dependency and failure-propagation defects in three pure
workflow packages. It exports only `dockpipe.terraform.core` 0.6.3,
`dockpipe.cloudflare.r2infra` 0.6.4 and `secretstore-r2-publish-test` 0.6.2.
It does not build or qualify a native VM helper, Qt module or Guest Tools archive.
The source repair and default-mode execution tests must be reviewed together.

`source-overlay.lock.json` pins the actual released Dockpipe 0.6.25 CLI and
compiled runtime core. Exact public baseline and DorkPipe bundle identities bind
the existing onepassword and DorkPipe resolver dependencies. Only those resolver
archives and the released core are seeded into an isolated global package store;
there is no sibling source, phantom r2 resolver or ambient installed dependency.
Terraform, infrastructure and secretstore workflows compile in dependency order
through the released CLI. Store export retains the CLI's dependency validation.
Only the three repaired identities are selected into the overlay, and existing
filename/hash identities prohibit changed bytes under an old package version.

The verifier is a separately pinned **public source archive** from core commit
`f077ab7650246d32145a124e546e43053814e622`. Its Python importer, composer and
Flatpak dependency preflight are QA tools, not a released runtime or SDK. They
verify the new flat archive against the existing optional catalog contract,
compose it over the exact historical baseline plus the public DorkPipe resolver,
and validate the full resulting package dependency graph. No Qt/native profile
is needed for those checks. Actual Flatpak Platform execution and the final core
release gate remain separate and mandatory.

The tracked source commit/transport is intentionally null. CI downloads the exact
accepted owning commit once and binds its actual archive hash in an effective
lock. There are no unresolved dependency URLs. A local `--development-source`
run is explicitly marked development and cannot publish; it does not claim a
committed source reproduction. Fresh CI compilation/execution is still required
after the repaired source and route are adopted.

The immutable wire is unchanged:

- Tag: `catalog-candidate-<full owning source SHA>`.
- Asset: `dockpipe-catalog-linux-amd64.tar.gz`.
- Flat contents: one `packages-store-manifest.json` and exactly its three
  versioned workflow archives; no core or unlisted files.
- `source-store-release.json` binds source, effective lock, source-only checks,
  exact ownership, all six public input pins and archive/manifest identities. It states `source_only: true`
  and `native_build: false`.
- Its `artifacts` and `owned` fields form the existing optional core catalog lock
  overlay. Only anonymous public readback can supply a promoted consumer pin.

The publisher reuses the native candidate's immutable transaction. Native input
and qualification validation remain in the native wrapper; source-only candidates
are refused by its default publication mode. Source-only publication requires an
accepted owning `dev` push, matching source/input/consumer receipts, and verified
hosted GitHub provenance for archive, descriptor and consumer proof. PR and manual
runs build only. Draft creation/resume checks numeric release identity and the
exact asset inventory. Existing different or extra bytes fail; published partial
releases cannot be repaired. No clobber, deletion, force, mutable latest pointer or
production promotion is provided. After publication every asset is anonymously
rehash-verified and GitHub's latest release must remain unchanged.

One catalog candidate is immutable per source SHA. A later full native catalog
must use a new source commit/tag; it cannot overwrite this source-only overlay.
The source-only workflow's narrow push paths reduce accidental competition, but
a conflicting candidate still fails closed rather than combining inventories.

The task-local draft does not authorize adoption, source commits, CI dispatch or
publication. Local contract tests are not hosted build provenance. The current
Mac optional patch remains a separate proposal; the shared publisher extraction
changes its aggregate packet identity even when its individual preimages match.
