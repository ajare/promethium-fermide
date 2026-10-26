# Primary Heat Source Specification

Status: approved. Published as GitHub issue **#899** (`ready-for-agent`). Not yet
implemented.

Purpose: the **prerequisite** to the RTG (and to the fission reactor). This spec
adds the missing root of the thermal graph — a component whose declared job is to
deliver heat — and documents what that unlock makes possible.

Label: `feature:heat-sources`.

---

## 1. The structural finding

**Heat in this model is only ever a by-product.** A complete enumeration of every
heat injector in the tick confirms it. Every call site that routes heat into a
thermal reservoir:

| Injector | Call site | Why it produces heat |
|---|---|---|
| Fuel cell | `src/core/StationTickGeneration.cpp:1303` | Waste from making electricity |
| Generator (waste) | `src/core/StationTickGeneration.cpp:1489`, `:1610` | Waste from making electricity |
| Generator (after-run) | `src/core/StationTickGeneration.cpp:1671` | Decay heat after electrical stop |
| Electrolyzer | `src/core/StationTickProcessing.cpp:209` | Waste from splitting water |
| Sabatier reactor | `src/core/StationTickProcessing.cpp:585` | Waste from the reaction |
| Methane processor | `src/core/StationTickProcessing.cpp:732` | Waste from processing |
| Microbial processor | `src/core/StationTickProcessing.cpp:1257` | Waste from processing |
| Equipment loads | `src/core/StationTickProtection.cpp:528` | Waste from operating |
| Chiller | `src/core/StationTickThermal.cpp:414` | Heat *moved* cold → hot |

Plus one non-component path: the reservoir's own `absorbedHeatKW`
(`include/core/StationSystems.h:617`), a **constant environmental gain** authored on
the reservoir itself, summed into the tick's heat-in at
`src/core/StationTickAtmosphere.cpp:939` and accumulated at `:1158`.

**There is no component whose primary function is to deliver heat.**

### 1.1 The consequence

The heat engine (`HeatEngineSettings`, `include/core/StationSystems.h:937`) reads a
hot reservoir via `hotReservoirId`, linked as a heat input at
`src/core/StationConnections.cpp:347`. But whatever fills that reservoir got there
*incidentally*, as some other machine's waste.

**The heat engine is therefore a waste-heat recycler, not a plant.** Its total
available input is bounded by how much waste heat other components happen to
generate. The designer cannot author a hot side — only inherit one.

This blocks an entire class of design: radioisotope sources, fission reactors,
solar-thermal collectors, deliberate furnaces. Anything whose job is to *make* heat
rather than spill it.

### 1.2 The model already knows about the gradient constraint

`src/core/StationPorts.cpp:892-895` refuses a heat engine whose hot source and
rejection reservoir are the same, because *"a single-temperature engine is a
perpetual-motion machine."* A nicely domain-respecting check — and a sign that the
thermal graph's semantics are taken seriously even where the source side is absent.

---

## 2. The decay decision: linear versus exponential

The RTG's defining property is decay on a fixed physical timescale. The question
was whether that needs a new mechanism.

### 2.1 The Wear layer already has everything except the curve

`InstallationWearSettings` (`include/core/StationSystems.h:427`):

| Wear layer property | Line | Matches radioactive decay? |
|---|---|---|
| `fadePerCalendarHour` — fades whether operating or not | `:432` | ✅ calendar-driven, duty-independent |
| Permanent, never healed by repair | `#843` semantics | ✅ |
| Presence is frozen topology | `#843` semantics | ✅ |
| Rates live-tunable, never repricing fade already banked | `#843` semantics | ✅ |
| Snapshot reporting of wear fraction | existing | ✅ |
| **Curve is linear** | — | ❌ **decay is exponential** |

### 2.2 The measurement

Pu-238: half-life **87.7 years** → ≈**0.79%/year** thermal output loss.

| Horizon | Linear @ 0.7%/yr remaining | True exponential remaining | Error |
|---|---|---|---|
| 30 years | 0.790 | 0.789 | **0.1%** — indistinguishable |
| 87.7 years | 0.386 | 0.500 | **23%** — badly wrong |

**The linear approximation is excellent over any plausible scenario horizon and
fails badly outside it.** And the qualitative error is worse than the quantitative
one: a linear source **dies** at `t = 1/rate`, while a real isotope asymptotes and
never stops.

This is the dangerous class of approximation — convincing exactly where it is
likely to be checked, structurally wrong where it is not.

### 2.3 Decision

**Add an exponential form to the shared Wear layer**, rather than inventing a decay
mechanism on the heat source.

```
InstallationWearSettings {
    fadePerCycle            // existing
    fadePerOperatingHour    // existing
    fadePerThroughputKg     // existing
    fadePerCalendarHour     // existing
    fadeHalfLifeHours       // NEW: 0 = disabled, linear behaviour unchanged
    maxWearFraction         // existing
}
```

**Default `0` keeps the linear path bit-for-bit unchanged**, so every existing wearer
in every existing document is unaffected. A clean expand.

**Exponential and linear compose multiplicatively — and that is physically correct,
not merely convenient.** A real RTG's electrical output degrades from two genuinely
independent processes:

- isotope decay: ~0.79%/year, exponential
- thermocouple degradation: ~2%/year early-life, roughly linear with time/use

Two different physics, multiplied, expressed on one machine with no special-casing.

**The `maxWearFraction` clamp stays as-is.** An exponential term alone never reaches
it asymptotically, so the clamp remains effectively a guard on the linear component.
No change needed.

### 2.4 Why not the alternatives

- **Bespoke decay on the heat source** — duplicates the accumulator, the
  never-healed semantics, the frozen-topology rule, the live-tunable-rate convention
  and the snapshot reporting. Leaves two implementations of one idea.
- **Reuse `ProfileSettings`** — profiles are *repeating* (`periodSeconds` with
  segments), or constant when `periodSeconds == 0`
  (`include/core/StationSystems.h:402`). A decay is neither. Wrong shape.
- **Linear fade and call it good** — see §2.2.

---

## 3. The component

A new station component kind — **primary heat source** — with a heat-only interface
into a named thermal reservoir, mirroring the shape the heat engine already uses on
its hot side.

```
HeatSourceSettings {
    std::string id;
    std::string targetReservoirId;   // heat-only interface, mirrors the engine's hot side
    std::string thermalClass;        // service class of the heat delivered
    double ratedOutputKW{0.0};       // heat delivered at rated, >= 0; 0 disables
    bool throttleable{true};         // false = cannot be commanded down (RTG)
    double commandedOutputKW{0.0};   // dispatch cap [0, rated]; ignored when !throttleable
    std::optional<InstallationWearSettings> decay;  // absent = constant output
    // fault opt-in — frozen topology
};
```

`thermalClass` draws on the existing service classes
(`StationThermalServiceClasses`, `include/core/StationSystems.h:384`:
`low`, `moderate`, `warm`, `generation`), so delivered heat is classified like
every other heat in the model.

Sizing follows the existing convention: `MaxStationThermalReservoirs{8}` and
`MaxStationHeatEngines{8}` (`:628`, `:970`) suggest a comparable cap for sources.

### 3.1 Hard boundary: no electricity

**The heat source produces no electricity.** Conversion belongs to the heat engine.

This keeps the source out of the power-producer generation total
(`StationTickAuditPower.cpp`, the `totalGeneration` accumulation) and prevents a
second, unconverted power path appearing in the single-pool audit. It is a
deliberate line, not an omission — and it is what makes the RTG a clean composition
rather than a hybrid that has to be special-cased in two audits.

### 3.2 Energy accounting: declared accounted thermal input

The source's energy enters the thermal ledger as a **declared accounted input**,
kept as a **distinct category** from the reservoir's environmental absorbed heat
(`totalAbsorbedKW`, `src/core/StationTickAuditPower.cpp:573`) so the two input
paths never blur.

The precedent is exact: absorbed environmental heat already enters the thermal
system as a tracked input that is *not* drawn from the power pool. The conservation
audit's job is that energy is **tracked**, not that it has a supply chain.

---

## 4. Throttleability

`throttleable = false` → the commanded output is ignored; the source always
delivers its decayed rating. This is a **declared machine property**, frozen as
topology — an RTG cannot be made throttleable by retuning it mid-run.

`throttleable = true` → the source obeys the standing dispatch convention:
commanded output is a **cap, never a boost**, exactly as the heat engine's
`commandedInputKW` works (`include/core/StationSystems.h:937`).

---

## 5. Over-temperature: already handled (corrected)

**Correction to an earlier draft of this spec.** It previously stated that
over-temperature checking "appears only on load heat sinks" and that a
deliberately-authored hot side was "a case the current checks were never written
for." That was wrong — it conflated two different mechanisms.

**Every thermal reservoir already raises an `above-limit` monitor.**
`src/core/StationSystemsMonitors.cpp:222-223` adds, for **every** reservoir, an
`above-limit` rule on `temperatureC` at `maxTemperatureC + 1e-9` with severity
**Warning**, plus a matching `below-limit`. This is reservoir-wide, not
load-specific. A primary heat source driving a reservoir over its declared limit
**already raises a Warning**.

**The tick-level check gates something else entirely.**
`src/core/StationTickAtmosphere.cpp:1292` and `:1319` test `load.heatSinkIndex`
against `maxTemperatureC` as a **restart-feasibility gate on the consuming load**
— *"restart held: heat sink at or above its maximum service temperature."* That is
a consumer-side gate, not a source-side limit.

**There is no undefined over-temperature gap.** The behaviour is defined.

**One emergent interaction worth asserting.** If a primary heat source drives a
reservoir above its limit, any load using that reservoir as a heat sink is **held
from restarting**. Physically sensible — waste heat cannot be dumped into a sink
that is already too hot — but worth a test so the interaction is understood rather
than surprising. Covered in ticket #901.

**What remains is a design question, not work:** is a Warning the right response to
an *unthrottleable* source driving a reservoir over its limit, or should the source
be capped? The source cannot be turned off, so the Warning is the only signal and
nothing intervenes. Worth a deliberate call; not a blocker and not a missing
behaviour.

It remains a **runtime consequence, not an authoring error**: whether a reservoir
overheats depends on the whole thermal system's state, not on the source alone. A
source that is safe next to a working radiator chain may be unsafe next to a broken
one.

---

## 6. Frozen topology

The decay opt-in and the throttleability declaration both join the live-node
topology-changed predicate, consistent with every comparable decision in this
codebase (wear opt-ins, fault opt-ins, fuel-cell chemistry, generator prime mover):

- a running Fault cannot be escaped by retuning the machine underneath it;
- a structural edit is subject to the paused-document seams and atomic rejection;
- rates *inside* the decay block stay live-tunable and never reprice fade already
  banked.

---

## 7. Persistence and schema

Additive named fields throughout:

- absent decay block → constant output;
- absent source → no source;
- documents with no heat sources write and read exactly as today.

Schema takes the next free version at implementation start, with the conditional
version chain claiming it only when a heat source is actually present. A station
with no heat sources writes the same version it writes today.

**Version numbers are a contended resource:** #890 took 123, #878 was asked to move
to 124, the generator prime-mover work takes the next free. Confirm at
implementation start.

---

## 8. Snapshot and metrics

Per source: **rated output**, **current decay fraction**, **delivered output**.

Station-wide thermal totals gain a **primary heat input** category, kept distinct
from absorbed environmental heat (`totalAbsorbedKW`) and from routed waste heat
(`totalRoutedKW`).

The existing metric pattern at `src/core/StationThermalMetrics.cpp:43`
(`add("Absorbed heat", R::Input, &S::absorbedHeatKW)`) is the shape to follow for
the new category.

---

## 9. What this enables

### 9.1 The RTG becomes a composition, not a new machine

```
[Primary heat source]      decaying, unthrottleable, low-grade
        ↓  heat only
[Thermal reservoir]      "generation" class, hot
        ↓
[Heat engine]            low fixed Carnot fraction = the thermocouple
        ↓
electricity + rejected heat → [sink] → [radiator]
```

**No new conversion machinery.** A thermocouple *is* a low-efficiency heat engine
and the model already has one. An isotope *is* a decaying heat source and this spec
adds one. An implementer places four existing-shaped components and gets a
physically honest RTG.

### 9.2 And a great deal besides

- **The heat engine becomes a primary plant.** The largest single win. It stops
  being capped by incidental waste heat and becomes a designable power plant of
  arbitrary scale.
- **The fission reactor becomes reachable.** Same unlock: a throttleable primary
  heat source with decay-heat-after-shutdown, plus a heat engine. Doing the reactor
  *without* this prerequisite means inventing the heat-source concept under maximum
  complexity pressure.
- **Solar-thermal collectors.** A primary heat source whose output follows the
  existing illumination profile — heat from sunlight rather than decay, for free.
- **Deliberate thermal architecture.** For the first time the designer can reason
  about where the hot side comes from.
- **Heat you cannot stop.** Every current source can be commanded to zero. An
  unthrottleable source cannot, which makes the rejection chain a genuine design
  constraint rather than a downstream consequence.
- **Mission age as a power-budget variable.** A decay clock means "this is a
  thirty-year-old station" shows up in delivered power, not just in flavour text.

---

## 10. Testing

**Standing rule: assert only on what a configured World publishes after being
advanced.** Never on integrator internals, never on private decay lookups.

**Seams — one primary, three supporting, zero new.** The same set approved for the
fuel cell (#891–#893) and generator prime-mover (#896–#898) features.

1. **Primary — configured-World seam.** `configureStationSystems` → advance via the
   station rig's step/seconds helpers → `getStationSystemsSnapshot()`.
2. **Persistence seam** — YAML round-trip plus reload: additive-field stability,
   absent-field defaulting, unknown-field refusal.
3. **Fault seam** — `injectStationFault`: frozen decay opt-in and throttleability
   against a running fault.
4. **Audit seam** — the per-tick conservation audit, which must close with the new
   primary-input category present.

### 10.1 The four tests that carry the value

**1. Decay is a clock, not a meter.** Advance with the heat engine consuming the
heat; advance again with nothing consuming it. The source's decay fraction must be
**identical**. If it differs, decay has been wired to duty instead of calendar time
and the entire RTG premise is broken. **The sharpest test in the set.**

**2. The shared-struct change is invisible.** Every existing wearer, with
`fadeHalfLifeHours` absent, must produce bit-identical wear to before the change.
This is what makes adding a field to a shared struct safe, and it protects every
existing station in the repo.

**3. The composition works with no special code.** A decaying heat source →
reservoir → heat engine must produce electricity with no component aware it is part
of an RTG. If this needs anything beyond placing four components, the prerequisite
has not unlocked what it claims.

**4. Unthrottleable means unthrottleable.** Commanding a non-throttleable source to
zero must not reduce delivery. Paired with: a throttleable source must obey, and
must still be a cap rather than a boost.

### 10.2 Prior art

- **Heat engine tracer-bullet checks (#804)** — a power source tested entirely
  through configured-World-published behaviour.
- **Wear-layer checks (#843, #848)** — permanent calendar-driven fade, frozen
  presence, live-tunable rates that never reprice banked fade.
- **Absorbed-heat accounting** — the precedent for a tracked non-pool thermal input.

New checks follow these shapes and take their own labelled CTest ownership.

**Verification lane:** Release only, `-j 16`, per `AGENTS.md`. The bundled reference
station has no heat sources and must simulate identically before and after — the
primary regression guard.

---

## 11. Open questions

1. **Over-temperature response for an unthrottleable source (§5).** The behaviour is
   already defined — a reservoir-wide `above-limit` Warning. The open call is whether
   a Warning suffices when the source cannot be turned off, or whether the source
   should be capped. **Not a blocker; no missing behaviour.**

2. **The RTG scale mismatch.** Real RTGs deliver hundreds of watts; this station's
   sources run in kilowatts and above. "Small emergency instrument source" and
   "primary deep-space plant" are very different components. This does **not**
   change the prerequisite — but it is the first thing the RTG spec must settle.

3. **Does the decay reference need to be authorable as an absolute timestamp, or is
   "beginning of life = simulation start" sufficient?** The former lets a scenario
   open mid-decay-curve without pre-advancing; the latter is simpler. Leaning
   toward authorable reference, since "this station is thirty years old" is one of
   the more attractive capabilities.

4. **Should `fadeHalfLifeHours` be exposed on every existing wearer immediately, or
   only wired up for the heat source in v1?** The field is general and harmless by
   default, but exposing it in every wearer's UI invites authoring against a curve
   nobody has validated for that machine.

---

## 12. Out of scope

- **The RTG itself** — its own spec: catalogue entry, decay constants, thermocouple
  efficiency, mass, and the scale question.
- **The fission reactor** — its own large feature.
- **Store-backed heat sources** — structurally a generator that outputs heat instead
  of electricity; a different component with a different ledger. v1 is
  declared-input only.
- **Electricity from the heat source** — hard boundary, see §3.1.
- **Radiation, contamination and shielding hazard modelling.**
- **Thermal storage treated as a source** — storage, not generation, as with the
  battery in the power system.
- **A generic role-based stream model** — inconsistent with the fixed-shape
  named-field components unless applied to all of them at once.
