# Atmosphere pathway stress verification (#792)

ADR-0026's closed-form diffusion is checked through `configureStationSystems`,
World advancement and `getStationSystemsSnapshot`, without a new solver seam.

## Calibration and horizons

Before pinning the long-horizon assertion, a Release CTest run observed a
10-second exchange through a fully open 1 m² pathway between two 100 m³
compartments at 0 °C. For each gas, the test infers the effective coefficient
from the observed mass-gap decay:

`k = -log(gap_after / gap_before) / (elapsed_seconds * 2 * 273.15 / 100)`

The measured oxygen, nitrogen, carbon-dioxide and water-vapour coefficients were
all **0.002**. The test retains that calibration assertion and output, so a
future coefficient change requires deliberate reconsideration of the horizon.
The pair's 95% relaxation time at this temperature is about 274 seconds.

`pathwayLongHorizon` joins three compartments of 100, 150 and 75 m³ at 0, 20
and 40 °C. It runs for 4800 simulated seconds, requiring equilibrium at every
publication from 3600 seconds onward (a further 1200-second hold). Each gas's
`m*T_K/V`, proportional to its partial pressure, must agree with the analytic
conserved-mass equilibrium to 1e-6 relative tolerance. Unequal volumes and
temperatures distinguish equal partial pressures from equal masses. Every step
also checks non-negative finite inventories, conservation, and no condensation
or temperature change.

`pathwayAbsurdGeometry` uses a 1e12 m² opening between 1e-6 and 2e-6 m³
compartments at different temperatures, with counter-flow and initially empty
species inventories. Over 600 simulated seconds (6000 default Station steps),
every publication must be finite, non-negative, conserved and monotone toward
its analytic equilibrium. The first step must already reach that interior
equilibrium, ruling out donor-inventory clamping as a false stability success.
Numerical comparisons use 1e-12 of the species total, not an absolute tolerance
large enough to hide these tiny inventories.

## Lanes and measured cost

Both new checks have `validation-stress`, not `validation-fast`, and 120-second
CTest limits. `scripts/tests/smoke_lanes.py` asserts the split, including existing
functional, thermal/command cadence and exhaustive component pathway checks.
No existing fast command, timeout or processor reservation changed.

Local Linux Release measurements, `ctest -C Release -L '^validation-fast$' -j 16`:

| | Before #792 | After #792 |
| --- | ---: | ---: |
| Tests | 455 | 455 |
| Sum of bounded registered timeouts | 40780 s | 40780 s |
| Wall time | 255.25 s | 224.82 s |

Command/timeout/processor inventories compared exactly equal. Wall times are
observations, not portable thresholds; the before run overlapped incremental
compilation. Both complete runs passed 454 tests and exposed the same existing
ownership-manifest omission for `StationPathwayChecks.cpp`. Adding its ownership
row was verified separately rather than repeating the costly full lane.

The new stress checks passed in 9.24 s (long horizon) and 0.98 s (absurd geometry)
when run together with the passing lane contract. These costs are outside the
fast lane. The ownership audit and all affected pathway registrations are also
verified separately on the final source state.
