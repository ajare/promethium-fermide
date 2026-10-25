# Station Fuel Cell Chemistry Specification

Status: approved. Published as GitHub issue **#890** (`ready-for-agent`). Not yet
implemented. All design questions resolved — see §16 (Decisions taken); §17 is
clear.

Initiative: Hindsight `kp-3666d8fea9334bb2a590cacc24a562ba`.

Canonical vocabulary: `CONTEXT.md` § Station systems. This spec adds **Fuel Cell
Chemistry** and **Chemistry Profile**. It changes no other noun, adds no port, and
supersedes no ADR.

Parent surface: the Fuel cell component (issue #406; `docs/station-components.md`
§ Fuel cell). Related: `docs/specs/sector_power_system_design_specification.md`
(consumable endurance), `docs/specs/station_power_buses_specification.md`
(source/bus role membership — independent of this spec).

---

## 1. Problem statement

The Fuel cell is a single chemistry. `FuelCellSettings` hardcodes the hydrogen/oxygen
reaction: three named store fields (`hydrogenStoreId`, `oxygenStoreId`,
`waterStoreId`), the stoichiometric constant
`StationFuelCellOxygenMassPerHydrogenKg`, and the fuel energy constant
`StationElectrolysisChemicalEnergyHydrogenKJPerKg`. Every real fuel cell is
hydrogen/oxygen in this model, so PEM, alkaline and solid-oxide machines — which
differ in efficiency, waste-heat grade, contamination tolerance and failure modes —
cannot be distinguished at all.

## 2. Decision

Add a **Fuel Cell Chemistry** enum to `FuelCellSettings`. The enum selects a
**code-owned Chemistry Profile**: the admitted efficiency band, the default maximum
sink temperature, the nominal operating temperature (reported, not simulated), and
which fault opt-ins the chemistry admits.

**The enum varies machine characteristics only. It does not vary stream topology.**
Every chemistry keeps exactly the current shape: two reactant stores in, one product
water store out, one heat-sink reservoir out, hydrogen/oxygen reaction throughout,
unchanged stoichiometry, unchanged port set.

This is the whole of the design's central constraint, and it is deliberate. The
alternatives that vary the stream set — a generic role-based stream table, or a
widened fuel/oxidant/product/byproduct shape — are excluded (§14) because they
would break the shape that `PortDescriptor`, the validator's
`declareGasStore`/`declareWaterStore` helpers, the Flow projection, the forecast,
and the snapshot all currently share with Sabatier, the Electrolyzer and the
Methane processor. Generalising one component's streams would make the codebase
less consistent, not more.

## 3. The chemistry catalogue

Three chemistries ship in v1. All are H₂/O₂; they differ in electrolyte, temperature
and tolerance.

| | `hydrogen-pem` | `hydrogen-alkaline` | `hydrogen-solid-oxide` |
|---|---|---|---|
| Real device | PEMFC | Alkaline fuel cell (AFC) | SOFC |
| Electrolyte | PFSA polymer membrane | aqueous KOH | yttria-stabilized zirconia ceramic |
| Ion conducted | H⁺, anode → cathode | OH⁻, cathode → anode | O²⁻, cathode → anode |
| Nominal operating °C | 75 | 200 | 750 |
| Admitted efficiency band | `(0, 1]` | `[0.35, 0.75]` | `[0.35, 0.70]` |
| Default efficiency | 0.60 | 0.60 | 0.55 |
| Default `maxSinkTemperatureC` | 80 | 95 | 200 |
| CO / carbon-poisoning tolerance | poor | very poor | tolerant |
| `catalyst-poisoning` opt-in | admissible | admissible | **refused** |
| `stack-degradation` wear opt-in | admissible | admissible | admissible |

Notes on the numbers:

- **`hydrogen-pem` keeps the unconstrained `(0, 1]` band on purpose.** It is the
  legacy default (§7.2), so no pre-bump document can fail to round-trip once the
  tightened rule exists. Tightening PEM is a separate change with its own migration;
  this is a compatibility concession, not an oversight.
- **Nominal operating temperature is reported, not simulated.** The Fuel cell has no
  startup contract (unlike the Generator's `startupPowerKW`/`startupSeconds`), so
  the hours-long SOFC cold start and the alkaline stack's carbonate-on-shutdown
  behaviour are **not expressible** in this shape. The value exists so the UI and
  docs can state what the machine is, and so a future startup contract has a place
  to read from. It gates nothing.
- **SOFC refuses `catalyst-poisoning`** because its ceramic catalyst is genuinely
  tolerant of the feed contamination that poisons PEM and alkaline stacks. This makes
  SOFC strictly more fault-tolerant in v1, because no SOFC-specific fault mode
  (thermal-shock on cold start, coking) is in scope. That asymmetry is documented
  here rather than left as an accident; a follow-up may add SOFC faults and rebalance.
- Efficiency bands are planning figures for the stated device class, not
  flight-qualified ratings — the same standing as
  `sector_power_system_design_specification.md` §1.

## 4. Data model

Following the existing `StationDispatchMode` pattern (`StationSystems.h:2227-2233`):

```cpp
// The fuel cell's electrochemical chemistry (#890). The chemistry selects a
// code-owned profile: the efficiency band the validator admits, the default
// maximum sink temperature, and which fault opt-ins the machine has. It does
// NOT select the stream set. Every chemistry keeps the cell's fixed topology -
// hydrogen and oxygen in, product water out, waste heat out - with the same
// stoichiometry, so no port, no store role and no ledger shape changes here.
// Chemistries whose products differ from water (methanol, metal-air) are not
// expressible in this shape; see the spec's out-of-scope section.
enum class StationFuelCellChemistry : std::uint8_t
{
    HydrogenPem,
    HydrogenAlkaline,
    HydrogenSolidOxide,
};
inline constexpr std::array<char const*, 3> StationFuelCellChemistryNames{
    "hydrogen-pem", "hydrogen-alkaline", "hydrogen-solid-oxide"};

// What a chemistry contributes. Code-owned and constexpr: the user tunes
// within the envelope the profile declares, and cannot author a cell that
// violates its own chemistry.
struct FuelCellChemistryProfile
{
    std::string_view label;
    std::string_view electrolyte;
    std::string_view ionConducted;
    double nominalOperatingC;
    double minEfficiency;          // inclusive
    double maxEfficiency;          // inclusive
    double defaultEfficiency;
    double defaultMaxSinkTemperatureC;
    bool supportsCatalystPoisoning;
};

inline constexpr std::array<FuelCellChemistryProfile, 3>
    StationFuelCellChemistryProfiles{{
        {"PEM", "PFSA polymer membrane", "H+ (anode to cathode)",
            75.0,  0.0, 1.00, 0.60,  80.0, true},
        {"Alkaline", "aqueous KOH", "OH- (cathode to anode)",
            200.0, 0.35, 0.75, 0.60,  95.0, true},
        {"Solid oxide", "yttria-stabilized zirconia", "O2- (cathode to anode)",
            750.0, 0.35, 0.70, 0.55, 200.0, false},
    }};

inline FuelCellChemistryProfile const& stationFuelCellChemistryProfile(
    StationFuelCellChemistry chemistry)
{
    return StationFuelCellChemistryProfiles[static_cast<size_t>(chemistry)];
}
```

`FuelCellSettings` gains exactly one field:

```cpp
struct FuelCellSettings
{
    std::string id;
    StationFuelCellChemistry chemistry{StationFuelCellChemistry::HydrogenPem};
    // ... all existing fields unchanged, in existing order ...
};
```

The field is placed after `id` and before the store references. No existing field is
renamed, retyped, reordered or removed. `operator==` remains `= default`.

## 5. Validation

All changes land in the existing fuel-cell loop (`StationSystemsValidation.cpp:2299`).
Every refusal is atomic — no partial draft state, contextual message — matching the
component's existing behaviour.

### 5.1 New refusals

| Condition | Message shape |
|---|---|
| Unknown chemistry value on load | `Station fuel cell 'x' has unknown fuel cell chemistry 'y'; declared chemistries are: hydrogen-pem, hydrogen-alkaline, hydrogen-solid-oxide` |
| `efficiency` outside the profile band | `Station fuel cell 'x' efficiency 0.82 is outside the admitted band [0.35, 0.70] for chemistry 'hydrogen-solid-oxide': a cell cannot be authored above what its chemistry converts` |
| `catalystPoisoning` present on a chemistry that does not support it | `Station fuel cell 'x' opts into catalyst-poisoning but chemistry 'hydrogen-solid-oxide' is not feed-contaminated: the fault mode does not exist for this stack` |

The existing generic `(0, 1]` efficiency rule stays as the outer bound; the profile
band is applied inside it.

### 5.2 Unchanged

Everything else about the fuel cell validates exactly as today: id non-empty and
within `MaxStationStoreIdLength`, id uniqueness, thermal class membership,
`ratedOutputKW` finite and non-negative, `commandedOutputKW` within rating,
`maxSinkTemperatureC` finite and above absolute zero, the hydrogen/oxygen store
aliasing refusal (#585), `declareGasStore` / `declareWaterStore` role checks, and
heat-sink existence plus thermal-class match.

`MaxStationFuelCells` (8) is unchanged and is **not** per-chemistry: the cap is on
cells, not on chemistry kinds.

## 6. Ports

**No change.** `StationPorts.h:91-94` keeps the four descriptors verbatim:

```
Fuel cell / Hydrogen in   Input   material  inventory
Fuel cell / Oxygen in     Input   material  inventory
Fuel cell / Water out     Output  material  inventory
Fuel cell / Heat out      Output  heat      heat-sink
```

This is the point of the design. `PortDescriptor` is keyed by `nodeType` string
alone with no per-variant dimension (`StationPorts.h:18-33`); because the stream
set does not vary, the port table needs no change, the Flow projection needs no
change, and `stationPlaceableTypes()` gains nothing. The `members[]` role table at
`StationPorts.cpp:582-586` is untouched.

## 7. Persistence and migration

### 7.1 Wire form

Named-field, additive, mirroring the `dispatchMode` pattern
(`StationReference.cpp:196` / `1488-1494`):

```cpp
// write (StationReference.cpp, fuelCells array)
serializer.writeString("chemistry",
    StationFuelCellChemistryNames[static_cast<size_t>(cell.chemistry)]);

// read
FuelCellSettings cell;
cell.id = serializer.readString("id");
if (serializer.hasField("chemistry"))
{
    auto const value = serializer.readString("chemistry");
    if (value == "hydrogen-pem")            cell.chemistry = StationFuelCellChemistry::HydrogenPem;
    else if (value == "hydrogen-alkaline")   cell.chemistry = StationFuelCellChemistry::HydrogenAlkaline;
    else if (value == "hydrogen-solid-oxide")cell.chemistry = StationFuelCellChemistry::HydrogenSolidOxide;
    else throw SerializationException("unknown fuel cell chemistry '" + value + "'");
}
// absent: cell.chemistry keeps its default, HydrogenPem
```

Written unconditionally on save. Read optionally.

### 7.2 Schema

`kWorldSchemaVersion` bumps **122 → 123** (`include/core/WorldDocument.h:24`), and
the conditional version chain at `WorldSerialization.cpp:1075` gains a term so a
document only claims 123 if it actually carries a non-default chemistry. A station
whose cells are all PEM writes the same version it writes today.

**Backward compatibility:** a pre-123 document has no `chemistry` field. It loads as
`HydrogenPem`, which is exactly its current behaviour, and validates under PEM's
unconstrained `(0, 1]` band. No migration pass is required and no authored value
is rewritten.

**Forward compatibility:** a 123 document opened by a pre-123 reader is refused by
the existing `version > kWorldSchemaVersion` guard (`WorldSerialization.cpp:1983`).

### 7.3 Round-trip

Save → load → save is byte-stable for the chemistry field. The bundled
`resources/station-systems/reference-station.yaml` (`fuelCellBackup`, line 620) is
left **without** a `chemistry` key so the absent-field path stays under continuous
test coverage. One new fixture adds the key explicitly.

## 8. Runtime behaviour

The tick reads the profile instead of the bare constant in exactly one expression
(`StationTickGeneration.cpp:1075-1076` and its mirror at `1151-1153`):

```cpp
auto const& profile = stationFuelCellChemistryProfile(cell.authored.chemistry);
auto const elecKJPerKgH2 = cell.authored.efficiency
    * StationElectrolysisChemicalEnergyHydrogenKJPerKg;
```

`StationElectrolysisChemicalEnergyHydrogenKJPerKg` (119,960 kJ/kg H₂ LHV) and
`StationFuelCellOxygenMassPerHydrogenKg` (≈7.94) are **not** per-chemistry: all
three chemistries burn the same fuel with the same stoichiometry. The profile
constrains what `efficiency` may be at authoring time; it introduces no new runtime
arithmetic.

Everything downstream is untouched: the reactant-draw and product-headroom limiters,
the `byH2KW` / `byO2KW` / `byWaterKW` cap set, the limiter-naming rule, the
endurance clock, waste-heat routing to the class-matched sink, the
`sinkQualified` gate, the #415 automatic-dispatch command override, and the
single-pool audit at `StationTickAuditPower.cpp:436`.

**Net runtime delta: zero new arithmetic.** The chemistry is fully resolved at
authoring time. This is the property that makes the change cheap and safe.

## 9. Faults, wear, and live-node topology

`LiveNode<FuelCellSettings>::topologyChanged` (`StationLiveNodes.h:325-333`)
currently returns true when either opt-in's presence flips. **Chemistry is added to
that predicate**: changing chemistry changes which fault modes exist, so it is a
topology change, not a live-tunable parameter.

```cpp
static bool topologyChanged(FuelCellSettings const& a, FuelCellSettings const& b)
{ return a.stackDegradation.has_value() != b.stackDegradation.has_value()
    || a.catalystPoisoning.has_value() != b.catalystPoisoning.has_value()
    || a.chemistry != b.chemistry; }
```

Consequences, all consistent with the existing frozen-topology contract:

- chemistry cannot be changed while a Fault is running against the cell;
- a chemistry change is a structural edit, subject to the same paused-document
  seams and atomic-rejection rules as adding or removing an opt-in;
- an injected `catalyst-poisoning` Fault is never reachable by retuning chemistry
  out from under it.

No new fault or wear mode is introduced. `stack-degradation` behaves identically
across all three chemistries.

## 10. Snapshot and telemetry

`StationFuelCellSnapshot` (`StationSystems.h:4400`) gains one field:

```cpp
std::string chemistry;   // wire name: "hydrogen-pem" | "hydrogen-alkaline" | "hydrogen-solid-oxide"
```

Published from the authored setting at `StationSystemsSnapshot.cpp:1225`. All
existing fields — output, reactant draws, product water, waste heat, limiter,
block reason, endurance, wear — are unchanged and are already chemistry-agnostic
because they are derived from the authored efficiency.

`StationPowerSnapshot.fuelCellOutputKW` and `fuelCellWasteHeatKW` remain
chemistry-blind station totals. No per-chemistry rollup is added: with three
chemistries and eight cells the useful breakdown is per-cell, which already exists.

## 11. Monitors

The existing `fuel-cell` Monitor subject family
(`StationStoreConnectionMonitors.cpp:165, 501`) is unchanged and covers all three
chemistries without new entries, because every monitored metric is derived from the
authored efficiency rather than from the chemistry.

One addition: the Monitor subject descriptor reports the chemistry string so an
inspector can distinguish subjects of the same family. No new default Alert Rules
are introduced in this spec.

## 12. Authoring UI

`renderFuelCellEntry` (`StationSystemsEntryEditor.cpp:1469`) gains one control,
placed immediately after `renderIdentity` and before the store-reference combos:

```cpp
renderEnumCombo(describe("fuel cell chemistry", label), cell.chemistry,
    core::StationFuelCellChemistryNames);
```

Behaviour rules:

- **Changing chemistry rewrites no other field.** It does not push the profile's
  default efficiency or sink temperature into the entry. Silently overwriting
  authored values on a catalogue change is the surprising behaviour; the profile
  is a constraint and a hint, not a template.
- The profile's admitted efficiency band and nominal operating temperature render
  as read-only hint text under the chemistry combo.
- An authored value outside the newly selected band shows as a validation error
  through the existing draft-validation surface. The user fixes it or picks a
  different chemistry. The value is never clamped.
- The `Fault: catalyst-poisoning` opt-in renders disabled with a stated reason
  when the selected chemistry does not support it, rather than being hidden.
- New cells created from the palette (`StationSystemsEntryEditor.cpp:2378-2400`)
  arrive as `HydrogenPem`, preserving today's behaviour exactly.

## 13. Invariants

These hold before and after, and are the properties the verification plan protects:

1. **Single-pool audit unchanged.** Total supplied never exceeds generation plus
   battery discharge. Fuel-cell output enters generation exactly once
   (`StationTickAuditPower.cpp:436`), for every chemistry.
2. **Mass ledger closes.** Product water mass equals hydrogen plus oxygen actually
   consumed, under the declared molar masses, for every chemistry.
3. **Energy ledger closes.** Fuel chemical energy = electrical output + waste heat,
   within `StationBalanceTolerance`.
4. **No chemistry can exceed its own conversion.** Enforced at authoring, so no
   runtime check is needed and no runtime path can produce an over-band cell.
5. **Chemistry is inert at runtime.** Two cells identical in every field except
   chemistry, with the same efficiency and sink temperature, produce identical
   tick output. The chemistry changes only what validation admits and which faults
   exist.
6. **Determinism.** All of the above is a pure function of settings, prior state
   and the tick's published values. Replay is identical.

Invariant 5 is the sharpest test of the design: if it fails, the profile has
leaked into runtime arithmetic and the design has drifted out of scope.

## 14. Explicitly out of scope

- **Any chemistry with a product other than water.** Direct methanol (CO₂ + water),
  zinc–air (solid ZnO), molten carbonate (CO₂ fed to cathode) all require a
  different stream set. Excluded by §2.
- **A generic role-based stream model** for the Fuel cell. Would require
  data-driven `PortDescriptor`s, table-driven validation, and a generic editor, and
  would be inconsistent with Sabatier / Electrolyzer / Methane processor unless
  applied to all four at once. A separate, repo-wide decision.
- **Startup, warm-up and cold-start contracts** for the fuel cell. The component has
  none today; SOFC's hours-long start is not expressible and is not added here.
- **SOFC-specific fault modes** (thermal shock, coking). See §3.
- **Per-chemistry cell count limits.** `MaxStationFuelCells` stays global.
- **Power Bus interaction.** Independent; `station_power_buses_specification.md`
  §4.1 makes source membership role-based, so this spec needs nothing from it and
  vice versa.
- **Changing `Conversion` into a power producer.** `Conversion` explicitly does
  not model energetics ("There is no
  automatic electricity input or thermal output", `docs/station-components.md`
  § Conversion). It is not an alternative to this work.

## 15. Verification plan

Release configuration only, per `AGENTS.md`. Focused CTest labels, run `-j 16`.

New checks, named for the behaviour protected:

| Check | Asserts |
|---|---|
| `fuelCellChemistryBands` | each profile admits its band and refuses outside it; PEM admits the full legacy `(0,1]`; refusal is atomic with a chemistry-named diagnostic |
| `fuelCellChemistryPoisonGate` | `catalyst-poisoning` admissible on PEM and alkaline, refused on solid-oxide with the stated reason |
| `fuelCellChemistryTopology` | changing chemistry is a `topologyChanged`; cannot be changed while a Fault stands against the cell |
| `fuelCellChemistryRoundTrip` | save/load/save byte-stable with the key present; absent key loads as PEM; unknown key refused |
| `fuelCellChemistryLegacyLoad` | a pre-123 document with no `chemistry` key loads as PEM, validates under the loose band, and simulates identically to the pre-change baseline |
| `fuelCellChemistryRuntimeInert` | invariant 5: two cells differing only in chemistry, same efficiency and sink, produce identical tick output, draws, heat and limiter |
| `fuelCellChemistryLedgers` | invariants 2 and 3 hold for all three chemistries under a full run |

Existing suites that must pass unchanged (they exercise the untouched shape):
`fuelCells`, `powerSourceMonitors`, `powerSourceReplay`, `powerSourceFailedStart`,
`stationWearCatalogue`, `stationPortChecks`, `documents`, `stationForecast`,
`stationPhysicalFidelity`, `headless-smoke`.

The reference station (`fuelCellBackup`, PEM by absence) must produce identical
simulation results before and after the change — the primary regression guard
against accidental runtime drift.

## 16. Decisions taken

### 16.1 PEM keeps the unconstrained `(0, 1]` band — DECIDED

Confirmed as spec'd in §3. PEM is not given a physically-honest band in v1.

**Rationale.** The migration risk of tightening an efficiency band is not spread
evenly across chemistries — it is concentrated entirely in the *load-default*
chemistry, because that is the only one pre-existing documents can arrive as.
Alkaline and solid oxide can only exist on schema ≥ 123 and therefore have no
legacy documents by construction, so tightening them is free. Tightening PEM is
not.

The specific hazard is a **re-save trap**, not a one-time load failure:

1. a legacy document (no `chemistry` key) loads as PEM;
2. the user edits something unrelated and re-saves at schema 123;
3. the `chemistry: hydrogen-pem` key is now written;
4. the next load binds the tightened PEM band to a value the user can no longer
   change, and the document is refused.

The document becomes unloadable *after* the save that made it visible. A rule that
only shows its teeth after the user has committed to it is worse than no rule.

**Measured, for the record.** Every authored fuel-cell efficiency in this repo is
**0.6** — the bundled reference station, the long-run workload, all four scenarios,
and every test. Tightening PEM would therefore break nothing in-repo today. (The
`0.35` values that appear alongside fuel cells in the bundled YAML belong to
generators; the single `efficiency = 0` in the tests is a *connection* efficiency,
used to zero a hydrogen cable and prove a blocked cell consumes no feed — the
cell's own field stays 0.6.) The concession is taken for documents outside this
repo, not because anything here is at risk.

**Rejected alternatives.**

- *Tighten PEM now, refuse on load.* Honest table, zero in-repo cost, but bricks
  any out-of-band external document.
- *Lenient on load, strict on author.* Collapses into the re-save trap above.
  Escaping it requires either clamping — silently mutating authored data — or
  carrying a "legacy unconstrained" flag, which retains a retired identity as a
  competing internal model against project convention.
- *Warn, don't refuse.* The nicest end state, but it needs a non-fatal warning
  channel in the station validator that does not exist yet; the power-buses spec
  designs one and is itself unimplemented.

**Closure path.** Retire the concession via the warn-don't-refuse route once a
station-validator warning channel exists. That is a later, separate change; nothing
in this spec needs to anticipate it beyond leaving the band in the profile table
where a tightening can reach it.

### 16.2 The profile's `defaultMaxSinkTemperatureC` is a UI default, not a validation rule — DECIDED

The profile carries a per-chemistry default maximum sink temperature (PEM 80 °C,
alkaline 95 °C, solid oxide 200 °C) which the authoring UI prefills and the
admitted band is shown beside as read-only hint text. It is **not** enforced by the
validator.

**Rationale.** `maxSinkTemperatureC` is an author choice about a specific
installation, not a property of the chemistry. A designer may legitimately run a
PEM stack with an unusually cold sink, or accept a warmer one because their
reservoir and radiator plant can hold it. Promoting the profile default to a hard
rule would refuse those legitimate choices for a number that is, at bottom, a
starting point.

The efficiency band *is* enforced because efficiency is genuinely bounded by the
chemistry — a PEM stack cannot convert more than its chemistry allows. Sink
tolerance is bounded by the *installation*, which the author knows better than the
catalogue does. The asymmetry between the two fields is deliberate and follows
from which one the chemistry actually determines.

The existing outer validation on `maxSinkTemperatureC` — finite, above absolute
zero — stays. Nothing else about the field's validation changes.

### 16.3 Wire names keep the `hydrogen-` prefix — DECIDED

Wire values stay `hydrogen-pem`, `hydrogen-alkaline`, `hydrogen-solid-oxide`.

**Rationale.** The prefix states the topology constraint on the wire itself. Every
chemistry in this design burns hydrogen and produces water; that is the constraint
that defines the whole feature (§2). A reader of a saved document should be able
to see it without consulting the catalogue.

It also future-proofs the naming. If a non-hydrogen chemistry is ever added — which
requires the stream-topology work explicitly excluded in §14 — `methanol-dmfc` is
obviously a different thing rather than a rename or a widening of what
`pem` meant. Bare names like `pem` would have to silently change scope to
accommodate it, which is how wire formats rot.

The redundancy is intentional and cheap; the ambiguity it prevents is not.

### 16.4 No fourth chemistry is pre-seated in v1 — DECIDED

The catalogue ships exactly three chemistries. No placeholder rows, no reserved
enum values.

**Rationale.** The profile table is a `constexpr` array indexed by the enum, so
adding a chemistry that keeps the same stream topology is one enum value, one names
array entry, one profile row, and a test case. There is no forward-compatibility
benefit to reserving space for something that costs nothing to add later.

Pre-seating would also force us to invent a chemistry we have no design for, and
documented-but-unspecified catalogue rows are worse than absent ones — they read as
available and invite authoring against something that isn't there.

## 17. Open questions

None. All questions raised during the design review are resolved in §16. The spec is
complete and ready for implementation.
