# Generator Prime Mover Specification

Status: approved. Published as GitHub issue **#894** (`ready-for-agent`).
Tracer bullet **#896** implemented: selector, optional-read/unconditional-write
`primeMover`, conditional schema 125, snapshot and editor telemetry. Existing
contracts and runtime arithmetic remain unchanged. **#897** enforces the profile
bands and ramp ceilings; **#898** gates injector wear and freezes prime mover on
both whole-settings and targeted live updates, including active Faults and banked
wear. Stirling's injector opt-in remains visible, disabled with its reason.
Release regression: `station-generatorPrimeMoverFaultGate` covers authoring and
live World/runtime seams, accumulator-orphan refusal, and identical repeated
failed starts across all archetypes; `station-editor-flow-power-chain` covers
the disabled opt-in and explanation.
Release coverage includes `station-generatorPrimeMover`, `station-referenceBundled`
and `station-editor-flow-power-chain` (selection, undo/redo, no rewritten dials).

Tickets: **#896** (tracer bullet) → **#897** (bands and ramp ceiling)
and **#898** (injector gate and frozen topology), independent of each other.
All three tickets implemented.

Related gap: **#895** — "Known modelling gap: fuelled Generators consume no oxygen
and produce no CO2". Filed separately so the largest honest gap in this component
stays visible rather than being buried in the Out of Scope list.

Canonical vocabulary: `CONTEXT.md` § Station systems. This spec adds **Prime Mover**
and **Prime Mover Profile**. It changes no other noun, adds no port, and supersedes
no ADR.

Parent surface: the Generator component (issue #399; `docs/station-components.md`
§ Generator). Sibling pattern: the Fuel Cell Chemistry spec (#890), whose
code-owned-profile-over-fixed-topology shape this spec reuses directly.

---

## 1. The framing correction

The request was for **fuel profiles**. That framing does not survive contact with
the model, and the reason is worth stating up front because it determines the whole
design.

**The Generator's fuel is energy-denominated, not mass-denominated.** The fuel
inventory must be a Resource store in `kJ`, `MJ`, `Wh` or `kWh` — the validator
refuses anything else, because the fuel ledger has to close in the same energy terms
as the electrical output.

Consequences:

- **Fuel specific energy is not a modelling parameter.** There is no kilogram of
  fuel in the model to divide by. The store already holds energy directly.
- **Fuel storage mass is not modelled.** A tonne of diesel and a tonne of jet-A are
  the same object to this system.
- **Fuel hazard class is not modelled.** The material-id convention is explicit
  that ids "describe substances, never store names, units, hazards or composition."
  The fuel store carries no hazard identity.

So the *fuel* carries almost no modelled identity at all. The model has already
normalised every fuel to "a pile of energy."

**What actually differs between a diesel genset and a gas turbine is the machine.**
Ramp behaviour, cranking contract, after-run heat, sink tolerance, which wear modes
exist. Those are properties of the **prime mover**, not of what it burns.

This spec therefore specifies a **Generator Prime Mover Profile**: an enum on
`GeneratorSettings` selecting a code-owned profile that varies the machine
contract. It is the Fuel Cell Chemistry pattern (#890) applied one component
across, with the same fixed-topology discipline.

### 1.1 If you actually want fuel identity

Modelling diesel-vs-hydrazine as *substances* — specific energy, storage mass,
hazard class — is a different and larger change: a **mass-denominated fuel store
with a declared specific-energy conversion** at the burn site. That is precisely
the shape the Fuel cell already uses (mass reactant stores plus a declared LHV
constant).

It is explicitly out of scope here (§14). It is also, on reflection, the weaker
option: it buys realism the current model has deliberately traded away, and it
would make the Generator's fuel ledger a two-unit conversion instead of the clean
single-unit closure it has today.

## 2. Decision

Add a **Prime Mover** enum to `GeneratorSettings`. The enum selects a
**code-owned Prime Mover Profile**: admitted efficiency band, admitted maximum ramp
band, the cranking and after-run heat character, the default maximum sink
temperature, and which fault and wear opt-ins the machine has.

**The profile varies machine characteristics only. It does not vary stream
topology.** Every prime mover keeps exactly the current Generator shape: one
energy-unit fuel store in, one heat-sink reservoir out, automatic power in for
cranking and power out for generation, unchanged port set, unchanged phase
machine (`Stopped → Starting → Running → Shutdown`).

## 3. The prime mover catalogue

Three archetypes ship in v1. All burn energy from the same kind of store; they
differ in how they convert it.

| | `reciprocating` | `gas-turbine` | `stirling` |
|---|---|---|---|
| Real device | Piston engine genset | Brayton-cycle microturbine | External-combustion dynamic converter |
| **Admitted efficiency band** | `(0, 1]` *(legacy — see §6.2)* | `[0.15, 0.38]` | `[0.20, 0.42]` |
| **Default efficiency** | 0.35 | 0.28 | 0.32 |
| **Admitted max-ramp band** | unconstrained *(legacy — §6.2)* | `[0, 200]` kW/s | `[0, 30]` kW/s |
| **Default max ramp kW/s** | 2.0 | 50.0 | 8.0 |
| **Cranking character** | high power, short | moderate power, long (spin-up + synchronisation) | low power, long (warm-up) |
| **Default startup** | 10 s | 90 s | 240 s |
| **After-run decay heat** | moderate, short | significant, long cooldown | low |
| **Default shutdown heat kW / s** | 1.0 / 60 | 4.0 / 300 | 0.5 / 90 |
| **Default max sink °C** | 90 | 180 | 130 |
| `failed-start` opt-in | admissible | admissible | admissible |
| `fouled-injector` wear opt-in | admissible | admissible | **refused** |

Notes on the numbers:

- **`reciprocating` is the legacy default and keeps both bands unconstrained.**
  This is the same concession, taken for the same reason, as decision 16.1 in the
  Fuel Cell Chemistry spec. See §6.2 for the measurement that forces it.
- **Efficiency bands are planning figures for the device class**, not
  flight-qualified ratings — the standing convention from
  `sector_power_system_design_specification.md` §1.
- **The Stirling refuses `fouled-injector` because it has no injector.** Combustion
  is external and the working fluid is sealed. This is the direct analogue of the
  solid-oxide fuel cell refusing `catalyst-poisoning`: a wear mode the machine
  physically does not have is refused at authoring rather than left available as
  decoration.
- **The turbine's wider ramp band is the design point.** A microturbine slews fast
  and is the right machine for load-following; a reciprocating genset cannot follow
  a fast load and the profile says so. This is the most simulation-meaningful
  difference in the catalogue.
- **The Stirling's warm-up is an approximation.** The model has exactly one
  startup shape — a power draw sustained for a duration before Running — so the
  Stirling's warm-up, which is *heat* rather than *crank*, is expressed as a long
  low-power draw. The shape cannot express the difference. The long duration
  captures the timing honestly; the thermal character is lost. Recorded rather
  than glossed.

## 4. Data model

Following the `StationDispatchMode` / `StationFuelCellChemistry` idiom:

```
enum StationGeneratorPrimeMover { Reciprocating, GasTurbine, Stirling }
wire names: "reciprocating" | "gas-turbine" | "stirling"

record PrimeMoverProfile {
    label
    minEfficiency, maxEfficiency          // inclusive; reciprocating = (0, 1]
    maxRampKWPerSecond                    // ceiling on the authored ramp;
                                         // reciprocating = unbounded
    defaultEfficiency
    defaultMaxRampKWPerSecond
    defaultStartupSeconds
    defaultShutdownHeatKW
    defaultShutdownSeconds
    defaultMaxSinkTemperatureC
    supportsFouledInjector
}
```

`GeneratorSettings` gains exactly one field, placed after `id`:

```
struct GeneratorSettings
{
    std::string id;
    StationGeneratorPrimeMover primeMover{StationGeneratorPrimeMover::Reciprocating};
    // ... all existing fields unchanged, in existing order ...
}
```

No existing field is renamed, retyped, reordered or removed. `operator==` remains
`= default`.

**Every profile default is a prefilled starting value, not an enforced rule** —
except the efficiency band and the ramp ceiling, which are enforced (§6). The
defaults for startup, decay heat and sink temperature are UI prefills only, for
the reason established in fuel-cell decision 16.2: those describe the
*installation*, which the author knows better than the catalogue.

## 5. Ports

**No change.** The Generator keeps its two existing descriptors verbatim —
`Fuel in` (material, inventory) and `Heat out` (heat, heat-sink) — plus its
automatic power-in for cranking and power-out for generation, and its
Supervision input. The stream set does not vary, so the port table, the placeable
type list, the Flow projection and link evaluation are untouched.

## 6. Validation

All changes land in the existing generator validation loop. Every refusal is
atomic — no partial draft state, contextual message — matching the component's
existing behaviour.

### 6.1 New refusals

| Condition | Message shape |
|---|---|
| Unknown prime mover value on load | `Station generator 'x' has unknown prime mover 'y'; declared prime movers are: reciprocating, gas-turbine, stirling` |
| `efficiency` outside the profile band | `Station generator 'x' efficiency 0.52 is outside the admitted band [0.15, 0.38] for prime mover 'gas-turbine': a machine cannot be authored above what its cycle converts` |
| `maxRampKWPerSecond` above the profile ceiling | `Station generator 'x' maximum ramp 500 kW/s exceeds the 200 kW/s ceiling for prime mover 'gas-turbine': the machine cannot slew that fast` |
| `fouledInjector` present on a prime mover that has no injector | `Station generator 'x' opts into fouled-injector but prime mover 'stirling' has no fuel injector: the wear mode does not exist for this machine` |

### 6.2 The legacy-unconstrained default — carried from fuel-cell decision 16.1

`reciprocating` keeps the unconstrained `(0, 1]` efficiency band and an
unbounded ramp ceiling. This is not an oversight; it is the same re-save-trap
argument, and the measurements here show it is load-bearing rather than
theoretical.

**Measured across the repo:**

- **Bundled YAML** — every generator in the reference station, the long-run
  workload and all four scenarios is uniformly `efficiency 0.35`,
  `maxRampKWPerSecond 2.0`, `startupSeconds 10.0`, `shutdownHeatKW 1.0`. That
  is exactly the `reciprocating` archetype, so the default matches bundled
  content precisely.
- **Tests** — generator efficiencies of **0.35, 0.4, 0.5 and 1.0** appear, and
  `maxRampKWPerSecond` spans **0.0 through 100000**. The extreme ramp values are
  deliberate: tests set an effectively-instant ramp so ramp dynamics do not
  interfere with whatever else is under test.

A constrained `reciprocating` band would therefore break existing tests at 0.4,
0.5 and 1.0, and would break every test that relies on a 100000 kW/s ramp. The
default prime mover must stay unconstrained on both axes. The non-default movers —
which can only arrive on the new schema and so have no legacy documents by
construction — are free to be constrained.

**Generalised lesson, now confirmed twice:** when adding a tightened validation band
over an existing field, the risk concentrates entirely in whatever value legacy
documents default to. Tighten every non-default variant freely; the default variant
needs a migration story before it can be tightened.

### 6.3 Unchanged

Everything else validates as today: id rules, id uniqueness, thermal service class
membership, the energy-unit requirement on the fuel store, heat-sink existence and
thermal-class match, non-negative rated output, commanded output within rating,
and the sink temperature above absolute zero.

`MaxStationGenerators` (8) is unchanged and is **not** per-prime-mover.

## 7. Persistence and migration

### 7.1 Wire form

Named-field, additive, mirroring the `dispatchMode` and fuel-cell `chemistry`
pattern. Written unconditionally; read optionally with an explicit unknown-value
refusal. Absent field → `Reciprocating`, which is exactly today's behaviour.

### 7.2 Schema

`kWorldSchemaVersion` bumps to the next free version. **Version numbers are a
contended resource in this repo** — the fuel cell chemistry work has already
claimed 123 and the Power Buses spec (#878) has been asked to move to 124. This
spec takes the next number that is free when implementation starts, and the
conditional version chain gains a term so a document only claims it when a
non-`reciprocating` generator is actually present. An all-reciprocating station
writes the same version it writes today.

**Backward compatibility:** a pre-bump document has no `primeMover` field, loads
as `Reciprocating`, and validates under the unconstrained legacy bands. No
migration pass; no authored value rewritten.

### 7.3 Round-trip

Save → load → save is byte-stable for the field. The bundled reference station is
left **without** a `primeMover` key so the absent-field path stays under
continuous test coverage. One new fixture carries the key explicitly.

## 8. Runtime behaviour

**Zero new runtime arithmetic.** The profile constrains what may be authored; it
introduces no new computation. The generator's existing phase machine, ramp
slewing, cranking-energy accumulation, shutdown-heat decay, sink qualification,
automatic-dispatch command override and fuel-ledger closure are untouched.

The profile is read at authoring time and never consulted during the tick. Two
generators identical in every field except prime mover, with the same efficiency
and ramp, produce identical tick behaviour — not because the profile is inert by
accident, but because the profile has no runtime path at all.

## 9. Faults, wear, and live-node topology

Prime mover joins the live-node `topologyChanged` predicate alongside the
`fouledInjector` opt-in, exactly as chemistry did for the fuel cell.

Consequences, all consistent with the existing frozen-topology contract:

- prime mover cannot be changed while a Fault is running against the generator;
- a prime-mover change is a structural edit, subject to the same paused-document
  seams and atomic-rejection rules;
- an injected `failed-start` Fault is never reachable by retuning prime mover out
  from under it;
- a `fouled-injector` wear accumulator cannot be orphaned by switching to a
  prime mover that has no injector — the switch is refused rather than silently
  stranding the accumulator.

No new fault or wear mode is introduced. `failed-start` behaves identically across
all three prime movers.

## 10. Snapshot and telemetry

`StationGeneratorSnapshot` gains one field: the prime mover wire name, published
from the authored setting. All existing fields — phase, output, fuel drawn, heat
routed, startup progress, cooling qualification, closing fuel ledger — are
unchanged and already prime-mover-agnostic because they derive from the authored
contract values.

Station-wide generator totals in `StationPowerSnapshot` remain prime-mover-blind.
No per-archetype rollup: with three archetypes and eight generators the useful
breakdown is per-generator, which already exists.

## 11. Monitors

The existing generator Monitor subject family covers all three prime movers with
no new entries, because every monitored metric derives from authored contract
values rather than from the archetype. The subject descriptor reports the prime
mover string so an inspector can distinguish same-family subjects.

One consideration worth recording: the turbine's long start and the Stirling's
longer warm-up mean a **start-duration Monitor threshold tuned for reciprocating
machines will false-alarm on the others**. No new rules are added in this spec, but
the default start-time thresholds should be reviewed when the archetype-specific
Monitor tables are next touched.

## 12. Authoring UI

The generator entry gains one combobox immediately after the identity field.

Behaviour rules, identical to the fuel cell's:

- **Changing prime mover rewrites no other field.** It does not push profile
  defaults into the entry. The profile is a constraint and a hint, never a
  template.
- The admitted efficiency band, the ramp ceiling, and the characteristic start
  time render as read-only hint text under the combobox.
- An authored value outside the newly selected band shows as a validation error
  through the existing draft-validation surface. Never clamped.
- The `fouled-injector` wear opt-in renders **disabled with a stated reason** on
  Stirling, rather than being hidden.
- New generators created from the palette arrive as `Reciprocating`, preserving
  today's behaviour exactly.

## 13. Invariants

1. **Single-pool audit unchanged.** Total supplied never exceeds generation plus
   battery discharge. Generator output enters generation exactly once for every
   prime mover; cranking draw stays inside the same pool.
2. **Fuel ledger closes.** Fuel energy = electrical output + waste heat, within
   tolerance, for every prime mover.
3. **Phase machine unchanged.** `Stopped → Starting → Running → Shutdown` with the
   same transitions and the same shutdown-heat obligation that persists after
   electrical output stops.
4. **No prime mover can exceed its own cycle.** Enforced at authoring, so no
   runtime check is needed.
5. **Runtime inertness.** Two generators identical in every field except prime
   mover, with the same efficiency and ramp, produce identical tick output, fuel
   draw, heat routing, phase transitions and limiter.
6. **Determinism.** All of the above is a pure function of settings, prior state
   and the tick's published values. Replay is identical.

Invariant 5 is the sharpest test of the design: if it fails, the profile has
leaked into runtime arithmetic and the design has drifted out of scope.

## 14. Explicitly out of scope

- **Fuel identity: specific energy, storage mass, hazard class.** The fuel store is
  energy-denominated by design and carries no substance identity. Modelling
  diesel-vs-hydrazine as substances requires a mass-denominated fuel store with a
  declared specific-energy conversion at the burn site — the Fuel cell's shape,
  not the Generator's. A much larger change that trades away the clean
  single-unit fuel ledger this component has today.
- **Fuel contamination and feed-borne poisoning.** The Generator's wear is
  duty-driven (`fouled-injector`), not feed-driven like the fuel cell's
  `catalyst-poisoning`. A fuel-quality axis is a separate design.
- **Multi-fuel or fuel-switching generators.** One fuel store per generator, as
  today.
- **Combustion oxygen demand.** A real combustion generator consumes station
  oxygen and produces CO₂. The current Generator has no atmosphere interface at
  all, and adding one is a substantial ECLSS coupling well outside this spec.
  Recorded here because it is the most significant thing this model does not say
  about burning fuel. **Tracked as its own issue: #895.**
- **A generic role-based stream model.** Same reasoning as the fuel cell spec §14:
  would be inconsistent with the Sabatier reactor, Electrolyzer, Methane
  processor and Fuel cell unless applied to all four at once.
- **Tightening the `reciprocating` bands.** Deferred with its own migration, per
  §6.2.
- **Startup-shape variety.** The model has one startup contract (a power draw for
  a duration). Warm-up-vs-crank is not expressible; see §3.
- **Power Bus interaction.** Independent — the bus spec makes source membership
  role-based, so neither spec needs anything from the other.

## 15. Verification plan

Release configuration only, per `AGENTS.md`. Focused CTest labels, `-j 16`.

**Seam: the existing configured-World seam, no new seams.** Build settings →
`World::configureStationSystems(settings, confirmReset, &diagnostic)` → advance
via the station rig helpers → assert on `World::getStationSystemsSnapshot()`.
Plus the live-update seam `World::updateStationSystemsSettings` for the
frozen-topology contrast, and the rig YAML round-trip for persistence. Same seam
set as the fuel cell tickets #891–#893.

New checks, named for the behaviour protected:

| Check | Asserts |
|---|---|
| `generatorPrimeMoverBands` | each profile admits its efficiency band and refuses outside it; reciprocating admits the full legacy `(0,1]`; refusal atomic with a prime-mover-named diagnostic |
| `generatorPrimeMoverRampCeiling` | turbine and Stirling ramp ceilings enforced; reciprocating unbounded; the 100000 kW/s test idiom still works on reciprocating |
| `generatorPrimeMoverInjectorGate` | `fouled-injector` admissible on reciprocating and turbine, refused on Stirling with the stated reason |
| `generatorPrimeMoverTopology` | changing prime mover is a `topologyChanged`; refused while a Fault stands; a `fouled-injector` accumulator cannot be orphaned by switching to Stirling |
| `generatorPrimeMoverRoundTrip` | save/load/save byte-stable with the key present; absent key loads as reciprocating; unknown key refused |
| `generatorPrimeMoverLegacyLoad` | a pre-bump document with no key loads as reciprocating, validates under the loose bands, and simulates identically to the pre-change baseline |
| `generatorPrimeMoverRuntimeInert` | invariant 5: two generators differing only in prime mover, same efficiency and ramp, produce identical tick output, draw, heat, phase and limiter |
| `generatorPrimeMoverLedgers` | invariants 1 and 2 hold for all three archetypes under a full run |

Existing suites that must pass unchanged: `generators`, `generatorFunding`,
`generatorTiming`, `powerSourceMonitors`, `powerSourceReplay`,
`stationWearCatalogue`, `stationPortChecks`, `documents`, `stationForecast`,
`headless-smoke`.

**The bundled reference station's generators are uniformly reciprocating-shaped
and must simulate identically before and after the change** — the primary
regression guard against accidental runtime drift.

## 16. Decisions taken

None yet — this section records decisions as they are settled in review.

## 17. Open questions

1. **Is three archetypes the right catalogue, or should a fourth be added?**
   A free-piston linear alternator is the obvious candidate but is thin on
   defensible numbers. Nothing in v1 requires a fourth.
2. **Should the ramp ceiling be a hard rule at all?** Spec'd as enforced, because
   ramp capability genuinely *is* bounded by the machine type — unlike sink
   temperature, which is an installation property. Worth confirming that the
   turbine's 200 kW/s and Stirling's 30 kW/s ceilings do not block a legitimate
   authoring case.
3. **Should the profile's default startup and decay-heat values be surfaced in the
   UI as prefills only, or should changing prime mover offer to apply them?**
   Spec'd as prefills never applied automatically, per fuel-cell decision 16.2.
   An explicit "apply archetype defaults" button is a reasonable alternative that
   does not silently mutate authored data.
4. **Combustion oxygen demand is the biggest honest gap in this component** (§14).
   **Resolved — filed as #895**, "Known modelling gap: fuelled Generators consume
   no oxygen and produce no CO2", labelled `enhancement` / `priority:low`, left
   unfunded but visible.
