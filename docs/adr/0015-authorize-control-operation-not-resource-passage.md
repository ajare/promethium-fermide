# Authorize control operation rather than resource passage

Status: accepted

Access permission requirements protect Agent-operated Interaction points, ordinary manual Door opening (which has no separate point), and Lift or Platform lift destination selection (authored per destination Stop). They do not authorize resource passage: authorization governs whether an Agent may cause a state change, not whether it may use a resource that is already locally observed to be usable. This preserves ADR 0001's separation between device control and traversal coordination and permits intentional piggybacking, such as boarding a Lift called by another Agent or crossing an already-extended Force Bridge.

## Consequences

- Different controls for one device may require different Access permissions.
- Lift and Platform lift destination selection has a World-owned requirement per destination Stop, shared across cars and boarding origins rather than attached to one generated onboard Interaction point. This protects the selection operation, not passage through an already usable transport; landing-control requirements remain independent. #263 extends the established Lift contract to mandatory ground and optional Walkway Stops of Platform lifts. Shuttle destination requirements remain outside this milestone.
- Lift and Platform lift destination selection uses current effective grants. Authorization loss before selection reconsiders affected Paths; gains use voluntary Route planning and Route persistence. Accepted shared journeys survive subsequent permission loss and requirement tightening, including paused authoring, and never lock passengers inside. Generic onboard Interaction points remain ineligible for direct requirements; their commands use the destination Stop's requirement.
- Routing excludes a journey when the Agent would need to perform an operation it cannot authorize, but may admit locally observed opportunistic use without relying on remote live state.
- Preventing unauthorized passage through an already usable resource would require a separate threshold-level access rule.
- ADR 0016 adds an independent Location permission requirement for passage into Rooms and Corridors; it does not turn control requirements into passage restrictions.
