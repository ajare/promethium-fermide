# Legacy headless compatibility (#305)

`prometheum-fermide-headless` links only its dispatcher, not production libraries,
smoke implementations, or the smoke harness. Prefer direct `pf-smoke-*` commands,
[dedicated tools](headless-tools.md), or CTest.

Building this target builds every configured smoke module, the five dedicated
tools, and compile contracts. With GUI enabled this includes Startup and its
required editor/runtime assets. This also works with `BUILD_TESTING=OFF`.
Dependencies are one-way: direct module builds never build the aggregate or
unrelated modules/tools. Children are resolved beside the running executable,
not in the working directory or PATH; keep the built environment together.

No arguments runs every configured module once, sequentially in alphabetical
target-name order. Startup is included only with `PF_BUILD_GUI=ON`; it tests a
controlled startup failure without a window. Tools, synthetic probes and the
separate standalone checks are not smoke modules and are not part of this run.
The orchestrator emits `RUN <target>` before each child and `PASS <target>` after
success, forwarding child stdout/stderr without a shell. Its ordering is stable;
any measurements emitted by a child are not made deterministic by the wrapper.

Every selection emits `DEPRECATED:` and names its direct replacement. Check
selections run whole owning modules (possibly broader than the old selection):

| Legacy selection | Module(s), in dispatch order |
|---|---|
| `--viewport-checks`, `--render-checks` | Render |
| `--agent-behaviour-checks` | Behaviours, Editor |
| `--access-permission-checks` | Permissions, Editor |
| `--route-planning-checks`, `--route-planning-time-checks` | Routing, Editor, Render |
| `--routing-scale-checks`, `--shuttle-route-checks`, `--restored-path-checks` | Routing |
| `--serialization-checks` | Persistence, Render |
| `--coordinated-document-checks` | Editor |
| `--metrics-checks` | Metrics |
| `--world-teardown-smoke` | Simulation |
| `--graphics-startup-smoke` | Startup (missing-product error in GUI-off builds) |

Check selections reject extra arguments. Tool selections forward all subsequent
arguments unchanged, including empty strings, spaces, quotes and metacharacters:

| Legacy selection | Dedicated command |
|---|---|
| `--restoration-benchmark` | `pf-restoration-benchmark` |
| `--write-routing-scale-world` | `pf-generate-routing-world` |
| `--pause-position-repro` | `pf-pause-position-repro` |
| `--lift-crossing-repro` | `pf-lift-repro crossing` |
| `--lift-stall-repro` | `pf-lift-repro boarding` |

Metrics selections launch `pf-metrics-server`: `--metrics` is removed,
`--metrics-port` becomes `--port`, `--metrics-world` becomes `--world`, and
`--metrics-detail=sector,queue` becomes `--detail=sector,queue`. Other arguments
are validated by the dedicated tool. Use `--ticks N` for bounded automation;
without it the explicitly selected service intentionally runs until stopped.
`--help` alone prints usage without launching a child. Unknown selections or
malformed wrapper arguments return 2.

## Outcomes and verification

- Child success returns 0; ordinary failure reports `FAIL <target>: exit N` and
  propagates N (Windows statuses above 255 normalize to 1).
- Missing executable returns 127; other launch failures return 126.
- Signal termination on Linux reports the signal and returns 128 + signal.
  Windows exception termination reports the Windows status and returns 1.
- Multiple-module selections continue after failure, returning the first nonzero
  outcome in deterministic dispatch order. Required missing children never skip.
- Windows system error dialogs are disabled and inherited by children; smoke
  modules/tools additionally suppress their own CRT dialogs.

CTest registers modules directly, **not** the compatibility aggregate. The
`headless-compatibility-contract` uses disposable synthetic children to test all
mappings, argument forwarding, stable sequential ordering, PATH invocation from
an unrelated directory, success/failure/missing/abnormal outcomes and continuation.
It never launches GUI windows, servers or production smoke checks. World's direct
contract covers the final preserved `marker-identity` and `two-sided-buttons`
suites. The Device-operation/Traversal-resource type assertion formerly in the
legacy main already belongs to `pf-interaction-api-compile-contract`.
