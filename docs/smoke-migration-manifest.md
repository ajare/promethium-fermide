# Smoke migration ownership manifest

Snapshot after the three-module pilot gate #283 (parent #278). Each source
below has exactly one current compilation/execution owner; all assertions inside
it belong to that owner.
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

The legacy aggregate is not yet a compatibility orchestrator; converting it is
follow-up work. Existing overlapping legacy CTest selections are unchanged.

Paths in the table are relative to `src/headless/`.

| Check source | Ownership | Target |
| --- | --- | --- |
| `AccessPermissionSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentActivationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourAssignmentSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourDeleteSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourPortabilitySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourRegistrySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourRuntimeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourSchemaReconciliationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentBehaviourWorkflowSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentColourSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupAssignmentSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupClipboardSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupCountSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupDeleteSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupIdAllocationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentGroupTopologySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentHeightSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentIndividualPropertySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentPathRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagAssignmentSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagClipboardSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagCoordinationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagDeleteSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagDocumentSaveSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagMobilityProfileSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagReconciliationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagRegistryChangeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagRegistrySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentTagReloadSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `AgentWalkSpeedSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `BackgroundCascadeDeleteSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `BackgroundPaintSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `BackgroundPlacementSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `BackgroundSectorSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `BackgroundSelectionPanelSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DocumentHistorySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenApartRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenLeftRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorOpenRightRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorPanelScopeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorPreflightSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `DoorTwoSidedButtonSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `EditorLayerSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `EscalatorWalkingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeDrawOrderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeEditorSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeRenderSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `FacadeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `GraphicsStartupSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `InteractionMobilitySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `InteractionPointGeometrySmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
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
| `PermissionAdherenceSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
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
| `ThresholdLayerOverlapSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ThresholdRefusalSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ThresholdRouteCostSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `TransportLandingAdherenceSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ViewportCullingSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ViewportDragScrollSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ViewportZoomSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `smoke/render/Walls.cpp` | module-owned | `pf-smoke-render` |
| `WindowIntoBackgroundSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `WindowLayerSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `WindowMultiBackgroundSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `WorldRenderLifetimeSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `WorldRenderSlotChecks.cpp` | module-owned (retained standalone) | `pf-world-render-slot-checks` |
| `WorldTeardownSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `ZeroSizeLocationSmokeChecks.cpp` | legacy-owned | `prometheum-fermide-headless` |
| `SmokeScenario.cpp` (all inline aggregate checks) | legacy-owned | `prometheum-fermide-headless` |
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

- `RenderGuiStubs.cpp` and `support/ImGuiContext.cpp`: reusable CPU-only support,
  compiled by `pf-headless-render-support`, not independent checks.
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
