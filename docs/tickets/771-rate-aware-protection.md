# #771: rate-aware protection and its certification

## Contract and scope

`core::StationProtectionBounds` in `include/core/StationProtection.h` is the
production registry for this slice. The measured **coarse protection ratio is
6** (nominally 0.1 s). Ratios 2, 3, 4, 5 and 6 are covered individually. This is
not a claim that 6 is the largest possible ratio, and it is not the all-observable
convergence certification belonging to #773.

The temperature bound is **1e-9 C** (the existing qualification association
allowance). It bounds excursions of the predicted envelope **while admitting
qualified work**, not the temperature of a body receiving unconditional heat
while that work is refused. The inventory bounds are zero additional material
below the drawable reserve or above capacity. Every registry row names its
basis, owning check and this evidence document.

Important distinctions, preserved from the existing model:

* Reservoir service-temperature and atmosphere pressure/O2/CO2 limits are
  **reported conditions**, not physical clamps or implicit shutoffs. An eclipse,
  an exhausted feed, inadequate heater capacity, continuing respiration, decay
  heat, retained source heat, or a leaking compartment can still violate them.
  No timestep can certify that inadequate equipment meets its service limits.
* Growth qualification stops growth, not circulation, lamps or respiration.
  Those independently accounted heat sources continue when growth is refused.
* Freeze protection anticipates the cold excursion and requests heating. Its
  setpoint bound requires adequate heating and actual electrical supply. A
  shortage remains explicit protected Unmet demand; it never buys fictitious
  heating or a temperature clamp.
* Ratio 1 deliberately retains the existing sampled protection and its original
  one-pass results. The new zero-additional-excursion claim is for **coarse
  admission**, not a retroactive claim that legacy samples had zero overshoot.
* Above ratio 6 the same algorithm runs, but the measurement here does not
  certify the protection margin. Finite-state, conservation, and failure audits
  continue to run. Editor certification presentation is #772.

## Pre-commit algorithm

The host ledger is authoritative. Before publishing a coarse step, its normal
allocator evaluates a transactional prediction using the actual shared power,
material and connection budgets. For every integration interval, each thermal
body retains a conservative range:

```
upper = max(integrated endpoint, start + duration * sum(positive component kW) / capacity)
lower = min(integrated endpoint, start - duration * sum(negative component kW magnitudes) / capacity)
```

Heating is not cancelled by simultaneous cooling. Opposing exchanger leaves and
radiators are bounded separately; the exchanger bound also includes the hub net
so transport losses cannot understate hub cooling. Latent heat, process heat,
buffer exchange, pump/heater work and the unconditional environmental terms are
included. Ranges retain extrema across **all** event/saturation sub-intervals,
not only the final endpoint.

Every used qualification gate is checked against that range before commitment.
An unsafe gate is latched refused for this step. A predicted freeze crossing
latches a heater request. The whole trial is restored and the allocator reruns.
This must repeat: power released by one refused machine can fund a second
machine that was harmless in the first trial. Refusals and heater requests only
accumulate, so the retry sequence terminates after a finite number of authored
gates. Only the stable, audited allocation becomes observable. There is no new
allocator, priority order, capacity clamp, phantom reserve, or material loss.

The transaction restores inventories, loss/delivery/harvest ledgers, connection
flows and retained heat, equipment state, protected credits, dispatch/recovery
signals, schedules, outages, and bounded event history. Lua lifecycles run only
once per Station step, outside the trial loop. Prediction state is temporary and
never saved or published.

Inventory protection already uses rate-times-duration doses capped by drawable
stock and headroom, with crossing-time resolution from #769. Those rules remain
unchanged and are represented in the protection registry rather than duplicated.

## Reproducible measurement

Release build and focused evidence command:

```
cmake --build build-linux --config Release --target pf-smoke-station -j 16
ctest --test-dir build-linux -C Release -V -j 16 \
  -R '^station-(rateAwareProtection|protectionReallocation|protectionFreeze|protectionGrossExcursion|saturationCrossing)$'
```

The permanent `StationProtectionChecks.cpp` harness uses the public standalone
Station drive and committed snapshots, never the prediction internals. It starts
from the bundled reference's real process declarations, isolates each machine,
provides ample feed/product headroom and power, reduces receiving capacity to
**1 kJ/K**, and places its qualification boundary only **1e-6 C** from its initial
25 C. Unconstrained controls must perform positive work, so indiscriminate
refusal cannot pass. Lower-window cases use real cold heat-exchanger coupling.
Each test also compares whole-duration and partitioned drives, including events,
and checks thermal conservation. The common horizon is 60 base quanta (nominally
one simulated second), derived by multiplication, not accumulated time.

Measured unsafe-case production over that horizon:

| Protected operation | Ratio 1 (legacy) | Every ratio 2--6 | Quantity |
| --- | ---: | ---: | --- |
| Generator upper sink | 0.00158730 | 0 | fuel energy, kJ |
| Fuel cell upper sink | 2.31559e-7 | 0 | hydrogen consumed, kg |
| Electrolyzer upper sink | 8.33333e-7 | 0 | water split, kg |
| Sabatier upper sink | 3.33333e-7 | 0 | CO2 consumed, kg |
| Methane processor upper sink | 1.66667e-7 | 0 | methane consumed, kg |
| Microbial upper / lower window | 1.66667e-7 | 0 | feed consumed, kg |
| Crop upper / lower window | 1.38889e-6 | 0 | growth, kg |
| Algae upper / lower window | 8.33333e-7 | 0 | growth, kg |
| Chiller upper condenser | 0.05 | 0 | extracted heat, kJ |

The observed additional **admitted-work** excursion is zero in every coarse
boundary fixture, inside the registered 1e-9 C allowance. The legacy production
shows why checking only the initial sample admits work into the crossing
interval. Lower-bound first-interval measurements are checked separately, since
later physical warming may legitimately restore eligibility.

Additional adversarial evidence:

* `protectionReallocation`: two electrolyzers on distinct tiny sinks compete for
  exactly one machine's electrical dose. Both must be refused after successive
  predictions. Five sub-step deliveries still add exactly 0.05 store units;
  rejected trials consume no feed and leave no heat behind.
* `protectionGrossExcursion`: radiator rejection makes the endpoint **colder**,
  but gross process heating would cross the upper qualification margin. The
  process must be refused without clamping or deleting the radiator's cooling.
* `protectionFreeze`: a 1 kJ/K loop starts at 5.001 C against a 5 C setpoint and
  a cold exchanger. With sufficient supply its heater requests and receives
  10 kW immediately: the first committed temperature is 5.64640 C, with zero
  protected Unmet demand. With only 0.5 kW generation, heater supply is zero,
  temperature is 4.81287 C, and protected Unmet demand is 10.5 kW. This shortage
  is explicitly **not** concealed by enforcing a fictitious setpoint.
* `rateAwareProtection` also checks a tiny 0.001 kg cold loop at ratios 2--6:
  finite chiller extraction, including pump work, never takes it below its
  5 C chilled setpoint (within the registered association allowance), and its
  energy account closes.
* `saturationCrossing`: reserve depletion, capacity exhaustion, held inventory,
  connection throughput/efficiency and all-loss paths retain their existing
  exact material accounting at ratios 1, 6 and 30.

## Validation

The Release Station fast core suite (excluding editor and long stability checks)
passed **169/169** checks, including the existing protection, thermal, atmosphere,
biology, generation, recovery, conservation and scheduled-event scenarios at the
default ratio 1. After final review, **21/21** focused protection, default-ratio
regression and conservation checks passed, as did both the Station clock-boundary
and seconds-based-test lints. Long-duration stability and editor validation are
not claimed by this slice.

The earlier #764 representative-outage spike did not approach a limit. Its zero
observed overshoot was explicitly not certification. These deliberately
margin-tight, positive-control fixtures replace that missing evidence; they do
not extend the scope of any reported-condition limit into an enforceable cap.
