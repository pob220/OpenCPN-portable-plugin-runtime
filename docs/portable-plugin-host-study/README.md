# Portable runtime host-plugin feasibility study

This directory records the evidence, experiments and architecture decision for
moving the experimental portable runtime out of a permanently modified OpenCPN
core and into a conventional managed native plugin where technically credible.

The study branch is `runtime-host-plugin-feasibility`, created from
`f85835669f97e3d913e4b26a531e47ae1f061b00`.  Experiments are isolated from
normal release packaging and must not modify the user's working OpenCPN 5.15
installation or profile.

## Evidence labels

- **Verified source fact**: directly established by pinned source code.
- **Verified experiment**: reproduced by a documented command or GUI test.
- **Architectural inference**: a reasoned conclusion based on cited facts.
- **Maintainer decision**: behaviour or policy which source and experiments
  cannot decide.

All externally changing sources are recorded with a commit SHA and access date
in `source-ledger.json` and `SOURCE-LEDGER.md`.

