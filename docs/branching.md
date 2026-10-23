# Branches and security automation

`master` is the default release branch. Development enters `dev`, qualifies through
`staging`, and reaches `master` through reviewed pull requests. Protected branch
rules are managed separately in GitHub; a checked-in workflow is not proof that
live rules or checks have run.

Dependabot version-update pull requests target `dev` weekly. Security-update pull
requests follow GitHub's default-branch behavior and may target `master` regardless
of `target-branch`; coordinate and qualify them through the protected release flow.
The catalog monitors actual Go modules, Actions, Terraform and Docker recipes.
Flatpak metadata named `package.json` is not an npm dependency manifest.

CodeQL reports `CodeQL (Go)` and `CodeQL (Actions)` on standard public Ubuntu
runners. Go analysis uses the repository's Dockpipe build workflow. The catalog
requires the exact externally published Dockpipe SDK locks and never substitutes
a sibling source checkout. Missing/unpublished artifacts fail that check clearly.

Linux CI reports `test`; the implemented Windows job reports `test-windows`
and invokes its dedicated Dockpipe workflow through a checksum-pinned Windows
bootstrap. Static PowerShell/Python parsing and workflow validation have passed;
the hosted Windows job has not yet run. Configuration is not Windows runtime
qualification. Native package ABI/rendering and broader language conformance
remain separate acceptance gates.
