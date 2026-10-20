# Generalized Bioconversion Stage — Microbial Processor Generalization

Status: core slice (#735), degenerate pass-through (#736), hot-digestion
scenario/hardening (#737), and editor derived-rate preview (#738) implemented.
Origin: audit finding G1 in ticket #724 (Station systems gap analysis).
Component: `MicrobialProcessorSettings` (`include/core/StationSystems.h`), validator
and runtime in `src/core/StationSystems.cpp`.

> **Migration note.** The `hazardClass` field and the class-matching rules this
> spec carried were removed by #748 (see
> [ADR 0024](../adr/0024-containment-is-modelled-not-declared.md)). The examples
> below are shown without it; a document still carrying the field is refused with a
> conversion-required diagnostic.

## 1. Purpose

The microbial processor (#407) is architecturally feed-agnostic — its validator
accepts any mass-denominated store that declares a composition, and its runtime
recovery math reads the feed's actual declared constituents. But every input
coefficient (`oxygenPerKgFeedKg`, `processWaterPerKgFeedKg`, `co2PerKgFeedKg`,
`consumablePerKgFeed`, `reactionHeatKJPerKgFeed`) is a fixed rate **per kg of
feed mass**, tuned for energy-dense organic waste. Applied to a dilute aqueous
feed such as urine, those coefficients are physically wrong by an order of
magnitude: processing the reference station's 4 kg/day of urine at organic-waste
rates would draw ~3.2 kg O2/day — roughly 94% of the crew's entire daily oxygen
production — to recover 48 g of nitrogen. Real nitrification of that same load
requires ~0.25 kg O2/day.

This specification generalizes the component so that process inputs are derived
from the **feed's declared composition** rather than the feed's mass, turning the
microbial processor into a general aerobic bioconversion stage: one component
shape that parameterizes many real processes (nitrification, composting, hot
digestion) selected by what it is fed. This is how real bioconversion engineering
works — oxygen dose from oxidizable content, heat from the degradable fraction,
dilution water from feed dryness — and it closes G1 (urine as a dead end) without
new machinery.

## 2. G2 prerequisite analysis: NOT required

Ticket #724 lists G2 ("water streams carry no species") as the keystone change
for urine processing. For this generalization, G2 is **not a prerequisite**:

- **Feed side.** The processor requires a compound feedstock (declared
  composition). The `urine` store already declares one (moisture 0.95; N 30%,
  P 10%, K 35%, minerals 15% of dry mass). No water-stream rule blocks it.
- **Product side.** Recovered nutrients and recovered water land as plain mass,
  exactly as today. The crop uptake world consumes plain nutrient stores already.
- **What G2 would add later** — a recovered-water stream that carries a residual
  dissolved-solids fraction, "too contaminated to treat" semantics, nutrient-rich
  eluate as a distinct product — is out of scope here and changes nothing in this
  specification. The plain-mass product rule (`declareWaterStore` refusal:
  "recovered water streams are plain mass stores") is preserved unchanged.

If G2 lands later, this component's product wiring may adopt it; nothing here
conflicts with that.

## 3. Design: rate basis

Add a `rateBasis` selector to the processor settings:

```
enum class StationRateBasis { FixedPerFeedMass, CompositionDerived };
```

- **`FixedPerFeedMass` (default)** — the existing behaviour, byte-for-byte. All
  current per-kg-feed fields are honoured exactly as today. Every existing
  configuration, save file, replay and smoke check continues to validate and run
  unchanged.
- **`CompositionDerived`** — the per-kg-feed fields are ignored (and may be
  omitted); process inputs are computed per tick from the feed's actual drawn
  composition using the model in §4 and the authored parameters in §3.1.

The component keeps its name, id space, placement, connectors,
thermal-class rules, store-distinctness rules, condition/recovery coefficients
(#408) and uniform-scale throttling. Only the input-rate model changes.

### 3.1 New parameters (CompositionDerived mode)

| Field | Default | Meaning |
|---|---|---|
| `organicOxidationFraction` | 0.9 | fraction of the feed's dryOrganics oxidized per pass |
| `o2PerKgOrganicOxidized` | 1.4 | kg O2 per kg organic oxidized (heterotrophic respiration proxy) |
| `nitrificationFraction` | 0.0 | fraction of recovered N that is nitrified (0 = pure heterotrophic, 1 = full nitrification) |
| `o2PerKgNitrifiedN` | 4.57 | kg O2 per kg N nitrified (stoichiometric NH4+ → NO3−) |
| `co2PerKgOrganicOxidized` | 1.8 | kg CO2 produced per kg organic oxidized |
| `processWaterTargetMoisture` | 0.0 | draw process water to bring the feed mix to this moisture; 0 disables the draw |
| `heatReleaseFraction` | 0.7 | fraction of oxidized chemical energy released as reaction heat to the sink |
| `consumablePerKgOrganicOxidized` | 0.05 | culture medium per kg organic oxidized |

Recovery fractions (`nitrogenRecovery`, `phosphorusRecovery`,
`potassiumRecovery`, `mineralsRecovery`, `waterRecovery`), the operating
temperature window, condition coefficients and `rateKgPerS` (rated feed
throughput) are unchanged and apply in both modes.

## 4. Derived rate model

Per tick, for the feed actually drawn (`feedKg`, actual drawn blend composition
per #601 — never the authored declaration of a drifting store):

```
dryKg        = feedKg × (1 − moisture)
organicKg    = dryKg × dryOrganicsFraction
               where dryOrganicsFraction = 1 − (N + atmN + P + K + minerals)
organicsOx   = organicKg × organicOxidationFraction
nitrifiedN   = feedN_kg × nitrogenRecovery × nitrificationFraction

O2 demand    = organicsOx × o2PerKgOrganicOxidized
             + nitrifiedN × o2PerKgNitrifiedN

CO2 produced = organicsOx × co2PerKgOrganicOxidized

processWater = max(0, feedKg × (processWaterTargetMoisture − moisture)
                        / (1 − processWaterTargetMoisture))

reactionHeat = organicsOx × energyKJPerKgDry × heatReleaseFraction

consumable   = organicsOx × consumablePerKgOrganicOxidized

nutrient recovery (unchanged):
  N_rec  = feedN_kg  × nitrogenRecovery   (etc. for P, K, minerals)
  water_rec = feedKg × moisture × waterRecovery

residue = feedKg + O2 + processWater + consumable
        − nutrients_rec − water_rec − CO2          (must be ≥ 0)
```

All draws are subject to the existing uniform-scale throttling: the stage runs at
one scale per tick, capped by the scarcest required input (feed, O2, process
water, consumable, power) and by destination capacity. A stage that cannot pay
for a tick does not run that tick.

The existing reaction-heat cap is preserved by construction: with
`heatReleaseFraction ≤ 1`, released heat can never exceed the chemical energy of
the organics actually oxidized, which can never exceed the feed's declared energy.

### 4.1 Nitrification placement

`nitrificationFraction` acts on the **recovered** nitrogen (the N booked into
the nutrient store), not on the feed's total N. The unrecovered N share stays in
residue and is not nitrified. Nitrification consumes O2 but produces no CO2 and
no heat in this model (chemolithotrophic growth yield is ignored — see §6).

## 5. Degenerate cases (explicitly allowed)

Degenerate feeds must validate and run. Their semantics are defined, not
rejected:

| Feed property | Derived behaviour |
|---|---|
| Zero dryOrganics (mineral/N-rich aqueous feed) | O2 = nitrification term only; CO2 = 0; heat = 0; consumable draw = 0; water passes at `waterRecovery`; residue = inert dry mass |
| Zero oxidizable energy with organics present | heat = 0 (cap binds at zero); O2/CO2 still scale with organics oxidized — energy declaration governs heat only |
| Zero N (or zero recovered N) | nitrification term = 0 regardless of `nitrificationFraction` |
| Fully dry feed (moisture 0) | water recovery = 0; process-water draw = `feedKg × target / (1 − target)` when target > 0 |
| Feed at or above target moisture | process-water draw = 0 |
| No oxidation/nutrient recovery, but recoverable water | water passes at `waterRecovery`; no O2/CO2/reaction heat/medium/nutrients; residue is inert dry mass **plus unrecovered water**; ordinary throughput/input/destination/power limits still apply |
| No oxidation/nutrient recovery and no recoverable water | every actual is zero; the stage consumes nothing and produces nothing. It must not divide by zero, stall the tick, or be a configuration error |

Water pass-through and the full no-op are distinct: a positive recovered-water
stream necessarily consumes feed and cannot also report zero feed throughput.
The latter row applies when `waterRecovery` is zero (or the feed has no water).
The existing compound-store moisture range remains [0, 1); near-pure water is
representable, while exactly 1 remains invalid. All-zero dry constituent fractions
mean **organic** dry mass, not inert material; use an explicitly inert balance
(such as atmospheric nitrogen, which this stage cannot recover).

A fully no-op stage is visible as such: the snapshot shows zero throughput with
the limiter reporting the feed carries nothing convertible. Authors are free to
wire one (useful for placeholder configs and editor demos); the model neither
punishes nor hides it.

## 6. Preserved idealizations (declared, per the audit)

This generalization deliberately keeps the existing simplifications. They are
features of the model's tick-level abstraction, not oversights:

1. **Instantaneous quasi-steady conversion.** Rates apply within the tick with
   no lag, no startup, no community acclimation to a feed change. The existing
   condition-coefficient set (#408: starvation, cold stress, heat stress,
   recovery) remains the only dynamic state.
2. **No inhibition dynamics.** Real nitrification crashes on free ammonia at
   high pH; real composting goes anaerobic when over-fed. Not modelled. The
   operating temperature window and condition coefficients are the only
   process-health gates.
3. **Constant recovery fractions**, independent of loading rate, composition
   interaction or inhibition.
4. **Single feed store per instance.** Co-digestion (blending urine + food waste
   + crop residue to a target C:N) is a real-practice follow-on requiring a
   multi-feed change; it is out of scope here.
5. **Products land plain.** Recovered nutrients and water arrive as plain mass;
   no characterized product streams (see §2, G2).
6. **Nitrification yield ignored.** The O2 cost of nitrification is booked; the
   small chemolithotrophic biomass yield is not.
7. **Composition is a store-level declared property.** Blends are spent
   proportionally (#601); there is no process-zone heterogeneity.

## 7. Validation changes

1. `rateBasis` must be a valid enum value; default `FixedPerFeedMass`.
2. In `CompositionDerived` mode: all §3.1 fields must be finite and within
   their documented ranges (fractions in [0, 1]; rates ≥ 0;
   `processWaterTargetMoisture` < 1).
3. The legacy config-time residue-balance check (fixed yields must not create
   mass) is replaced in derived mode by the same check evaluated over the
   derived formulas: the derived residue rate must be ≥ 0 for the declared
   composition. In `FixedPerFeedMass` mode the existing check is unchanged.
4. The reaction-heat-vs-feed-energy cap check is unchanged in fixed mode and
   automatic-by-construction in derived mode (still validated:
   `heatReleaseFraction ≤ 1`).
5. The compound-feedstock requirement is **unchanged**: a `CompositionDerived`
   processor still refuses a feed store that declares no composition ("a
   bioconversion stage recovers from a declared compound feedstock, not from an
   uncharacterised stream"). Degenerate-but-compound feeds (zero oxidizable
   content) pass; plain feeds still refuse.
6. Thermal-class, store-distinctness, mass-unit and
   store-existence rules are unchanged in both modes. (The hazard-class rule
   listed here originally was later removed by #748.)

## 8. Snapshot and Lua surface

- `rateBasis` and the §3.1 parameters are exposed read-only in the processor
  snapshot and Lua mirror alongside the existing fields.
- Derived per-tick rates (O2 demand, CO2, heat, process water) are exposed as
  the same existing snapshot fields (they already report actuals); no new
  actual fields are required beyond `nitrifiedNitrogenKg` per tick/lifetime.
- The limiter wording for a degenerate feed: `"feed '<id>' carries no
  convertible content: the stage has nothing to oxidize or recover"`.

## 9. Editor / authoring

- **Place node > Biology > Microbial processor** is unchanged. The settings
  form gains a **Rate basis** selector at the top of the rates group:
  - *Fixed per feed mass* shows the existing fields.
  - *Composition derived* hides the per-kg-feed fields and shows the §3.1
    parameters with their defaults.
- **Derived-rate preview:** when a feed store is wired and declares a
  composition, the form shows a one-line preview computed from the declared
  composition at rated throughput, e.g. `≈ 0.06 kg O2 per kg feed at the
  declared feed composition`. Preview only — the runtime always uses the
  actual drawn blend. The shared settings form (including the Flow inspector)
  displays O2, process water, CO2, medium and nitrified N in kg/s and reaction
  heat in kW at rated feed throughput. It recomputes each frame from the draft,
  is hidden for fixed mode or a missing/undeclared feed, and marks the estimate
  as "at declared feed composition (not runtime)". Non-finite or negative draft
  results show an unavailable-preview notice instead of numeric rates.
- All edits use draft undo/redo; only Apply changes the World (unchanged).
- Link refusals are unchanged plus the new derived-mode validation messages.

## 10. Example configurations

Small self-contained scenario configs under
`resources/station-systems/scenarios/`, loadable in the editor. Numbers are
per simulated day at rated throughput.

### 10.1 Urine nitrification stage (`bioconversion-urine.yaml`)

The G1 closure. Four crew produce 4 kg/day of urine (60 g N). The stage
nitrifies the recovered N and passes the water.

```yaml
microbialProcessors:
  - id: urineNitrifier
    rateBasis: CompositionDerived
    feedStoreId: urine            # N 30%, P 10%, K 35%, min 15%, moist 0.95
    oxygenStoreId: processOxygen
    waterStoreId: fresh
    consumableStoreId: digesterMedia
    nutrientStoreId: nutrients
    recoveredWaterStoreId: rawWater
    co2StoreId: co2
    residueStoreId: residue
    thermalClass: low
    heatSinkId: processSink
    rateKgPerS: 4.629630e-5     # 4 kg/day
    organicOxidationFraction: 0.9
    o2PerKgOrganicOxidized: 1.4
    nitrificationFraction: 1.0
    o2PerKgNitrifiedN: 4.57
    co2PerKgOrganicOxidized: 1.8
    processWaterTargetMoisture: 0.0   # feed is already wetter than any target
    heatReleaseFraction: 0.7
    consumablePerKgOrganicOxidized: 0.05
    nitrogenRecovery: 0.8
    phosphorusRecovery: 0.8
    potassiumRecovery: 0.8
    mineralsRecovery: 0.7
    waterRecovery: 0.7
```

Expected daily actuals: N recovered 48 g (nitrified); O2 ≈ 0.24 kg
(219 g nitrification + 25 g organic); CO2 ≈ 32 g; process water 0;
heat 10.08 kJ; recovered water 2.66 kg; residue 1.41206 kg (including
0.0009 kg of medium). The standalone scenario isolates the processor at the
four-crew production rate; it does not add a crew or change the reference station.
Derived-mode medium is mass-denominated (g or kg); fixed mode retains its legacy
store-unit dose and accounting.
Compare legacy fixed-rate equivalent: 3.2 kg O2/day — the point of the spec.

### 10.2 Food-waste hot digestion (`bioconversion-foodwaste.yaml`)

Spoiled/surplus food (dry, energy-dense) as a fuel-flexible heat source plus
nutrient recovery.

```yaml
microbialProcessors:
  - id: hotDigester
    rateBasis: CompositionDerived
    feedStoreId: spoiledFood      # food composition: N 5%, moist 0.0, 17 MJ/kg dry
    # ... store wiring as above, destinations distinct ...
    rateKgPerS: 5.787037e-6     # 0.5 kg/day
    organicOxidationFraction: 0.9
    nitrificationFraction: 0.0  # heterotrophic hot composting profile
    processWaterTargetMoisture: 0.6   # dry feed needs dilution water
    heatReleaseFraction: 0.7   # default; ~55 W for this food composition
    # recovery fractions at defaults
```

The shipped food composition matches reference food: dry N/P/K/minerals
fractions 0.05/0.02/0.03/0.02, leaving 0.88 organics, at 17 MJ/kg dry.
Daily actuals for 0.5 kg food: organics oxidized 0.396 kg; O2 0.5544 kg;
CO2 0.7128 kg; dilution water 0.75 kg; medium 0.0198 kg;
nutrients 0.047 kg (N 0.020, P 0.008, K 0.012, minerals 0.007 kg);
recovered water and nitrified N zero; residue 1.0644 kg.
Reaction heat is 4712.4 kJ/day (54.54 W continuous). The 2 kWh/kg
electrical dose contributes another 3600 kJ/day: exactly 8312.4 kJ/day
reaches `processSink`. The default 0.7 heat release is intentional: the
earlier illustrative 0.8 would give 5385.6 kJ/day (62.33 W), not ~55 W.
Likewise, default 0.8 N recovery gives 20 g/day, not the earlier ~18 g.

Open this standalone configuration in Station systems, then Apply settings.
Stores, solar and thermal headroom support at least one day at rated throughput.
`bioconversionHotDigestion` verifies daily-normalized actuals through public
World ticks/snapshots and exactly-once routing to the wired sink (an unwired
sink receives none). `bioconversionOxygenStarvation` checks uniform scaling
of every nonzero material/energy stream, and `bioconversionBlend` checks
actual nutrient-rich, energy-depleted crop-death feed rather than its unchanged
organic declaration. `station/bioconversionFoodwasteWorkflow` opens and applies
the shipped file through the real editor and checks the resulting actuals.

### 10.3 Legacy equivalence (`bioconversion-legacy.yaml`)

The current reference-station `dig` entry with `rateBasis: FixedPerFeedMass`
and all existing fields unchanged. Acceptance: identical tick-by-tick actuals
to the pre-change component for the same scenario (regression anchor).

### 10.4 Degenerate pass-through (`bioconversion-degenerate.yaml`)

A stage fed a compound store with no bioconvertible dry content: moisture
0.999, atmospheric nitrogen dry fraction 1, other dry fractions 0, energy 0.
The shipped scenario uses `waterRecovery: 1` to separate all water from the
inert dry balance. Lower recovery leaves unrecovered water in residue; setting
recovery to zero demonstrates the zero-throughput no-op.

```yaml
microbialProcessors:
  - id: passThrough
    rateBasis: CompositionDerived
    feedStoreId: nearlyWater      # compound; inert atmospheric N, not nutrient N
    # ... wiring as above ...
    rateKgPerS: 1.0e-5
    nitrificationFraction: 1.0  # meaningless: no N to nitrify
```

Acceptance: validates; runs every tick; O2 = CO2 = heat = consumable =
nutrient recovery = 0; water passes at `waterRecovery`; residue = inert dry
mass plus unrecovered water; no division by zero; limiter reports the no-convertible-content reason
when destinations and inputs are otherwise healthy and throughput is zero.

## 11. Migration and compatibility

- Schema: `rateBasis` defaults to `FixedPerFeedMass` on load for documents
  written before this change; save round-trips the explicit value. No forced
  migration of existing configs.
- The reference station is **not** switched to derived mode in this change;
  adding `urineNitrifier` to the reference station is a follow-up (it changes
  the shipped closure story and deserves its own ticket).
- Replays of old documents are unaffected (fixed mode is the old code path).

## 12. Test plan (smoke)

1. **Derived-mode urine nitrification:** §10.1 numbers hold through public
   World ticks and snapshots; N conservation closes; O2 draw is within 5% of
   the hand-computed derived value and an order of magnitude below the legacy
   fixed-rate draw for the same feed.
2. **Derived-mode hot digestion:** §10.2 heat, O2, CO2 and process-water
   draws hold; heat booked exactly once to the sink.
3. **Legacy equivalence:** §10.3 tick-identical to current behaviour.
4. **Degenerate pass-through:** §10.4 acceptance holds; config validates;
   zero-throughput limiter wording appears.
5. **Validation:** derived-mode range checks; plain-feed refusal retained;
   negative derived residue refused at config time; `heatReleaseFraction > 1`
   refused.
6. **Throttling:** O2-starved derived stage scales all streams uniformly to
   the binding input and reports the named limiter.
7. **Editor round-trip:** rateBasis toggle, field visibility, draft undo/redo,
   save/load, Apply; derived-rate preview matches the declared composition.
8. **Blend honesty (#601):** derived rates follow the actual drawn blend when
   the feed store drifts from its declared composition, not the declaration.

## 13. Out of scope

- G2 compound water streams (see §2).
- Multi-feed co-digestion and C:N blending.
- Characterized product streams; contamination-as-physics.
- Process dynamics: lag, acclimation, inhibition, pH, ammonia crash.
- Anaerobic digestion / methane coupling (the methane processor remains a
  separate component).
- Switching the shipped reference station to derived mode.
