# Gradual C++ direction

The user-selected long-term direction is an incremental C++ implementation across
the Dockpipe ecosystem, including packages and PipeLang. This is a future backlog
direction, not a rewrite included in repository extraction.

Choose bounded components, preserve their public commands, manifests, wire formats,
SDK/ABI and state/cancellation/resource contracts, and compare behavior against the
current implementation before replacing it. Keep independently buildable products
and explicit language/runtime dependencies throughout. Measure build, runtime and
maintenance effects before claiming improvements. No new language rewrite or mass
source conversion is authorized by this extraction task.
