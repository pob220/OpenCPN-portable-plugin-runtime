# Questions requiring OpenCPN maintainer decisions

Source inspection and experiments answer the mechanics. These questions are
limited to policy, intended contract or roadmap choices which code cannot
decide.

1. **Would maintainers accept a generic stable dynamic toolbar-action API?**
   Specifically: plugin-owned string keys, automatic rebuild, native
   visible/enabled/checked state, explicit unregister and persistence/custom-
   isation by `(managed plugin identity, stable key)`. The existing API and
   workaround are documented in `TOOLBAR-ACTION-FEASIBILITY.md`.

2. **Should plugin toolbar actions become user-customisable alongside built-in
   actions?** Current source intentionally skips all plugin tools. A decision
   is needed on persistence/tombstone policy for temporarily absent dynamic
   actions, not on whether the skip exists.

3. **Is there interest in a generic secure credential service for native
   plugins?** If so, which platforms/backends and consent semantics should be
   part of the supported OpenCPN contract? The runtime can use session-only
   secrets without it.

4. **Would OpenCPN consider a bounded immutable chart-information query for
   ordinary plugins, and what semantics may be described as coverage, land,
   depth or hazard?** This is needed only if advisory GSHHS crossing is
   insufficient. The study does not ask for private chart pointers or a
   runtime-specific API.

5. **Does the project want portable child packages ever to appear as
   first-class entries in the standard Plugin Manager/catalogue?** If yes,
   OpenCPN must decide ownership of identity, signing, review, permissions,
   updates, revocation, crash attribution, translations and support. If no,
   the host will consistently present them as child extensions.

6. **Would the official managed-plugin catalogue permit one native plugin to
   operate a separately signed child-package catalogue?** If permitted, what
   disclosure, review, trust-root, incident-response and removal requirements
   apply? This policy gate exists even though the implementation is feasible.

7. **Which desktop/mobile target set would maintainers require before such a
   host may enter a beta catalogue?** The study recommends executed desktop
   target conformance and explicitly defers Android; the catalogue acceptance
   threshold is a project decision.

8. **For a production third-party ecosystem, would maintainers require the
   Wasmtime/package executor to run out of process?** Evidence supports an
   in-process project-signed preview and recommends a broker for open third-
   party packages, but the acceptable native-process risk is governance policy.

No maintainer question is needed about multiple icons, late add/remove, callback
routing, numeric-ID lifetime, toolbar rebuild implementation or current
customisation behaviour; source and P1 resolved those.
