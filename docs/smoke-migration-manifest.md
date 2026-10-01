# Smoke migration ownership manifest

Snapshot after the cross-domain Editor migration #295 (parent #278). Each
source below has exactly one current compilation/execution owner; all assertions
inside it belong to that owner.
`legacy-owned` means `prometheum-fermide-headless`. `module-owned` names the
independent target. The four pre-existing standalone checks are retained as-is,
not migrated into the new harness. Future batches must update this table and
remove the old source **and invocation** when transferring ownership.

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
removed; `--access-permission-checks` returns 2 with migration guidance. Remaining
inline Interaction scenarios and the compile-only API contract remain legacy-owned
for their separate follow-up tickets.

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
inline checks remain with their existing owners for separate tickets.

#293 migrates the six workspace and perceived route-cost sources into 31
individual Routing registrations (71 core registrations in total). Context resolves
all checked-in World fixtures; no source-location or working-directory lookup
remains. Original workloads, reference oracles, Path digests, and all 241 assertion
call sites remain in their new owners, including explicit tool assertions.
`RoutingTools.cpp` retains only the restoration benchmark and World export command.
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
isolation regression. Earlier migration descriptions above record the former
tiers; the table below is the current ownership authority. Selectors now prefix
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
| `DoorTwoSidedButtonSmokeChecks.cpp` (core only) | legacy-owned | `prometheum-fermide-headless` |
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
| `GraphicsStartupSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/InteractionMobility.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/InteractionGeometry.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/routing/IsolatedSectors.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/LadderForceBridgeRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/transports/Boarding.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/routing/LiftRouteCost.cpp` | module-owned | `pf-smoke-routing` |
| `MarkerIdentitySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `MetricsChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/routing/Mobility.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Movement.cpp` | module-owned | `pf-smoke-routing` |
| `NonFiniteTimingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `OccupantPackingChecks.cpp` | module-owned (retained standalone) | `pf-occupant-packing-checks` |
| `smoke/transports/Deletion.cpp` | module-owned | `pf-smoke-transports` |
| `smoke/editor/Palette.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/Workspace.cpp` | module-owned | `pf-smoke-routing` |
| `PausePositionSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/Adherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/AdherenceEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/render/DrawOrder.cpp` | module-owned | `pf-smoke-render` |
| `smoke/routing/RestoredPaths.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/Planning.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningEditor.cpp` | module-owned | `pf-smoke-editor` |
| `smoke/routing/PlanningTime.cpp` | module-owned | `pf-smoke-routing` |
| `smoke/routing/PlanningTimeEditor.cpp` | module-owned | `pf-smoke-editor` |
| `SectorTilesetChecks.cpp` | module-owned (retained standalone) | `pf-sector-tileset-checks` |
| `SerializationSmokeChecks.cpp` (remaining World/domain checks after #282/#296 below) | legacy-owned | `prometheum-fermide-headless` |
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
| `WorldTeardownSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/world/ZeroSizeLocations.cpp` | module-owned | `pf-smoke-world` |
| `SmokeScenario.cpp` (remaining inline aggregate checks after Agent identity extraction) | legacy-owned | `prometheum-fermide-headless` |
| `InteractionApiCompileCheck.cpp` (compile-only contract) | legacy-owned | `prometheum-fermide-headless` |
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
below. Remaining World restoration, compatibility and domain/editor checks retain
legacy ownership and continue through `--serialization-checks` (also the existing
legacy aggregate). Neither legacy entry point executes the migrated checks.

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
- `LiftBoardingSmokeChecks.cpp` owns only the legacy Lift reproduction commands;
  `PausePositionSmokeChecks.cpp` also owns the pause-position reproduction;
  `RoutingTools.cpp` owns the restoration benchmark and routing-scale World
  generator; `MetricsChecks.cpp` also owns metrics serving.
  These remain legacy-owned until their dedicated tool extraction tickets.
- `support/RoutingPopulation.cpp`: shared deterministic population workload and
  assertions, compiled once by `pf-routing-population-support`. Only explicit
  tools enable its export and timing/memory reporting; no extra CTest owner.
- `scripts/generate_new_world.cpp` and `scripts/tests/world_generator_cli.cmake`
  remain with the existing `pf-generate-world` target and generator CTest entries.
  Vendored dependency tests are outside this migration.

This is source-level ownership, not a promised future taxonomy: mixed dependency
sources still need splitting before their follow-up domain migrations.
