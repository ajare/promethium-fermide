# Retire terminal coordination records at tick boundaries

Status: accepted

Interaction requests and device operations were created for every interaction
and never removed once they reached a terminal outcome. Nothing released them:
the normal completion path in `SimulationCoordinator::updateInteractionResults`
published the outcome but left the record in `World::mInteractionRequests` and
`World::mDeviceOperations`, and draining `consumeSimulationEvents()` does not
touch either registry. Every later request therefore scanned the whole
historical set in `findOrCreateDeviceOperation` and `requestInteraction`, and
every tick iterated it again in `advanceDeviceOperations` and
`updateInteractionResults`. A single Agent repeating one light-switch
interaction made each successive batch of requests progressively slower.

Terminal records are now retired once no live owner names them. The
coordinator sweeps at the tick boundary - after the previous tick has published
its transitions and offered every waiting traversal its chance to observe, and
before the next tick's owners look at anything. A record is retained while any
of its readers still holds its identity:

- an `InteractionPoint` still names it as `mActiveRequest` or in `mQueue`;
- a `TraversalResource` still names it as `mActivePreparation`;
- an `Agent` still names it as `mEarlyDoorPressInteraction`;
- a device operation is additionally retained while another interaction request
  names it in `mOperations`, or a `TraversalRequest` names it as
  `mPreparationOperation`.

Retirement publishes the removal through the existing
`SimulationEventType::InteractionRequestRemoved` and
`DeviceOperationRemoved` events, using the existing `removeDeviceOperation`
and a new `removeInteractionRequest` that mirrors it. The outcome itself
remains observable: it is captured in the terminal `InteractionRequestChanged`
and `DeviceOperationChanged` events and in the live snapshot before the record
goes.

## Considered options

- **Keep every terminal record and add a bounded history registry beside the
  hot registries.** Rejected: the hot registries are what every scan and tick
  traverses, so a second registry only relocates the cost. The event stream
  already carries the value snapshot, so an explicit history answers no query
  the events do not.
- **Erase a record as soon as its result is published.** Rejected: a waiting
  traversal observes its preparation on a later tick, so erasing on publication
  would turn "Succeeded" into a re-issued preparation. Reference counting is
  what makes the result survive until it has been read.
- **Retire inside the publication pass.** Rejected: the dirty-publication
  contract derives `DeviceOperationChanged` events from the snapshot diff, so a
  record removed in the same tick as its transition makes the published events
  disagree with the projected state. The tick boundary keeps the diff whole.
- **An explicit `consume`/`release` call per outcome.** Rejected: every
  cancellation, removal, and preparation-failure path would have to remember to
  release, and one missed path reintroduces the leak. A reference-based sweep
  derives retention from the owners that already exist.

## Consequences

- Active coordination memory and per-tick work scale with active interactions
  and traversals, not with lifetime traffic.
- `lookupInteractionRequest` and `lookupDeviceOperation` may report a handle as
  invalid after its outcome has been consumed. Callers that need a durable
  outcome read it from the published event or from the live snapshot, not from
  a registry lookup after the fact.
- `removeInteractionRequest` is the counterpart of `removeDeviceOperation`; the
  sweep calls both, and callers may call them directly.
