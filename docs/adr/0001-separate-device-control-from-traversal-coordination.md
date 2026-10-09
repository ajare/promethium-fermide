# Separate device control from traversal coordination

Status: accepted

The simulation will replace the overlapping `Controller`, `Controllable`, `Orchestrator`, and `VertexController` hierarchies with explicit interaction points, typed device operations, and shared traversal resources. Edges describe route topology but traversal resources exclusively own queues, capacity reservations, and permits; `World` owns simulation entities and relationships use stable typed handles. This separation was chosen over extending the existing inheritance and callback model so that device state, movement permission, and object lifetime each have one authoritative owner.

## Distance-limited operation (#534, verified by #544)

Object usage changes the eligibility and approach for device control, not the
traversal authority. A Path identifies its next required eligible object; remote
range entry can begin its normal operation early without a control-only detour.
It does not activate unrelated nearby objects, shorten device/press durations,
reserve capacity, or replace physical crossing, boarding and disembarking.
Queues, interlocks, Access permissions and Mobility remain authoritative. Pending
operations revalidate paused capability edits; admitted crossings and committed
passenger exits retain their established completion guarantees.
