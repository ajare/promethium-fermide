# Station timestep verification checkpoint

All previously identified failing checks now have recorded Release passes from
focused runs. Already-passing checks were not rerun merely to obtain one green
aggregate; this does not certify global physical timestep independence.

## Physical-fidelity and performance follow-up

The [physical-fidelity report](station-physical-fidelity.md) records the new
same-configuration ODE/convergence gates, generator depletion/outage/inlet
repairs, closed fuel-cycle check, and comparable workload benchmark. The
benchmark measured **5.95× biology / 5.83× reference** Station advance speedup
at 10 Hz versus 60 Hz; it is not a whole-application speedup claim.

There are **39 distinct recorded latest focused passes** for this follow-up,
not a full-suite aggregate. No failing checks remain. General coupled-state
certification is still not claimed. Pre-existing radiator coefficient
calibration is tracked separately in
[#783](https://github.com/ajare/promethium-fermide/issues/783).

## Four-check repair status (before the physical-fidelity follow-up)

All four requested checks have recorded Release passes:

- `station-generators` and `station-protectedServices`: passed in
  `/tmp/pf-four-fixes.log` and were not rerun after passing.
- `station-power` and `station-recovery`: passed in
  `/tmp/pf-four-fixes-final.log` (two checks, 1.23 s wall time).

Repairs migrated World-step dose assumptions to committed Station time, made
step-relative bookkeeping fixtures explicit, and corrected surge/battery
expectations for interval means. A production recovery defect was also fixed in
host and Lua: integration now splits at startup-surge completion, so normal
service cannot borrow power across that boundary. The recovery assertion checks
both portions analytically rather than weakening the power balance.

These are focused regression passes, not full-suite or global timestep
certification. Those focused repair runs did not rerun the separate 19 checks.

## Investigation of the remaining checks

A subsequent Release CTest invocation ran only those 19 checks: **4 passed,
15 failed**, in 0.80 s (`/tmp/pf-station-19-checks.log`). The passes were `gas`,
`nutrientRecovery`, `carbonRecovery` and `crops`. Investigation of the existing
log and source below required no further builds or test executions. These are
first-failure diagnoses, not claims that whole checks will pass after migration.

Subsequently, `station-forecast` passed its targeted Release run (0.86 s;
`/tmp/pf-forecast-fix.log`), leaving **14** checks from this batch unresolved.
Its test now checks World and Station clocks independently for read-only
projection, and sizes the live crew through-flow expectations from actual
Station duration and committed elapsed time. Forecast implementation was unchanged.

Next, `station-failures` and `station-outages` both passed their targeted Release
run (1.06 s wall time; `/tmp/pf-failures-outages.log`), leaving **12** unresolved.
The failure fixtures now inject after a Station-counted prefix, verify diagnostic
Station indices independently of World rollback counters, and stop before the
actual incident boundary. The incident fixture retains its full-history rollback
coverage at coarse steps. Both Lua outage probes now check the authored physical
5..15 s window instead of assuming 60 callbacks per second. No production code
was changed, and no other tests were run for these fixes.

`station-deliveries` and `station-wasteEjections` then passed their targeted
Release run (0.62 s wall time; `/tmp/pf-deliveries-waste.log`), leaving **10**
unresolved. Event and recurrence assertions now bracket actual Station
boundaries, forecast probes capture pre-event state without advancing time,
the Lua disposal probe checks elapsed seconds rather than World tick counts,
and the delivery batching fixture respects World's wall-frame clamp. One
incremental build and only those two tests were needed; no production code changed.

The next requested batch (`transfers`, `compounds`, `containment`, `buffers`,
`thermal`) now has passing Release results, leaving **5** unresolved checks:
`atmospheres`, `coolant`, `crews`, `chillers`, `fuelCells`. The first targeted run
passed four and exposed a later compound roundoff assertion
(`/tmp/pf-five-checks.log`); only `compounds` was rerun after its fix and passed
(`/tmp/pf-compounds-fix.log`). Two incremental builds were used; no other tests ran.

These checks now interpret doses over actual Station duration and cumulative
work over committed elapsed time. Transfer and fractional conversion accounting
fixtures size headroom per step to preserve their admission scenarios, not to
claim physical convergence. Thermal batching respects World's frame clamp,
and the buffer reset assertion no longer borrows from a temporary snapshot.

The compound regression additionally exposed a production quantity/rate
round-trip error: a conversion's reported delivered amount could exceed its
actual draw by one ulp, or its binding flow could stop one ulp short of a capacity
or reserve boundary. Conversion connections now book quantities directly and
preserve the exact binding amount across equal-rate pathways; Lua mirrors the
binding calculation. The regression also verifies exact connection input and
delivery totals. Strict boundary assertions and conservation tolerances were
not relaxed.

The final five (`atmospheres`, `coolant`, `crews`, `chillers`, `fuelCells`) now
also have passing Release results. The first targeted run passed atmospheres,
crews and fuel cells (`/tmp/pf-last-five.log`); only coolant and chillers were
rerun after correcting their later fixtures, and both passed in 0.41 s
(`/tmp/pf-last-two.log`). Two incremental builds were used, with no other tests.
**No known failed checks remain.**

The exchange fixtures now advance once per committed Station observation and
retain snapshots safely. Coolant's pump-heated final gap is checked against its
analytical recurrence at 1e-9 rather than an old cadence-dependent threshold.
Crew and chiller dose/headroom fixtures use actual step durations; the chiller
crossing and refusal rigs are explicitly step-relative, not convergence proofs.
The fuel-cell cycle checks the existing one-Station-interval sequencing lag;
that test migration does not remove the physical lag. No production code or
conservation tolerances changed in this final batch.

| Check (historical failure) | Source-level explanation of its first failure |
| --- | --- |
| `transfers` | Assumes 60 kg/s means 1 kg per step; a default 0.1 s step moves about 6 kg. |
| `compounds` | Harvest input/output doses are compared with rate/60 instead of rate times Station duration. |
| `containment` | Cross-class transfer dose expectations multiply by the World quantum. |
| `buffers` | Expected stored energy uses 10 kW times the World quantum instead of the actual Station duration. |
| `thermal` | The radiator temperature decrement is calculated for a World step, not the committed Station step. |
| `crews` | The supposed half-demand water inventory is sized for half a World step's demand; at ratio 6 it supports only 1/12 of Station demand. |
| `chillers` | The finite-cold-store duty calculation divides the available heat by the World duration: it expects about 1.09 kW instead of about 1.015 kW. |
| `fuelCells` | The cycle assertion subtracts one World quantum for the fuel-cell/electrolyzer ordering lag; the implementation actually lags by a Station interval. |
| `deliveries` | At World tick 599, Station has already committed step 100, slightly beyond 10 s; the supposed pre-arrival snapshot is after arrival in Station time. |
| `wasteEjections` | Same pre-event sampling error as deliveries. |
| `forecast` | Compares the World counter with snapshot `worldTick`, which now counts Station steps; earlier inventory/projection equality assertions passed. |
| `failures` | The injected Lua source returns successfully while `tick <= 60`; after the 1 s prefix its Station counter is only 10, so the next callback never executes the injected failure. |
| `outages` | The Lua wrapper interprets its Station-step argument as a World tick and checks 300..900 instead of the physical 5..15 s outage window. |
| `coolant` | A World-tick loop rereads the same exchange snapshot, updates `previousGapK`, then compares that old exchange against the already-reduced gap. It also uses the wrong duration. |
| `atmospheres` | Same repeated-observation error as coolant. It also binds element references through temporary snapshot vectors; those references dangle after each statement. |

The resolved atmosphere fixture retains snapshot values before borrowing air
and reservoir entries (`World::getStationSystemsSnapshot()` returns by value).
Both exchange fixtures observe each committed Station step once and use its
actual duration; conservation/equilibrium tolerances were not widened.

Passing migrated assertions will still not establish physical independence.
Fuel cells run in `generation()` before electrolysis in `processing()`, so the
one-Station-interval reactant lag is genuine timestep sensitivity. Likewise the
implicit passive thermal solve is timestep-dependent, even though it preserves
the equilibrium bound. These require separate fidelity/convergence evidence;
the original equilibrium assertion failures did not prove the thermal solver overshoots.

## Sampling policy

`[1,60]` defines allowed ratios, not an exhaustive test matrix. Fidelity and clock
checks sample **1, 6, 7, 30, 60**: bounds, production default, a non-divisor and a
coarse interior point. Extra component registrations sample **1, 7, 60** (45
registrations instead of 900); ordinary cases already cover the default. Earlier
all-value sweep results below are historical, not the continuing test policy.

## Contract and implemented work

- World remains nominally 60 Hz. Supported Station ratios are integers 1–60
  (nominal steps 1/60 s through 1 s); new settings default to ratio 6.
  This does not add arbitrary real-valued step durations.
- No CLI ratio means authored settings are respected. Explicit ratio comparisons
  own their reference/candidate clocks and restore the caller's override.
  Snapshots report the effective ratio and duration.
- Transfers/deliveries/finite-store saturation are compared at matching committed
  instants against both a ratio-1 reference and an analytical answer for every
  sampled ratio. Continuous transfer occurrence counters are not mistaken for
  cumulative continuous flow: those counters stay zero; connection accounts and
  inventories carry the physical evidence.
- Profile forcing now splits on the shared solar/load/crew/crop/algae transition
  timeline in the host and Lua mirror. Positive intervals sample inside the
  interval rather than the next segment at its right endpoint. All-ratio tests
  prove analytical profiled material movement and that non-overlapping solar
  supply/load demand cannot perform work.
- Step-relative unit fixtures use actual Station duration. Rate assertions no
  longer divide Station doses by World duration. Nitrogen accounting observes
  each new Station snapshot once, rather than repeatedly adding stale doses.
  Snapshot counter/persistence/UI assertions have been migrated in several
  suites; conservation audit tolerances remain unchanged.
- The registry distinguishes global `certifiedRatio` from fixture-scoped
  `verifiedThroughRatio`. Mixed aggregates are explicitly unverified. A source
  contract checks every published snapshot member has a coverage classification;
  catalogue completeness is not physical fidelity.

## Passing evidence

All runs used incremental Release builds and CTest with `-j 16`.

- Earlier constant-transfer and analytical profile fidelity runs passed all 60
  ratios; maintained gates now use the five representative ratios above.
- Historical **900 parameterized cases**: ten connection cases plus Scheduled transfers,
  recurring transfers, pantry, lifecycle and event-history checks at all 60
  ratios. These passed across incremental runs, not one final monolithic run.
  Cases whose purpose is step-relative bookkeeping intentionally scale fixture
  schedules/headroom with the step; these are not a same-configuration physical
  equivalence proof. The independent fidelity fixtures do not rescale their
  physical profiles/rates between the reference and candidate.
- Registry coverage, effective-clock checks, transition-overflow rollback and
  seconds lint pass. The new overflow check verifies all physical snapshot state
  is unchanged on refusal, apart from the unhealthy flag.
- Both `pf-smoke-station` and `pf-smoke-editor` build in Release.

The first 600 cases were run using `/tmp/pf-station-component-ratios`; the next
300 used the permanent `station-timestep-components-*` registrations, with only
failed cases rerun during repair. A batching fixture initially exceeded World's
frame clamp at large ratios; bounded frames now deliver the intended total time.
A reservation fixture now keeps maintenance active through its observed event,
instead of accidentally observing completion at the 1 Hz extreme.

## Repaired physical gates: generator startup, ramp and funding

`station-timestep-invariance-generator-timing` compares identical, constantly
qualified/supplied generators at the same committed time (approximately 6 s):

| Station ratio | Nominal frequency | Before (kJ) | Repaired (kJ) |
|---|---:|---:|---:|
| 1 | 60 Hz | 30.3333 | **30.4** |
| 6 | 10 Hz | 30.0 | **30.4** |
| 60 | 1 Hz | 20.0 | **30.4** |
| Analytical physical-time model | — | **30.4** | **30.4** |

The fixture has a 0.2 s crank, followed by a 2 kW/s ramp to 8 kW. Its electrical
work is the integral of `min(8, 2 * max(0, t - 0.2))`. Before repair, even ratio 1
was a numerical approximation, not ground truth. Cranking energy itself closes at 0.2 kJ in all
three runs; the defect is timing/integration, not missing fuel accounting.

The old `Stopped` branch consumed an interval before cranking, startup completion
lost the rest of its interval, and endpoint ramp power was multiplied by the
whole step. Both the host and Lua now resolve zero-time start transitions in the
current interval, split at startup/ramp completion, and integrate affine ramp
work including the constant tail. Dispatch reserves the same interval-mean
output production delivers, rather than overestimating the ramp endpoint and
leaving a gap for backups.

The maintained timing gate covers **20 cases**: five representative ratios, two
horizons, constant solar and solar arriving at 0.5 s. Lua-internal assertions verify its independent
mirror against the same analytical integral. Endpoint `outputKW` is distinct from
new `averageOutputKW`; fuel doses, generator/pool means and connection rates are
checked against actual interval work. Split-step means use duration weights;
they do not feed accumulated reporting values back into subsequent sub-intervals.

A second counterexample involved generator-funded cranking: an interval-average
source could qualify a downstream crank before its source reached the required
instantaneous power. The manual generator/solar-only solver now splits at affine
crank-qualification crossings, respecting generator order. The funding gate
samples five ratios with 1e-13 s, 1 ms and 1 s cranks (**15 cases**), checking both host
and Lua startup/work, source work and downstream load service analytically. Its
scope deliberately excludes battery/fuel-cell portfolios and arbitrary feedback.

These remain ordinary fidelity gates, not `WILL_FAIL` or disabled tests. Their
**1e-8 kJ** analytical work tolerance is unchanged. This is event-driven
integration, not a hidden 60 Hz loop. The full implementation is still incomplete:
fuel depletion/decay, recovery controllers and nonlinear coupled workloads need
further same-configuration convergence evidence.

## Broad-suite status

The last broad run (excluding the separately measured timestep labels) was
**198 passed, 44 failed, 242 entries**. It used authored settings, including
explicit 60 Hz reference/stability fixtures, so it was not an all-10-Hz benchmark.
Subsequent focused runs repaired and passed 19 of those failed entries; no final
full-suite pass is claimed. Remaining first failures include legacy step/dose/
schedule assumptions, but must not all be classified as test-only: the generator
counterexample is a verified production defect. Coupled thermal, atmospheric,
biological and controller convergence still needs its own evidence.

See [ADR 0025](adr/0025-station-systems-clock-and-timestep-certification.md) and
[the historical timing ledger](station-test-timings.md). No passing full-suite
speedup or blanket timestep-independence certification exists yet.
