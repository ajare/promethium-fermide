# Use Local depth as a secondary complete-Path objective

Status: accepted

Furniture introduces sector-local render depth without adding a physical spatial dimension. Route selection first minimises total Perceived route cost, then, only among equally costly complete Paths, minimises the sum of absolute numerical Local-depth changes, including the Agent's incoming or retained depth. This preserves ADR 0014's minimum-cost guarantee while making otherwise equivalent movement favour continuity in front of or behind Furniture.

## Considered options

- **Choose the closest-depth outgoing edge greedily.** Rejected: locally equivalent edges can lead to different total costs or later depth changes, so local tie-breaking does not express the agreed complete-Path preference.
- **Add a small depth penalty to Perceived route cost.** Rejected: even a small finite penalty can select a more costly Path and turns a tie-break into a primary preference.
- **Use ordinal depth ranks or physical depth distance.** Rejected: numerical gaps matter to the agreed continuity preference, but they represent neither another World Layer nor extra movement distance or time.

## Consequences

- Route search must preserve enough incoming-depth context to compare optimal continuations. Reaching one vertex at different depths is not necessarily an interchangeable search state.
- Depth numbers are local to a Sector. Entry into another Sector resets the continuity baseline to 0 rather than comparing unrelated numbers.
- An Agent adopts the active edge's integer render depth immediately, retains incoming depth while stationary, and starts at 0 without edge history. Depth-changing waypoints are non-skippable.
- Existing hard feasibility, Mobility, permissions, Route observations, and primary Perceived route-cost rules remain independent of this secondary objective.
