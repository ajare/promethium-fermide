# Carbon recovery and fuel-cell exchange observations (#710)

The applied observation catalogue registers committed Sabatier, methane processor
and fuel-cell exchanges. They use the shared World-owned bounded history,
declarative aggregate/instance queries and generic Station Flow chart renderer.
No selection runs simulation or changes the Configuration draft.

## Process accounts

- Sabatier: CO2 and hydrogen consumed; methane and water produced; electrical
  service; released reaction heat; produced heat (electrical dose plus reaction
  heat).
- Methane processor: methane consumed; recovered hydrogen and solid carbon
  produced; electrical service; absorbed reaction heat; produced heat
  (electrical dose minus absorbed reaction heat).
- Fuel cell: dedicated hydrogen and oxygen consumed; water produced. Reuse the
  existing Generation and Production waste heat observations rather than adding
  duplicate electrical/thermal process accounts. Generation offers the committed
  commanded output as its optional request.

Materials remain separate properties even when they share kg/s units. Process
material observations convert authoritative per-tick kg to kg/s. Electrical and
thermal power remain separate role-labelled kW properties; heat accounts retain
thermal service class. Fuel-cell
reactants come only from the configured dedicated stores, never from breathing
oxygen or another life-support inventory.

CO2 conversion and methane processing expose their authoritative requested feed
rates; electrical service exposes demanded power. Product quantities do not gain
invented requests. Actual aggregate rates are the default; generic Instance
breakdowns and optional requested overlays reuse the existing chart controls.
Known inactive processes report zero. Missing snapshot authority is unavailable,
not zero, and incomplete query membership produces a gap.

## Connection accounts

The shared process-connection adapter additionally registers accepted input,
delivered output and external transport loss for each physical material,
electrical and heat connection, plus retained source heat in kWh. These read the
committed connection ledger, including throughput/efficiency constraints.
Produced heat, delivered heat, retained heat and transport loss are distinct
accounts. Reaction heat and total produced heat are also alternative explanatory
accounts, not additive independent production. Process exchanges must not be
added to connection endpoint observations as if they were additional production.
Existing electrical-family endpoint views remain available.

## Verification

- `station-carbonExchangeObservations`: public World configuration, advancement
  and queries; stoichiometric products, dedicated reactant exhaustion, constrained
  connections and losses, missing observations, cross-class legality, inactive
  zeros, disabled history, aggregate/stack equivalence through real coarsening,
  and read-only viewing.
- `station-editor-carbon-observations`: real CPU ImGui property selection and
  Instance controls, retained hidden history, separate material/heat/electrical
  rates, constrained/inactive/disabled runs, and no World or draft mutation.
