# PipeLang tools for Dockpipe

Install the standalone [PipeLang CLI and SDK](https://github.com/Dockpipe-Industries/pipelang) before using these workflows. Dockpipe's required model SDK is pinned by the engine; this package exposes CLI commands without copying language implementation into the catalog.

Set `PIPELANG_INPUT` to your entry file and invoke `pipelang.check`. `pipelang.compile` also requires `PIPELANG_OUT`; `pipelang.invoke` requires `PIPELANG_METHOD`, accepts `PIPELANG_CLASS`, and forwards the string array `PIPELANG_ARGS_JSON` as individual `--arg` values. `PIPELANG_BIN`, when set, must be an absolute executable path. Otherwise an installed `pipelang` is selected from PATH.

The initial Go installation entrypoint is `go install github.com/Dockpipe-Industries/pipelang/src/cmd/pipelang@v0.1.0` after that module tag is published. Native CLI archives are produced by PipeLang's own release workflow. Go module tags are the SDK distribution channel; Homebrew and OS package registries are follow-up distribution work, not published artifacts yet.
