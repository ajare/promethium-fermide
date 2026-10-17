# Separate Station connection design from flow allocation

A connection's medium describes what flows; its physical connection type describes
how it is carried. Store per-connection carrier overrides and maximum throughput
ratings as authored Station settings, shared by World and standalone configuration
persistence, rather than as disposable diagram layout. Infer defaults only when
the existing configuration determines the carrier; do not infer material phase
from user-chosen names.

The initial design-only stage is superseded by enforcement in the authoritative
allocator, not by a second diagram execution model. Physical material, electrical,
and thermal exchanges cap connection input first, then apply efficiency (default
1.0, inclusive range [0, 1]). Receiver demand is compensated upstream within the
input cap and available supply. Actively requested input at zero efficiency is
entirely lost; absent demand does not create flow. Existing priorities, inventory
and equipment limits, connection compatibility, and automatic electrical pool
membership remain authoritative. Carrier overrides affect presentation, not
whether a relationship is enforced.

Book undelivered input as explicit external connection loss rather than silently
returning it or injecting waste into an unrelated reservoir. Produced heat unable
to enter a capped connection remains at its source and can drain in later ticks;
it is not transmission loss. Separate produced, input, delivered, lost, and
retained heat accounts preserve conservation without inventing a new heat sink.

The separate declared-profile forecast does not reproduce this allocator. Mark it
unavailable for non-default physical throughput or efficiency, with an explicit
reason and no projected range, rather than publish unconstrained promises or
introduce a second transport execution model. Exact constrained forecasting is
not yet supported; styling-only and non-physical properties leave its existing
scenario unchanged.

Identify a connection by its owning reference, peer, and exchange role, never its
position in a graph vector. Separate exchanges sharing one authored reference
(such as a population's atmosphere exchanges) have independent design properties,
while disconnecting the reference still removes the complete exchange bundle.
Rename preserves properties, disconnect removes them, and undo restores them.
Cooling-service allocations, controls, schedules, circulation gates, ambient
biological exchanges, delivery/waste-ejection and airlock events, maintenance,
and outages remain non-physical relationships. Their saved ratings are
informational: a visible diagram edge does not necessarily represent an installed
pipe or cable.
