# Enforce Mobility profiles as routing constraints

Status: accepted

An Agent's effective Mobility profile is a tag-supplied bitfield of forbidden traversals. It spans transits, ordinary and Bulkhead Doors, and the ability to operate interaction points. Routing classifies a forbidden edge as untraversable, with the traversal-time gate repeating the check only as defence in depth for an existing Path. It is not a dynamic coordination decision or capability probe.

The Buttons bit is a capability rather than a traversal kind. Its consequences are derived from authored resource data through `Edge::requiresButton()`: remote-controlled Doors, all Lifts and Shuttles, extensible Force Bridges, and extensible Ladders require buttons. The classification never depends on current open, extended, moving, lease, or queue state, preserving deterministic routing. Lift and Shuttle landing Doors remain governed by their transport and Buttons bits, not by the Door bit.

## Consequences

- A forbidden traversal is omitted from every Path by returning the graph's untraversable cost. If no alternative exists, the existing Route loss contract reports `unreachable`.
- Both transit mount and body edges apply the same classification.
- Mobility profiles are derived from Agent tag registry data and are neither cached nor persisted on Agents.
- A zero or absent profile preserves existing routing exactly.
- Runtime traversal refusal protects Paths created before an effective profile changed, but coordination does not reinterpret or override the profile.
