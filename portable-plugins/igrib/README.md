# iGRIB portable component

iGRIB is the first installable proof component for OpenCPN's experimental
portable-plugin runtime. It exercises deterministic `.ocpnp` packaging,
manifest and component-identity validation, Component Model instantiation,
typed host calls, toolbar registration, vessel-position access, private
settings, cancellable work, retained geographic overlays and trap containment.

In this Test-OpenCPN proof, its primary action requests the typed
`org.opencpn.environment.viewer` host service. The 0.1 host compatibility
adapter fulfils that request using an enabled xGRIB native plugin, so iGRIB
opens the same xGRIB window and the same installed environmental data without
passing native pointers, wxWidgets objects or graphics contexts to Wasm. The
portable component still works when that optional provider is absent, but only
its proof overlay, setting and job remain available.

This is deliberately a hybrid compatibility proof, not an independent rewrite
of xGRIB's native implementation. A production iGRIB port still needs the RFC's
environment dataset, HTTP, storage, declarative UI and supervised-helper
services. The package is unsigned and accepted only in a test build with both
the experimental runtime and developer mode explicitly enabled.
