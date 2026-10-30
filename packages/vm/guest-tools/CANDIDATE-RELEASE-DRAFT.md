# Guest Tools immutable candidate draft

This candidate lane adds a native CI emitter, admission and an immutable
publisher around the existing native producer. Its implementation and synthetic
contract tests do not constitute a native build, published artifact or catalog
update. The current CLI pin intentionally blocks hosted execution until a real
compatible public candidate is selected.

The additive adoption is based on catalog commit
`30859228a77ba20a23a4b69ab81aae133d6ccfe9` plus the separately reviewed VM
1.3.6 compatibility metadata. Integrating these new files changes source identity: the actual accepted dev commit must receive a fresh native
build and its own candidate tag. The emitter observes that actual commit; it
does not add a new source label to an existing development build receipt.

## Adoption prerequisites and bootstrap compatibility

The adoption patch is additive. It does not replace the current shared
`tooling/bootstrap_dockpipe.py`, `tooling/bootstrap_runtime_core.py`, runtime-core
lock or existing CI files. This specific native producer passes
`--no-compile-deps` and uses a package-owned host build with no resolver/package
scope dependency, so a compiled runtime-core bootstrap is not a prerequisite of
this lane. The catalog's broader integration/store lanes still require it.

The current owner pin `0.6.25` remains unchanged and is intentionally rejected
by the new early candidate guard. Before hosted execution, the owner must select
an actually published, publicly readback-verified CLI candidate containing the
native Windows environment repair; update `dockpipe.config.json` and the exact
public `dockpipe.lock.json` artifact triplets together. No development binary,
local receipt, guessed version, fabricated SHA or unverified candidate may fill
those inputs. The task adoption packet records these fields as null pending
actual publication. Candidate naming binds source identity but does not itself
prove the required capabilities: public candidate provenance and native smoke
results must also be reviewed. Shared bootstrap still independently verifies
archive hash/size, executable size and the actual native version.

Python is pinned to 3.13.7 for the existing hash-locked Windows extractor kit.
The emitter remains bound to an actual clean committed checkout, hosted runner
identity, real tool versions, named native CTest results and installer rejection
checks. Adopting source is not a native execution or publication receipt.

## Separate delivery and release contracts

Keep the existing `guest-tools-release.json` unchanged. It binds the ZIP,
platform, entrypoints, complete payload inventory and producer source-inventory
digest. The release wrapper is `guest-tools-candidate-manifest.json`, schema 1,
product `DockpipeGuestToolsCandidate`, artifact class `guest-tools`.

The immutable GitHub prerelease belongs to
`Dockpipe-Industries/dockpipe-packages`, with tag
`guest-tools-candidate-<40-character artifact source commit>`. It must be a
prerelease with `latest=false`, never a production channel update.

The manifest contains `source_repository`, `source_sha`,
`source_state: committed-snapshot`, `version: 1.0.0`, `platform: windows-amd64`,
`tag`, the exact toolchain versions and lock digest, and these required checks:

- `native-windows-build`
- `native-windows-ctest`
- `installer-integrity`

Its `artifacts` map has exactly these roles. Every record contains only `name`,
`url`, `sha256`, and `size_bytes`. URLs are the exact immutable GitHub tag path;
there are no local paths, query strings, `latest` aliases, or source fallbacks.

| Role | Asset |
| --- | --- |
| `guest_tools` | `dockpipe-guest-tools-1.0.0-windows-amd64.zip` |
| `guest_descriptor` | `guest-tools-release.json` |
| `guest_source_inventory` | `guest-tools-source-inventory.json` |
| `guest_native_provenance` | `guest-tools-native-provenance.json` |
| `guest_toolchain_lock` | `guest-tools-toolchain.lock.json` |

Publish those five assets and the release manifest, exactly six assets. The
publication plan remains outside the asset directory and is not uploaded.

## Actual native prerequisites still required

The included `build_candidate.py` emits provenance from the existing producer,
actual native tool versions, CTest JUnit and installer-integrity results.
`candidate_ci.py` acquires the six locked official tool archives, installs the
hash-locked Python extractor and invokes the emitter through pinned Dockpipe.
These new wrappers have local admission tests; **neither the wrappers nor the
new workflow have run on a hosted Windows runner yet**. That execution is still
a mandatory qualification gate.

1. Acquire the exact artifact source commit in a clean, isolated checkout. Record
   its actual identity; reject a development tree or an operator-supplied commit
   string that was not verified against the materialized source. The publishing
   workflow revision may be newer than the artifact source; record both honestly.
2. Select a publicly available checksum-pinned Dockpipe binary and install the
   official Qt/MinGW/CMake/Ninja artifacts through the existing toolchain lock.
   A task-private `src/bin` executable is development proof, not a public pin.
3. Run the existing native `guest-tools/workflows/build/config.yml` producer.
   Admit actual Qt 6.10.3, GNU/MinGW 13.1.0, CMake 3.30.5 and Ninja 1.12.1.
   Keep toolchain and build outputs outside the source inventory root.
4. Retain successful native results for both named CTests,
   `guest-tools-status` and `guest-tools-clipboard`; skipped or cross-built tests
   do not satisfy this gate. Run the existing
   `tests/InstallerIntegrity.Tests.ps1` against the produced bundle with a fresh
   test output directory. Require installer parsing, corrupt-executable rejection,
   and unchanged UI task registration. Do not install, elevate, repair or provision
   a guest in this hosted artifact job.
5. Export the producer's complete source inventory as a JSON array of
   `{path, sha256}` records. Preserve native Windows path ordering (case-folded
   path components). Its canonical compact JSON digest must equal the original
   delivery descriptor's `source_inventory_sha256`. Bind the toolchain lock as
   the `tooling/toolchain.lock.json` inventory entry.
6. Emit bounded, nonsecret `guest-tools-native-provenance.json` from the actual
   source/tool/test observations. Do not hand-author a successful receipt around
   the existing development artifact. Do not publish private paths, usernames,
   credentials, resolved environment, or raw test logs.

The exact provenance schema enforced by `prepare_candidate.py` has these fields:

- `schema: 1`, `product: DockpipeGuestToolsNativeBuild`
- `source_repository`, `source_sha`, `source_state: committed-snapshot`
- `native_host: windows-amd64`
- `qualification_origin: github-actions-native-windows`
- `source_inventory_sha256`, `archive_sha256`, `descriptor_sha256`,
  `toolchain_lock_sha256`
- `qt_version`, `compiler_id`, `compiler_version`, `cmake_version`, `ninja_version`
- `checks`: exactly the three check names above
- `ctest_tests`: exactly `{guest-tools-status: passed, guest-tools-clipboard: passed}`
- `installer_integrity`: `{installer_parse: passed,
  corrupt_executable_rejected: true, ui_task_unchanged: true}`
- `workflow`: `{repository, run_id, run_attempt, workflow_sha}` from the actual
  hosted job. The workflow SHA is independent of the artifact source SHA.

Unknown fields, duplicate JSON keys, incorrect types, mismatched hashes,
source collisions, symlinks, unlisted files, and incomplete native checks fail.
The current delivery-only development output deliberately fails preparation
because it lacks the required committed/native provenance inputs.

## Prepare, review, then publish separately

After a hosted native build emits real inputs, preparation is local and has no
network or credential access:

```text
python prepare_candidate.py --inputs <absolute-five-input-directory>
  --output <absolute-fresh-prepared-directory>
  --source-sha <ACTUAL_NEW_40_CHARACTER_SOURCE_COMMIT>
```

Review `publication-plan.json` and all six prepared assets. Preparation copies
only the byte snapshot that passed admission, so later input-file changes cannot
alter the planned artifact identity.

A separately authorized publisher must use the established immutable prerelease
semantics: resolve the tag to the exact source commit, discover drafts through
the release inventory, create a non-latest prerelease draft if absent, and reject
any conflicting tag, extra/missing asset, size, or SHA-256. Never clobber, delete,
or silently replace published assets. An identical published candidate may be
reused. A draft without a created tag can only proceed after its target/source
and exact asset inventory match; final published readback requires the tag.
`validate_remote_release` is a pure admission function. The separate
`publish_candidate.py` implements bounded authenticated GitHub discovery, draft
creation, partial-draft resume with only missing exact assets, final publication,
and anonymous public readback of all six assets. Before any release operation,
it independently verifies all six admitted byte snapshots using GitHub
attestations bound to the owning workflow, exact source digest, dev ref and hosted
signer. Public readback rejects HTTP downgrade, credentials and redirects outside
approved GitHub asset hosts, has an overall deadline, and verifies that the latest
release identity remains unchanged. A concurrent latest change fails readback
without rollback or destructive recovery. It uses admitted byte snapshots
for uploads, never `--clobber`, deletion, force tags or production pointers.
A conflicting draft is preserved and rejected; an identical published candidate
is read back without mutation. No publisher operation has been executed locally.

After publication, independently fetch the public manifest and delivery pair,
verify exact sizes/hashes and source tag, then populate the catalog producer's
required `guest_release_manifest`, `guest_tools`, and `guest_descriptor` roles.
The catalog validator must cross-check the two delivery-role triplets against the
pinned release manifest. An artifact source commit may differ from a later catalog
source commit only through explicit independent pins. No placeholder lock values
or private/local artifact locations satisfy this gate.

## Existing evidence and limits

The task's Guest Tools 8 ZIP is `b6ded45509d50e2916a66f75e8c813320463657a3a77f72db8350de8965518e2`,
14,940,756 bytes. Its clean descriptor SHA is
`9af40905507fd315fd08d6cfcbea604a3c6a93f2259317e9f358d4d97ea4569d`.
All 36 payload files verified before this candidate draft, and those 35 historical component source files matched its
source inventory digest
`02f484c586b039f364858245d878c2083753c9d5f23e26505c46b170eb9838e0`.
These historical facts support development qualification and source-byte comparison. They
are not a new hosted build, public availability, a source-commit attestation, or
publication authority. No bit-identical native-binary rebuild is claimed;
deterministic ZIP ordering/timestamps alone do not establish that property.

Run the draft's focused tests with:

```text
python -B -m unittest discover -s tests -p 'test_candidate*.py'
```

## CI and publication gates

`.github/workflows/guest-tools-candidate.yml` builds on dev PRs and accepted dev
pushes. Only the push event on `refs/heads/dev` gets the separate publication job
with contents/attestations/id-token write permissions. All six exact assets are
attested before publication. The PR job has read-only permissions and cannot
publish. There is no workflow_dispatch, force/latest/production/clobber/delete
option. The runner uses process-scoped Git newline configuration before checkout;
the emitter rejects any material byte conversion against the committed blobs.
Python 3.13.7 is selected explicitly. The setup-python action is pinned to official
[v6.1.0](https://github.com/actions/setup-python/releases/tag/v6.1.0), commit
`83679a892e2d95755f2dac6acb0bfd1e9ac5d548`; other action pins reuse the owning
core's reviewed candidate lane pins.

The workflow acquires Dockpipe through the owning `dockpipe.lock.json` and
`tooling/bootstrap_dockpipe.py`. The guard accepts CLI candidates with numeric
base 0.6.26 or newer and their exact full-source candidate identities. It also
accepts the reviewed version 0.6.33 release
`v0.6.33-staging.40247668385.1.b107ea64782c`, mapped explicitly to source
`b107ea64782c5a153440fb786791f1500a55416e`. That source contains the native
Windows environment repair `f01897a4ce7eb8cf6d6d4141490264555a0844ef` and
allocation fix `0684173fb7b304fed1d8d5d884aef921784cd90d`. Other staging
release identities require review; numeric version alone does not admit them.
The exact official Windows archive URL, checksum and bounded sizes remain
required. The shared bootstrap verifies actual downloaded bytes and the native
CLI version before building. Guard acceptance does not replace the hosted
native build and qualification. Historical 0.6.0/0.6.25 candidate bases and the
old stable pin remain rejected. No task-local CLI, sibling source build,
placeholder checksum or handwritten success receipt is accepted as a release
dependency.

The five-input native provenance schema and six-asset release wire are complete,
but installing this draft into source, running a fresh hosted build, verifying
attestations and immutable public readback, and supplying the resulting public
manifest/ZIP/descriptor triplets to the catalog lock remain outstanding. The
Windows catalog native environment closure is also a separate public prerequisite.
None of these remaining gates is replaced by the existing Guest Tools 8 functional
proof. Tests use synthetic inputs and mocked transports only.
