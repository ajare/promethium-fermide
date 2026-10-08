# Enforce Mobility profiles in routing and traversal

Status: accepted (amended)

An Agent's effective Mobility profile assigns one of three Mobility uses to every traversal kind: Can use, Cannot use, or Only if no other option. It spans transits, ordinary and Bulkhead Doors, and the ability to operate interaction points.

Cannot use is a hard traversal constraint. Only if no other option is a last-resort routing constraint rather than a perceived-cost preference: routing first searches with every last-resort traversal excluded, then repeats with them admitted only when the first search finds no Path. This was chosen over a large finite penalty, which could cease to mean "only" in a sufficiently large World and would mix Mobility use into perceived route cost.

The Buttons entry describes interaction-point use rather than a traversal kind. Its consequences are derived from authored resource data through `Edge::requiresButton()`: remote-controlled Doors, all Lifts and Shuttles, extensible Force Bridges, and extensible Ladders require buttons. When both a traversal entry and Buttons apply, Cannot use takes precedence over Only if no other option, which takes precedence over Can use. Classification never depends on current open, extended, moving, lease, or queue state, preserving deterministic routing.

## Consequences

- A Cannot use traversal is omitted from every Path. If no permitted Path exists, the existing Route loss contract reports `unreachable`.
- Routing uses at most two searches: one without last-resort traversals and, only if that fails, one with them.
- Runtime traversal gates refuse Cannot use but permit a last-resort traversal already selected in a Path.
- Both transit mount and body edges apply the same classification.
- Mobility profiles may be supplied by an Agent tag, authored directly on an Agent under ADR 0012, or frozen from the Agent type script at construction.
- An absent individual and tag profile exposes the complete frozen script default; a profile whose entries are all Can use preserves ordinary routing exactly.
- Runtime traversal refusal protects Paths created before an effective profile changed, but coordination does not reinterpret or override the profile.
