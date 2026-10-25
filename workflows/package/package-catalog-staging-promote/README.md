# Optional staging catalog promotion

This producer owns the optional Marketplace staging catalog. It never publishes
runtime core, changes `packages/runtime/latest.json`, or changes production,
compiler, Homebrew, native-candidate tags, or updater pointers.

The product input is an independently qualified immutable native catalog candidate.
Its source SHA is separate from the orchestration source SHA. A later publisher
commit does not requalify or relabel any package executable. The native tag must be
`catalog-native-candidate-<product SHA>`. Source-only candidates are not accepted.

Every baseline platform and every owned package needs an explicit promote,
retain-baseline, or retire decision with a reason. Unrelated baseline entries stay
byte-identical. Core is explicitly excluded. New entries need actual compiled
package/config platform declarations and resolver-aware transitive dependency
closure. A same-named workflow cannot satisfy `requires_resolvers`. Missing native
platform evidence is never supplied by copying another platform's binary.

The pending VM1.3.7 candidate requires the genuine independently published self-contained CLI and actual
Mac startup qualification. This implementation does not contain a fabricated pin
for that future candidate. Candidate64 remains immutable evidence, not a repaired
VM1.3.7 delivery artifact. Secretstore must be explicitly retired where its required
DorkPipe resolver is unavailable; the independent Terraform pair is unaffected.

## Transaction and CI boundaries

1. A secret-free planning job admits public baseline/candidate manifests, archive
   hashes, compiled declarations, exact tool pins, product qualification, and the
   reviewed platform policy. No arbitrary success booleans establish qualification.
2. A privileged immutable-upload job reads the actual admin pointer and S3 ETag,
   requires anonymous public bytes to match the reviewed baseline, and creates
   objects only beneath the exact dual-SHA catalog prefix using conditional
   PutObject `If-None-Match: *`. Existing exact objects can be reused. A collision,
   unknown write outcome, timeout, or changed baseline stops immediately.
3. Three secret-free native consumer jobs use the exact public CLI on Linux,
   Windows, and Mac ARM. They read the same-origin immutable wrapper using actual
   `package catalog` and install each promoted package in its own fresh global
   store. Each result must include the exact returned path and complete selected
   dependency archive/hash set. A shared preinstalled QEMU cannot mask missing app
   metadata. Existing source-independent named-workflow/native proofs are independently
   required; catalog installation does not establish startup, rendering, or input.
4. A final privileged job accepts consumer artifacts only from those required
   successful jobs of the same trusted workflow run and exact plan. A handwritten
   request receipt is not a substitute. It rechecks the complete immutable object
   set, public/admin baseline bytes, and original admin ETag. The sole mutable
   write is `packages/latest.json`, using `If-Match` from admin GetObject, or
   `If-None-Match: *` for admitted absence. Its Cache-Control is `no-cache`.
5. Final readback verifies admin metadata, public pointer bytes and the full object
   inventory. Unknown outcomes require a later explicit readback-first
   reconciliation; writes are never blindly retried, clobbered, deleted or rolled
   back. Native candidate and product qualification remain separate authorities.

The object-store adapter selects an exact hash-admitted AWS CLI and verifies its
model exposes both conditional PutObject headers before mutation. Its R2 endpoint
must match a lowercase32-hex account hostname. The exact official2.32.9 ZIP
is pinned in `release/catalog-promotion/aws-cli-2.32.9.json`; its detached signature
was independently verified against the AWS-published signing key. CI extracts that
hash-admitted ZIP privately without running its installer. Its temporary root is owned0700
with no aliased ancestors. Credential values exist only in the privileged child
process environment and are never serialized or printed. Native consumers inherit
only a declared system baseline and fresh private home/XDG/global roots.

## Required configuration and unresolved execution inputs

The owning registered catalog pipeline uses the existing `release-staging`
GitHub environment and the established `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `R2_ENDPOINT_URL` and `DOCKPIPE_RELEASE_BUCKET`
references. A read-only prepare-stage API check requires that environment to
exist before dependent upload/promote jobs may bind it. This check never creates
an environment, reads secret values, or proves credentials are configured.
Missing settings still fail before provider writes. Request destination fields
name only the two standard public-setting references and the reviewed SHA256 of
canonical endpoint/bucket JSON; resolved values are not committed or printed.

The existing `package-release-setup` Dockpipe workflow supports upload-only
checks using the existing `packages-staging` secret-environment mapping and
`RELEASE_SETUP_ENVIRONMENT=release-staging`. Its public environment ID remains
an operator placeholder. Use an actual already configured private mapping;
never infer a vault ID or borrow core environment secrets. Setup defaults to
read-only validation; applying settings is a separate explicit pipeline setup
operation. The target environment must already exist with its intended branch
policy. Neither setup nor catalog execution creates access or provider credentials.

Only upload and promote bind the environment. Source checks, request preparation
and all three native consumers receive no publication credentials. Core runtime
publication remains independent and continues to own only its runtime pointer.
The credential policy must restrict writes to the reviewed immutable catalog
prefix and optional `packages/latest.json` key.

The runnable request/CI wrapper still needs the actual public candidate N, the
independently published CLI and SDK pins, the reviewed all-platform policy,
and the source-bound named-workflow qualification projection. No default, placeholder
or source-cache fallback may fill those values. These missing inputs do not affect
independent core runtime publication.

## Bounds and source checks

Objects are at most512MiB, total catalog payload at most8GiB, at mostsix platforms
and1000 selected entries. Commands retain at most64KiB metadata output and enforce
125-second kill/wait bounds; anonymous reads are bounded and same-origin. Only one
private payload is staged per upload. Each CI job is bounded to45 minutes. Each
provider invocation checks its45-minute budget between operations; an operation
already in flight has its own125-second bound plus up to5 seconds for cleanup.
These are per-invocation/job bounds, not a single45-minute bound for the complete
multi-job transaction. Each native CLI command is bounded to300 seconds plus
cleanup within its job limit. Actual provider/native execution remains a per-run
gate, distinct from fixture tests.

Run focused source tests without provider mutation:

```sh
python3 -B -m unittest discover -s workflows/package/package-catalog-staging-promote/tests -v
```

The declarations reader uses the owning pinned PyYAML verifier dependency. It reads
hash-admitted compiled archive metadata, never a sibling source checkout.

## Registered launch route

The existing, registered `.github/workflows/ci.yml` remains the entry point. Its
normal Linux/Windows package checks are unchanged. Promotion is disabled by
default and requires an explicit `workflow_dispatch` on staging, a reviewed
source request under `release/`, its exact SHA256, and `catalog_promotion_execute`.
A new workflow file on a nondefault branch is not assumed to be dispatchable.
The final three-job gate checks actual current-run job results and receipt bytes.

The candidate product's `release/native-store.lock.json` binds the actual public
CLI archive; `release/dependencies.lock.json` binds the published SDK source and
archive identities. Readiness rechecks the independent runtime pointer, CLI
producer metadata, SDK manifest and actual package-owned QEMU adapter before the
optional catalog CAS. No mandatory core archive or runtime package store exists.
The reviewed strict Linux isolated/offline receipt must name the actual public
CLI; current-run native capability checks separately prove source independence.
See `REQUEST-CONTRACT.md` for exact receipt fields and limits.

The provider contract uses R2's documented atomic conditional PutObject headers,
not an assumed compatibility property: [S3 API compatibility](https://developers.cloudflare.com/r2/api/s3/api/)
and [consistency model](https://developers.cloudflare.com/r2/reference/consistency/).
The AWS signing key and detached-signature procedure come from the
[official AWS CLI installation guide](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html).
Fixture tests and the offline CLI model do not establish actual provider behavior.

## API and consumer checkout isolation

Prepare, upload and promote use the ephemeral owning `github.token` through the
step-local `GH_TOKEN` reference with only `contents: read` and `actions: read`.
The helper admits only its fixed `api.github.com` run/job/release/tag endpoints
and rejects redirects before forwarding a header. Anonymous mode remains valid
outside CI. Neither native CLI children nor AWS provider children receive that
token; no token is serialized into the public-data bundle or receipts.

Native consumer jobs check out only the publisher's Python helper scripts using
non-cone sparse checkout and `persist-credentials: false`. Before any CLI call,
the helper verifies the actual checkout contains only those regular helper files
and inert Git metadata. Unexpected packages, src, tooling, source, root config,
or aliased files are rejected. The resulting checked helper inventory is recorded
in the consumer receipt; `source_independent` follows that check. The CLI itself still
runs in a distinct fresh project and uses a fresh global store for each app.
