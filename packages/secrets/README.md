# Secrets package

Select `DOCKPIPE_BIN` as the absolute path to a pinned installed Dockpipe CLI.
Run source commands from this package repository; no sibling core checkout is required.

Resolver-backed named secret environments for 1Password Environments, AWS Secrets Manager, Azure Key Vault, and Infisical. Existing 1Password template workflows remain supported.

See [Secret environments](https://github.com/Dockpipe-Industries/dockpipe/blob/master/docs/runtime/vault.md) for configuration, provider authentication, limits, and the generic resolver contract.

Build native helper binaries and compile the resolvers from this checkout:

```bash
"$DOCKPIPE_BIN" build --no-images
"$DOCKPIPE_BIN" package test --workdir . --only secrets
```

Release stores contain a helper built for the store's OS and CPU. Install the matching platform store; no Go compiler is needed by consumers. Provider CLIs (`op`, `aws`, `az`, or `infisical`) must be installed on the host. Infisical supports browser login during interactive workflow startup; configure a dedicated profile and instance as described in the vault guide. Other providers require an existing login. Source-only resolver compilation needs the source-build hook to package the helper first.
