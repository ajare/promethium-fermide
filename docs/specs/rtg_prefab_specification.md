# RTG Prefab Specification

Status: approved. Published as GitHub issue **#900** (`ready-for-agent`). Not yet
implemented.

**Blocked by #899** — Station primary heat source. Native `blockedBy` relationship
set on the tracker. Hard dependency, not advisory.

Label: `feature:heat-sources`.

---

## 1. The reframe: the RTG is not a component

The initial framing — a new radioisotope component with its own tick path, snapshot
surface and validator branch — turned out to be unnecessary. The **prefab expander
(#810)** already provides exactly the needed mechanism.

From `include/core/StationPrefab.h`:

> *"Prefabs (#810): authoring-time composite machine templates. A template carries
> internal entries written in the existing stationSystems entry vocabulary, an
> exposed-port alias table, and a parameter (dial) mapping table. Expansion
> flattens a template into ordinary draft entries with namespaced ids; **the
> simulation, the authoritative reader and the validator never learn that prefabs
> exist.**"*

**This is the difference between a small feature and a large one**, and it was only
visible after finding the expander. The habit worth keeping: **look for the
composition mechanism before inventing the component.**

### 1.1 The substrate

Verified in `include/core/StationPrefab.h`:

| Structure | Role |
|---|---|
| `PrefabTemplate` | `id`, `displayName`, `instancePrefix`, internal `entries` (a full `StationSystemsSettings`), `exposedPorts`, `parameters` |
| `PrefabExposedPort` | A composite-facing name aliasing one connector of an internal template entry |
| `PrefabParameter` | A dial mapped to one field of an internal entry, with default and permitted numeric range |
| `PrefabOverrides` | Per-instance override map (`std::variant<double,bool>`) |
| `PrefabIdSeparator` | `"."` — namespacing is `<instance>.<templateEntryId>` |
| `PrefabReservedIdDelimiter` | `':'` — reserved (#822); instance ids must never contain it, since layout keys, pin keys and authored-link identities all split on it |

The catalogue document (`prefabCatalogue` → `prefabs` → array of templates) parses
**transactionally** — `parsePrefabCatalogue`, `src/core/StationPrefab.cpp:522`:
the first malformed template or duplicate id refuses the whole catalogue and leaves
the output empty, so a station can never end up with half its units loaded.

Internal entries are read through the authoritative `stationSystems` reader, so the
existing entry format and validation rules apply **verbatim**.

---

## 2. The composition

```
[Primary heat source]   decay opt-in present, throttleable = false   ← #899
        ↓ heat
[Hot reservoir]         "generation" thermal class
        ↓
[Heat engine]           low Carnot fraction — this IS the thermocouple
        ↓
exposed: Electrical out · Rejection out · Hot tap
```

The hot reservoir is **internal** to the composite. A real RTG is sold as a sealed
unit with power leads and a heat-rejection surface; that is the shape the prefab
presents.

**Exposed ports:**

| Port | Aliases | Why |
|---|---|---|
| **Electrical out** | heat engine power-out connector | Joins the power system like any source |
| **Rejection out** | heat engine cold-side rejection | Feeds the station's radiator chain |
| **Hot tap** | a connector on the internal hot reservoir | A warm-water or process-heat loop can take heat upstream of the converter — a genuine capability of the real device, free to expose |

**No new simulation component. No new conversion machinery. No new tick code.** The
RTG flattens to ordinary entries and simulates exactly like the same three
components placed by hand.

---

## 3. The dial set is a constraint, not a convenience

**`throttleable` is deliberately NOT exposed as a dial.** It is fixed to `false` in
the template. Exposing it would let an author make an RTG throttleable — not a
thing a real radioisotope unit can do, and it destroys the property that makes the
unit interesting.

This is the same principle as the code-owned profile in the fuel-cell (#890) and
generator-prime-mover (#894) work, applied one layer up: **what the author may vary
is a deliberate boundary on the machine's identity, not an open field set.**

Dials that *are* exposed:

| Dial | Maps to | Rationale |
|---|---|---|
| Beginning-of-life thermal output | heat source `ratedOutputKW` | Sizing |
| Half-life | decay `fadeHalfLifeHours` | Variants can use different isotopes |
| **Initial age** | decay age offset | See §4 — the real work |
| Conversion quality | heat engine `carnotFraction` | Thermocouple grade |
| Thermal class | reservoir / engine class | Integration |

---

## 4. The initial-age offset: the one real new behaviour

The Wear layer (`InstallationWearSettings`, `include/core/StationSystems.h:427`)
accumulates from zero. A half-life alone therefore decays from "brand new at
t = 0".

**Lowering the rated output instead is not equivalent.** It changes the *level*
without changing the *rate* — a supposedly thirty-year-old unit would keep decaying
at its first-year rate. The error is invisible at authoring time and only shows over
long horizons.

Required form:

```
output(t) = ratedOutput × 2^(−(initialAge + t) / halfLife)
```

so the unit starts partway down the curve and continues along the *same* curve.

The simulation's time model supports this directly: `elapsedSeconds` is the clock
(available at the tick and in profile evaluation), with no absolute epoch — so an
authored *age* is the natural expression, not a timestamp.

**This belongs in the shared decay form, not in the prefab**, because any decaying
component may want to start aged. It should land with #899's decay work so the
shared form is complete before anything composes on it.

Same *class* of error as the linear-versus-exponential trap recorded in #899: right
where you would check it, wrong in a way that only emerges later.

---

## 5. The Carnot fraction captures the thermocouple better than an efficiency would

The heat engine (`HeatEngineSettings`, `include/core/StationSystems.h:937`)
expresses conversion as `carnotFraction` — *actual / Carnot efficiency* — rather
than as a fixed absolute efficiency.

Setting a low fraction models a thermocouple **and** inherits the
physically-correct behaviour that output varies with the temperature difference
across the machine:

- a hotter hot side yields more electricity;
- a reservoir drifting toward its sink yields less.

That is truer than a dedicated fixed-efficiency thermocouple model, and it comes
free from a component that already exists.

Rough calibration for a real thermocouple: ~7% absolute efficiency across a
~500 °C / ~30 °C difference is a Carnot fraction near **0.11–0.12**.

---

## 6. Scale: positioned as a low-power source, not a plant

Real radioisotope units deliver **hundreds of watts**, against this station's
kilowatt-and-above sources. Rather than inflate the fiction:

- The catalogue default sits at realistic scale.
- The dial range permits larger units where a scenario needs them.
- **The RTG is explicitly not the plant.** Long-duration, low-power, unattended,
  works where solar cannot. The fission reactor is the thing that scales to station
  power.

Naming the role honestly keeps the catalogue credible and stops the RTG being
mistaken for a reactor substitute.

---

## 7. Provenance

The provenance block (#814) records which template, which instance and which
overrides produced the flattened entries. Per the existing design:

> *"The expansion is the truth and stands on its own; this block is editor metadata
> used to re-group the composite on load, and is inert wherever the template is
> absent."*

That property is load-bearing for this spec's test 5 (§8.1): the flattened entries
must be a valid standalone station.

---

## 8. Testing

**Standing rule: assert only on what a configured World publishes after being
advanced.** Never on integrator internals, never on private template lookups.

**Seams — two, both existing, zero new.**

1. **Primary — configured-World seam.** Expand the prefab, feed the flattened
   settings to `configureStationSystems`, advance via the station rig's
   step/seconds helpers, assert on `getStationSystemsSnapshot()`. Tests the RTG as
   a *machine*.
2. **Prefab seam — template + overrides → flattened entries.** The existing #810
   expansion seam. Tests namespacing, link writing, override application, exposed
   port aliasing and provenance.

**The discipline that matters: most tests go through seam 1, not seam 2.** Testing
the RTG mostly means testing that the flattened thing behaves like a radioisotope
unit — not that the prefab machinery works, which its own checks already cover.

### 8.1 The five tests that carry the value

**1. Open mid-curve.** Author `initialAge = 30 years`, `halfLife = 87.7 years`.
Output at t=0 must be ≈79% of beginning-of-life, and must keep decaying from there
along the same curve rather than restarting. This is the capability the scenario
author actually wanted, and the test that proves the age offset is a **rate** offset
rather than a level offset.

**2. The clock is not a meter.** Advance with the electrical output fully consumed,
and again with nothing consuming it. Decay fraction must be **identical**. Inherited
from #899 but re-asserted *through the composite*, because the composition is where
a duty-coupling could plausibly be reintroduced.

**3. The dial set cannot break the machine.** Assert that **no exposed dial makes the
unit throttleable.** An assertion about absence — unusual, but it is what protects
the design principle. A future edit that exposes `throttleable` as a dial should
fail this test.

**4. The ledger closes across the whole chain.** Heat delivered by the source equals
electricity out plus rejection out plus reservoir storage change, within tolerance,
at every tick. This is the composite-level conservation test, and it is the one that
catches heat or power going missing *between* components rather than inside them.

**5. The flattened output is a valid standalone station.** Expand, strip the
provenance block, reload: the entries must stand on their own and simulate
identically. Proves the expansion is a real flattening rather than something that
needs the template to survive.

### 8.2 Prior art

- **Prefab expansion and provenance checks (#810, #814, #822)** — the seam this
  rides on.
- **Heat-engine tracer-bullet checks (#804)** — a power source tested through
  configured-World-published behaviour.
- **Wear-layer checks (#843, #848)** — permanent calendar-driven fade.
- **Primary-heat-source checks (#899)** — the component directly underneath this one.

**Verification lane:** Release only, `-j 16`, per `AGENTS.md`. The bundled reference
station has no RTG and must simulate identically before and after.

---

## 9. Dependencies

**Hard dependency on #899** (primary heat source). Without it there is still
nothing in the model that can author a hot side, and the RTG would have to invent
one. The age-offset addition should land with #899's decay work.

No dependency on the fuel-cell (#890) or generator-prime-mover (#894) work, though
all three share the code-owned-constraint philosophy.

---

## 10. Open questions

1. **How many RTG variants ship initially, and do they differ by isotope
   (different half-lives) or only by size?** An 87.7-year Pu-238-class unit and a
   shorter-lived, higher-power unit are meaningfully different design choices and
   give the catalogue very different character.

2. **Should the hot tap be exposed by default, or only on a variant template?**
   Exposing it is free and genuinely useful, but it also invites authors to take
   heat before the converter, which changes the unit's electrical behaviour in ways
   that may surprise.

3. **Does the composite need its own rejection reservoir internally, or should
   rejection go straight to an exposed port?** Internal keeps the unit sealed and
   self-contained; exposed keeps the station's thermal graph flatter. Leaning
   exposed, with the station owning the sink.

---

## 11. Out of scope

- **The fission reactor** — its own large feature; this prefab is the template for
  how it will be built, but it is not this.
- **Radiation, contamination, shielding and hazard semantics.** A real
  radioisotope source carries serious handling and hazard properties. None of that
  is modelled. The RTG here is a heat-and-electricity contract and nothing more.
- **RTG as a primary plant** — explicitly not the role; see §6.
- **Isotope procurement, fuel supply chains, unit replacement.** The decay is a
  declared property, not a consumable drawn from a store.
- **A distinct thermoelectric-generator component** — the heat engine at a low
  Carnot fraction *is* the thermocouple, and more honestly so.
- **Radiator and rejection-chain design** — exposed as a port; what the station
  does with it is the station's business.
- **New simulation component types.** The RTG adds none. If this spec appears to
  need one, the composition has gone wrong.
- **Age-based failure modes beyond decay.** Real RTG failure is dominated by
  thermocouple degradation and specific-heat drift; only generic decay and the
  existing fault opt-ins are modelled.
