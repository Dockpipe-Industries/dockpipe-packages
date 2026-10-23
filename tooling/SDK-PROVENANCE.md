# External SDK bootstrap

`fetch_sdk_release.py` and the SDK validation portion of `go_candidate.py` were
adapted from DorkPipe tooling during extraction. The catalog owns these import
helpers; DorkPipe candidate builders and receipt logic are excluded. Immutable SDK
source and dependency descriptors are retained under `release/`. The import validates
manifest, archive and per-source hashes before any Go command.

`catalog_go.py` is the catalog-owned consumer. It imports verified release inputs
into a temporary directory and creates an ephemeral Go modfile pointing to that
artifact. It never uses a sibling engine checkout or committed replacement.
Go builds run with workspace discovery disabled and downloads disabled; prime
public dependencies through an explicit preparation workflow when required.

The shared `lib/package-source-build.sh` came from Dockpipe's neutral package
source build helper at extraction. Its Go invocation now calls `catalog_go.py`;
it contains build orchestration only, with no engine implementation.

The required SDK snapshot must be published by the source owner named in
`release/sdk-source.lock.json`. Fetch requires an authenticated `GH_TOKEN` with
release read access, passed only through the environment. A missing release or
credential is a failed dependency boundary, never a reason to compile against a
local Dockpipe checkout. `DOCKPIPE_GO_SDK_INPUTS` can point to an already downloaded
pair of `sdk.json` and `sdk.tar.gz`; verification remains mandatory.

The download CLI supervises its worker with a total wall-time deadline on Linux,
macOS and Windows. Socket and response-byte limits also apply inside the worker.
