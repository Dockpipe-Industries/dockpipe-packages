# agent

Tracked first-party package family for governed AI stages in Dockpipe.

What lives here:

- `resolvers/claude/` — Claude resolver profile
- `resolvers/codex/` — Codex resolver profile
- `resolvers/ollama/` — local Ollama resolver profile
- `workflows/agent.cloud-lanes.doctor/` — portable diagnostic for cloud-backed agent lanes

This package keeps Dockpipe's separation of concerns intact:

- Dockpipe is the governed runtime and orchestration layer.
- Resolver profiles such as `claude` and `codex` are tool adapters, not the product.
- AI workers are workflow/package stages with explicit YAML contracts: prompts, context, access
  boundaries, model policy, output artifacts, verification, and approval requirements.
- DorkPipe is the preferred harness when those stages need model execution, verifier artifacts,
  escalation, and approval handling.

This tracked package is the canonical first-party location going forward.

## Promoted resolvers

The promoted `claude`, `codex`, and `ollama` resolver trees preserve their existing:

- `config.yml` delegate workflow
- `profile` environment contract
- `README.md` usage notes
- `assets/compose/` examples
- `assets/images/<resolver>/Dockerfile`

No binaries, caches, vendor trees, local environment files, or build outputs were copied during
promotion.

## Package workflow

`workflows/agent.cloud-lanes.doctor/` checks whether the Codex and Claude resolver containers have
the expected auth mounts, skills, and optional live prompt behavior.

Repo-specific docs orchestration dogfood now lives under the repository root `workflows/` tree
instead of being published as part of this package.

## Image contexts and qualification

Each resolver owns its `assets/entrypoint.sh` and Docker build context. These
entrypoint snapshots preserve Dockpipe's command/action/signal protocol from
Dockpipe source revision `e491c7938fd721a27f842ec8dc7bba9889d7f96e`;
changes to that protocol require an explicit compatibility review here. They
are authored package assets, so compiled resolver archives build independently
of a Dockpipe source checkout. Manual image builds must use the selected
`resolvers/<name>/` directory as their context, with
`-f assets/images/<name>/Dockerfile`.

`tests/integration/run.sh` first compiles this package's resolver sources with
an explicitly selected installed Dockpipe CLI. Its smoke/tooling tests run the
compiled `claude` workflow, including data-volume behavior and installed Node
and Claude CLI checks. They do not invoke a model API. Docker daemon failure
is a failed qualification, never a portable-test skip.
