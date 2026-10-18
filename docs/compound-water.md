# Compound water: full-removal treatment (#740)

Water treatment source, treated and concentrate ports accept plain or compound
mass stores. Other process ports keep their existing contracts; fractional
removal, consumer quality gates and producer carry-over belong to later #739
slices. No new fields or schema migration are required. The reference station
remains unchanged.

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

Compound concentrate is a qualified product **of its own hazard class**, allowing
same-composition routing and scheduled waste ejection. Qualified concentrate is
not purified water: hazard segregation and the compound-to-plain boundary still
apply. Plain scalar concentrate keeps its historical held state, so existing
documents' treatment actuals remain unchanged. Hazardous treated output retains
its existing energy-evidence and release-clearance gates.

`StationSystemsStoreSnapshot::concentrations` and Lua `stores[i].concentrations`
report kg of each existing mass constituent per kg stream (energy excluded).
They derive from the actual ledger, not the authored composition. Empty inventory
reads zero; plain treatment water ports read zero dry load. Compound ledger slots
remain in `actualConstituents`; the editor shows live concentrations in Store
inspector telemetry and the live Resource stores section.

## Examples

Load either standalone configuration in Station systems and Apply:

- `resources/station-systems/scenarios/water-greywater.yaml`: full removal of
  100 kg/day with 0.2% organics and 85% water recovery gives **84.83 kg/day pure
  treated water** and **15.17 kg/day concentrate**, containing all 0.2 kg/day
  organics plus 14.97 kg/day rejected water. These are the exact mass-based
  specification equations, rather than the rounded 85/15 ticket shorthand.
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
