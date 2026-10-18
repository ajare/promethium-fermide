# Scheduled external and maintenance exchange observations (#714)

The applied catalogue registers separate properties for:

- **Resupply**: Delivered, Accepted (inputs), and Rejected (boundary output).
  Delivered equals accepted plus rejected. Only accepted material enters the
  Resource store. Rejected material never entered the station, so it is not a
  station loss. These properties are alternative accounts, not additive stock.
- **Waste ejection**: Ejected material, separately named as a Loss. Only the
  actual removal of qualified, disposal-ready contents counts. Unmet ejection
  and contents held awaiting treatment or clearance are not losses.
- **Maintenance**: one Consumed requirement input per consumed requirement.
  Reusable tools, reservations and installation isolation are not consumption.
  Actual committed spend moves into the existing waste store; it is not an
  external loss or an additional inventory owner.

## Event quantities versus rates

Delivery `lastScheduled`, `lastAccepted`, `lastRejected`, waste `lastEjected`,
and maintenance `lastConsumed` are **discrete quantities for the last event**,
not per-tick measurements. They can remain unchanged for many ticks and are
never sampled directly as rates. Snapshot cumulative counters are quantities
since the applied run began. Maintenance requirements expose `consumedTotal`,
an observation-only ledger incremented at the existing authoritative completion
commit, including recurring completions; reservations and interrupted work do
not increment it. It is runtime state, not persisted authored configuration.

Charts show **interval-average event rates**: the difference between committed
cumulative quantities at consecutive observations, divided by the actual elapsed
simulation seconds. The first interval starts at run time zero and includes
boundary-time-zero events. All events between observations count, including
multiple recurrences and events exactly at a sample boundary. After an event,
an interval without another exchange records zero, not the last event again.
These are neither instantaneous nor sustained operating rates. The generic
renderer displays this distinction in each property's accounting note.

Store unit conversion precedes presentation: mass uses kg/s, volume m3/s,
amount of substance mol/s, and energy kW (including kWh-to-kJ conversion).
Unsupported units do not acquire a fabricated physical measurement. Explicit or
validated relationship-resolved material identity, hazard class, and source
store canonical conversion are retained. Unclassified identities stay visibly
unclassified and are not combined across instances. No requested overlays are
invented. Missing authoritative schedules or requirements produce gaps.

The shared World recorder records these observations while the editor is hidden
and before a property is selected. Existing aligned bounded history, coarsening,
selection, instance breakdowns, pause and applied-run Reset semantics are reused.
No diagram connection execution, allocation changes or equipment-specific
rendering is introduced. Outages and supervisory configuration create no
exchange merely by having a schedule.

## Acceptance checks (Release)

- `station-scheduledExchangeObservations`: catalogue availability before samples,
  time-zero and sample-boundary deliveries, multiple recurrences per observation,
  canonical g-to-kg conversion, capacity-limited acceptance/rejection,
  treatment/clearance-gated hazardous ejection, material-limited maintenance,
  returned tools, zero versus unavailable, aggregate/stack equality, inventory
  ownership, pause, and deterministic Reset of quantities and rate history.
- `station-editor-scheduled-observations`: real grouped selector and Instance
  controls, visible window semantics and canonical units, all scheduled families
  including failed/passed hazardous clearance scenarios, hidden/preselection
  history, and read-only World/document behavior.
