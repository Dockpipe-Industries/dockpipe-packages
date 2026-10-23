# Ownership and dependencies

Each `packages/<name>/` tree owns its product-specific manifests, workflows,
resolvers, tests, assets, documentation and source. Generic workflow/runtime,
resolver, strategy, package-store and SDK contracts belong to Dockpipe. Compiler,
application IR and language semantics belong to PipeLang.

Declare external tools and artifacts explicitly. `DOCKPIPE_BIN` must identify the
reviewed absolute executable; the package publisher uses a pinned released binary,
not `go run` from a sibling Dockpipe checkout. Language models consume the pinned
PipeLang compiler/SDK. Native builds consume the matching Dockpipe LauncherSDK
and product SDKs with their ABI, platform, inventory and digest checks intact.
Missing dependencies fail clearly rather than falling back to copied monorepo
source. A local development override must remain explicit and cannot qualify a
release artifact.

Use package declarations and workflow fields before requesting a new generic
engine primitive. A producer owns only its declared package identities; it cannot
replace runtime core or unrelated packages. Released catalog pins stay immutable.
Migration snapshots under `docs/migration/` describe former paths and qualification;
they are evidence, not working producer entrypoints or new publication authority.
