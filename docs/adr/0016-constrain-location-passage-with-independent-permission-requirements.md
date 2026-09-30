# Constrain Location passage with independent permission requirements

Status: accepted

Rooms and Corridors may own a Location permission requirement that makes all of their vertices unavailable to an Agent lacking every required Access permission. This passage constraint is independent of the operation requirements in ADR 0015: entering through a protected control may require satisfying both rules, while an Agent already inside may use the Location only to leave and is never trapped there by the Location requirement itself.

## Consequences

- Facades do not support Location permission requirements.
- Location requirements are hard route constraints and are unaffected by Permission adherence or Mobility use.
- Authorization is checked when entry starts; an entry already underway finishes safely before the Agent replans.
- The destination Location governs boundary entry, so authorization is not required merely to exit a Location.
