# Validation

Run the repository's Dockpipe build/test workflows with an explicit pinned binary.
Read each selected package's `package.yml` hooks; a missing dependency or hook
failure must not be reported as a passing package. Keep generated output in
Dockpipe artifact/package-runtime scopes.

For a focused package, choose absolute `PACKAGE_REPOSITORY` and `PACKAGE_SOURCE`:

```bash
"$DOCKPIPE_BIN" package compile workflows --workdir "$PACKAGE_REPOSITORY" --from "$PACKAGE_SOURCE" --force
"$DOCKPIPE_BIN" package compile resolvers --workdir "$PACKAGE_REPOSITORY" --from "$PACKAGE_SOURCE" --force
"$DOCKPIPE_BIN" package test --workdir "$PACKAGE_REPOSITORY" --only <package>
```

Qualify in an isolated checkout with no sibling Dockpipe or PipeLang source.
Verify package source builds, tests, archives, metadata, store ownership and
installed execution. Typed-model checks require the pinned PipeLang SDK.
Native rendering, Qt/SDK ABI, runtime closure, private-symbol handling and actual
installation require their own evidence. Skips and offscreen-only checks cannot
satisfy native gameplay/rendering proof.

Before release, bind the exact source revision, dependency locks, package identities,
archive digests, sizes and acceptance receipts. Do not publish an invented pin or
select an arbitrary local build. Report failing/unavailable external producer
proof and stop that publication boundary while retaining authored work.


## VM host matrix

The catalog root builds and its secrets/remote consumer tests run on both Linux
and Windows against the same verified SDK. VM tools have separate host contracts:

| Host | VM build and tests | Qualification claim |
| --- | --- | --- |
| Linux | Complete `packages/vm/tools` module (`go build ./...`, `go test ./...`) and the VM package hook | Linux provisioning, executor, permission and durability checks remain intact. Live VM qualification still requires separate evidence. |
| Windows | Protocol, controller, manifest, guest, recovery and key-material libraries plus the guest-agent compatibility command; full protocol/controller/manifest/guest tests, recovery state-machine tests and key collision rejection | Portable compatibility only; no Linux qualification execution, POSIX private-key mode or directory-fsync claim. |

`tooling/vm_go.py` owns the explicit matrix used by build and Windows acceptance
workflows. Linux-only provisioning/executor/identity-material dependencies are
excluded from the Windows build because their security and execution contracts
require Linux, rather than replacing UID, mode, mmap or pidfd checks with no-ops.
Windows recovery tests exercise the state machine, not its Linux owner-only file
store. Windows key-material tests exercise collision rejection, not private-key ACL
qualification. Those filesystem assertions still run unchanged in the full Linux
suite. The package guest-agent compatibility command is distinct from the core
Windows VM guest-services agent and the Qt Guest Tools application.

Use the native `workflows/ci.windows` workflow for Windows acceptance. Cross-built
Windows test executables are compilation evidence only; they are not executed test
proof. A successful Windows slice does not qualify the Linux QEMU controller or
its live provisioning/execution paths.
