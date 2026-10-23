# package-store-publish

Select `DOCKPIPE_BIN` as the absolute path to a pinned installed Dockpipe CLI.
Run source commands from this package repository; no sibling core checkout is required.

Repo-local maintainer workflow for the self-hosted package mirror.

What it does:

1. uses the pinned installed Dockpipe binary and external SDK artifacts
2. invokes the repository-owned `catalog.build` workflow
3. writes `release/artifacts/packages-store-manifest.json` and package tarballs
4. uploads selected artifacts with `dockpipe release upload` when explicitly selected

This repository publishes optional workflow/resolver stores; Dockpipe owns the required core release artifacts. It does **not** tar the whole `release/artifacts/` directory into one object.

## Secrets

The workflow uses `vault: environment`. Configure your own named secret environment and explicitly supply `R2_BUCKET` and `R2_ENDPOINT_URL` (or your account ID) through private configuration. The public project config contains placeholders only. Scope upload credentials to your publication bucket; keep account-management and Terraform-state credentials separate.

The endpoint and bucket are non-secret workflow variables. No `op://` template or broad Cloudflare token is loaded. Install a 1Password CLI version supporting `op run --environment` and authenticate it first. Build the secrets resolver with `dockpipe build --no-images` after updating this checkout.

This local workflow builds a **single host's** store. The release pipeline builds separate stores for each OS/CPU pair because resolvers contain native helpers. Use the release pipeline for the public cross-platform catalog; do not replace it with a store from a different host.

## Run

Dry-run is the default:

```bash
"$DOCKPIPE_BIN" --workflow package-store-publish --secret-environment packages-production --
```

On a machine without the AWS CLI, Dockpipe will prompt to install it from the workflow dependency definition before continuing.

Real upload:

```bash
"$DOCKPIPE_BIN" --workflow package-store-publish --secret-environment packages-production --var R2_PUBLISH_DRY_RUN=0 --
```

Build only:

```bash
"$DOCKPIPE_BIN" --workflow package-store-publish --secret-environment packages-production --var PACKAGE_RELEASE_SKIP_UPLOAD=1 --
```

If you still need bucket/domain provisioning, run `package-store-infra` separately first.
