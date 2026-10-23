# dockpipe-packages agent router

This public repository owns the extracted Dockpipe packages, their assets, tests,
examples and producer workflows. The generic runtime, CLI, desktop host and host
SDK belong to [Dockpipe](https://github.com/Dockpipe-Industries/dockpipe).
The compiler and language SDK belong to [PipeLang](https://github.com/Dockpipe-Industries/pipelang).
Routing: [docs/agents/index.yaml](docs/agents/index.yaml).

## Before editing

Read [ownership](docs/agents/ownership.md) and the selected package's README and
manifest. Preserve unrelated changes. Select an absolute pinned `DOCKPIPE_BIN`,
then read [validation](docs/agents/validation.md). Never copy runtime/compiler
source into this repository or use a sibling checkout as an implicit dependency.

## Rules

- Keep product behavior in the owning package's YAML, assets, scripts and tests.
- Use Dockpipe workflows as repository build, test and release entrypoints.
- Put workflow-specific scripts beside `config.yml`; shared helpers belong at
  the lowest common owning boundary. Do not add root script shadows.
- Preserve public manifests, package identities, state contracts, resource bounds,
  cancellation and expected-failure behavior during extraction.
- Typed `.pipe` models require the separately pinned PipeLang compiler/SDK. Native
  packages consume explicit pinned Dockpipe host SDK and other native SDK artifacts.
- Do not infer that source migration authorizes publication, credentials, provider
  operations, commits or pushes. Use the repository's runtime-owned Git contract.
- Secrets stay references; generated stores, resolved configuration, caches and
  test evidence do not become authored source.
- Report exact checks and skipped native/provider/platform proof. Portable test
  success is not native rendering or package-distribution qualification.

## Direction

The long-term ecosystem direction is a gradual C++ implementation across
Dockpipe, packages and PipeLang. Preserve behavior and public contracts through
bounded, independently verified replacements. This extraction does not implement
that rewrite. See [direction](docs/agents/cpp-direction.md).
