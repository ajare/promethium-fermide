# Compound water

Water ports accept plain or compound mass stores, with live quality gates,
per-constituent treatment removal and producer carry-over (#739–#744).
Schema 101 adds opt-in selective crop irrigation. Treatment now qualifies output
on complete energy evidence alone (#753); retired release-test fields require
explicit document conversion.

## Full-removal treatment (#740)

The existing composition editor seeds a compound store's constituent ledger.
Treatment draws the **actual blend**, including previous deposits and dilution:

- Treated mass = drawn water mass × recovery ratio, with no dry load or chemical energy.
- Concentrate = rejected water plus every drawn dry constituent and its chemical energy.
- A compound source requires a compound concentrate destination; a missing or
  plain destination is refused with the dry constituent named. A plain treated
  destination is valid because full removal cannot send it dry mass.
- Zero-water feed is a zero-rate refusal with `stream carries no water`.
- Output headroom and connection losses use the actual split and constituents.

A plain inflow to a compound **treatment water port** deposits water only. It
cannot introduce the destination's authored dry composition. This applies to
continuous and scheduled transfers, not to arbitrary uncharacterised materials;
material identity remains descriptive and never grants this permission.
Compound-to-plain transfers remain refused at the compound-material boundary.

Compound concentrate is a qualified product in its own right, allowing
same-composition routing and scheduled waste ejection. Qualified concentrate is
not purified water: the compound-to-plain boundary still
applies. Plain scalar concentrate keeps its historical held state, so existing
documents' treatment actuals remain unchanged. Treated output retains
the energy-evidence gate: complete energy qualifies output; partial energy holds it.

`StationSystemsStoreSnapshot::concentrations` and Lua `stores[i].concentrations`
report kg of each existing mass constituent per kg stream (energy excluded).
They derive from the actual ledger, not the authored composition. Empty inventory
reads zero; plain treatment water ports read zero dry load. Compound ledger slots
remain in `actualConstituents`; the editor shows live concentrations in Store
inspector telemetry and the live Resource stores section.

## Examples

Load either standalone configuration in Station systems and Apply:

- `resources/station-systems/scenarios/water-greywater.yaml`: clearing its
  `removalFractions` map selects full removal of
  100 kg/day with 0.2% organics and 85% water recovery gives **84.83 kg/day pure
  treated water** and **15.17 kg/day concentrate**, containing all 0.2 kg/day
  organics plus 14.97 kg/day rejected water. These are the exact mass-based
  specification equations, rather than the rounded 85/15 ticket shorthand.
  The shipped 95% organic-removal setting instead gives 84.84 kg/day treated
  with 0.01 kg/day organics, and 15.16 kg/day concentrate with 0.19 kg/day organics.
- `resources/station-systems/scenarios/water-legacy-anchor.yaml`: all-plain legacy
  split gives exactly 85 kg/day treated and 15 kg/day held concentrate, at the
  same energy and consumable dose. Pure water at recovery ratio 1 produces zero
  concentrate and runs even when that destination is full; a lower ratio still
  rejects its declared water fraction.

Focused Release CTest coverage: `station-compoundWater*` and
`station-editor-compound-water`, plus existing water, containment, transfer and
connection regressions. World YAML/binary reloads reproduce exact tick snapshots;
the editor workflow covers open, composition edit, undo/redo, draft serialization,
Apply and live inspector telemetry.

## Selective crop irrigation (#744)

A nonempty `cropPopulations[].irrigationUptakeFractions` map opts in. Keys are
`nutrientNitrogen`, `phosphorus`, and `potassium`; finite fractions lie in [0, 1].
For each kilogram of **pure water** required by photosynthesis and transpiration,
the current dry-to-water ratio determines the accompanying dissolved load. The
specified fraction is absorbed, capped at growth's demand for that nutrient;
separate fertilizer ports supply the deficit. Omitted species, unabsorbed N/P/K,
organics, atmospheric nitrogen and other minerals remain in the loop ledger.
A nonempty all-zero map retains every salt. Empty/absent maps preserve the old
homogeneous irrigation draw. Plain water remains supported; a zero-water stream
cannot grow crops. All coefficients are provisional, not validated agronomy.

Pure water reaches the atmosphere as transpiration; the photosynthetic water
share participates in the existing biomass stoichiometry. Uptake from irrigation
is counted in living nutrients, not in addition to fertilizer demand.
Snapshots/Lua expose `lastIrrigationUptake` and `irrigationUptake` (kg N/P/K);
existing nutrient-draw counters now include both sources in selective mode,
while `waterDrawnKg` counts pure water. The Flow inspector displays the irrigation
uptake separately and subtracts it from fertilizer-port actuals.

### Hydroponic salt accumulation

Open `resources/station-systems/scenarios/water-hydroponics.yaml` in Station
systems, Apply, and Run for **300 simulation seconds (0.003473 days)**.
This deliberately accelerated, finite-tank stress rig is not a day-scale crop
calibration: constant growth potential is 0.001 kg/s and pure transpiration is
0.01 kg/s. Circulation through the 1 kg hydroponic reservoir is idealized; the
model accounts for loaded make-up, selective uptake, pure vapour and purge, not
an explicit pipe transit time. Make-up tops up available tank headroom. The
atmosphere retains the vapour; no unmodeled condensate return is claimed.

| At 300 s | Purge enabled | Purge transfer removed |
| --- | ---: | ---: |
| Other-mineral concentration, kg/kg | 0.000911686390 | 0.002121767843 |
| Growth scale | 1 | 0.565581828384 |
| Cumulative dry growth, kg | 0.300000015646 | 0.243385121536 |
| First observed quality throttle | none | 96 s (0.001112 days) |

The crop tolerance is 0.0012 kg/kg other minerals. Purge holds concentration below
that gate for the entire horizon, with less than 0.000003 kg/kg change over the
last minute. To reproduce failure, **remove only the purge Transfer** from the
draft, Apply and confirm Reset; continuous Transfer rates cannot be zero.
Growth binds on `water quality: otherMinerals above crop tolerance` before
150 s (0.001737 days). The test closes N/P/K/minerals across all stores, living
biomass and purge, and water across stores, atmosphere and photosynthetic uptake
at each one-second observation, in addition to the per-tick constituent audit.

## Energy-evidenced treatment (#753)

Treatment may connect any stores. A full rated energy dose
qualifies treated output; a partial dose produces held, unqualified output.
There is no separate release-test consumable or class-conversion opt-in.
Process consumables, removal fractions, capacity limits and consumer quality
tolerances still apply. Retired `clearanceStoreId`, `clearancePerKg` and
`releaseToNormalClass` fields are refused with conversion-required diagnostics.

Open `resources/station-systems/scenarios/water-contamination.yaml`, Apply and
Run for **10 seconds**. Loaded ingress propagates through the
contaminated loop. Its crew/crop acceptance probes have zero dissolved tolerance and name
`water quality: total dissolved above crew/crop tolerance`; they consume no
loaded water. The authored normal consumers use the treated outlet. Wiring them directly to
the loaded loop is legal, but their water-quality tolerances prevent uptake. These are acceptance
probes, not a crew-exposure/health model.

After one startup tick, treatment processes 0.499166692700 kg over 10 seconds,
returning 0.395340020619 kg pure treated water and retaining
0.103826672082 kg concentrate. Its organic load is retained wholly
in the concentrate; normal crew/crops receive water. Reduce solar capacity to
1 kW and Apply/Reset: treatment still processes feed, but its incomplete energy
evidence holds output unqualified with an `energy curtailed` reason. No-power
operation leaves output empty. Generic cross-class transfers are legal; consumer
quality tolerances remain independent requirements.

Focused Release checks: `station-waterSelectiveIrrigation`,
`station-waterHydroponics`, `station-waterContamination`, and
`station-editor-water-flagships`. They use public World ticks/snapshots, C++/Lua
accounting, YAML/binary World replay, and real editor Open/edit/undo/redo,
configuration serialization and Apply/reset paths.
