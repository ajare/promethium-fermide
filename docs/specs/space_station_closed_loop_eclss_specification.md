# Closed-Loop Environmental Control & Life Support System

## 1. Design Basis

-   **Reference population:** 100 humans
-   **Biological component:** hydroponic crops + algae bioreactors
-   **Normal atmosphere:** 101 kPa, approximately 21% O₂ / 78% N₂
-   **Nominal inhabited atmospheric volume:** 10,000 m³
-   **Normal environmental systems:** shared between habitation and
    non-hazardous laboratories
-   **Hazardous systems:** physically isolated atmosphere, drainage, and
    waste handling
-   **Design objective:** \>95% water recovery, substantial carbon/O₂
    recycling, nutrient recovery, and minimal external consumables

The station maintains several inventories rather than attempting perfect
instantaneous balance. Tanks, gas reservoirs, and biological buffers
allow production and consumption rates to differ temporarily.

## 2. Overall Architecture

``` text
                       EXTERNAL INPUTS
                             │
        ┌────────────────────┼─────────────────────┐
        │                    │                     │
       H₂O                  N₂/O₂              FOOD/
        │                makeup gases          NUTRIENTS
        │                    │                     │
        ▼                    ▼                     ▼
┌────────────────┐    ┌──────────────┐      ┌─────────────┐
│ WATER STORAGE  │    │ GAS STORAGE  │      │ AGRICULTURE │
└───────┬────────┘    └──────┬───────┘      └──────┬──────┘
        │                    │                     │
        ▼                    ▼                     │
┌─────────────────────────────────────┐            │
│         NORMAL ECLSS                │◄───────────┘
│                                     │
│ Water / Atmosphere / Carbon / Waste │
└───────┬──────────────────┬──────────┘
        │                  │
        │                  ▼
        │              ┌──────────┐
        │              │ HUMANS   │
        │              └────┬─────┘
        │                   │
        │       CO₂ / H₂O / waste / heat
        │                   │
        └───────────────────┘

                ═════ ISOLATION BOUNDARY ═════

┌─────────────────────────────────────────────┐
│             HAZARDOUS ECLSS                 │
│                                             │
│ Independent air + drainage + waste tanks    │
└──────────────────┬──────────────────────────┘
                   │
             destructive treatment
                   │
           ┌───────┴─────────┐
           ▼                 ▼
    certified H₂O/gas   concentrated waste
           │                 │
           ▼                 ▼
     NORMAL ECLSS       WASTE EJECTION
```

There are six major material loops:

1.  Atmosphere
2.  Water
3.  Carbon/Oxygen
4.  Food/Nutrients/Biomass
5.  Solid waste
6.  Thermal control

Hazardous systems duplicate portions of the atmosphere, water, and waste
loops.

## 3. Human Metabolic Design Load

For design purposes, assume each person consumes approximately:

  Resource                 Per person/day   100 crew/day
  ---------------------- ---------------- --------------
  O₂                              0.84 kg          84 kg
  Drinking/food water               3.0 L          300 L
  Hygiene water                      15 L        1,500 L
  Food dry mass                   0.62 kg          62 kg
  Food including water           \~1.8 kg         180 kg

Approximate outputs:

  Output                           Per person/day   Station/day
  ------------------------------ ---------------- -------------
  CO₂                                    \~1.0 kg        100 kg
  Urine                                   \~1.5 L         150 L
  Faecal wet waste                      \~0.13 kg         13 kg
  Respiratory/perspiration H₂O       \~1.0--2.0 L    100--200 L
  Metabolic heat average                  \~100 W         10 kW

Activity causes large fluctuations, so equipment should be sized
considerably above averages.

## 4. Normal Atmosphere System (NAS)

### Atmosphere Inventory

For 10,000 m³ at roughly Earth-normal pressure, atmospheric gas mass is
approximately **12,000 kg**:

-   N₂: \~9,000 kg
-   O₂: \~2,800 kg
-   Ar/CO₂/trace gases: remainder

The large atmosphere itself provides a substantial buffer.

### NAS-1 Air Circulation

Design circulation: **60,000 m³/h**, giving roughly six complete nominal
air exchanges through environmental circulation per hour.

``` text
Cabin
  ↓
Particulate filter
  ↓
HEPA/filter stage where required
  ↓
Condensing heat exchanger
  ↓
Trace contaminant removal
  ↓
CO₂ control
  ↓
Temperature control
  ↓
Cabin
```

Atmospheric sensors continuously measure O₂ partial pressure, CO₂
concentration, pressure, humidity, temperature, CO, VOCs, particulates,
smoke, and selected biological contaminants.

## 5. CO₂ Management

Humans generate approximately **100 kg CO₂/day**.

Three destinations are available:

``` text
                    CO₂ REMOVAL
                         │
              ~100 kg CO₂/day
                         │
             ┌───────────┼────────────┐
             ▼           ▼            ▼
        Hydroponics     Algae      Sabatier
```

A reasonable nominal allocation:

-   **Hydroponics:** 40 kg/day
-   **Algae:** 40 kg/day
-   **Sabatier:** 20 kg/day

These values are dynamically controlled. Plants do not consume CO₂ at a
constant rate, so the atmosphere-control system adjusts distribution
automatically.

## 6. Biological Atmosphere Processor

### Hydroponic Farm

Allocate approximately **2,000--3,000 m² of illuminated growing area**.
Stacked growing racks can substantially reduce floor area.

Nominal circulating hydroponic water inventory: **40 m³**.

``` text
40,000 L nutrient reservoir
          │
          ▼
        Pumps
          │
          ▼
    Plant root beds
          │
          ▼
 Filtration/sensors
          │
          └────► Reservoir
```

Plants consume CO₂ and produce O₂ while illuminated. Transpired water is
recovered:

``` text
Plants
  ↓
H₂O vapour
  ↓
Greenhouse dehumidifiers
  ↓
Condensate
  ↓
Water treatment
  ↓
Hydroponic reservoir
```

### Algae Bioreactors

Provide **20--40 m³ active culture volume**, divided into multiple
independent reactors; for example, **8 × 5 m³ reactors**.

Each receives water, CO₂, recovered nitrogen, phosphorus, potassium,
trace minerals, and artificial light.

Nominal design range: **0--50 kg CO₂/day processing capacity**.

Harvested biomass can be divided between food, nutrient extraction, and
waste processing.

## 7. Electrolysis Oxygen Generator

Biology is not trusted as the sole O₂ source.

Design electrolysis capacity: **120 kg O₂/day**, versus normal human
demand of \~84 kg/day.

Reaction:

`2H₂O → 2H₂ + O₂`

Producing 84 kg O₂ requires approximately **95 kg water/day** and
generates roughly **11 kg H₂/day**.

Provide approximately **500 kg usable O₂ reserve**, equivalent to
roughly six days of metabolic consumption without biological O₂
production.

## 8. Hydrogen and Carbon Recovery

Hydrogen from electrolysis feeds Sabatier reactors:

`CO₂ + 4H₂ → CH₄ + 2H₂O`

Water returns to water recovery. Methane can be pyrolysed:

`CH₄ → C + 2H₂`

``` text
CO₂ + H₂
   ↓
Sabatier
   ↓
CH₄ + H₂O
 │      │
 │      └────────► Water
 ▼
Pyrolysis
 │
 ├── H₂ ─────────► Sabatier
 │
 └── solid C ────► Industrial store/waste
```

Recovered hydrogen returns to the Sabatier process. Excess solid carbon
can enter industrial stores or terminal waste.

## 9. Water System

A sensible starting water inventory is **100 m³ = 100,000 L ≈ 100
tonnes**.

  Reservoir                   Volume
  ------------------------- --------
  Potable reserve              20 m³
  Process/reclaimed water      15 m³
  Hydroponics                  40 m³
  Emergency reserve            15 m³
  Treatment/buffer tanks       10 m³

The emergency 15 m³ reservoir should be physically isolated.

## 10. Water Collection and Recovery

Normal wastewater is separated into condensate, urine, hygiene
greywater, and agricultural/food water.

Approximate daily flows:

-   Human atmospheric condensate: **\~150 L/day**
-   Urine: **\~150 L/day**
-   Hygiene greywater: **\~1,500 L/day**
-   Plant transpiration: potentially **thousands of litres/day**, mostly
    internally recycled

``` text
              NORMAL WATER RECOVERY

Urine ────────────────► Urine processor ──┐
                                         │
Greywater ────────────► Filtration ───────┤
                                         │
Atmospheric condensate ──────────────────┤
                                         ▼
                                  Water processor
                                         │
                                  ┌──────┴───────┐
                                  ▼              ▼
                              Purified       Concentrate
                               water            waste
                                  │
                                  ▼
                           CLEAN WATER STORE
```

Target overall recovery: **97%**.

With secondary recovery, nominal external water makeup should be
approximately **10--30 L/day**.

## 11. Nutrient Loop

Plants require nitrogen, phosphorus, potassium, calcium, magnesium,
sulfur, iron, and trace elements.

Human waste contains recoverable nutrients.

``` text
Urine
  ↓
Nutrient recovery
  ↓
NH₄⁺ / nitrogen compounds
  ↓
Nitrification
  ↓
NO₃⁻
  ↓
Hydroponics
```

Phosphorus and potassium are similarly recovered from urine and
processed organic waste.

``` text
Organic waste
      │
      ▼
Waste Processor
 │        │         │
 ▼        ▼         ▼
H₂O      CO₂     Mineral salts
 │        │         │
 ▼        ▼         ▼
Water   Carbon    Nutrient
system  system    processing
```

## 12. Non-Recoverable Waste

Non-recoverable material includes filtration residues, concentrated
salts, heavy-metal contamination, damaged materials, toxic laboratory
residues, excess carbon, unrecoverable plastics, contaminated sorbents,
and biological treatment residues.

``` text
Waste
  ↓
Sorting
  ↓
Water extraction
  ↓
Volatile extraction
  ↓
Sterilization
  ↓
Compression
  ↓
Sealed waste cartridge
  ↓
WASTE EJECTION SYSTEM
```

Waste cartridges must be placed on controlled disposal trajectories
rather than simply released near the station.

Nominal terminal waste target: **\<10 kg/person/year**, or **\<1,000
kg/year** for the reference station.

## 13. Airlocks and Atmosphere Recovery

Assume a normal personnel airlock volume of **30 m³**. At 101 kPa it
contains roughly **36 kg atmosphere**.

``` text
AIRLOCK
  │
  ▼
Compressor
  │
  ▼
500–1,000 L high-pressure receiver
  │
  ▼
Airlock pressure falls
  │
  ▼
Vacuum pump
  │
  ▼
Outer hatch permitted to open
```

Target gas recovery should exceed **95%**, preferably reaching
**98--99%** through staged pumping.

Recovered atmosphere returns through filtration and analysis before
reuse.

## 14. Nitrogen System

Nitrogen is not significantly metabolically consumed. Losses arise from
airlocks, leaks, maintenance, docking, venting, and emergencies.

Provide approximately **2,000 kg reserve N₂**.

``` text
N₂ STORE ─────────┐
                  │
O₂ STORE ─────────┼──► ATMOSPHERE MIXING
                  │
Recovered air ────┘
```

Control total pressure, O₂ partial pressure, and CO₂ partial pressure
independently.

## 15. Hazardous ECLSS

The hazardous research sector has **no direct return-air path** to the
normal station.

Use a pressure cascade:

``` text
NORMAL STATION
     │
     ▼
  101 kPa
     │
Pressure lock
     │
     ▼
HAZARDOUS CORRIDOR       ~98 kPa
     │
     ▼
HAZARDOUS LAB            ~95 kPa
```

Leaks therefore preferentially flow into hazardous containment.

### Hazardous Atmosphere

``` text
Hazardous Lab
      │
      ▼
Particulate filtration
      │
      ▼
HEPA filtration
      │
      ▼
Chemical sorbents
      │
      ▼
Thermal/plasma/UV sterilization
      │
      ▼
Gas analysis
      │
      ├── FAIL ─────► Recirculate
      │
      └── PASS ─────► Recovery reservoir
```

Even treated gas should normally remain within the hazardous ECLSS.

### Hazardous Water

There are no common drains between normal and hazardous systems.

``` text
Hazardous sinks
Hazardous showers
Decontamination
Experiment drainage
       │
       ▼
HAZARDOUS WASTEWATER TANK
       │
       ▼
Destructive treatment
       │
       ▼
Distillation
       │
       ├── purified H₂O
       │       │
       │       ▼
       │   quality verification
       │       │
       │       ▼
       │   process-water system
       │
       └── concentrate
               │
               ▼
         terminal waste
```

Provide approximately **10 m³ hazardous wastewater capacity**.

## 16. Hazardous Emergency Mode

The hazardous sector must be capable of operating as an independently
sealed environment.

Suggested reserves:

-   **O₂:** \~200 kg
-   **Water:** \~5 m³
-   **CO₂ scrubbing:** ≥7 days
-   **Battery:** ≥24 h
-   **Atmosphere:** independent
-   **Cooling:** isolated internal loop

Thermal energy may cross the containment boundary through a heat
exchanger, but fluids must not:

``` text
Hazard coolant
      │
      │ HEAT ONLY
      ▼
Heat exchanger
      ▲
      │
Normal station coolant
```

## 17. Thermal System

Assume station electrical consumption reaches several megawatts, largely
because of agricultural lighting and scientific equipment. Almost all
electrical energy ultimately becomes heat.

Illustrative heat rejection capacity:

-   **Nominal:** 5 MW
-   **Peak:** 7 MW

Use separate internal habitat, hazardous, and external radiator coolant
loops. Exchange heat through isolated heat exchangers so loss or
contamination of one loop does not contaminate the others.

## 18. External Inputs

The system is closed-loop but not perfectly closed. Long-term operation
requires periodic replenishment.

Expected external inputs include:

  Input                          Purpose
  ------------------------------ ----------------------------------------
  Water                          Makeup for unrecoverable losses
  N₂                             Atmospheric leakage/airlock makeup
  O₂                             Emergency reserve replenishment
  Food                           Supplements agricultural production
  N/P/K compounds                Replace unrecoverable nutrient losses
  Trace minerals                 Hydroponics/algae makeup
  Filter media/sorbents          Atmospheric and water purification
  Replacement chemicals          Treatment processes
  Replacement biological stock   Crop/algae failures
  Equipment/spares               Pumps, membranes, lamps, sensors, etc.

Nominal water makeup target: **10--30 L/day**.

Nitrogen and oxygen resupply depend strongly on leakage and airlock
usage.

## 19. Material Flow Summary

The principal material flows are:

``` text
HUMANS
 │
 ├── consume O₂ ◄──────── Plants / Algae / Electrolysis
 ├── consume H₂O ◄─────── Water Recovery
 ├── consume food ◄────── Hydroponics / Algae / External Supply
 │
 ├── CO₂ ────────────────► Plants / Algae / Sabatier
 ├── H₂O vapour ─────────► Condensate Recovery
 ├── urine ──────────────► Water + Nutrient Recovery
 └── solid waste ────────► Organic Waste Processing

PLANTS / ALGAE
 │
 ├── consume CO₂ ◄──────── Humans / Waste Processing
 ├── consume H₂O ◄──────── Water System
 ├── consume nutrients ◄── Waste/Nutrient Recovery + Makeup
 │
 ├── O₂ ─────────────────► Atmosphere
 ├── food/biomass ───────► Humans
 ├── H₂O vapour ─────────► Condensate Recovery
 └── waste biomass ──────► Organic Waste Processing

TERMINAL WASTE
 │
 ├── recover H₂O
 ├── recover useful volatiles/materials
 ├── sterilize
 ├── compact
 └── controlled ejection/disposal
```

## 20. Design Philosophy

The station is best treated as several coupled resource economies:

1.  **Oxygen/carbon economy** --- humans, plants, algae, electrolysis,
    Sabatier, and carbon recovery.
2.  **Water economy** --- potable water, hygiene, condensate, urine,
    hydroponics, treatment, and reserves.
3.  **Nutrient economy** --- food, human waste, plant waste, mineral
    recovery, and hydroponics.
4.  **Atmospheric buffer economy** --- O₂, N₂, recovered air, airlocks,
    and pressure control.
5.  **Waste economy** --- recovery first, concentration second, disposal
    only for irrecoverable residues.
6.  **Thermal economy** --- all inhabited and processing systems
    ultimately feed heat to isolated coolant loops and external
    radiators.

Normal systems share resources wherever this improves efficiency.
Hazardous systems remain physically segregated and exchange only tightly
controlled resources or heat across verified containment boundaries.
