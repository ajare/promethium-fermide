# Internal Interaction evaluation contexts (#617)

`World::InteractionEvaluationContext` replaces the internal `requireReach`
boolean. Public World and SimulationCoordinator entry points and return values
are unchanged. The contexts describe evaluation, not new capabilities,
authorization, or traversal admission.

| Context | Arms reach policy | Request state policy |
| --- | --- | --- |
| `AutonomousApproach` | May accept before reaching an ordinary control. BoothWindow and Dumbwaiter owned controls still require reach at request. | Idle or WaitingForTraversal. |
| `ImmediatePress` | Must already be within both Object usage distance and Interaction point geometry reach. No approach implied. | Idle or WaitingForTraversal. |
| `RemoteActivation` | Immediate reach if evaluated for Arms; never grants Remote control capability. | Remote control permits requests during movement and Route planning. |
| `FurnitureFinishEffects` | Immediate reach, as for other staged Action device effects. | The current finishing Agent may request before its staged departure physically commits. |

**Every context** uses the Agent's effective Object usage. Remote control always
requires its existing eligible-control classification, current Sector, remote
property gates, ownership/occupancy where applicable, and inclusive straight-line
range; even `AutonomousApproach` cannot waive that range. None always refuses.
Activation and Buttons Mobility checks remain common prerequisites.

The operation-capability query does not check movement state. The request query
adds the state policy above, including the existing Remote control bypass and
scoped `mFinishingFurnitureUseAgent` exception. That exception remains available
during both Action-effect preflight and subsequent ordinary request construction;
it cannot be obtained just by choosing the finishing context. Furniture finishing
preflight still requires immediate reach and does not commit partial effects on
refusal. General Marker Action device effects use `ImmediatePress`.

## Lifecycle boundaries and precedence

- Ordinary public requests select `AutonomousApproach`, or `RemoteActivation`
  for a Remote control Agent. Acceptance alone never moves the Agent.
- Traversal range-entry checks and anticipatory remote activation use
  `RemoteActivation`. Actual traversal remains under the resource's authority.
- Pending capability edits, queue allocation, and press execution retain their
  existing capability rechecks. Player work additionally uses `ImmediatePress`
  request eligibility at allocation and execution; Player input also selects only
  currently reachable Arms controls. Walking away cancels unpressed work rather
  than chasing the control. An ordinary public request made for a Player retains
  its historical initial acceptance behavior; the later Player rechecks still
  prevent approach.
- Access panel activation retains its final immediate-reach check. Its separate
  action/state validation and the request query's Access panel preflight retain
  their original order.
- The special physical press-in-passing seam retains its existing immediate
  activation, movement-state handling, authorization order, and pending-request
  behavior. It is not reinterpreted as an ordinary queued request. Owned
  request-reach controls and Remote control continue through ordinary requests.
- Ineligibility returns an empty handle before ordinary permission rejection.
  Eligible unauthorized requests still create a Rejected record. Pending-request
  coalescing, operation creation, queue insertion, press duration, event order,
  cancellation and record retention are unchanged.

## Characterization

The simulation check `typedLightingInteractionCoalescesAndCancelsByRequester`
includes a public World request matrix for Arms/Remote control/None, short/long
range, idle/planning/moving Agents, and protected/unprotected controls. It checks
acceptance versus recorded rejection, request-time positions, diagnostics and
construction event order. Existing Agent Object usage checks exercise owned
controls and exact remote range; `playerControlButtons`,
`playerControlBoothWindows`, and `playerControlAccessPanels` cover Player reach
and no-chase behavior. `markerActions/furnitureFinishDevice` covers reachable and
unreachable device effects at staged departure, including ordinary refusal,
occupancy release, logging and successful device completion.
