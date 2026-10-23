# Dockpipe packages

Independent source, assets, tests and producer workflows for the remaining
Dockpipe package catalog. The runtime and desktop host live in
[Dockpipe](https://github.com/Dockpipe-Industries/dockpipe); the language compiler
and SDK live in [PipeLang](https://github.com/Dockpipe-Industries/pipelang).
Previously extracted [DorkPipe/MCP](https://github.com/Dockpipe-Industries/dorkpipe)
and [Pipeon](https://github.com/Dockpipe-Industries/pipeon) retain their own repositories;
Pipeon source currently requires repository access.

## Packages and examples

| Source | Purpose |
| --- | --- |
| [agent](packages/agent) | AI resolver integrations and cloud-lane diagnostics |
| [cloud](packages/cloud) | Cloud storage, registry and publication workflows |
| [Cockpit (external)](docs/migration/cockpit-retirement.md) | Independently owned SpatialPipe package; consume its pinned published runtime |
| [ide](packages/ide) | Editor and development-container integrations |
| [native-sdk](packages/native-sdk) | Product/native SDK packaging support |
| [pipelang](packages/pipelang/README.md) | Standalone language-command workflows |
| [remote](packages/remote/README.md) | Remote-edge provider helper |
| [secrets](packages/secrets/README.md) | Resolver-backed secret environments |
| [terraform](packages/terraform) | Shared Terraform package helpers |
| [vm](packages/vm/README.md) | VM lifecycle, tools and workflow examples |

Read the selected package's manifest and documentation before running its commands.
For installation, use the runtime's [package quickstart](https://github.com/Dockpipe-Industries/dockpipe/blob/master/docs/packages/package-quickstart.md).
Source migration does not publish a new package version or replace existing pinned releases.

## Build and test

Choose an absolute pinned `DOCKPIPE_BIN`. Typed models require the separately
versioned PipeLang SDK; Go helpers require the exact Dockpipe Go SDK release locks.
Native packages additionally require their matching host/product SDK artifacts.
Fetch the declared SDK inputs through `tooling/fetch_sdk_release.py` as documented
by the committed release locks. Missing or unpublished artifacts fail clearly;
there is no sibling source-checkout fallback.

Resolver discovery and integration tests also require the compiled runtime core.
Its independent immutable archive is pinned in `release/dependencies.lock.json`.
Install it into a fresh task-private global root before running those workflows:

```bash
python3 tooling/bootstrap_runtime_core.py --output /your/task-runtime
export DOCKPIPE_GLOBAL_ROOT=/your/task-runtime
```

The helper sets this variable automatically in GitHub Actions. It requires curl,
checks the exact archive size/hash and bounded layout, and installs only the compiled
tarball. It preserves existing output directories by refusing to overwrite them.
This bootstrap dependency does not qualify new native VM runtime features.

From this repository root:

```bash
"$DOCKPIPE_BIN" --workflow-file workflows/catalog.build/config.yml --
"$DOCKPIPE_BIN" --workflow-file workflows/catalog.test/config.yml --
```

[Agent guidance](AGENTS.md) describes ownership and validation. Put scripts specific
to one workflow beside its `config.yml`; keep genuinely shared helpers at their
common owning boundary. Package tests, native rendering and platform installation
remain distinct proof gates. [Branching](docs/branching.md) explains the protected
release flow and dependency/security automation; report vulnerabilities through
[SECURITY.md](SECURITY.md).

Dockpipe is pinned with `dockpipe_version` in `dockpipe.config.json`.
`dockpipe.lock.json` records the exact release archives and SHA-256 checksums.
Run `python3 tooling/bootstrap_dockpipe.py --output /your/tool-directory` to install
that version; CI uses the same lock. Update the config and lock together.
This initial pin uses the published 0.6.25 staging release because the stable
release predates the required workflow features. The lock adds no cache service.
