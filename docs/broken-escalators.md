# Broken Escalators (#317)

Open `resources/test-worlds/broken-escalators.world.yaml` and run the simulation.
The left, up-configured Escalator starts Broken: its steps stay fixed while the
two Agents travel in opposite directions using ordinary stationary stair speeds.
The right Escalator remains a working, downward-only comparison.

Select the left Escalator on Layer **Escalators**. **Initially Broken** is an
undoable authored edit while paused. **Live Broken** breaks/restores the device
while paused or running, without modifying the document. The status says
**Broken: stationary Staircase**, and the orange canvas warning sits above the
unchanged physical geometry, including on wireframe Layers. Restore resumes the
configured belt animation and direction without relocating Agents. Admitted
traversals finish safely, including dismounting stair-only users after restore.
Reset restores authored condition and clears every Agent's condition memories.

Broken changes both body and mount eligibility to Staircase Mobility use, and
uses stationary stair ascent/descent speeds, Stair speed modifier, effort and
interaction costs. Stationary Staircases and Stairwells have no Broken controls.
This device-specific exception to authored Mobility classification follows #226
and #317; it does not generalise live-state classification to other resources.

Each Agent observes an Escalator from either landing Location or inside its
Transit, even while passing without a Path. It remembers Broken and the
configured physical movement condition. Route decisions use local observations,
then remembered condition, then authored baseline; remote live changes never
update memories, eligibility, direction or cost. Memory has no timeout.
Unusable remaining Paths mandate Route planning and produce Route loss if no
replacement exists. Still-usable Paths and restoration use voluntary planning
and Route persistence. Committed movement is not interrupted.

Schema 36 stores optional `initiallyBroken` on Escalator construction records;
YAML and binary documents preserve authored, not live, state. Older Worlds
without the field default to working devices.

## Headless regression checks

- `pf-smoke-simulation --check escalators/stationaryBrokenRules`
- `pf-smoke-simulation --check escalators/localConditionMemory`
- `pf-smoke-simulation --check escalators/brokenPlanningAndSafety`
- `pf-smoke-simulation --check escalators/brokenRouteSelectionAndPersistence`
- `pf-smoke-persistence --check escalatorBrokenLifecycle`
- `pf-smoke-editor --check escalators/brokenControlsAndHistory`
- `pf-smoke-render --check escalators/brokenWarningPreservesPosition`

The editor and renderer checks use CPU-only ImGui contexts, simulated clicks and
recorded draw geometry. None opens a window or dialog. Random failure,
independent button failures and Agent-driven break/repair are not implemented.
