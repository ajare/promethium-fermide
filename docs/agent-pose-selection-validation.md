# Shared Agent pose fit, selection, routing and movement (#522)

The frozen API-v2 declarations from #521 now drive Room movement and passenger
Door crossing. `Agent::poseFits` uses the declared envelope, effective Standing
Height, bodily width, real support and `PoseFitTolerance` (0.00001 World units).
Lying's vertical extent is bodily width; Crawling uses its declared height ratio.
Decorative offsets never enter fit.

`selectAutomaticPose` returns the first fitting context choice and its speed
ratio, or no result. Room selection uses clearance above the approach Floor or
Walkway, and preserves the non-Room Standing policy. Ordinary movement derives
its pose from the physical Sector under the Agent rather than lagging logical
membership. Unsupported or impossible creation and relocation are refused before
position/ownership publication.

`Door::selectAgentCrossing` supplies the same decision to classification/admission,
route capture and permit adoption. This includes ordinary manual, automatic and
remote Doors, standalone Bulkheads, Airlocks, both Chamber subtypes and Lift and
Shuttle thresholds. Broken vertical apertures scale clearance; Broken horizontal
apertures retain the existing bodily-width rule. Windows and BoothWindows gain no
passenger behavior. Human preserves Room Standing/Crouching/Crawling and Door
Standing/Crawling orders. A supported Crouching Door choice is usable; a
Standing-only robot cannot obtain Crawling through Mobility or permissions.

Direct Room and Door route facts delegate to the same captured-input evaluator.
Capture records the selected motion ratio, not a second selection policy or a
live Agent query.
Ordinary Room arc motion estimates split at physical Room boundaries. Force
Bridge walking uses the same Room-context motion estimate, without changing
extension timing, controls or risk costs. Remaining Paths and new admission reject impossible target Rooms through the existing Route
planning/Route loss mechanism. Failed new permit adoption replans rather than
inventing a pose. Admitted crossings retain their selected pose/ratio until
physical completion. Room ratios affect walking; Door ratios affect threshold
motion, including the six-tick inter-Layer crossing. Device operation/cycle times,
transport rides, queues, lanes, capacity, permissions and Mobility are unchanged.

## Headless Release coverage

Focused CTests use the `pose-selection` label:

- `agentPosesSupportedRooms`: actual Lua-supported orders, impossible robot
  placement/relocation, Marker Route loss, context walking duration, direct/captured
  Room motion facts and physical-boundary recovery.
- `agentPosesSupportedBridge`: supported Crouching on an upper Walkway and
  Force Bridge, both-direction direct/captured motion estimates and actual
  Marker journey duration.
- `agentPosesSupportedDoors`: both directions and all three activation modes,
  impossible robot journeys and a feasible alternative Door Path.
- `agentPosesCrouchingDoor`: supported custom Crouching, normal/Broken apertures,
  direct/captured estimates, Standing waiting/recovery and 24-tick crossings.
- `agentPosesApertureBoundaries`: ordinary and standalone Bulkhead journeys,
  both directions, effective Height, exact/within-tolerance/over-tolerance fit,
  Broken height/width apertures and user-visible refusal.
- `*/supportedPoseRefusal`: real Standing-only resource Marker journeys through
  Airlocks, both Chamber subtypes, Lifts and Shuttles, including impossible
  Chamber exits before admission and both authored directions.

Existing DoorClearance, Broken aperture, low-Room, automatic/mixed-Crawling,
transport/chamber gates, capacity/queue, rendering and route-cost regressions
remain part of the final Release inventory. Tests use production World APIs,
fixed ticks, events and snapshots, not a private pose setter.

Final validation: incremental full Release build (including editor and headless)
succeeded. All 32 selected CTests passed: the ten `pose-selection` tests plus
Agent, Simulation, Routing, Persistence, Render, Editor, World, Transports,
Permissions, Behaviours and Agent-tag suites and their contracts. No submodule
tests were run. `git diff --check` passed.

Furniture pose requirements/eligibility remain #523. Full Action/Furniture
ownership release, edit protection, fresh-lifetime placement integration and
accepted grandfathered-exit recording remain #524. This slice preserves existing
committed completion using supported frozen choices; it does not claim the final
accepted-exit lifecycle contract or add runtime persistence.
