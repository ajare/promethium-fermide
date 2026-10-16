# Agent Path evaluation flowchart

Open **[agent-path-evaluation.svg](agent-path-evaluation.svg)** in a browser or SVG viewer and zoom in. This is a general decision flow, not a trace of one particular Path.

| Panel | Logic |
| --- | --- |
| A | Movement request, planning and six-phase tick overview |
| B | Source selection, search, fallback Mobility and Route persistence |
| C | Directed edge feasibility and perceived route cost |
| D | Waypoint approach, skipping, queue arrival and intent creation |
| E | Runtime traversal admission and resource dispatch |
| F | Control necessity, Arms/Remote/None, reach, press and device operations |
| G | Lift, Platform lift, Shuttle, Airlock and Chamber journeys |
| H | Permit adoption, pose, movement and commit |
| I | Waiting, retries, refusal, replanning and Route loss |

Green nodes link to the indicated panel or continuation. Source references link to repository files with line numbers. `SC` means `SimulationCoordinator`; World forwards the traversal APIs to it. A diamond labelled `[inline]` is a condition within the named function, not an invented predicate function.

The chart follows the current implementation's **Edge type and directed source/target Sector** dispatch. Vertex type/subtype alone is insufficient: an Interactable affects approach/skipping, a Door may belong to a transport, and a Platform lift's endpoints can occupy the same Room Sector.

It covers autonomous Path following, including safe completion of admitted crossings and committed journeys. Player input-specific decisions, unrelated nearby device operation, rendering, persistence and the internals of selected Lua Actions are not expanded. Function-level branches are summarized; defensive guards and individual scheduler transitions are not exhaustive call graphs. A control press or a selected Path never substitutes for a traversal permit.

## Regenerate

From any working directory, using Python 3 (standard library only):

```sh
python3 /path/to/promethium-fermide/docs/diagrams/generate_agent_path_flow.py
```

The generator resolves referenced symbols against the checkout, updates source line numbers, and checks internal node targets. Commit the SVG with generator changes. Re-run after relevant C++ changes; the SVG does not update itself.

See also [Agent movement and coordination](../agent-movement-and-coordination.md), [Object usage](../object-usage.md), and [Route planning](../route-planning.md).
