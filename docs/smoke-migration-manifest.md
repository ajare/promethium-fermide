# Smoke migration ownership manifest

Final Linux ownership snapshot after #305/#306 (parent #278). Each current
smoke-check source below has exactly one compilation/execution owner; all
assertions inside it belong to that owner. `module-owned` names an independent
smoke target and `retained standalone` names one of the four pre-existing direct
checks. The compatibility executable owns no smoke checks. Historical migration
sections retain their contemporary wording, including transitional `legacy-owned`
and “returns 2” descriptions; those are not current invocations. Future changes
must update the authoritative table and remove the old source **and invocation**
when transferring ownership.

## Current exactly-once ownership (#306)

This table is the authoritative current inventory. The `smoke-ownership-audit`
CTest compares it with every check translation unit and its explicit CMake owner;
configuration fails on duplicate module source ownership, and the audit fails on
any missing, stale, duplicate, or mismatched manifest row. Runner, state, harness,
support, tool, compatibility, and compile-only sources are listed separately below
because they do not own smoke-check execution.

<!-- current-ownership-begin -->
| Current check source | Ownership | Target |
| --- | --- | --- |
| `OccupantPackingChecks.cpp` | retained standalone | `pf-occupant-packing-checks` |
| `SectorTilesetChecks.cpp` | retained standalone | `pf-sector-tileset-checks` |
| `SimulationStepTimingChecks.cpp` | retained standalone | `pf-simulation-step-timing-checks` |
| `WorldRenderSlotChecks.cpp` | retained standalone | `pf-world-render-slot-checks` |
| `smoke/agent/Activation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/ActivationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Colour.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/ColourEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Group.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupAssignment.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupAssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupClipboardEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupCount.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupCountEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupDelete.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupDeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupIdAllocation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupIdAllocationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupTopology.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupTopologyEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Height.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/HeightEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Identity.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/IndividualProperties.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/WalkSpeed.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/WalkSpeedEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/Assignment.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/AssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/DeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/PortabilityEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/Registry.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RegistryEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/RuntimeAuthorization.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeCallbacks.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeContainment.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeDeterminism.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeFailures.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeInstances.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeMovement.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimePreflight.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeScale.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeScheduling.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/SchemaReconciliationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/Workflow.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/WorkflowEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/Background.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/DoorPanel.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/Facade.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/History.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/Isolation.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/Palette.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/metrics/Metrics.cpp` | module-owned | `pf-smoke-metrics` |
| `smoke/permissions/Access.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/AccessEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/Adherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/AdherenceEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/DestinationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/Destinations.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/InteractionGeometry.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/InteractionMobility.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/LandingAdherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/LandingAdherenceEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/Locations.cpp` | static Location authorization (#273) | `pf-smoke-permissions` |
| `smoke/permissions/LocationLifecycle.cpp` | Location requirement lifecycle (#275) | `pf-smoke-permissions` |
| `smoke/permissions/LocationLifecycleEditor.cpp` | Location history and permission deletion UI (#275) | `pf-smoke-editor` |
| `smoke/permissions/LocationChanges.cpp` | changing Location authorization (#274) | `pf-smoke-permissions` |
| `smoke/permissions/LocationOccupancy.cpp` | unauthorized occupancy/placement (#276) | `pf-smoke-permissions` |
| `smoke/permissions/LocationPlacementEditor.cpp` | authored placement/drop/relocation (#276) | `pf-smoke-editor` |
| `smoke/permissions/LocationsEditor.cpp` | Location Selection/history (#273) | `pf-smoke-editor` |
| `smoke/permissions/Preflight.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/Refusal.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/persistence/AgentRestoration.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/BrokenDoors.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DeepLayerReplay.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DocumentPaths.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DocumentSaves.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DomainReplay.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DoorDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/Formats.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/Layers.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/LiftDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/MalformedInput.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/RecentDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/Restoration.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/ShuttleDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/TransactionalWrites.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/WorldDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/render/AgentPaths.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorButtons.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorOpenApart.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorOpenLeft.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorOpenRight.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DrawOrder.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/FacadeDrawOrder.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/Facades.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/Lifetime.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/SerializationRendering.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/Transports.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/ViewportCulling.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/ViewportDragScroll.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/ViewportZoom.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/Walls.cpp` | module-owned | `pf-smoke-render` |
| `smoke/routing/IsolatedSectors.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/LadderForceBridgeRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/LiftRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Mobility.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Movement.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PathSource.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Planning.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/PlanningTime.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningTimeEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/RestoredPaths.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/ShuttleRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/StairRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/ThresholdRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Workspace.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/simulation/BrokenDoors.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/BrokenBulkheadDoors.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/CrossingBands.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/DoorQueues.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Doors.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Interactions.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Observation.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Pause.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Scale.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Teardown.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Ticking.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Timing.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Traversal.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/startup/Startup.cpp` | module-owned | `pf-smoke-startup` |
| `smoke/tags/Assignment.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/AssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/ClipboardEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/CoordinationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/DeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/DocumentSaveEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/MobilityProfile.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/Reconciliation.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/Registry.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/RegistryChangeEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/RegistryEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/ReloadEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/transports/Boarding.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Deletion.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/DoorQueries.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Escalators.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/EscalatorsEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/transports/ForceBridges.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Ladders.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/LayerJourney.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Lifts.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Occupants.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/PlatformLifts.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Shuttles.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Stairs.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/world/BackgroundCascadeDelete.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPaint.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPlacement.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundSector.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/Facades.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/FloorsAndWalls.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/LayerDeletion.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/Layers.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/MarkerIdentity.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/ObjectEditing.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/ThresholdLayerOverlap.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/Topology.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/TwoSidedButtons.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowIntoBackground.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowLayers.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowMultiBackground.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/ZeroSizeLocations.cpp` | module-owned | `pf-smoke-world` |
| `smoke/editor/BrokenExtensibles.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/persistence/BrokenExtensibles.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/render/BrokenExtensibles.cpp` | module-owned | `pf-smoke-render` |
| `smoke/simulation/BrokenExtensibles.cpp` | module-owned | `pf-smoke-simulation` |
<!-- current-ownership-end -->
Simulation Observation was migrated in #280. Its stable registered check
is `simulation/observation`; its old path was
`src/headless/SimulationObservationSmokeChecks.cpp`. It is no longer compiled
or invoked by the legacy aggregate. CTest executes it only as `smoke-simulation`.
#281 migrates `WallRenderSmokeChecks.cpp` to `smoke/render/Walls.cpp`, registered
as `render/walls`. Neither the legacy aggregate nor `--render-checks` invokes
it anymore; its domain CTest owner is `smoke-render`. Agent Path rendering moved
in #291, Shuttle rendering in #292, and remaining Render checks in #294.

#284 migrates the core-only Layer, Sector, Background, Window, Facade,
zero-sized Location, and threshold-layer topology groups to `smoke/world`, with
stable registrations under `smoke-world`. The editor-dependent Background and
Facade panel checks migrated to Editor in #295;
renderer-dependent Facade checks migrated in #294. The legacy aggregate and `--render-checks` no longer
invoke the migrated core Window checks.

#285 migrates all eight Agent activation/Agent group sources into `smoke/agent`,
plus `worldOwnsTypedEntitiesAndInvalidatesHandles` and the Agent ID type assertion
from `SmokeScenario.cpp`. Each of the 101 former named checks is registered
individually, plus `identity`: 53 registrations in `pf-smoke-agent` (core) and
49 in `pf-smoke-agent-editor` (editor). Checks that assert panel labels, undo
history, or clipboard behavior belong to the editor tier, including scenarios
that combine those assertions with World API assertions. Core translation units
contain none of those dependencies. All old sources, declarations, and calls
were removed; neither tier calls the legacy aggregate.

#286 migrates the Colour, Walk speed, Height, and individual-property sources
into the existing Agent tiers as 18 individually selectable checks: 10 core
registrations and eight editor registrations. Registry, World, precedence, and
persistence assertions are core-owned unless a scenario also uses production
editor history, Selection-panel text, or real CPU-side rendering; those mixed
scenarios are editor-owned as coherent checks. The four old sources, declarations,
and calls were removed from the legacy aggregate.

#287 migrates all ten `AgentTag*SmokeChecks.cpp` sources into `smoke/tags`:
13 individually selectable core checks in `pf-smoke-agent-tags` and 38 Editor
checks in `pf-smoke-agent-tags-editor`. Registry and assignment sources are split
by dependency; coherent scenarios that assert panel eligibility, document history,
clipboard, reload, or coordinated save workflows belong to Editor. Every original
assertion is retained, including multi-World and external-registry assertions.
Temporary fixtures use distinct subdirectories of the invocation's unique Context
root. The legacy aggregate and `--coordinated-document-checks` no longer execute
any migrated tag checks. #288 also removes the remaining behaviour portability
owner from that legacy selection.

#288 migrates six Agent behaviour authoring sources into `smoke/behaviours`:
12 individually selectable core checks in `pf-smoke-behaviours` and 14 editor
checks in `pf-smoke-behaviours-editor`. Registry, assignment, and Workflow are
physically split by dependency. Portability, coordinated deletion, schema
reconciliation, and scenarios asserting document history or panels use the editor
tier. All 243 original assertion call sites are preserved. Temporary packages
live below Context-owned invocation roots; each fixture gets its own directory.
The old sources, declarations, and calls are removed. The now-empty
`coordinated-document-checks` CTest entry is removed; its legacy CLI selection
returns 2 with directions to the independent editor modules rather than silently
running unrelated checks.

#289 completes runtime ownership in `pf-smoke-behaviours`: the oversized
`AgentBehaviourRuntimeSmokeChecks.cpp` is split into ten responsibility-based
translation units below, registering all 26 original invocations independently
(38 core checks total). Route-loss versions are selectable as
`routeLossAndTopologyLifecycleV1` and `routeLossAndTopologyLifecycleV2`; all other
selectors retain the original scenario names. Determinism, callbacks, scheduling,
failure containment, and scale assertions are retained. Runtime package files use
Context-owned directories and bundled sources use Context fixture lookup. The old
source, declaration, aggregate call, and dedicated call are removed. The empty
`agent-behaviours` CTest entry is retired; `--agent-behaviour-checks` returns 2 with
directions to the independent modules.

#290 migrates all seven Access permission, Permission adherence, transport landing
adherence, Interaction point mobility/geometry, Door preflight, and threshold
refusal sources into `smoke/permissions`. There are 88 selectable core checks in
`pf-smoke-permissions` and nine Editor checks in `pf-smoke-permissions-editor`.
The former Access runner's calls into adherence suites are now explicit peer
registrations. Transport kinds and landing/destination change variants each have
stable selectors. Core journeys set tag properties through the registry API;
registry-history and Selection assertions are independent Editor peers for all
three transport kinds. Mixed destination authoring/history/persistence scenarios
remain coherent Editor checks. All 549 original assertion call sites are retained.
The old sources, declarations, calls, and `access-permissions` CTest entry are
removed; `--access-permission-checks` returns 2 with migration guidance. Inline Interaction scenarios subsequently migrate in #298 below; the
compile-only API contract remains legacy-owned for its separate follow-up ticket.

#291 migrates all seven Route planning, planning-time property, movement-command,
Mobility profile, restored Path, isolated-Sector pathing, and Agent Path rendering
sources. `pf-smoke-routing` owns 40 individual core registrations;
`pf-smoke-routing-editor` owns nine coherent scenarios asserting clipboard,
Selection presentation, or document/registry history. `pf-smoke-render` owns
`agentPaths`, including Path lines and planning/queue badges. All 347 original
assertion call sites are retained; the Lift cancellation determinism assertion is
instantiated independently for each of its four original boundary values.
Restoration files live in per-check directories below the Context temporary root.
No check reads fixtures from the working directory. The old sources, declarations,
and invocations are removed, including the dedicated restored-Path invocation.
The two empty planning CTest entries are retired; `--route-planning-checks`,
`--route-planning-time-checks`, and `--restored-path-checks` return 2 with guidance.
Route-cost, workspace, and scale checks subsequently migrate in #293 below;
remaining inline checks migrate in #298 below.

#293 migrates the six workspace and perceived route-cost sources into 31
individual Routing registrations (71 core registrations in total). Context resolves
all checked-in World fixtures; no source-location or working-directory lookup
remains. Original workloads, reference oracles, Path digests, and all 241 assertion
call sites remain in their new owners, including explicit tool assertions.
The restoration benchmark and World export command now have standalone tool
entry points under `src/headless/tools/` (#303).
The population workload/assertions compile once in `pf-routing-population-support`,
shared by Routing and the export tool; smoke does not export, sample memory, or
report benchmark timings. Legacy aggregate calls and suite declarations are removed;
`--routing-scale-checks` and `--shuttle-route-checks` return 2 with migration guidance.

#292 migrates 26 inline Transit/transport scenarios from `SmokeScenario.cpp`
into Stairs, Occupants, PlatformLifts, Lifts, and Shuttles, retaining byte-identical
scenario bodies and their timing limits. Escalator walking, onboard Agent deletion
(including the climbing lease case), and Shuttle Door queries each retain their
named scenarios and assertions. `pf-smoke-transports` owns 41 individual checks;
the coherent Escalator property/history workflow belongs to the independently
buildable `pf-smoke-transports-editor`. The two Shuttle Door rendering scenarios
belong to Render (`carriageDoors`, `carriageImages`). The former standalone sources,
declarations, and aggregate/render invocations are removed.

Four full/reduced Lift boarding/crossing checks resolve the checked-in World via
Context. Their sole domain CTest owner is now `smoke-transports`; the two legacy
regression CTest entries are removed. `support/LiftBoarding.cpp` compiles shared
assertions once in `pf-lift-boarding-support`; the legacy file-driven reproduction
wrappers remain outside the module interface until their dedicated tool ticket.
Permission/adherence checks migrated in #290 and movement checks migrated in #291
keep those owners. Route costs remain legacy-owned for #293.

The legacy aggregate is not yet a compatibility orchestrator; converting it is
follow-up work. Other overlapping legacy CTest selections are unchanged.

Paths in the table are relative to `src/headless/`.

#294 completes Render with 78 stable registrations. Ten legacy sources move to
`smoke/render`: draw order (including Window/Background composition), three Door
opening styles, Facade rendering/order, viewport culling/drag/zoom, and render
lifetime. Named scenarios and Door width/open-state variants are individually
selectable; the four existing Wall, Agent Path and Shuttle selectors stay stable.
A new lifetime regression verifies scoped context restoration on success/failure.

Only `backButtonRendersAsOutlineOnly` moves from the mixed two-sided Door Button
source; core authoring, persistence and traversal checks remain legacy-owned.
All 366 original assertion call sites across affected sources remain in their
respective owners. Legacy Render/viewport declarations, calls and CTest entries
are removed; those CLI switches return 2 with migration guidance pending the
separate compatibility-dispatch ticket. Existing standalone Render-slot/tileset
executables and Editor-owned checks retain their owners.

#295 consolidates all six extracted Editor tiers into `pf-smoke-editor` and
migrates the remaining Background, Facade, Door panel, palette and Document
history checks. There are 164 explicit registrations, including a new shared-state
isolation regression. Earlier migration descriptions above record the former tiers; the table below
is their historical migration mapping. The #306 table at the top is the current
ownership authority. Selectors now prefix
scenario names with `agent/`, `tags/`, `behaviours/`, `permissions/`, `routing/`,
`transports/`, `background/`, `facade/`, `doorpanel/`, `palette/`, or `history/`.
No migrated Editor source or invocation remains in the legacy aggregate.
All Editor checks share scoped CPU ImGui and UI-state cleanup. Core module
source lists and dependency tiers are unchanged.

| Check source | Ownership | Target |
| --- | --- | --- |
| `smoke/editor/Isolation.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/Access.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/Destinations.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/DestinationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/AccessEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Identity.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/Activation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/ActivationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/Registry.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/Assignment.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/Workflow.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RegistryEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/AssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/PortabilityEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/DeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/SchemaReconciliationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/WorkflowEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/behaviours/RuntimePreflight.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeContainment.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeInstances.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeMovement.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeCallbacks.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeScheduling.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeFailures.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeDeterminism.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeScale.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RuntimeAuthorization.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/agent/Colour.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/ColourEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupAssignment.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupAssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupClipboardEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupCount.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupCountEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupDelete.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupDeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupIdAllocation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupIdAllocationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Group.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/GroupTopology.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupTopologyEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/Height.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/HeightEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/IndividualProperties.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/render/AgentPaths.cpp` | module-owned | `pf-smoke-render` |
| `smoke/tags/Assignment.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/AssignmentEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/ClipboardEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/CoordinationEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/DeleteEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/DocumentSaveEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/MobilityProfile.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/Reconciliation.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/RegistryChangeEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/Registry.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/RegistryEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/tags/ReloadEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/agent/WalkSpeed.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/WalkSpeedEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/world/BackgroundCascadeDelete.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPaint.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPlacement.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundSector.cpp` | module-owned | `pf-smoke-world` |
| `smoke/editor/Background.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/editor/History.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/render/DoorOpenApart.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorOpenLeft.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/DoorOpenRight.cpp` | module-owned | `pf-smoke-render` |
| `smoke/editor/DoorPanel.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/permissions/Preflight.cpp` | module-owned | `pf-smoke-permissions` |
| `DoorTwoSidedButtonSmokeChecks.cpp` (core only) | `world/TwoSidedButtons.cpp` (#305) | `pf-smoke-world` |
| `smoke/render/DoorButtons.cpp` | module-owned | `pf-smoke-render` |
| `smoke/world/Layers.cpp` | module-owned | `pf-smoke-world` |
| `smoke/transports/Escalators.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/EscalatorsEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/transports/Stairs.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Occupants.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/PlatformLifts.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Lifts.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/Shuttles.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/render/FacadeDrawOrder.cpp` | module-owned | `pf-smoke-render` |
| `smoke/editor/Facade.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/render/Facades.cpp` | module-owned | `pf-smoke-render` |
| `smoke/world/Facades.cpp` | module-owned | `pf-smoke-world` |
| `smoke/startup/Startup.cpp` | module-owned | `pf-smoke-startup` (GUI-enabled builds) |
| `smoke/permissions/InteractionMobility.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/InteractionGeometry.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/routing/IsolatedSectors.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/LadderForceBridgeRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/transports/Boarding.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/routing/LiftRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `MarkerIdentitySmokeChecks.cpp` | `world/MarkerIdentity.cpp` (#305) | `pf-smoke-world` |
| `smoke/metrics/Metrics.cpp` | module-owned | `pf-smoke-metrics` |
| `smoke/routing/Mobility.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Movement.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/simulation/Timing.cpp` | module-owned | `pf-smoke-simulation` |
| `OccupantPackingChecks.cpp` | module-owned (retained standalone) | `pf-occupant-packing-checks` |
| `smoke/transports/Deletion.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/editor/Palette.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/Workspace.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/simulation/Pause.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/permissions/Adherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/AdherenceEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/render/DrawOrder.cpp` | module-owned | `pf-smoke-render` |
| `smoke/routing/RestoredPaths.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Planning.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/PlanningTime.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningTimeEditor.cpp` | module-owned | `pf-smoke-editor` |
| `SectorTilesetChecks.cpp` | module-owned (retained standalone) | `pf-sector-tileset-checks` |
| `SerializationSmokeChecks.cpp` | removed; all scenarios mapped under #282/#296/#297 below | — |
| `smoke/persistence/{WorldDocuments,AgentRestoration,Layers,DomainReplay,DoorDocuments,LiftDocuments,ShuttleDocuments,DeepLayerReplay,Restoration}.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/render/SerializationRendering.cpp` | module-owned | `pf-smoke-render` |
| `smoke/persistence/Formats.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/TransactionalWrites.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DocumentPaths.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/DocumentSaves.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/MalformedInput.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/persistence/RecentDocuments.cpp` | module-owned | `pf-smoke-persistence` |
| `smoke/transports/DoorQueries.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/render/Transports.cpp` | module-owned | `pf-smoke-render` |
| `smoke/routing/ShuttleRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `SimulationStepTimingChecks.cpp` | module-owned (retained standalone) | `pf-simulation-step-timing-checks` |
| `smoke/routing/StairRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/world/ThresholdLayerOverlap.cpp` | module-owned | `pf-smoke-world` |
| `smoke/permissions/Refusal.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/routing/ThresholdRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/permissions/LandingAdherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/LandingAdherenceEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/render/ViewportCulling.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/ViewportDragScroll.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/ViewportZoom.cpp` | module-owned | `pf-smoke-render` |
| `smoke/render/Walls.cpp` | module-owned | `pf-smoke-render` |
| `smoke/world/WindowIntoBackground.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowLayers.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowMultiBackground.cpp` | module-owned | `pf-smoke-world` |
| `smoke/render/Lifetime.cpp` | module-owned | `pf-smoke-render` |
| `WorldRenderSlotChecks.cpp` | module-owned (retained standalone) | `pf-world-render-slot-checks` |
| `smoke/simulation/Teardown.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/world/ZeroSizeLocations.cpp` | module-owned | `pf-smoke-world` |
| `SmokeScenario.cpp` | dispatch/platform helpers only; no product scenarios | `prometheum-fermide-headless` |
| `smoke/world/ObjectEditing.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/FloorsAndWalls.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/Topology.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/LayerDeletion.cpp` | module-owned | `pf-smoke-world` |
| `smoke/routing/PathSource.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/simulation/Ticking.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Traversal.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Interactions.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Doors.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/DoorQueues.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/CrossingBands.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/simulation/Scale.cpp` | module-owned | `pf-smoke-simulation` |
| `smoke/transports/Ladders.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/ForceBridges.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/transports/LayerJourney.cpp` | module-owned | `pf-smoke-transports` |
| `InteractionApiCompileCheck.cpp` (compile-only contract) | compile-contract-owned | `pf-interaction-api-compile-contract` (`pf-compile-contracts`) |
| `smoke/simulation/Observation.cpp` | module-owned | `pf-smoke-simulation` |

## Persistence pilot extraction (#282)

The following functions moved out of `SerializationSmokeChecks.cpp` into
`smoke/persistence/Formats.cpp`; their definitions and legacy runner calls were
removed together. Existing assertions are unchanged; only temporary-path setup,
cleanup ownership, function signatures, and the assertion helper changed.

| Original function | Persistence check |
| --- | --- |
| `stringYamlRoundTripsPrimitiveValues` | `yaml-primitives` |
| `binarySerializerHonoursTheSerializerContract` | `binary-contract` |
| `fileYamlRoundTrips` | `yaml-file` |
| `transactionalWriterPreservesOpaqueBytes` | `transactional-bytes` |
| `worldDocumentsUseTheirExactSuffixFormat` | `world-document-formats` |
| `malformedValuesAndInvalidUsageThrowUsefulErrors` | `yaml-errors` |

`checked-in-world` is additional fixture-resolution coverage using
`resources/Office.world.yaml`. #296 further splits these pilot groups as described
below. #297 completes the remaining World restoration, compatibility and domain
migration; neither legacy entry point executes migrated checks.

## Persistence infrastructure extraction (#296)

Ten more functions leave `SerializationSmokeChecks.cpp`, including their legacy
runner calls. All 43 original `require` call sites remain; recent-document path
expectations now use the Context root instead of literal `/tmp` filenames.

| Original function | Persistence check | Source |
| --- | --- | --- |
| `lateWriteFailurePreservesThePreviousSaveFile` | `transactional-late-failure` | `TransactionalWrites.cpp` |
| `saveNeverTouchesAPredictableTemporaryPath` | `transactional-predictable-path` | `TransactionalWrites.cpp` |
| `saveThroughSymlinkUpdatesItsTarget` | `transactional-symlink-target` | `TransactionalWrites.cpp` |
| `saveNeverFollowsASymlinkedTemporaryPath` | `transactional-symlink-temp` | `TransactionalWrites.cpp` |
| `savePreservesExistingFilePermissions` | `transactional-permissions` | `TransactionalWrites.cpp` |
| `concurrentSavesCommitOnlyCompleteDocuments` | `transactional-concurrent` | `TransactionalWrites.cpp` |
| `failedSavePreservesUnsavedChangesState` | `save-dirty-state` | `DocumentSaves.cpp` |
| `serializableTracksModificationState` | `serializable-modification-state` | `DocumentSaves.cpp` |
| `recentFilesPersistAcrossStartup` | `recent-documents-restart` | `RecentDocuments.cpp` |
| `missingRecentFilesCanBeRemovedPersistently` | `recent-documents-missing` | `RecentDocuments.cpp` |

The existing pilot selectors remain, without duplicate registrations:
`transactional-bytes` moves to `TransactionalWrites.cpp`, `yaml-errors` to
`MalformedInput.cpp`, and `world-document-formats` to `DocumentSaves.cpp`.
The latter's suffix, Save As and base-path assertions are extracted once into
`DocumentPaths.cpp` as `document-paths`. Primitive YAML, binary contract (including
invalid binary envelopes/types), YAML file and fixture checks stay in `Formats.cpp`.
This gives 18 selectable checks. The three POSIX-only symlink/permission checks
remain listed on Windows and report an explicit capability skip there.

All writes and recent-document references use Context-owned absolute temporary
paths; fixture reads use `Context::fixture()`. No check changes the working
directory or opens a UI/dialog. `WriteFailure.h` scopes fault-injection reset even
when a check throws. `RecentFiles` is reused from the existing production core;
no editor dependency or duplicate production compilation is introduced.

Verification: full Release build, 81 CTest entries sequentially and at `-j 8`,
fresh GUI-disabled Persistence-only build, and Debug/high-analysis Persistence
and affected legacy tests pass. The contract selects every check from an empty
external directory and runs eight full invocations concurrently. Source comparison
retains all 734 `require` call sites across the original legacy suite and pilot
(normalizing only recent-document paths), with one definition and registration
for each of the ten transferred functions. #297's World/domain migration is not
part of this change.

## Complete World-document Persistence (#297)

All 63 remaining legacy serialization functions are removed with their runner.
The following 61 core scenarios keep their function names as Persistence selectors.
Sources below are under `smoke/persistence`; together with the #282/#296 tables,
this accounts for every former serialization scenario once.

| Original function / Persistence selector | Source |
| --- | --- |
| `worldRoundTripsAuthoredStateAndAgents` | `WorldDocuments.cpp` |
| `legacyWorldYamlStillLoads` | `WorldDocuments.cpp` |
| `legacyVersion3WorldYamlStillLoadsWithDefaultLayers` | `WorldDocuments.cpp` |
| `version4WorldYamlStillLoads` | `WorldDocuments.cpp` |
| `layerFieldsAcceptLegacyNamesAndIndices` | `WorldDocuments.cpp` |
| `agentRestoreRejectsMalformedPositions` | `AgentRestoration.cpp` |
| `agentRestoreRejectsBackgroundAndUnreachableDestination` | `AgentRestoration.cpp` |
| `worldLayerNamesRoundTrip` | `Layers.cpp` |
| `addedLayersAppendToTheBackAndRoundTrip` | `Layers.cpp` |
| `layerCountIsCappedAtCoreMaxLayers` | `Layers.cpp` |
| `oversizedWorldDimensionsAreRefusedBeforeCellAccess` | `Layers.cpp` |
| `levelsHaveNamesLimitsAndCascadingDeletion` | `Layers.cpp` |
| `deletingAMiddleLayerCompactsTheLayersAboveIt` | `Layers.cpp` |
| `deletingTheFrontLayerRemovesTransitsOneLayerBehind` | `Layers.cpp` |
| `layerDeletionPreservesAuthoredRecordDependencies` | `Layers.cpp` |
| `layerDeletionKeepsAtLeastTwoLayers` | `Layers.cpp` |
| `locationEditsArePlannedAndAppliedAtomically` | `DomainReplay.cpp` |
| `editedShuttleRoundTripsWithoutSchemaChanges` | `DomainReplay.cpp` |
| `physicalControlsPreferDistinctWallPositions` | `DomainReplay.cpp` |
| `platformLiftStopDurationRoundTrips` | `DomainReplay.cpp` |
| `enclosedLiftsSupportMultiLevelRooms` | `DomainReplay.cpp` |
| `stopDerivingAddLiftRejectsInvalidLayerIndex` | `DomainReplay.cpp` |
| `stairwellSectorsAreCanvasSelectable` | `DomainReplay.cpp` |
| `staircasesConnectAdjacentCorridorsAndRoundTrip` | `DomainReplay.cpp` |
| `laddersCanBeValidatedEditedAndDeleted` | `DomainReplay.cpp` |
| `stairwellsCanBeValidatedEditedAndDeleted` | `DomainReplay.cpp` |
| `stairwellEditsReplayLocationsBeforeTransits` | `DomainReplay.cpp` |
| `stairwellEditsReplayWalkwaysBeforeTransits` | `DomainReplay.cpp` |
| `ladderEditsReplayLocationsBeforeTransits` | `DomainReplay.cpp` |
| `staircaseEditsReplayLocationsBeforeTransits` | `DomainReplay.cpp` |
| `bulkheadDoorsSupportIndependentObjectEditing` | `DoorDocuments.cpp` |
| `doorOpeningStyleIsAuthoredPersistedAndLegacyDefaulted` | `DoorDocuments.cpp` |
| `doorHeightPersistsAndIsLimitedToRooms` | `DoorDocuments.cpp` |
| `doorOpenLeftPersistsThroughEveryEditorPath` | `DoorDocuments.cpp` |
| `doorOpenRightPersistsThroughEveryEditorPath` | `DoorDocuments.cpp` |
| `doorOpenApartPersistsThroughEveryEditorPath` | `DoorDocuments.cpp` |
| `liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp` | `DoorDocuments.cpp` |
| `doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads` | `DoorDocuments.cpp` |
| `liftStopDoorStyleOverridesArePerStopAndPersist` | `LiftDocuments.cpp` |
| `liftCreationStopDoorStylesAreAuthoredAndPersist` | `LiftDocuments.cpp` |
| `liftShortStopDoorStyleVectorEditPreservesEarlierOverrides` | `LiftDocuments.cpp` |
| `liftCarKeepsItsShaftRelativeLevelWhenExtendedDownward` | `LiftDocuments.cpp` |
| `liftDoorStylesFollowStopsWhenTheLiftMovesOrResizes` | `LiftDocuments.cpp` |
| `liftDoorStylesReconcileWhenStopsChange` | `LiftDocuments.cpp` |
| `shuttleDoorStyleOverridesAreIndividualAndPersist` | `ShuttleDocuments.cpp` |
| `shuttleDoorStylesSurviveShuttleMovement` | `ShuttleDocuments.cpp` |
| `shuttleDoorStylesReconcileWhenStopsChange` | `ShuttleDocuments.cpp` |
| `shuttleDoorStylesReconcileWhenCarriageAndDoorLayoutChanges` | `ShuttleDocuments.cpp` |
| `shuttleVehicleEditsRejectZeroValuedFields` | `ShuttleDocuments.cpp` |
| `layerHelperApiIsConsistentWithLayerCount` | `DeepLayerReplay.cpp` |
| `graphConstructionWalksEveryAdjacentLayerPair` | `DeepLayerReplay.cpp` |
| `thresholdsAndTransitsPairTheirOwnAdjacentLayerPair` | `DeepLayerReplay.cpp` |
| `doorAndWindowRemovalWorksOnDeepLayerPairs` | `DeepLayerReplay.cpp` |
| `shuttleDoorCandidatesAreFoundOnTheShuttleLayer` | `DeepLayerReplay.cpp` |
| `candidateReplayIncludesAllLayers` | `DeepLayerReplay.cpp` |
| `liftEditsUseTheLiftsOwnLayer` | `DeepLayerReplay.cpp` |
| `shuttleEditsUseTheShuttlesOwnLayer` | `DeepLayerReplay.cpp` |
| `shuttleDeletionRemovesWindowsOverTheShuttleItself` | `DeepLayerReplay.cpp` |
| `ladderEditsUseTheLaddersOwnLayer` | `DeepLayerReplay.cpp` |
| `stairwellEditsUseTheStairwellsOwnLayer` | `DeepLayerReplay.cpp` |
| `staircaseEditsReturnTheStaircaseOwnLayer` | `DeepLayerReplay.cpp` |

Three rows have explicitly split ownership, with no assertion duplication:
`stairwellSectorsAreCanvasSelectable`,
`staircasesConnectAdjacentCorridorsAndRoundTrip`, and
`laddersCanBeValidatedEditedAndDeleted` retain their core geometry/replay assertions
above. Their leading canvas/render-policy assertions live in
`smoke/render/SerializationRendering.cpp`, registered under the original name
plus `Rendering`. The two entirely rendering-only former scenarios
`onlyTheSelectedLayerIsDrawn` and
`transitsOnTheLayerBehindAreOnlyDrawnThroughApertures` live in that same Render
source under their original selectors. This preserves all 646 legacy assertion
call sites exactly once while keeping Persistence free of editor/render headers.

The former `restoration-checks` CTest workload (five cycles) is now
`persistence/restorationPreservesStatePathsAndLifetimes`, registered through
`smoke/persistence/Restoration.cpp`. Its nine assertions compile once in
`support/Restoration.cpp`, retaining public document-load/reset, registry,
snapshot, serialization, and weak-reference seams. The explicit benchmark in
`tools/RestorationBenchmark.cpp` reuses that workload with optional timing/memory output; it is
not separately registered as smoke coverage. The `serialization-checks` and
`restoration-checks` CTest entries are removed, and `--serialization-checks`
returns 2 with migration guidance rather than silently succeeding.

Coordinated documents remain exclusively in their already-migrated Editor sources
(`tags/CoordinationEditor.cpp`, registry-change/history/save workflows,
and Behaviour portability/reconciliation). #297 does not duplicate or replace
those public API checks. See #287/#288 and the Editor ownership entries above.
Persistence has 80 registrations; Render has 83. Both contract tests enumerate
every selector and run eight concurrent full invocations from an empty external
working directory. The complete Release CTest run also verifies the unchanged
coordinated-document checks.

## Central scenario decomposition (#298)

All 56 remaining product scenarios leave `SmokeScenario.cpp`, with 60 explicit
registrations in their owning modules. Source paths below are relative to
`smoke/`. A selector is the original function name unless variants are listed.
Every original scenario is mapped exactly once here; helpers are not extra checks.

World owns structural authoring, Floor/Wall edits, topology rebuild and Layer
deletion/compaction. Routing owns inferred Path-source selection. Transports owns
Ladder and Force Bridge authoring/admission/leases and the three-Layer Lift journey.
Simulation owns fixed ticks, phases, ordinary traversal, Interaction outcomes,
Door/Bulkhead/Window coordination, queue and crossing-band behavior, and scale.
Determinism assertions stay with the scenario whose outcome they compare, rather
than becoming a central scenario library. Existing Persistence scenarios and
serialization/replay assertions retain their owners unchanged.

| Original scenario | Owner/source | Selector variants (otherwise original name) |
| --- | --- | --- |
| `markerPlacementEnforcesPaletteCoreRules` | `world/ObjectEditing.cpp` (`pf-smoke-world`) | — |
| `corridorDoorPlacementEnforcesPaletteRules` | `world/ObjectEditing.cpp` (`pf-smoke-world`) | — |
| `objectMoveValidatesAndRebuildsOnceCommitted` | `world/ObjectEditing.cpp` (`pf-smoke-world`) | — |
| `windowResizeUsesWindowPlacementRules` | `world/ObjectEditing.cpp` (`pf-smoke-world`) | — |
| `doorResizeRespectsDoorPlacementRules` | `world/ObjectEditing.cpp` (`pf-smoke-world`) | — |
| `sharedLocationWallsCanBeOpenedAndRestored` | `world/FloorsAndWalls.cpp` (`pf-smoke-world`) | — |
| `walkwayEditingEnforcesPlacementMovementAndOccupancyRules` | `world/FloorsAndWalls.cpp` (`pf-smoke-world`) | — |
| `deletingWalkwayPreservesUnrelatedRoomDoor` | `world/FloorsAndWalls.cpp` (`pf-smoke-world`) | — |
| `pausedTopologyRebuildIsAtomicAndCleansOwnership` | `world/Topology.cpp` (`pf-smoke-world`) | — |
| `traversalGeometryPolicyIsWorldOwned` | `world/Topology.cpp` (`pf-smoke-world`) | — |
| `runMiddleLayerDeletion` | `world/LayerDeletion.cpp` (`pf-smoke-world`) | — |
| `inferredPathSourceDoesNotMakeAgentDoubleBack` | `routing/PathSource.cpp` (`pf-smoke-routing`) | — |
| `accumulatedRenderTimeAdvancesWholeTicksOnly` | `simulation/Ticking.cpp` (`pf-smoke-simulation`) | — |
| `runOrdinaryPathScenario` | `simulation/Ticking.cpp` (`pf-smoke-simulation`) | — |
| `ordinaryTraversalCommitsOnlyAtDestination` | `simulation/Traversal.cpp` (`pf-smoke-simulation`) | — |
| `deniedTraversalCannotBeCrossed` | `simulation/Traversal.cpp` (`pf-smoke-simulation`) | — |
| `cancellationReleasesPermitWithoutCommitting` | `simulation/Traversal.cpp` (`pf-smoke-simulation`) | — |
| `typedLightingInteractionCoalescesAndCancelsByRequester` | `simulation/Interactions.cpp` (`pf-smoke-simulation`) | — |
| `repeatedInteractionsRetireTerminalRecords` | `simulation/Interactions.cpp` (`pf-smoke-simulation`) | — |
| `interactionBindingAggregationIsMeaningful` | `simulation/Interactions.cpp` (`pf-smoke-simulation`) | — |
| `singleAgentDoorJourney` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | `singleAgentDoorJourneyManual`, `singleAgentDoorJourneyAutomatic` |
| `automaticBulkheadSensesNearbyNonTraveller` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `bulkheadAndWindowThresholdsUseTraversalResources` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `agentsPressUpcomingDoorButtonsWhilePassing` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `remoteDoorUsesOnePhysicalOperatorAndSharedOperation` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `remoteDoorWithoutReachableControlIsUnavailable` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `wideDoorLanesAndGracefulDisableAreSafe` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `doorLeasesAndSensorObservationsPreventUnsafeClosure` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `unavailableDoorRejectsTraversal` | `simulation/Doors.cpp` (`pf-smoke-simulation`) | — |
| `fairDoorQueuesServeBothSidesInStableOrder` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `queueChainsFollowWithoutCompressing` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | `queueChainsFollowWithoutCompressingDefaultLeft`, `queueChainsFollowWithoutCompressingDefaultRight`, `queueChainsFollowWithoutCompressingWideLeft`, `queueChainsFollowWithoutCompressingWideRight` |
| `overflowingQueueAlwaysHasWalkableTailTargets` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `queuePositionsPreferObjectProximityThenAgentProximity` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `doorQueueRequestsBeforeOccupiedTail` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `queuedCancellationReleasesAndAdvancesPositions` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `resilientWaitingRetainsPriorityAndExpiresPermits` | `simulation/DoorQueues.cpp` (`pf-smoke-simulation`) | — |
| `doorCrossingBandPredicateShape` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `doorVertexCarriesCrossingWidth` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `crossingWidthGrantsHeadOfQueueBeforeCentre` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `narrowDoorBandArrivalGrantsAtCentreTolerance` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `bandArrivalCrossesWideDoorFromStandingPosition` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `bandArrivalComposesWithEarlyStopForContendedDoor` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `bandArrivalLeavesNonCrossingAgentsUnaffected` | `simulation/CrossingBands.cpp` (`pf-smoke-simulation`) | — |
| `runScaledWorld` | `simulation/Scale.cpp` (`pf-smoke-simulation`) | — |
| `roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `ladderQueuePositionsPreferAgentApproachSide` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `ladderAdmissionsMaintainPhysicalSpacing` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `extensibleLadderUsesDesiredStateAndLeases` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `directionalLadderBoundsBatchesAndPreventsOpposingAdmission` | `transports/Ladders.cpp` (`pf-smoke-transports`) | — |
| `forceBridgeObjectEditingIsAtomic` | `transports/ForceBridges.cpp` (`pf-smoke-transports`) | — |
| `forceBridgeWalkwayDeletionUpdatesItsDestination` | `transports/ForceBridges.cpp` (`pf-smoke-transports`) | — |
| `forceBridgePreparationUsesNearControl` | `transports/ForceBridges.cpp` (`pf-smoke-transports`) | — |
| `extendedForceBridgeAllowsConcurrentTwoWayTraffic` | `transports/ForceBridges.cpp` (`pf-smoke-transports`) | — |
| `extensibleForceBridgeCompletesThroughPhysicalControl` | `transports/ForceBridges.cpp` (`pf-smoke-transports`) | — |
| `runThreeLayerTransitJourney` | `transports/LayerJourney.cpp` (`pf-smoke-transports`) | — |

The queue-chain variants retain both separations (default and 0.8) and both
approach directions, each with repeated trace comparison. A failed queue assertion
now throws through the harness instead of returning `false` (exit status zero)
from the old `main`. Each check boundary reports failure and continues its module.
The 500-Agent/32-resource/60-tick workload still compares metrics-disabled and
metrics-enabled event/snapshot digests, followed by the 1,000-Agent stretch case.
Only informational wall-clock/working-set sampling and printed digest reports are
removed; no elapsed-time threshold or product assertion changes.

`support/PathFixture.h` constructs the same two-node Path for World, Simulation
and Transports; `support/SimulationTrace.h` retains the exact snapshot/event
canonicalization used by Layer deletion, the deep Transit journey and ordinary
Path determinism. These are narrow shared mechanics, not scenario registrations
or World builders. `simulation/InteractionResults.h` locally shares terminal-event
observation between Interaction and Door safety checks. Builders, assertions and
scenario-specific trace types remain in their owning translation units.

World now has 22 checks, Routing 72, Transports 53, and Simulation 37.
Their public CLI contracts enumerate every selector and run each from an empty
external directory; Simulation also gains eight concurrent full invocations.
The central runner retains only dispatch to unmigrated suites/tools and platform
memory/process helpers. It no longer defines or invokes any extracted scenario;
compatibility orchestration, tool separation, remaining standalone source
migrations and compile-only target work are deliberately not part of #298.

## Complete Simulation lifecycle migration (#299)

The three remaining legacy Simulation suites move into the core-only
`pf-smoke-simulation` module as 18 explicit registrations. Together with the 37
existing checks, Simulation now owns 55 stable selectors.

| Former legacy group | Module source | Selectable checks |
| --- | --- | ---: |
| Pause position | `smoke/simulation/Pause.cpp` | 5 |
| Non-finite and boundary timing | `smoke/simulation/Timing.cpp` | 9 |
| World teardown and topology replacement | `smoke/simulation/Teardown.cpp` | 4 |

Pause registers Path clearing, both ordinary walking states, Staircase traversal,
and Stairwell traversal independently. Timing registers each Door/transport/API/
replay validation family and both valid-boundary groups independently. Teardown
registers built and unbuilt World destruction, Door threshold ownership, and stale
Graph replacement independently. All original assertions remain in their moved
or shared bodies; the stable `observation` group and existing repeated-run
Simulation determinism assertions remain owned by the same module.

`NonFiniteTimingSmokeChecks.cpp`, `WorldTeardownSmokeChecks.cpp`, and their legacy
aggregate calls are removed. Pause-position assertions compile once in
`support/PausePosition.cpp`; the five Simulation registrations are their sole
smoke owner, while the explicit legacy reproduction command reuses the support
without becoming another smoke execution path. The old teardown CLI selection
returns migration guidance. `SimulationStepTimingChecks.cpp` remains an independent standalone check for
#302 and is not consolidated solely for naming consistency. Along with
`OccupantPackingChecks.cpp`, `WorldRenderSlotChecks.cpp`, and
`SectorTilesetChecks.cpp`, it keeps a direct executable target while receiving the
shared C++20, warning, elevated-analysis, timeout, and labelled CTest policy.

The exact 55-selector contract executes every check independently and performs
eight simultaneous complete invocations from an empty external directory.
Simulation links only smoke support, pause support, production core, YAML, and Lua;
there are no Editor, renderer, ImGui, HTTP, graphics, dialog, or interactive-input
dependencies.

## Metrics smoke module (#300)

The formatting, endpoint, concurrent scrape, lifecycle, resource-label and
cardinality assertions from `MetricsChecks.cpp` move together to
`smoke/metrics/Metrics.cpp`, registered as `metrics` in `pf-smoke-metrics`.
Their assertion expressions and observable HTTP expectations are unchanged.
The module is the sole smoke owner and links HTTP support explicitly; the old
aggregate source, call, and `metrics-smoke` CTest entry are removed.

`smoke-metrics-contract` verifies listing, focused selection, misuse, external
working-directory cleanliness, and eight concurrent runs using ephemeral
loopback ports. CTest owns direct execution through `smoke-metrics`, labelled
`smoke;metrics;http`, with a 30-second timeout. The metrics service now lives in
`tools/MetricsServer.cpp`, built independently as `pf-metrics-server` (#303);
it is not a smoke registration.

## Graphics Startup smoke module (#301)

The graphics-initialization subprocess check moves from
`GraphicsStartupSmokeChecks.cpp` to `smoke/startup/Startup.cpp`, registered as
`graphicsInitializationFailure` in `pf-smoke-startup`. The module launches the
required `editor` product with an unavailable SDL video driver and accepts only a
normal non-zero exit. A signal, Windows abort/crash code, timeout, launch failure,
zero exit, or missing/non-executable child product fails the check; required
products never become skips.

The target and its two CTest entries exist only with `PF_BUILD_GUI=ON`, preventing
impossible registration where the optional GUI capability is disabled. The target
depends on `editor`, embeds its built path for direct execution, and permits an
explicit `PF_GUI_EXECUTABLE` override for the missing-product contract. Both tests
are labelled for GUI graphics subprocess behavior and use `RUN_SERIAL` because the
child writes the production startup log. The old `graphics-startup-failure` entry
and aggregate source ownership are removed; `--graphics-startup-smoke` returns 2
with migration guidance.

## Support and non-smoke code

- `smoke/editor/Main.cpp`: the sole cross-domain Editor registry. The former
  domain `EditorMain.cpp` files are removed. `editor/State.cpp` scopes shared UI
  state and CPU ImGui; domain `EditorState.h` files alias that common boundary.
- `smoke/tests/EditorContract.cmake`: exact inventory, every selection, misuse,
  empty working directory and eight concurrent full invocations; owned by
  `smoke-editor-contract` (`harness;editor`).
- `smoke/{agent,tags,behaviours,permissions,routing}/Main.cpp`: core registries.
  `Checks.h` files declare registrations. Domain temporary-directory and runtime
  fixture helpers remain local, with Context-owned storage.
- Agent, Tags, Behaviours, Permissions and Routing contract scripts now test only
  their core runners (`harness;core`); Editor coverage has one contract owner.
- `src/DocumentEdit.cpp`, `src/AgentGroupsPanel.cpp`,
  `src/AgentGroupAssignmentPanel.cpp`, and `src/AgentClipboard.cpp`: production
  seams compiled once in `pf-agent-editing`, shared by the GUI, remaining legacy
  checks, and cross-domain Editor module. `src/DoorPanel.cpp` now also compiles
  in this shared library rather than separately in each consumer. Not smoke sources.

- `RenderGuiStubs.cpp` and `support/ImGuiContext.cpp`: reusable CPU-only support,
  compiled by `pf-headless-render-support`, not independent checks.
- `smoke/world/Main.cpp`: explicit World registry, not extra coverage.
- `smoke/tests/WorldContract.cmake`: World CLI and arbitrary-working-directory
  contract (`harness;core`, not smoke coverage).
- `smoke/persistence/Main.cpp`: explicit Persistence registry, not extra coverage.
- `smoke/tests/PersistenceContract.cmake`: Persistence CLI, external working
  directory, and concurrent invocation contract (`harness;core`, not smoke).
- `smoke/render/Main.cpp`: explicit Render registry, not extra coverage.
- `smoke/render/State.cpp` and `Checks.h`: module-local per-registration UI,
  selection, tileset and CPU ImGui isolation, not extra domain coverage.
- `smoke/tests/RenderContract.cmake`: public Render CLI contract and no-output-file
  verification, executed by `smoke-render-contract` (harness, not smoke coverage).
- `smoke/startup/Main.cpp` and `Checks.h`: the GUI-capability-gated Startup registry.
- `smoke/tests/StartupContract.cmake`: Startup CLI, controlled child failure, and
  required-child failure contract; owned by serialized `smoke-startup-contract`.
- `smoke/simulation/Main.cpp`: explicit Simulation registry, not extra coverage.
- `smoke/support/Smoke.cpp`: harness mechanics only, owned by `pf-smoke-support`.
- `smoke/tests/Probe.cpp`: synthetic harness contract checks, owned by
  `pf-smoke-harness-probe`; CTest owns their execution through
  `smoke-harness-contract`, not as domain smoke coverage.
- `smoke/transports/Main.cpp` and `Checks.h`: explicit core registry;
  `EscalatorFixture.h`: module-local fixture shared by core and editor tiers.
- `smoke/tests/TransportsContract.cmake`: exact registries, individual selection,
  CLI misuse, empty working directory, and eight concurrent core invocations;
  owned by `smoke-transports-contract` (`harness;core`).
- `support/LiftBoarding.cpp`: shared assertion helpers compiled once by
  `pf-lift-boarding-support`, not an additional CTest execution owner.
- `support/PausePosition.cpp`: shared pause-position assertion/reproduction
  mechanics compiled once by `pf-pause-position-support`. Simulation owns their
  only smoke registrations; `tools/PausePositionRepro.cpp` reuses the mechanics
  for the standalone minimal and file-backed diagnostic (#304).
- `support/LiftBoarding.cpp`: shared Lift assertion/reproduction mechanics
  compiled once by `pf-lift-boarding-support`. Transports owns the smoke
  registrations; `tools/LiftRepro.cpp` owns the standalone crossing and boarding
  diagnostic contract (#304). No Lift repro source is compiled by a smoke runner.
- `tools/RestorationBenchmark.cpp`, `tools/GenerateRoutingWorld.cpp`,
  `tools/MetricsServer.cpp`, `tools/LiftRepro.cpp`, and
  `tools/PausePositionRepro.cpp` own independent executables (#303/#304), with no
  smoke-check linkage. Legacy selections return 2 with migration guidance.
  `headless-tools-contract` verifies their [command contracts](headless-tools.md).
- `support/Restoration.cpp`: shared public load/reset workload and assertions,
  compiled once by `pf-restoration-support`. Persistence owns smoke execution;
  `tools/RestorationBenchmark.cpp` reuses it only for the explicit reporting benchmark.
- `support/RoutingPopulation.cpp`: shared deterministic population workload and
  assertions, compiled once by `pf-routing-population-support`. Only explicit
  tools enable its export and timing/memory reporting; no extra CTest owner.
- `scripts/generate_new_world.cpp` and `scripts/tests/world_generator_cli.cmake`
  remain with the existing `pf-generate-world` target and generator CTest entries.
  Vendored dependency tests are outside this migration.
- `InteractionApiCompileCheck.cpp`: an object-only public API contract owned by
  `pf-interaction-api-compile-contract`; `pf-compile-contracts` includes it in the
  default build, so a violated static assertion fails compilation without a
  runnable smoke owner.
- `SimulationStepTimingChecks.cpp`, `OccupantPackingChecks.cpp`,
  `WorldRenderSlotChecks.cpp`, and `SectorTilesetChecks.cpp`: retained direct,
  headless standalone executables. Their CTest labels include `smoke;standalone`
  plus `core` or `render`; they use the same warnings and elevated analysis as
  modular smoke targets.

This is source-level ownership, not a promised future taxonomy: mixed dependency
sources still need splitting before their follow-up domain migrations.
