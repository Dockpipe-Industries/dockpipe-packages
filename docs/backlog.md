# Package backlog

Future package ideas and discovery work owned by this repository. Entries are
proposals, not shipped features or scheduled commitments. Existing implementation
and migration work retains its current owning records.

## PKG-001: RFC and patch-series review

Status: Future / proposed  
Proposed package identity: `review` (confirm during design)

Provide a package-owned Review / RFC tooling tab alongside Dockpipe's Git surface.
Start with Linux kernel mailing-list workflows, preserving their existing email
and patch-series conventions.

### Intended experience

- Browse and search RFCs, patch series and threaded discussions.
- Read cover letters and diffs; compare revisions of a series.
- Apply a selected series to a Dockpipe-managed worktree without disturbing the
  user's current checkout.
- Run package-defined build and test workflows against that worktree, keeping
  results associated with the exact series revision.
- Draft replies and review trailers, with explicit user authorization before
  sending email or publishing review feedback.

### Ownership and integration

- Keep Lore, Patchwork and b4 integration, external tool declarations and review
  behavior in this optional package.
- Use Dockpipe's existing package tooling-surface contract for the host tab,
  editor integration, managed worktrees and workflow execution. Keep the launcher
  in the Dockpipe repository.
- Consider a Qt front end through the published host SDK; inspect the existing
  contracts before choosing the UI implementation.
- Request generic runtime/SDK additions only when the existing contracts cannot
  express a demonstrated need. Core must not require this package or contain
  kernel-specific review logic.
- Consume versioned external tools and SDKs through declared dependencies and
  supported minimum versions; do not rely on sibling source checkouts.

### Discovery and completion criteria

1. Check the current tooling-surface and Git worktree contracts, then document
   any genuinely missing generic capability.
2. Select supported Lore/Patchwork/b4 interfaces, including authentication,
   pagination, resource bounds, cancellation and expected offline failures.
3. Demonstrate browsing, revision comparison and an isolated worktree/test flow
   for a real public series. A tracked series may be an RFC or a normal patch set.
4. Verify that draft creation does not send anything, and that an authorized reply
   preserves message threading and the kernel's plaintext email conventions.
5. Verify installed-package behavior against pinned released Dockpipe artifacts,
   including native UI qualification on the supported host platforms.

References: [Lore](https://lore.kernel.org/),
[Patchwork](https://patchwork.readthedocs.io/en/stable/),
[b4](https://b4.docs.kernel.org/en/latest/).
