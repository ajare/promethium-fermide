# Station physical fidelity and comparable workload benchmark

Follow-up to #774 after `18971636`. Station defaults to 10 Hz; World remains
60 Hz. These are fixture-scoped physical comparisons and convergence bounds,
**not universal timestep-independence certification**.

## Method and maintained checks

`src/headless/smoke/station/StationPhysicalFidelityChecks.cpp` authors the same
physical settings at ratios **1, 6, 7, 30, 60**. Only the Station clock changes.
Comparisons use matching committed elapsed instants, not equal step counts or
step-scaled doses. The common long comparison is 4200 World quanta, or
70.0000036508 seconds with the actual float World quantum. Generator probes
instead evaluate their independent analytical reference at each sample's exact
committed endpoint, never at a mismatched nominal target. Conservation checks
retain the existing ledger tolerances.

- `station-fidelity-physicalCoupling`: analytical two-body thermal exchange
  (coolant/pump/reservoir and atmosphere/reservoir), independently inverted
  nonlinear radiator ODE, biological respiration with gas/heat coupling, and
  an electrolysis/fuel-cell cycle. Truncation bounds depend on the actual step;
  they are not arbitrary enlarged accounting tolerances.
- `station-fidelity-generatorExhaustion`: finite fuel, affine ramp, exact fuel
  exhaustion and decay completion; cumulative fuel/electricity/heat/load work,
  ordered phase messages, interval-mean load fractions and routed heat.
- `station-fidelity-generatorOutage`: an outage at 1.25–1.50 seconds, decay and
  restart; analytical energy and ordered event messages across the samples.
- `station-fidelity-generatorPortCapacity`: a 2-unit/s fuel inlet at 50%
  connection efficiency. Its 0.5 kW ceiling and 2.625-second inventory
  exhaustion must not be confused with interval-throughput saturation.
- `station-fidelity-recoveryConvergence`: dwell/surge recovery with a bounded
  observation delay and eventual Active state. The service-work error is at
  most two Station intervals in this fixture.

Independent Lua assertions cover the unthrottled generator exhaustion and
outage trajectories. The physical inlet-capacity/loss check validates the host
connection ledger; it is not evidence of general Lua/host connection-graph
parity. Event message order is compared; cadence-dependent event step indices
are not asserted identical.

The observable inventory now has **57 entries**. New bounded leaves and decay
energy remain fixture-scoped with `certifiedRatio = 0`; mixed aggregates remain
Unverified. Largest sampled ratio 60 does not mean every intermediate ratio was
tested.

## Recorded physical errors

Errors against the continuous model after the common approximately 70 seconds:

| Ratio | Coolant °C | Air °C | Radiator °C | Biomass kg | Fuel-cycle electricity kJ |
|---:|---:|---:|---:|---:|---:|
| 1 | 0.000574690 | 0.000567573 | 0.000573034 | 0.000028970 | 0.134233 |
| **6 (10 Hz)** | **0.003448497** | **0.003405812** | **0.003438980** | **0.000173890** | **0.805400** |
| 7 | 0.004023329 | 0.003973533 | 0.004012324 | 0.000202889 | 0.939633 |
| 30 | 0.017250526 | 0.017037451 | 0.017213507 | 0.000871167 | 4.026998 |
| 60 | 0.034518654 | 0.034093380 | 0.034473669 | 0.001746644 | 8.053996 |

These fixtures demonstrate first-order convergence, not exact equality of
finite-step thermal or biological state. The fuel-cell check explicitly
measures the existing generation-before-processing reactant lag: electricity
is `P * (T - dt)`, versus continuous-limit `P * T`, with one interval of hydrogen
buffered. The lag is quantified, not removed. Recovery service-work errors at
ratios 1/6/7/30/60 were respectively 0.016667/0.1/0.083333/0.5/0.5 kJ.

## Bugs found and repaired during the audit

- Finite fuel and decay could occupy whole steps rather than their actual
  physical intervals. Integration now splits at fuel exhaustion and decay
  completion, publishing the resulting phase immediately.
- A ramping source could fund a single uncomplicated load using an interval
  mean that invented temporal overlap. Its instantaneous source/load crossing
  is now an integration boundary in the supported manual portfolio.
- End-of-interval outage sampling applied a transition to the wrong positive
  interval. Host/Lua now apply the left-boundary state and publish endpoint
  schedule state separately. Lua schedules include logistics and maintenance
  boundaries; maintenance progress uses actual elapsed time and resets stale
  availability.
- Split-step delivered fractions and routed thermal rates could describe only
  the final subinterval. Fractions now use integrated supplied/requested work;
  routed reservoir/radiator rates are duration-weighted means.
- Zero-duration generator ramp evaluation could divide by zero.
- Fuel-port saturation could be mistaken for inventory exhaustion. Generator
  ceilings include inlet throughput/efficiency, and shutdown distinguishes
  inventory from the amount deliverable during one interval.
- Water molar mass was rounded inconsistently with H₂ and O₂. The host and Lua
  now use **0.01801528 kg/mol**; the closed electrolysis/fuel-cell cycle and
  independent atmosphere oracle retain strict assertions without drift.
- Generator, dispatch and battery-hour smoke fixtures retained references into
  temporary snapshot vectors. The affected elements are now copied.

### Open physical-calibration finding

**[#783](https://github.com/ajare/promethium-fermide/issues/783)** tracks the
pre-existing radiator coefficient: host and Lua use `5.670374419e-14` under a
kW/(m² K⁴) name, whereas the SI kW value is `5.670374419e-11` (1000× larger).
The radiator convergence oracle uses the current model coefficient; passing
it does not validate that calibration. This follow-up does not silently change
existing radiator balances/authoring by 1000×. Recalibration and configuration
compatibility require separate work.

Arbitrary nonlinear feedback, shared-fuel fleets, battery/fuel-cell startup
portfolios and every observable/configuration are not certified by these
fixtures.

## Comparable 60 Hz versus 10 Hz workload benchmark

`station-benchmark-workloads` advances identical fresh Station workloads over
the same 4200 World quanta: **4200 versus 700 Station steps**. Three paired runs
alternate frequency order. CTest runs this benchmark serially. Startup and
advance are timed separately; diagnostics and explicit final snapshot reads
are outside the advance timer. Normal runtime publication remains included.
No test-registration counts or mixed-frequency suite durations are compared.

Release, GCC 15.2.0, AMD Ryzen AI MAX+ 395, 32 logical CPUs:

| Workload | 60 Hz advance median | 10 Hz advance median | Speedup | Startup medians, 60/10 Hz |
|---|---:|---:|---:|---:|
| Biology | 1716.537 ms | 288.445 ms | **5.95×** | 15.863 / 15.632 ms |
| Bundled reference | 6326.788 ms | 1085.675 ms | **5.83×** | 18.382 / 18.266 ms |

Advance ranges: biology 1709.400–1729.929 / 288.082–291.915 ms; reference
6315.955–6348.129 / 1081.780–1085.743 ms (60/10 Hz). This is approximately 83%
less Station advance CPU time, not a whole-application FPS claim. The reference
window includes startup and early normal operation, not its later scheduled
maintenance/outage/logistics cycle. Timing is reported, not a flaky CI speed
threshold.

## Validation record

**39 distinct focused checks have recorded latest passes** across incremental
Release runs; no failures from this follow-up remain. This is not a single
39-test final run or a full-suite aggregate. Checks covered the five new fidelity
entries, benchmark, generator timing/funding/profiles, affected power/thermal/
connection/recovery/logistics/maintenance/events, species/biology/atmosphere,
editor generator workflow, metadata coverage and seconds lint. The last two
battery-hour lifetime checks passed in 5.18 seconds wall time.

Logs: `/tmp/pf-physical-third.log`, `/tmp/pf-physical-affected.log`,
`/tmp/pf-physical-final.log`, `/tmp/pf-physical-chemistry.log`,
`/tmp/pf-physical-last-two.log`, `/tmp/pf-physical-battery-lifetimes.log`,
`/tmp/pf-physical-metadata.log`.
Earlier failures in these logs were resolved by subsequent focused runs;
benchmark and coupled numerical results above are from the chemistry run.
Already-passing checks were rerun only when affected by subsequent changes.
No Debug/submodule tests or full-suite green-aggregate rerun was used.

Reproduce selectively in Release with `ctest --test-dir build-linux -C Release
-j 16 --output-on-failure -R '^station-fidelity-'`; use `-V -R
'^station-benchmark-workloads$'` for timing output.
