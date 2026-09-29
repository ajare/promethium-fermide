# Separate perceived route cost from traversal facts

Status: accepted

Path selection will distinguish three concerns: hard traversal feasibility, objective traversal facts, and an Agent's perceived route cost. The graph search will correctly minimise perceived route cost over the routes the Agent considers; imperfect human judgement will be represented by observations, uncertain estimates, stable personal preferences, and route persistence rather than by an inadmissible search heuristic.

This was chosen over treating `Edge::getWeight()` as both travel time and preference, or deliberately allowing A* to return an arbitrary non-minimal result. One overloaded weight cannot consistently support runtime timing, estimated arrival, preference, diagnostics, and admissible search. An invalid heuristic makes route choices depend on graph geometry and expansion order rather than recognisable human considerations.

## Consequences

- A forbidden traversal remains a hard exclusion under ADR 0011 and is repeated at the runtime gate as defence in depth. A last-resort traversal is selected through ADR 0011's two-pass feasibility rule, not by distorting perceived route cost. A disliked but ordinarily usable traversal has finite perceived route cost.
- Objective facts such as distance, direction, device timing, capacity, and vehicle speed remain independent of the Agent's preferences.
- Perceived route cost is a seconds-equivalent score, not a prediction that the journey will take that many seconds.
- Search uses Dijkstra or a demonstrably admissible lower bound and returns the minimum perceived-cost Path for one immutable route-decision context.
- An Agent may use local Route observations and baseline expectations, but route search does not inspect unobservable remote live state.
- Agent properties may alter both physical movement and subjective route choice. Individual properties continue to override tag-supplied properties under ADR 0012; Mobility profiles remain routing and capability constraints rather than perceived-cost preferences.
- Replanning compares perceived route costs and applies Route persistence, preventing small changing estimates from causing oscillation.
- Edge implementations no longer own the complete route-choice policy. They expose or reference traversal facts from which a route-cost evaluator produces directed, Agent-specific costs.
