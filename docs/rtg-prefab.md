# RTG prefab (#904)

Place **RTG (long-duration low-power source)** from the Station diagram's Prefabs
menu. `resources/prefabs/rtg-unit.yaml` is registered as the `radioisotope`
PrefabCatalogue resource. New variants are catalogue content, not component code.

The unit expands to exactly three ordinary entries:
`rtg1.isotope` (unthrottleable Primary heat source), `rtg1.hot` (Thermal
reservoir), and `rtg1.thermocouple` (Heat engine). Internal references are
namespaced with the instance. There is no RTG simulation component.

## Interfaces and calibration

- **Electrical out** aliases the engine's Power out, automatically on the main
  Power Bus like an ordinary engine. Provide a load or battery charge headroom.
- **Rejection out** aliases Rejection heat. Connect it to an outside reservoir
  of the selected rejection Thermal service class and give that reservoir a
  radiator. The station owns this chain; an unwired RTG is a draft, not an
  applicable standalone station.
- **Hot tap** aliases the hot reservoir's Heat out, upstream of conversion.
  Another thermal intake can draw here; competing withdrawals affect electricity.

Default: 2 kW beginning-of-life thermal output, 87.7 Julian-year half-life
(768778.2 hours), age zero, Carnot fraction 0.115. The hot reservoir starts at
500 C with capacity 100 kJ/K and a 0–1000 C band. The engine accepts hot-side
temperatures of at least 100 C and rejects only below 80 C. Its hot reservoir
and source are `generation`; its rejection class defaults to `moderate`.
The engine class describes its **sink**, not its hot source (ADR-0029).

At a 30 C sink, eta = 0.115 × (1 − 303.15 / 773.15) = 0.0699088:
approximately **140 W electrical and 1860 W rejection**. Auxiliaries are zero.
The source and engine ratings both start at 2 kW, so the engine cannot mine the
initial hot inventory at a station-plant-sized rate. This is an unattended,
long-duration low-power supply, not a substitute for a reactor.

The initial hot temperature is authored stored energy, not a thermal governor.
Actual reservoir temperatures determine efficiency. With an aged source, a hot
tap, or inadequate electrical acceptance, storage changes; with no acceptance,
the engine idles while the source keeps heating. Size the external rejection
chain and monitor temperatures. No radiation/shielding model is implied.

## Dials and authoring support

Five dials expose output (0.1–1000 kW), half-life (0.001–10000000 hours), initial
age (0–10000000 hours), conversion quality (0.01–0.2), and rejection Thermal
service class. `throttleable` remains false and is not addressable.

The existing numeric/boolean provenance format is unchanged. Thermal class uses
an integer index into `StationThermalServiceClasses` (low, moderate, warm,
generation); the editor presents a combobox and fractional indices are refused.
Changing it after wiring must still satisfy the ordinary sink-class validator.

Prefab parameters can optionally declare `additionalTargets`, each with
`nodeType`, `entryId`, and `field`. The same value is written to every target on
a candidate copy. This keeps the source rating, engine rating and commanded
intake coupled under sizing edits; missing or invalid targets refuse atomically.
Decay mappings address existing opted-in `wear.fadeHalfLifeHours` and
`wear.initialAgeHours`; conversion maps `carnotFraction`. A missing Wear opt-in
cannot be created by a dial.

An exposed, empty engine rejection endpoint is validated against a temporary
same-class reservoir **in an authoring-only copy**. The peer never enters the
expansion, persistence or World. All component validation remains strict and
unchanged; an arbitrary missing reference is still an error. Template defaults
are expanded and validated at catalogue load, including coupled targets.

## Aged units and Monitors (#905)

Set **Initial age (hours)** to **262980** for a thirty-Julian-year-old unit.
With the default 87.7-year half-life it opens at 78.89% of beginning-of-life
thermal output and continues on the same curve. Electrical output also depends
on the live temperature gradient, stored heat and bus acceptance, not just age.

The collapsed unit inspector exposes its constituent Monitors without exposing
internal settings. Select `rtg1.isotope` there to author a `heat-source` Monitor
on `decayFraction`, `decayedOutputKW` or `deliveredOutputKW`. `decayFraction` is
**the fraction lost**, not remaining output: an enabled Above rule at 0.3 warns
when the source has lost more than 30%. New Monitors remain disabled until enabled.
Select `rtg1.thermocouple` for `heat-engine` metrics `electricalOutputKW` and
`rejectionKW` (both last-step kW), alongside its existing trip/headroom metrics.
The isotope's published snapshot already carries ageing at t=0.

These are ordinary component subjects, not a provenance-dependent RTG simulation
object. Their Monitors persist and run even after ungrouping or catalogue loss;
normal component rename/delete cascades still apply. No dial exposes source
throttleability, directly or through coupled targets; dial endpoints are tested
to preserve that absence.

## Verification

`station-prefab-rtgDials`, `station-prefab-rtgMonitors`, `station-prefab-rtgConversionLedger`, and
`station-prefab-rtgDecayStandalone` cover the shipped catalogue, constrained dials,
actual delivered power and radiator rejection, temperature-sensitive efficiency,
per-step composite energy closure, mid-curve initial age, calendar decay with
and without electrical consumption, and identical published simulation after
serializing and stripping provenance. Existing reference-station content and
runtime integration are unchanged.
