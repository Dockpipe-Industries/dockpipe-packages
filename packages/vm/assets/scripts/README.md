The native source producer is `build-source.py`. It requires Python 3, Go and an
absolute pinned `DOCKPIPE_BIN`. It invokes the catalog's checksum-verified SDK
wrapper with its offline module boundary intact, builds the manager and native
QEMU shim, obtains the package-owned staging scope through the public CLI, and
compiles the owning resolver. It does not inspect a sibling core checkout.

The existing `build-source.sh` delegates to Python for callers using the old
manifest hook. The manifest remains on that compatibility entrypoint until the
catalog's pinned CLI supports native Python package hooks. Windows can invoke
the Python entrypoint directly through a Dockpipe host workflow without Bash.
`DOCKPIPE_HOST_PYTHON_BIN`, when used by the compatibility wrapper, must identify
the selected interpreter; explicitly empty values fail.

Display SDK and matching Qt inputs remain explicit. The display producer checks
its native factory platform and library before the surface descriptor is
changed. A Qt kit enables the clipboard producer only on Linux; it does not
imply clipboard support on another host. Guest Tools archive and descriptor
inputs must both be present and nonempty. Resolver staging is removed on normal
completion or an expected build failure, and package compilation errors propagate.

On Windows, only transient resolver source and compiler staging use a fresh
OS temporary directory. Deep durable package paths exceed MSBuild's 260-character
limit even with a short state base. Published artifacts still use the public
package compiler/output boundary; no system long-path setting is changed. The
private staging directory is removed on success and failure. Other platforms
retain package-scoped staging.
