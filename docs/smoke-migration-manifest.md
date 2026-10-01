# Smoke migration ownership manifest

Snapshot after the Permissions migration #290 (parent #278). Each
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
it anymore; its domain CTest owner is `smoke-render`. All other Render checks
remain legacy-owned for follow-up migration.

#284 migrates the core-only Layer, Sector, Background, Window, Facade,
zero-sized Location, and threshold-layer topology groups to `smoke/world`, with
stable registrations under `smoke-world`. The editor-dependent Background and
Facade panel checks and renderer-dependent Facade checks remain legacy-owned for
their dedicated migrations. The legacy aggregate and `--render-checks` no longer
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

The legacy aggregate is not yet a compatibility orchestrator; converting it is
follow-up work. Other overlapping legacy CTest selections are unchanged.

Paths in the table are relative to `src/headless/`.

| Check source | Ownership | Target |
| --- | --- | --- |
| `smoke/permissions/Access.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/Destinations.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/DestinationEditor.cpp` | module-owned | `pf-smoke-permissions-editor` |
| `smoke/permissions/AccessEditor.cpp` | module-owned | `pf-smoke-permissions-editor` |
| `smoke/agent/Identity.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/Activation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/ActivationEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/behaviours/Registry.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/Assignment.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/Workflow.cpp` | module-owned | `pf-smoke-behaviours` |
| `smoke/behaviours/RegistryEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
| `smoke/behaviours/AssignmentEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
| `smoke/behaviours/PortabilityEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
| `smoke/behaviours/DeleteEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
| `smoke/behaviours/SchemaReconciliationEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
| `smoke/behaviours/WorkflowEditor.cpp` | module-owned | `pf-smoke-behaviours-editor` |
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
| `smoke/agent/ColourEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupAssignment.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupAssignmentEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupClipboardEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupCount.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupCountEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupDelete.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupDeleteEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupIdAllocation.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupIdAllocationEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/Group.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/GroupTopology.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/GroupTopologyEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/Height.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/HeightEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/agent/IndividualProperties.cpp` | module-owned | `pf-smoke-agent` |
| `AgentPathRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/tags/Assignment.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/AssignmentEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/ClipboardEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/CoordinationEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/DeleteEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/DocumentSaveEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/MobilityProfile.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/Reconciliation.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/RegistryChangeEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/Registry.cpp` | module-owned | `pf-smoke-agent-tags` |
| `smoke/tags/RegistryEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/tags/ReloadEditor.cpp` | module-owned | `pf-smoke-agent-tags-editor` |
| `smoke/agent/WalkSpeed.cpp` | module-owned | `pf-smoke-agent` |
| `smoke/agent/WalkSpeedEditor.cpp` | module-owned | `pf-smoke-agent-editor` |
| `smoke/world/BackgroundCascadeDelete.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPaint.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundPlacement.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/BackgroundSector.cpp` | module-owned | `pf-smoke-world` |
| `BackgroundSelectionPanelSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DocumentHistorySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenApartRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenLeftRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenRightRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorPanelScopeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/Preflight.cpp` | module-owned | `pf-smoke-permissions` |
| `DoorTwoSidedButtonSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/world/Layers.cpp` | module-owned | `pf-smoke-world` |
| `EscalatorWalkingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeDrawOrderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeEditorSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/world/Facades.cpp` | module-owned | `pf-smoke-world` |
| `GraphicsStartupSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/InteractionMobility.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/InteractionGeometry.cpp` | module-owned | `pf-smoke-permissions` |
| `IsolatedSectorPathingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `LadderForceBridgeRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `LiftBoardingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `LiftRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `MarkerIdentitySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `MetricsChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `MobilityProfileRoutingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `MovementCommandSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `NonFiniteTimingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `OccupantPackingChecks.cpp` | module-owned (retained standalone) | `pf-occupant-packing-checks` |
| `OnboardAgentDeletionSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `PaletteTraySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `PathfindingWorkspaceSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `PausePositionSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/Adherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/AdherenceEditor.cpp` | module-owned | `pf-smoke-permissions-editor` |
| `RenderOrderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `RestoredPathMobilitySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `RoutePlanningSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `RoutePlanningTimePropertySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `SectorTilesetChecks.cpp` | module-owned (retained standalone) | `pf-sector-tileset-checks` |
| `SerializationSmokeChecks.cpp` (remaining checks after the format extraction below) | legacy-owned | `prometheum-fermide-headless` |
| `smoke/persistence/Formats.cpp` | module-owned | `pf-smoke-persistence` |
| `ShuttleDoorQuerySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ShuttleDoorRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ShuttleRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `SimulationStepTimingChecks.cpp` | module-owned (retained standalone) | `pf-simulation-step-timing-checks` |
| `StairRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/world/ThresholdLayerOverlap.cpp` | module-owned | `pf-smoke-world` |
| `smoke/permissions/Refusal.cpp` | module-owned | `pf-smoke-permissions` |
| `ThresholdRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/permissions/LandingAdherence.cpp` | module-owned | `pf-smoke-permissions` |
| `smoke/permissions/LandingAdherenceEditor.cpp` | module-owned | `pf-smoke-permissions-editor` |
| `ViewportCullingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ViewportDragScrollSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ViewportZoomSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/render/Walls.cpp` | module-owned | `pf-smoke-render` |
| `smoke/world/WindowIntoBackground.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowLayers.cpp` | module-owned | `pf-smoke-world` |
| `smoke/world/WindowMultiBackground.cpp` | module-owned | `pf-smoke-world` |
| `WorldRenderLifetimeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
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
`resources/Office.world.yaml`. All remaining serialization, save-transaction,
World restoration, editor, and recent-file checks retain legacy ownership and
continue through `--serialization-checks` (also the existing legacy aggregate).
Neither legacy entry point executes the migrated checks.

## Support and non-smoke code

- `smoke/behaviours/Main.cpp` and `EditorMain.cpp`: explicit per-tier registries.
  `Checks.h` declares registrations; `TemporaryDirectory.h` reserves Context-owned
  fixture directories; `RuntimeFixtures.h` writes runtime package text;
  `EditorState.h` resets editor state. These helpers do not own extra checks.
- `smoke/tests/BehavioursContract.cmake`: exact listings, every selection, misuse,
  external empty working directory, and eight concurrent invocations per tier;
  owned by `smoke-behaviours-contract` (`harness;core;editor`).

- `smoke/tags/Main.cpp` and `EditorMain.cpp`: explicit per-tier Agent tag registries.
  `Checks.h` declares registration functions; `TemporaryDirectory.h` allocates
  distinct Context-owned fixture directories; `EditorState.h` resets tag panel,
  pending confirmation, World history, and transactional failure-injection state.
- `smoke/tests/TagsContract.cmake`: exact listings, every single-check selection,
  CLI misuse, external empty working directory, and eight concurrent invocations
  of each tier; owned by `smoke-agent-tags-contract` (`harness;core;editor`).

- `smoke/agent/Main.cpp` and `EditorMain.cpp`: explicit per-tier Agent registries.
  `Checks.h` declares registration functions; `EditorState.h` resets panel and
  document-history state before and after each editor check, including failures.
- `smoke/tests/AgentContract.cmake`: exact registries, every single-check selection,
  misuse, external empty working directory, and absence of working-directory
  output; owned by `smoke-agent-contract` (`harness;core;editor`).
- `src/DocumentEdit.cpp`, `src/AgentGroupsPanel.cpp`,
  `src/AgentGroupAssignmentPanel.cpp`, and `src/AgentClipboard.cpp`: production
  seams compiled once in `pf-agent-editing`, shared by the GUI, remaining legacy
  checks, and Agent editor tier. Not smoke-check sources.

- `RenderGuiStubs.cpp` and `support/ImGuiContext.cpp`: reusable CPU-only support,
  compiled by `pf-headless-render-support`, not independent checks.
- `smoke/world/Main.cpp`: explicit World registry, not extra coverage.
- `smoke/tests/WorldContract.cmake`: World CLI and arbitrary-working-directory
  contract (`harness;core`, not smoke coverage).
- `smoke/persistence/Main.cpp`: explicit Persistence registry, not extra coverage.
- `smoke/tests/PersistenceContract.cmake`: Persistence CLI, external working
  directory, and concurrent invocation contract (`harness;core`, not smoke).
- `smoke/render/Main.cpp`: explicit Render registry, not extra coverage.
- `smoke/tests/RenderContract.cmake`: public Render CLI contract and no-output-file
  verification, executed by `smoke-render-contract` (harness, not smoke coverage).
- `smoke/simulation/Main.cpp`: explicit Simulation registry, not extra coverage.
- `smoke/support/Smoke.cpp`: harness mechanics only, owned by `pf-smoke-support`.
- `smoke/tests/Probe.cpp`: synthetic harness contract checks, owned by
  `pf-smoke-harness-probe`; CTest owns their execution through
  `smoke-harness-contract`, not as domain smoke coverage.
- `LiftBoardingSmokeChecks.cpp` also owns the Lift reproduction commands;
  `PausePositionSmokeChecks.cpp` also owns the pause-position reproduction;
  `PathfindingWorkspaceSmokeChecks.cpp` also owns the restoration benchmark and
  routing-scale World generator; `MetricsChecks.cpp` also owns metrics serving.
  These remain legacy-owned until their dedicated tool extraction tickets.
- `scripts/generate_new_world.cpp` and `scripts/tests/world_generator_cli.cmake`
  remain with the existing `pf-generate-world` target and generator CTest entries.
  Vendored dependency tests are outside this migration.

This is source-level ownership, not a promised future taxonomy: mixed dependency
sources still need splitting before their follow-up domain migrations.
