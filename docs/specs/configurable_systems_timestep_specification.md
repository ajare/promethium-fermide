# Configurable Systems Simulation Timestep — Specification

Version 1.0 — 20 October 2026

## 1. Purpose

Let the station/systems simulation (life support, power, thermal, water, logistics, bioconversion) run at a
timestep the operator or test author chooses, independently of the World tick, **without changing what the
simulation means**. A scenario authored once shall produce the same results, expressed in simulated seconds,
whether the systems step is one World tick or sixty.

Today the systems simulation has no clock of its own. `SimulationCoordinator::advanceTick()` calls
`StationSystemsRuntime::advance(tick, World::getFixedTimestep())` once per World tick
(`src/core/SimulationCoordinatorTick.cpp:733-734`), and 25 further sites inside station code re-derive time
from `World::getFixedTimestep()` rather than from the dt they were handed. The physics is already
dt-parameterised — every exchange integrates as `ratePerSecond * seconds` — but the *clock* is welded to the
World tick. This specification introduces the systems clock and makes timestep a declared, testable property
instead of an implicit constant.

The motivation is cost and reach: a coarser systems step divides the per-simulated-second cost of the Lua
sandbox step by the step ratio, letting long-duration ECLSS scenarios run far further in the same budget,
while the agent/movement World stays at 60 Hz where its fidelity is needed.

## 2. Scope

### 2.1 In scope

- A first-class **systems clock** owned by `StationSystemsRuntime`: step count, elapsed simulated seconds, and
  a configurable step duration.
- Removal of every World-timestep dependency from station time, cadence, validation, and rate reporting.
- **Event-split integration** so authored event times are honoured exactly at any step duration.
- A **declared-invariance registry** covering every public systems observable.
- A test harness that runs any station check at a configurable step and cross-compares results across steps.
- Persistence of the step configuration and adoption on load.
- Editor and telemetry correctness under a non-default step.

### 2.2 Out of scope

- The **World tick** and `World::getFixedTimestep()`. These remain 1/60 s and continue to own Agent movement,
  traversal coordination, Door/Lift/Shuttle timing, behaviour scheduling, and rendering pacing.
- Agent-side timing: `sleep(ticks)`, `TraversalWaitingPolicy` tick literals
  (`include/core/Coordination.h:566-572`), route-planning intervals, `getMaxTicksPerUpdate()`.
- Making the systems simulation *finer* than the World tick. Only `systemsStep ≥ 1 World tick` is supported.
- Render interpolation, and any new Agent↔Systems coupling. Station code does not reference Agents today and
  shall not start.

## 3. Terminology

**World tick** — the 1/60 s tick owned by `World`/`SimulationCoordinator`. Unchanged by this work.

**Systems step** — one execution of the systems simulation. Its duration is the **systems timestep**.

**Step ratio (k)** — the number of World ticks per systems step. `k` is a positive integer. `k = 1` is today's
behaviour. The nominal "1/10 s systems step" is `k = 6`.

**Systems clock** — the triple `{systemsTick, systemsElapsedSeconds, systemsTimestepSeconds}` owned by the
systems runtime, advanced only by the systems runtime itself.

**Timestep-invariant observable** — a public systems observable whose value at a given simulated instant does
not depend on the configured step ratio. Invariance is a *declared* property per observable, not an
assumption; see §6.

**Event split** — dividing a systems step's interval at each interior authored event time and integrating each
sub-interval under the rates in force for that sub-interval.

_Avoid_: "simulation speed", "time scale" (already means `World::mTimeScale`), "tick" used for a systems step.

## 4. Configuration model

### 4.1 The setting is an integer ratio, not a duration

`StationSystemsSettings` gains:

| Field | Type | Default | Meaning |
|---|---|---:|---|
| `systemsStepWorldTicks` | `uint32_t` | `1` | World ticks per systems step. `1` preserves current behaviour exactly. |

`systemsTimestepSeconds` is **derived**: `systemsStepWorldTicks × World::getFixedTimestep()`.

Rationale for the integer form: the relationship is then exact by construction. Configuring a duration and
testing divisibility is not — `6 × (1.0f/60.0f)` is `0.100000005…`, not `0.1f`. Defining the step as *k
World ticks* means step boundaries always coincide with World tick boundaries, no accumulator carries a float
remainder, and "one systems step every k World ticks" is exactly inspectable. The nominal "0.1 s" is a label
for `k = 6`, not a stored value.

### 4.2 Constraints

1. `systemsStepWorldTicks` shall be ≥ 1 and ≤ `CORE_MAX_SYSTEMS_STEP_WORLD_TICKS` (600, i.e. a 10 s ceiling).
2. Non-finite, zero, negative, and over-ceiling values shall be rejected before any systems object is
   constructed, with a diagnostic naming the offending value and the allowed range.
3. The step shall be changeable only while the simulation is paused, through the same
   `pauseSimulation()` / `rebuildTraversalTopology()` / `resumeSimulation()` structural protocol used for
   other structural edits. A rejected change leaves the previous step in force and the World unchanged.
4. Changing the step mid-run shall reset no systems state; it takes effect from the next systems step boundary.
   (Reset semantics belong to the existing reset path, not to this setting.)
5. The setting shall be validated by the existing shared validation path (`StationSystemsValidation.cpp`) so
   the editor, loader, API, and scenario YAML all apply the same rule.

### 4.3 Reference values

| Nominal | k | Systems timestep | Use |
|---|---:|---|---|
| Current | 1 | 1/60 s | Default; bit-identical to today |
| Coarse | 6 | ~1/10 s | Long-duration ECLSS runs at ~6× lower step cost |
| Very coarse | 30 | ~1/2 s | Stress and convergence testing |
| Ceiling | 600 | ~10 s | Config bound; not a fidelity claim |

## 5. The systems clock

### 5.1 Shape

`StationSystemsRuntime` shall own a `StationClock`:

| Member | Type | Contract |
|---|---|---|
| `systemsTick` | `uint64_t` | Systems steps completed. Advances by exactly 1 per systems step. |
| `systemsElapsedSeconds` | `double` | Derived: `systemsTick × systemsTimestepSeconds`. Never accumulated. |
| `systemsTimestepSeconds` | `double` | Derived from `systemsStepWorldTicks`. Immutable for the life of a run. |

**All systems time shall be derived from integer step counts multiplied by the timestep.** No site shall add a
float dt to a running total; float accumulation error is the mechanism by which a clock silently desynchronises
from the World tick.

### 5.2 Driving: catch up to a simulated instant

The drive is **`advanceTo(simulatedSeconds)`**, not a tick modulo. A `worldTick % k == 0` test inside the
runtime would force any test wanting coarse systems steps to supply `k` times as many World ticks — each one
running the full tick pipeline — and would keep the test's unit as World ticks, which is exactly the coupling
§12.3 forbids.

```cpp
// The drive contract.
bool advanceTo(double simulatedSeconds);
// Runs whole systems steps until elapsed >= simulatedSeconds. Never a partial step.
```

The coordinator keeps its single call at the tick boundary and supplies the World's elapsed seconds:

```cpp
// every World tick
mStationSystemsRuntime->advanceTo(worldElapsedSeconds);
```

The runtime performs 0 or 1 steps per call — identical behaviour to today. With `k ≥ 1` there is at most one
systems step per World tick, so there is no catch-up spiral and no unbounded inner loop.

**The runtime never names the World timestep.** It sees a target instant and its own duration; the World
timestep stays inside the World. This is what makes the systems simulation directly drivable with no World
present — see §12.2 — and it is the reason the drive is expressed in seconds rather than ticks.

`worldElapsedSeconds` is derived as `worldTick × World::getFixedTimestep()` by the caller, never accumulated.

### 5.3 Accounting invariant

The existing invariant `step.completed != next.completedTicks[i] + 1`
(`src/core/StationSystems.cpp:1759-1763`) shall be restated against the systems clock: each systems step
performs exactly one systems step and every object's completed count increments by exactly one **systems step**.
`completedTicks` and the persisted `worldTick`/`lastFiredTick` fields shall be documented and renamed where
practical as *systems* step counts, not World ticks.

## 6. Timestep-invariance contract

"Results do not vary with timestep" is only meaningful per observable. Every public systems observable shall be
classified in one registry, and the harness shall fail on divergence beyond the class and on any observable that
is not classified at all. Timestep sensitivity becomes a declared, reviewable property rather than a surprise
discovered in review.

### 6.1 Class A — exactly invariant

Must be identical across every supported `k`, subject only to floating-point association
(≤ 1e-12 relative). Applies to:

1. **Authored event instants** — resupply arrivals, waste ejections, maintenance jobs, scheduled outages,
   scheduled transfer starts. With event split (§7) these land at their authored simulated-second instant
   regardless of `k`.
2. **Event ordering** — the existing total order (occurrence time ascending, service priority descending,
   declaration order) shall be identical across `k`. Coarser steps coalesce more events into one step; the
   order within the step shall not change.
3. **Conservation identities** — mass and energy: `in = out + Δstorage`, to the association bound, at every
   step boundary, for every store, loop, and atmosphere.
4. **Unsaturated constant-rate cumulative totals** — a transfer at `ratePerSecond` over `[0, T]` with no
   limit reached shall total the same for every `k`.
5. **Counts** — occurrences fired, deliveries completed, jobs finished, by simulated second.

### 6.2 Class B — convergent within a declared tolerance

Feedback-coupled continuous state: coolant loop and reservoir temperatures, atmosphere partial pressures,
store levels under variable demand, dispatch controller outputs, crop and algae growth state.

Contract: for each observable the registry declares `(ε_abs, ε_rel, p)` with `p ≥ 1`, and the harness verifies

- `|x_k(t) − x_ref(t)| ≤ max(ε_abs, ε_rel · |x_ref(t)|)` at the shipped `k`, and
- **convergence order**: comparing `k`, `2k`, `4k`, the error shall shrink by at least `2^(p−δ)` with
  `δ = 0.1` tolerance for floating-point noise. A quantity that fails to converge is a bug, not a tolerance
  problem.

Default for first-order explicit integration: `p = 1`, `ε_rel ≤ 1e-3` at `k = 6`, tightening with `k`. Each
observable's registered values are the authority; the default only applies where a quantity has not yet been
measured and declared.

### 6.3 Class C — declared step-sensitive, with a bound

Protection and control stability. A protection check evaluated once per systems step samples 6× less often at
`k = 6`, so the excursion past a limit before protection engages grows with the step.

Contract:

1. Protection shall evaluate against the **pre-step worst-case rate**, not only the sampled state: the predicted
   excursion `worstRate × systemsTimestepSeconds` shall be considered before committing the step.
2. The registry declares a **bound** per protected limit — e.g. "overshoot past `maxSinkTemperatureC` shall not
   exceed `B` °C at the certified step ratio".
3. The specification declares a **certified step ratio** `k_cert` at which every Class C bound is proven.
   `k_cert` is set by measurement during implementation, recorded in the registry, and surfaced in the editor.
   Configurations above `k_cert` shall be permitted but visibly marked as beyond the certified bound.

### 6.4 The registry

One table, in one place, covering every field of the committed systems snapshot. Each entry:

| Column | Meaning |
|---|---|
| Observable | Snapshot field or derived metric name |
| Class | `Exact` / `Tolerance{ε_abs, ε_rel, p}` / `Bound{B, k_cert}` |
| Basis | Why this class — the integration form or the protection path |
| Verified by | Check name that enforces it |

Rules:

1. Every observable in the committed snapshot shall appear. An unregistered observable shall fail the invariance
   check, not pass silently.
2. A new observable added without a registry entry shall fail CI.
3. Downgrading a class (e.g. `Exact` → `Tolerance`) requires a written basis and review, because it is a
   weakening of a guarantee other code may rely on.

## 7. Event-split integration

This is the mechanism that makes Class A exactness achievable at a coarse step.

1. For each systems step over `[t₀, t₁)`, collect every authored event whose time falls in the half-open
   interval. The existing "due since the last committed tick" collection (`src/core/StationTickPrepare.cpp`,
   #392, #403) already does this and shall be reused, not replaced.
2. Sort by the existing total order.
3. Split `[t₀, t₁)` at each distinct event time, yielding sub-intervals
   `[t₀, e₁), [e₁, e₂), …, [eₙ, t₁)`.
4. Integrate continuous state over each sub-interval under the rates in force for that sub-interval.
5. Apply each discrete event at its exact time between sub-intervals.

Consequences:

- Authored event times are honoured exactly at any `k`. The recurrence floor therefore stays at the World tick
  and shall **not** be raised to the systems timestep; sub-step recurrences remain representable.
- The step's instruction budget must cover the sub-intervals. The per-step Lua budget
  (`src/core/StationSystems.cpp:1751`) shall be charged across the whole split, and budget exhaustion shall
  remain terminal through the existing failure path.
- Zero-length sub-intervals (coincident events) shall be handled without dividing by zero; the existing
  `drawable / seconds` round-trips (`src/core/StationTickAuditPower.cpp:143,160,210,244,273`) are the
  sites at risk and shall be restructured to carry rates rather than divide by a possibly-zero width.

## 8. Work required at existing World-timestep welds

Twenty-five sites inside station code read `World::getFixedTimestep()` directly. Each shall read the systems
clock instead.

| Site | Current | Required |
|---|---|---|
| `SimulationCoordinatorTick.cpp:733-734` | Passes `World::getFixedTimestep()` as dt | Pass the World tick as a scheduling input only; runtime derives its own dt |
| `StationSystems.cpp:1759-1763` | `+1` per World tick accounting | `+1` per systems step (§5.3) |
| `StationSystems.cpp:1004-1006` | Observation cadence in World ticks | Cadence in systems steps from `systemsTimestepSeconds` |
| `StationSystems.cpp:1812-1814` | Sample interval in World ticks | Sample interval in systems steps |
| `StationTickPrepare.cpp:201` | `elapsed = tick × World::getFixedTimestep()` — **ignores the dt argument** | `systemsElapsedSeconds`. This is the trap: passing a new dt without fixing this makes the systems run at the wrong rate while integrating the wrong flow per step. |
| `StationHistory.cpp:214` | Sample tick stamp from World timestep | Systems tick stamp |
| `StationSystemsTrends.cpp:107,114,135-136,144-145,150,207` | Per-step amounts ÷ `World::getFixedTimestep()` to recover per-second rates | ÷ `systemsTimestepSeconds`. Left unfixed, every displayed rate is inflated by `k`. |
| `StationSystemsValidation.cpp:72,262-264,1445,1489,1556,1603` | Recurrence floor = one World tick | Floor unchanged (§7); diagnostics shall name the systems timestep where it is the operative limit |

A grep for `getFixedTimestep` inside `src/core/Station*.cpp` shall return zero hits when this work is complete,
and that grep shall itself be a CI check so the weld cannot silently regrow.

## 9. Persistence

Persisted systems state is per-step: `completedTicks` (`StationSystems.h:4076`), `worldTick` (4074, 4204,
4225, 4276, 4297), `lastFiredTick` (2182), `lastTickIn`/`lastTickOut` (1903-1904). A 60 Hz archive loaded
into a runtime with a different step would reinterpret every per-step amount at the wrong rate.

Rules:

1. The save shall record `systemsStepWorldTicks` and the `systemsTick` at save time.
2. **Adopt on load.** The systems step is a property of the saved run. Loading adopts the saved `k`; the
   current configuration does not override it. This avoids rejecting saves entirely and keeps per-step fields
   meaningful, because they were produced at the adopted step.
3. If the saved `k` is outside the currently supported range, the load shall be rejected with a diagnostic
   naming the saved value and the supported range, leaving the target World unchanged — consistent with the
   existing transactional-load convention.
4. A save predating this change carries no `k` field and shall be read as `k = 1`.
5. Changing `k` after load is a structural edit under §4.2(3) and shall be undoable with the same
   all-or-nothing semantics as other structural edits.

## 10. Validation

1. `systemsStepWorldTicks` range validation per §4.2, shared across editor, loader, API, and YAML.
2. The recurrence floor stays at the World tick (§7). Where an authored recurrence is finer than the systems
   step and therefore only representable through an event split, validation shall pass; where a quantity cannot
   be split (a rate-limited exchange with no discrete event), validation shall reject with a diagnostic naming
   the systems timestep and the offending recurrence.
3. Validation shall report the effective systems timestep in every timing diagnostic so an author reading a
   rejection sees the operative clock.
4. A scenario whose authored timing cannot be represented at the configured step shall fail at load, not drift
   silently at run time.

## 11. Observability and reporting

1. The committed snapshot shall expose `systemsStepWorldTicks`, `systemsTimestepSeconds`, `systemsTick`, and
   `systemsElapsedSeconds`.
2. Trend and telemetry rates (`StationFlowTelemetry`, trend charts) shall be computed from
   `systemsTimestepSeconds`. Displayed units stay per-second; only the divisor changes.
3. The editor shall show the configured step and whether it is within `k_cert`.
4. The observation cadence remains an event-sampling interval and shall keep its existing
   "bounded event sampling, not a clock" semantics (`StationSystems.h:1763`).

## 12. Testing requirements

This section is the point of the exercise: **any station check can be run at any supported step, and a check's
verdict must not depend on the step it happened to run at.**

### 12.1 Runner knob

The station smoke module (`src/headless/smoke/station/Main.cpp`, `smoke::run`) shall accept:

```
pf-smoke-station [--list | --check <name>] [--systems-step <k>]
```

- `--systems-step <k>` sets `systemsStepWorldTicks` for the run. Default `1`.
- Invalid `k` exits `2` with a usage diagnostic, matching the existing unknown-check contract.
- The selected step is echoed in the run header so evidence records the clock it was produced under.

### 12.2 Scenario helper

A single helper shall own "run this scenario to T simulated seconds at step k" so no check hand-rolls a loop:

```cpp
SystemsRun runStationScenario(StationSystemsSettings const&, double durationSeconds, uint32_t stepWorldTicks);
```

It drives `advanceTo(durationSeconds)` on the systems runtime **directly — no World, no Agents, no tick
pipeline**. It returns observables keyed by **simulated second**, never by tick count. Checks assert against
seconds.

**Two drive paths, one seam concept.** The pure-systems path above covers physics, conservation, schedules,
saturation, protection, and invariance. The existing `World::advanceTicks` rig is retained **only** for
World↔systems interaction: pause/resume, save/load adoption and rejection, step-ratio change under the paused
structural-edit protocol, and failure-pauses-the-World. Making the systems runtime directly drivable requires
its drive entry to be reachable from the smoke module, which today it is not; that is the single genuinely new
public surface this design needs.

### 12.3 The rule for check authors

**No station check shall assert a raw tick count, a step count, or a `advanceTicks(<literal>)`-derived
timing value.** Every timing assertion is expressed in simulated seconds, or derived from the configured step
through the helper. Existing checks that use `60.0 * dt` or literal tick counts
(`src/headless/smoke/station/*.cpp`) shall be migrated. A grep-enforced lint over the station smoke sources
shall back this rule.

### 12.4 Cross-step invariance checks

A new check family, `stationTimestepInvariance/*`, one per registry entry or grouped by subsystem:

1. Run the scenario at the reference step `k = 1` and at each tested step.
2. Compare observables at matching **simulated seconds** against the registry class.
3. `Exact` entries must match to the association bound.
4. `Tolerance` entries must satisfy the bound **and** the convergence-order test across `k`, `2k`, `4k`.
5. `Bound` entries must satisfy the declared overshoot bound at `k_cert`.
6. Any observable present in the snapshot but absent from the registry fails the check.

### 12.5 Required scenarios

The invariance suite shall cover, at minimum:

| Scenario | Stresses |
|---|---|
| Unsaturated constant-rate transfer chain | Class A exactness of `rate × T` totals |
| Scheduled resupply with coincident occurrences | Event split, ordering ties, coalescing |
| Store filling to `Full` then stalling | Saturation crossing time, status-transition ordering |
| Thermal loop with heater and radiator feedback | Class B convergence order |
| Atmosphere O₂/CO₂ with crew demand and electrolyzer response | Class B convergence, conservation |
| Protection trip on `maxSinkTemperatureC` and starvation degrade | Class C bound at `k_cert` |
| Maintenance job with `progressSeconds` across a step boundary | Duration accounting under split |
| Long-run reference workload to a long `T` | Drift accumulation over many steps |

### 12.6 Lanes and CTest

- **Fast lane**: `k ∈ {1, 6}`. Runs in the existing fast budget and gates every PR.
- **Exhaustive lane**: adds `k ∈ {30, 600}` and the long-duration reference scenario, plus the
  convergence-order comparisons.
- CTest label `station-timestep-invariance`, with per-check labelled ownership consistent with the existing
  smoke-module convention.
- `k = 1` results shall be treated as the reference. Where a golden exists at `k = 1`, other steps compare
  against it; goldens shall not be re-recorded per step.

### 12.7 Regression guard

A check shall assert that `k = 1` output is **bit-identical to the pre-change baseline** for the existing
station suite. The default configuration must not change by even a rounding digit; that is what makes the
change safe to land.

## 13. Performance

1. Systems step cost per simulated second shall fall by approximately the step ratio: `k = 6` gives ~6× fewer
   Lua sandbox steps for the same simulated duration.
2. Per-step work is roughly unchanged, so the per-step instruction budget stays valid; event split adds
   sub-intervals only where events fall inside the step, and that cost shall be measured and reported.
3. The performance claim shall be measured, not asserted: a check shall compare wall-clock per simulated hour
   at `k = 1` and `k = 6` and report the ratio. A regression that makes the coarse step *slower* per
   simulated second than the fine step shall fail.
4. The World tick path is untouched; agent-heavy worlds see no change.

## 14. Acceptance criteria

1. `systemsStepWorldTicks` is configurable via settings, editor, API, and scenario YAML, validated through
   one shared path, changeable only under the paused structural protocol.
2. `grep -c getFixedTimestep src/core/Station*.cpp` returns `0`, and a CI check enforces it.
3. All systems time is derived from integer step counts; no float dt accumulation exists in station code.
4. The declared-invariance registry covers every committed-snapshot observable; the invariance check fails on
   any unregistered observable.
5. `pf-smoke-station --systems-step <k>` runs any check at any supported `k`, and every station check passes
   at every `k` in its lane.
6. No station check asserts a raw tick count; the lint passes.
7. Class A observables are exactly invariant across all tested steps.
8. Class B observables meet their declared tolerance and demonstrate declared convergence order.
9. Class C bounds hold at `k_cert`, and `k_cert` is recorded and surfaced in the editor.
10. Saves record and adopt their step; out-of-range saves are rejected transactionally; pre-change saves load
    as `k = 1`.
11. `k = 1` output is bit-identical to the pre-change baseline.
12. Measured per-simulated-second systems cost at `k = 6` is at least 5× lower than at `k = 1`.
13. `docs/station-systems.md` and an ADR record the systems clock, the invariance classes, and `k_cert`.

## 15. Decomposition

Tracer-bullet sequence; each step lands green before the next starts.

| # | Slice | Delivers |
|---|---|---|
| 1 | **Systems clock seam** — introduce `StationClock`, derive dt from `k`, keep `k = 1` bit-identical | The seam exists; nothing changes behaviour |
| 2 | **De-weld** — replace all 25 World-timestep sites; add the grep guard | Station time no longer depends on the World tick |
| 3 | **Event split** — sub-interval integration, zero-width safety | Authored event times exact at any `k` |
| 4 | **Invariance registry + harness** — registry, `--systems-step` knob, scenario helper | Cross-step comparison runs |
| 5 | **Check migration** — seconds-based assertions, remove tick-literal assertions | Existing suite passes at any `k` |
| 6 | **Class B/C measurement** — measure tolerances, set `k_cert`, protection pre-step bound | Guarantees declared with evidence |
| 7 | **Persistence** — record and adopt `k`, range rejection | Saves survive the change |
| 8 | **Editor + telemetry** — show step and certification, trend divisor | Author-visible correctness |
| 9 | **Exhaustive lane + docs + ADR** | Full matrix and recorded rationale |

## 16. Risks and open questions

1. **Convergence order may be less than 1** for some stiff couplings. If a quantity cannot meet `p ≥ 1`, the
   options are a better integrator for that coupling or a widened declared tolerance with a written basis. This
   must be measured in slice 6, not assumed.
2. **Saturation is where exactness actually breaks.** `rate × dt` is exact while unsaturated; once a store hits
   its limit inside a step, the crossing time must be solved rather than clamped at the step end, or the
   delivered amount depends on `k`. Slice 3 must handle crossing-time resolution for rate-limited exchanges,
   not just discrete events.
3. **Lua sandbox budget under event split.** A step with many interior events costs more per step. The budget
   must be charged across sub-intervals and exhaustion must remain terminal.
4. **`k_cert` may be small.** If protection overshoot at `k = 6` exceeds acceptable bounds, the certified step
   may be lower than the economically interesting one. Surfacing `k_cert` honestly is better than certifying a
   step the protection path cannot hold.
5. **Should `k` be per-subsystem rather than global?** A thermal loop and a logistics schedule may want different
   steps. Deferred: a global step keeps the clock simple, and nothing in the current design prevents a later
   per-subsystem ratio.
6. **Do we ever need `k < 1`?** Explicitly unsupported (§2.2). If a future need arises, the accumulator-free
   design has to be revisited and this specification amended.
