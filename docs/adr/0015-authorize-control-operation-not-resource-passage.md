# Authorize control operation rather than resource passage

Status: accepted

Access permission requirements belong to Agent-operated Interaction points, with a direct requirement on an ordinary manual Door only because its opening interaction has no separate point. They do not belong to traversal resources: authorization governs whether an Agent may cause a state change, not whether it may use a resource that is already locally observed to be usable. This preserves ADR 0001's separation between device control and traversal coordination and permits intentional piggybacking, such as boarding a Lift called by another Agent or crossing an already-extended Force Bridge.

## Consequences

- Different controls for one device may require different Access permissions.
- Lift, Shuttle, and Platform lift onboard destination selectors remain ineligible for requirements; landing controls may be protected.
- Routing excludes a journey when the Agent would need to perform an operation it cannot authorize, but may admit locally observed opportunistic use without relying on remote live state.
- Preventing unauthorized passage through an already usable resource would require a separate threshold-level access rule.
- ADR 0016 adds an independent Location permission requirement for passage into Rooms and Corridors; it does not turn control requirements into passage restrictions.
