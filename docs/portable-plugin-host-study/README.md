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

## Document index

- `EXECUTIVE-SUMMARY.md` — decision, confidence and next action;
- `FEASIBILITY-STUDY.md` — complete evidence-backed report;
- `CURRENT-CORE-CHANGES.md` / `current-core-changes.json` — source inventory;
- `CAPABILITY-MATRIX.md` / `capability-matrix.json` — 61 A–G decisions;
- `TOOLBAR-ACTION-FEASIBILITY.md` — toolbar call path and acceptance results;
- `REPRESENTATIVE-PLUGINS.md` — production/template audit;
- `ARCHITECTURE-OPTIONS.md` — weighted matrix and sensitivity analysis;
- `RECOMMENDED-ARCHITECTURE.md` — process, modules, registry, lifecycle and
  packaging design;
- `MINIMAL-OPENCPN-CHANGES.md` — preview/beta/first-class API boundary;
- `MIGRATION-PLAN.md` — incremental phases, gates and rollback;
- `PROTOTYPE-RESULTS.md` — reproducible experiment log;
- `RISK-REGISTER.md` — technical/security/governance/platform risks;
- `UPSTREAM-QUESTIONS.md` — only unresolved maintainer policy decisions;
- `STUDY-COMPLETION-AUDIT.md` — requirement-to-evidence completion audit;
- `IMPLEMENTATION-WORKSPACE.md` — frozen baselines, isolated workspace,
  stock-core invariant and implementation stop point;
- `SOURCE-LEDGER.md` / `source-ledger.json` — pinned evidence versions.
