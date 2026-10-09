# Each smoke check has one module owner; compatibility only launches executables.
# No source globbing: each module explicitly owns its runner and check sources.
function(pf_smoke_target_policy target)
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}"
        FOLDER "Tests/Smoke")
endfunction()

add_library(pf-smoke-support STATIC support/Smoke.cpp)
target_include_directories(pf-smoke-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support"
    PRIVATE "${PF_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../support")
target_link_libraries(pf-smoke-support PUBLIC promethium-fermide-core)
target_compile_definitions(pf-smoke-support PRIVATE PF_SMOKE_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_compile_features(pf-smoke-support PUBLIC cxx_std_20)
pf_smoke_target_policy(pf-smoke-support)
set_target_properties(pf-smoke-support PROPERTIES FOLDER "Tests/Support")

function(pf_add_smoke_contract)
    cmake_parse_arguments(CONTRACT "" "NAME" "COMMAND" ${ARGN})
    add_test(NAME ${CONTRACT_NAME} COMMAND ${CONTRACT_COMMAND})
    set_property(TEST ${CONTRACT_NAME} PROPERTY PF_CONTRACT_COMMAND "${CONTRACT_COMMAND}")
endfunction()

function(pf_add_smoke_module target)
    cmake_parse_arguments(MODULE "" "NAME;TIMEOUT" "SOURCES;LIBRARIES;LABELS" ${ARGN})
    if(MODULE_UNPARSED_ARGUMENTS OR NOT MODULE_NAME OR NOT MODULE_SOURCES OR NOT MODULE_TIMEOUT)
        message(FATAL_ERROR "${target}: expected NAME, TIMEOUT and explicit SOURCES")
    endif()
    foreach(source IN LISTS MODULE_SOURCES)
        get_filename_component(absolute "${source}" REALPATH BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_property(owned GLOBAL PROPERTY PF_SMOKE_OWNED_SOURCES)
        if(absolute IN_LIST owned)
            message(FATAL_ERROR "Smoke source already owned by another module: ${absolute}")
        endif()
        if(TARGET promethium-fermide-headless)
            get_target_property(legacy_sources promethium-fermide-headless SOURCES)
            foreach(legacy_source IN LISTS legacy_sources)
                get_filename_component(legacy_absolute "${legacy_source}" REALPATH
                    BASE_DIR "${PROJECT_SOURCE_DIR}")
                if(absolute STREQUAL legacy_absolute)
                    message(FATAL_ERROR "Smoke source still owned by legacy executable: ${absolute}")
                endif()
            endforeach()
        endif()
        set_property(GLOBAL APPEND PROPERTY PF_SMOKE_OWNED_SOURCES "${absolute}")
    endforeach()
    foreach(dependency IN LISTS MODULE_LIBRARIES)
        if(NOT TARGET ${dependency})
            message(FATAL_ERROR "${target}: expected a library target: ${dependency}")
        endif()
        get_target_property(kind ${dependency} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            message(FATAL_ERROR "Smoke modules cannot link executable ${dependency}")
        endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY PF_SMOKE_MODULE_TARGETS ${target})
    add_executable(${target} ${MODULE_SOURCES})
    target_link_libraries(${target} PRIVATE pf-smoke-support ${MODULE_LIBRARIES})
    pf_smoke_target_policy(${target})
    add_test(NAME smoke-${MODULE_NAME} COMMAND ${target})
    set_tests_properties(smoke-${MODULE_NAME} PROPERTIES
        LABELS "smoke;functional;validation-fast;${MODULE_LABELS}" TIMEOUT ${MODULE_TIMEOUT})
endfunction()

pf_add_smoke_module(pf-smoke-simulation
    NAME simulation
    SOURCES
        simulation/Main.cpp simulation/Observation.cpp
        simulation/Pause.cpp
        simulation/Timing.cpp
        simulation/Teardown.cpp
        simulation/Ticking.cpp
        simulation/Traversal.cpp
        simulation/Interactions.cpp
        simulation/Doors.cpp
        simulation/BrokenDoors.cpp
        simulation/BrokenBulkheadDoors.cpp
        simulation/BrokenExtensibles.cpp
        simulation/BrokenEscalators.cpp
        simulation/BrokenLifts.cpp
        simulation/BrokenPlatformLifts.cpp
        simulation/BrokenShuttles.cpp
        simulation/DoorQueues.cpp
        simulation/CrossingBands.cpp
        simulation/Scale.cpp
        simulation/Airlocks.cpp simulation/AirlockCrawling.cpp simulation/SecurityScanners.cpp simulation/SecurityScannerCrawling.cpp simulation/DecontaminationCrawling.cpp simulation/LiftCrawling.cpp simulation/ShuttleCrawling.cpp simulation/MixedCrawling.cpp
        simulation/AccessPanels.cpp
        simulation/RoomHeights.cpp
        simulation/BoothWindows.cpp
        simulation/Dumbwaiters.cpp
        simulation/Furniture.cpp
        simulation/MarkerActions.cpp
    LIBRARIES promethium-fermide-core pf-pause-position-support
    LABELS core
    TIMEOUT 60
)

pf_add_smoke_contract(NAME smoke-simulation-contract COMMAND ${CMAKE_COMMAND}
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SimulationContract.cmake")
set_tests_properties(smoke-simulation-contract PROPERTIES LABELS "harness;core" TIMEOUT 240)

pf_add_smoke_module(pf-smoke-metrics
    NAME metrics
    SOURCES metrics/Main.cpp metrics/Metrics.cpp
    LIBRARIES pf-metrics httplib::httplib
    LABELS "metrics;http"
    TIMEOUT 30
)

add_test(NAME smoke-metrics-contract COMMAND ${CMAKE_COMMAND}
    "-DMETRICS=$<TARGET_FILE:pf-smoke-metrics>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/MetricsContract.cmake")
set_tests_properties(smoke-metrics-contract PROPERTIES LABELS "harness;metrics;http" TIMEOUT 60)

# Startup owns a real GUI subprocess and therefore only exists when its required
# optional GUI capability and child product can be built. Both CTest entries are
# serialized because the child writes the application's shared startup log.
if(PF_BUILD_GUI)
    pf_add_smoke_module(pf-smoke-startup
        NAME startup
        SOURCES startup/Main.cpp startup/Startup.cpp
        LABELS "startup;graphics;subprocess;gui"
        TIMEOUT 40
    )
    target_compile_definitions(pf-smoke-startup PRIVATE
        "PF_STARTUP_GUI_EXECUTABLE=\"$<TARGET_FILE:editor>\"")
    add_dependencies(pf-smoke-startup editor)
    set_tests_properties(smoke-startup PROPERTIES RUN_SERIAL TRUE)

    add_test(NAME smoke-startup-contract COMMAND ${CMAKE_COMMAND}
        "-DSTARTUP=$<TARGET_FILE:pf-smoke-startup>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/StartupContract.cmake")
    set_tests_properties(smoke-startup-contract PROPERTIES
        LABELS "harness;startup;graphics;subprocess;gui"
        TIMEOUT 120
        RUN_SERIAL TRUE)

    if(BUILD_TESTING)
        # Synthetic children never touch the application's startup log, so this
        # contract can run in parallel. No editor/graphics library is linked.
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(pf-startup-probe tests/StartupProbe.cpp startup/Startup.cpp)
        target_link_libraries(pf-startup-probe PRIVATE pf-smoke-support)
        target_include_directories(pf-startup-probe PRIVATE "${PF_INCLUDE_DIR}")
        target_compile_definitions(pf-startup-probe PRIVATE PF_STARTUP_GUI_EXECUTABLE="unused")
        pf_smoke_target_policy(pf-startup-probe)
        set_target_properties(pf-startup-probe PROPERTIES FOLDER "Tests/Harness")
        add_test(NAME smoke-startup-failures COMMAND ${Python3_EXECUTABLE}
            "${PROJECT_SOURCE_DIR}/scripts/tests/startup_failures.py"
            "$<TARGET_FILE:pf-smoke-startup>" "$<TARGET_FILE:pf-startup-probe>")
        set_tests_properties(smoke-startup-failures PROPERTIES
            LABELS "harness;startup;subprocess" TIMEOUT 90)
    endif()
endif()

pf_add_smoke_module(pf-smoke-agent
    NAME agent
    SOURCES
        agent/Main.cpp
        agent/Identity.cpp
        agent/Activation.cpp
        agent/Group.cpp
        agent/GroupAssignment.cpp
        agent/GroupCount.cpp
        agent/GroupDelete.cpp
        agent/GroupIdAllocation.cpp
        agent/GroupTopology.cpp
        agent/Colour.cpp
        agent/WalkSpeed.cpp
        agent/Height.cpp
        agent/DoorClearance.cpp
        agent/BulkheadCrawling.cpp
        agent/IndividualProperties.cpp
        agent/AgentTypes.cpp
        agent/NoneUsage.cpp
        agent/RemoteIntegration.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

# Explicit diagnostic invocations retain the smoke runner's ordinary CLI output
# contract while providing the labelled traces required by ticket #476.
add_test(NAME agent-type-preview-cache COMMAND pf-smoke-editor
    --check agentTypesPreviewQueriesReuseValidatedResource)
add_test(NAME agent-type-managed-preview-cache COMMAND pf-smoke-editor
    --check agentTypesManagedPreviewIsReadOnly)
set_tests_properties(agent-type-preview-cache agent-type-managed-preview-cache
    PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME agent-type-reserved-behaviour COMMAND pf-smoke-agent
    --check agentTypesReservedBehaviourMember)
set_tests_properties(agent-type-reserved-behaviour PROPERTIES LABELS "core;functional;smoke;validation-fast")

foreach(check remoteMixedWorld remoteMixedControls remoteMixedCosts agentTypesRemoteBoothWindowShutters agentTypesRemoteShutterProperties agentTypesRemoteShutterDeclarations agentTypesRemoteAccessPanels agentTypesRemotePanelProperties agentTypesRemotePanelDeclarations agentTypesRemoteOnboardJourneys agentTypesRemoteOrdinaryDoors agentTypesRemoteDoorGates agentTypesRemoteTightDoor agentTypesRemoteDeclarations agentTypesRemoteGeometry agentTypesRemoteJourneys agentTypesRemotePendingEdits agentTypesRemoteLandingButtons agentTypesInheritedObjectUsage agentTypesInheritedObjectUsageRoutes agentTypesObjectUsageOverrides agentTypesObjectUsageCommittedCrossing agentTypesNoneDeclarations agentTypesNoneInvalidDeclarations agentTypesNoneDirectRefusals agentTypesNoneDoors agentTypesNoneOtherResources agentTypesNoneSharedAirlock agentTypesNoneLifetimes agentTypesNoneSharedJourneys)
    add_test(NAME object-usage-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(object-usage-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
endforeach()

foreach(check agentTypesArmsDeclarations agentTypesArmsPhysicalOperations agentTypesArmsManualDoor agentTypesArmsOnboardSelector)
    add_test(NAME object-usage-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(object-usage-${check} PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
endforeach()

add_test(NAME agent-type-cleaning-bot COMMAND pf-smoke-agent --check agentTypesCleaningBot)
set_tests_properties(agent-type-cleaning-bot PROPERTIES
    LABELS "agent;functional;validation-fast" TIMEOUT 60)

add_test(NAME agent-type-topology-lifetime COMMAND pf-smoke-agent
    --check agentTypesTopologyReplayPreservesLiveInstances)
add_test(NAME object-usage-overrides-panel COMMAND pf-smoke-editor --check agentObjectUsagePanelEdits)
set_tests_properties(object-usage-overrides-panel PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-inherited-history COMMAND pf-smoke-editor --check agentTypesInheritedObjectUsageWorkflows)
set_tests_properties(object-usage-inherited-history PROPERTIES LABELS "agent;agent-tags;editor;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-overrides-history COMMAND pf-smoke-editor --check agentTypesObjectUsageOverrideWorkflows)
set_tests_properties(object-usage-overrides-history PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME object-usage-none-history COMMAND pf-smoke-editor --check agentTypesNoneClipboardAndHistory)
set_tests_properties(object-usage-none-history PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)
add_test(NAME agent-type-history-lifetime COMMAND pf-smoke-editor
    --check agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents)
set_tests_properties(agent-type-topology-lifetime agent-type-history-lifetime
    PROPERTIES LABELS "agent;functional;validation-fast" TIMEOUT 60)

add_test(NAME pose-lifecycle-ownership COMMAND pf-smoke-simulation --check markerActions/poseOwnershipAndEditSafety)
add_test(NAME pose-lifecycle-robot-exit COMMAND pf-smoke-simulation --check shuttles/robotCommittedExit)
set_tests_properties(pose-lifecycle-ownership pose-lifecycle-robot-exit PROPERTIES
    LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
add_test(NAME pose-lifecycle-editor-history COMMAND pf-smoke-editor --check roomHeightScale/occupiedPoseHistory)
set_tests_properties(pose-lifecycle-editor-history PROPERTIES
    LABELS "editor;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)

foreach(check IN ITEMS agentTypesPoseDeclarations agentTypesInvalidPoseDeclarations agentPosesSupportedRooms agentPosesSupportedBridge agentPosesSupportedDoors agentPosesCrouchingDoor agentPosesApertureBoundaries)
    add_test(NAME pose-selection-${check} COMMAND pf-smoke-agent --check ${check})
    set_tests_properties(pose-selection-${check} PROPERTIES
        LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
endforeach()
foreach(family IN ITEMS airlocks lifts shuttles securityScanners decontamination)
    add_test(NAME pose-selection-${family} COMMAND pf-smoke-simulation --check ${family}/supportedPoseRefusal)
    set_tests_properties(pose-selection-${family} PROPERTIES
        LABELS "core;functional;smoke;validation-fast;pose-selection" TIMEOUT 60)
endforeach()

add_test(NAME door-live-clearance-diagnostic COMMAND pf-smoke-agent --check liveDoorClearance)
add_test(NAME door-live-clearance-committed COMMAND pf-smoke-agent --check committedDoorEnvelope)
add_test(NAME door-live-clearance-editor COMMAND pf-smoke-editor --check scaled-doors/crossing-history)
set_tests_properties(door-live-clearance-diagnostic door-live-clearance-committed door-live-clearance-editor
    PROPERTIES ENVIRONMENT "PF_DOOR_CLEARANCE_TRACE=1" LABELS "door-clearance;functional;smoke;validation-fast" TIMEOUT 120)
add_test(NAME door-pose-clearance-diagnostic COMMAND pf-smoke-agent --check poseDoorClearanceDiagnostics)
add_test(NAME door-pose-clearance-movement-reset COMMAND pf-smoke-agent --check poseDoorMovementReset)
set_tests_properties(door-pose-clearance-diagnostic door-pose-clearance-movement-reset
    PROPERTIES LABELS "smoke;functional;validation-fast;core" TIMEOUT 30
    ENVIRONMENT "PF_DOOR_CLEARANCE_TRACE=1")

pf_add_smoke_contract(NAME smoke-agent-contract COMMAND ${CMAKE_COMMAND}
    "-DAGENT=$<TARGET_FILE:pf-smoke-agent>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/AgentContract.cmake")
set_tests_properties(smoke-agent-contract PROPERTIES LABELS "harness;core" TIMEOUT 60)

pf_add_smoke_module(pf-smoke-agent-tags
    NAME agent-tags
    SOURCES
        tags/Main.cpp
        tags/Registry.cpp
        tags/Assignment.cpp
        tags/Reconciliation.cpp
        tags/MobilityProfile.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-agent-tags-contract COMMAND ${CMAKE_COMMAND}
    "-DTAGS=$<TARGET_FILE:pf-smoke-agent-tags>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TagsContract.cmake")
set_tests_properties(smoke-agent-tags-contract PROPERTIES LABELS "harness;core" TIMEOUT 90)

pf_add_smoke_module(pf-smoke-behaviours
    NAME behaviours
    SOURCES
        behaviours/Main.cpp
        behaviours/Registry.cpp
        behaviours/Assignment.cpp
        behaviours/Workflow.cpp
        behaviours/RuntimePreflight.cpp
        behaviours/RuntimeContainment.cpp
        behaviours/RuntimeInstances.cpp
        behaviours/RuntimeCoroutines.cpp
        behaviours/RuntimeMovement.cpp
        behaviours/RuntimeCallbacks.cpp
        behaviours/RuntimeScheduling.cpp
        behaviours/RuntimeFailures.cpp
        behaviours/RuntimeDeterminism.cpp
        behaviours/RuntimeScale.cpp
        behaviours/RuntimeAuthorization.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,60,30>"
)

pf_add_smoke_contract(NAME smoke-behaviours-contract COMMAND ${CMAKE_COMMAND}
    "-DBEHAVIOURS=$<TARGET_FILE:pf-smoke-behaviours>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,60,15>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,120,45>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BehavioursContract.cmake")
set_tests_properties(smoke-behaviours-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,240,90>")

pf_add_smoke_module(pf-smoke-permissions
    NAME permissions
    SOURCES
        permissions/Main.cpp
        permissions/Locations.cpp
        permissions/LocationLifecycle.cpp
        permissions/LocationChanges.cpp
        permissions/LocationOccupancy.cpp
        permissions/Access.cpp
        permissions/Destinations.cpp
        permissions/Adherence.cpp
        permissions/LandingAdherence.cpp
        permissions/InteractionMobility.cpp
        permissions/InteractionGeometry.cpp
        permissions/Preflight.cpp
        permissions/Refusal.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
)

pf_add_smoke_contract(NAME smoke-permissions-contract COMMAND ${CMAKE_COMMAND}
    "-DPERMISSIONS=$<TARGET_FILE:pf-smoke-permissions>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,900,300>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PermissionsContract.cmake")
set_tests_properties(smoke-permissions-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,1800,600>")

pf_add_smoke_module(pf-smoke-transports
    NAME transports
    SOURCES
        transports/Main.cpp
        transports/Stairs.cpp
        transports/Occupants.cpp
        transports/PlatformLifts.cpp
        transports/Lifts.cpp
        transports/Shuttles.cpp
        transports/Escalators.cpp
        transports/Deletion.cpp
        transports/DoorQueries.cpp
        transports/Boarding.cpp
        transports/Ladders.cpp
        transports/ForceBridges.cpp
        transports/LayerJourney.cpp
    LIBRARIES pf-lift-boarding-support
    LABELS core
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,120>"
)

pf_add_smoke_contract(NAME smoke-transports-contract COMMAND ${CMAKE_COMMAND}
    "-DTRANSPORTS=$<TARGET_FILE:pf-smoke-transports>"
    "-DCONCURRENT_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,120>"
    "-DRUN_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,300,240>"
    "-DPARALLEL_TIMEOUT=$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,600,240>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TransportsContract.cmake")
set_tests_properties(smoke-transports-contract PROPERTIES LABELS "harness;core"
    TIMEOUT "$<IF:$<AND:$<PLATFORM_ID:Windows>,$<CONFIG:Debug>>,900,300>")

pf_add_smoke_module(pf-smoke-routing
    NAME routing
    SOURCES
        routing/Main.cpp
        routing/Planning.cpp
        routing/PlanningTime.cpp
        routing/Movement.cpp
        routing/PathSource.cpp
        routing/RestoredPaths.cpp
        routing/IsolatedSectors.cpp
        routing/Mobility.cpp
        routing/Workspace.cpp routing/DepthContinuity.cpp routing/SeatOccupancy.cpp
        routing/ThresholdRouteCost.cpp
        routing/StairRouteCost.cpp
        routing/LiftRouteCost.cpp
        routing/ShuttleRouteCost.cpp
        routing/LadderForceBridgeRouteCost.cpp
    LIBRARIES promethium-fermide-core pf-routing-population-support
    LABELS core
    TIMEOUT 120
)

pf_add_smoke_contract(NAME smoke-routing-contract COMMAND ${CMAKE_COMMAND}
    "-DROUTING=$<TARGET_FILE:pf-smoke-routing>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RoutingContract.cmake")
set_tests_properties(smoke-routing-contract PROPERTIES LABELS "harness;core" TIMEOUT 300)

pf_add_smoke_module(pf-smoke-world
    NAME world
    SOURCES
        world/ScaledDoors.cpp world/Main.cpp world/AccessPanels.cpp world/Airlocks.cpp world/Furniture.cpp world/SecurityScanners.cpp world/BoothWindows.cpp
        world/MarkerIdentity.cpp
        world/TwoSidedButtons.cpp
        world/BackgroundSector.cpp
        world/BackgroundPaint.cpp
        world/BackgroundPlacement.cpp
        world/BackgroundCascadeDelete.cpp
        world/Layers.cpp
        world/WindowLayers.cpp
        world/WindowIntoBackground.cpp
        world/WindowMultiBackground.cpp
        world/Facades.cpp
        world/ZeroSizeLocations.cpp
        world/ThresholdLayerOverlap.cpp
        world/ObjectEditing.cpp
        world/FloorsAndWalls.cpp
        world/Topology.cpp
        world/RoomHeightScale.cpp
        world/LayerDeletion.cpp
    LIBRARIES promethium-fermide-core
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-world-contract COMMAND ${CMAKE_COMMAND}
    "-DWORLD=$<TARGET_FILE:pf-smoke-world>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/WorldContract.cmake")
set_tests_properties(smoke-world-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-persistence
    NAME persistence
    SOURCES
        persistence/Main.cpp persistence/AccessPanels.cpp persistence/Airlocks.cpp persistence/Furniture.cpp persistence/SecurityScanners.cpp persistence/BoothWindows.cpp
        persistence/Formats.cpp
        persistence/TransactionalWrites.cpp
        persistence/DocumentPaths.cpp
        persistence/DocumentSaves.cpp
        persistence/MalformedInput.cpp
        persistence/RecentDocuments.cpp
        persistence/WorldDocuments.cpp
        persistence/AgentRestoration.cpp
        persistence/AgentPoses.cpp
        persistence/Layers.cpp
        persistence/DomainReplay.cpp
        persistence/DoorDocuments.cpp
        persistence/RoomHeightScaleDocuments.cpp
        persistence/BrokenDoors.cpp
        persistence/BrokenExtensibles.cpp
        persistence/BrokenEscalators.cpp
        persistence/BrokenLifts.cpp
        persistence/BrokenPlatformLifts.cpp
        persistence/BrokenShuttles.cpp
        persistence/LiftDocuments.cpp
        persistence/ShuttleDocuments.cpp
        persistence/DeepLayerReplay.cpp
        persistence/Restoration.cpp
    LIBRARIES promethium-fermide-core pf-restoration-support
    LABELS core
    TIMEOUT 30
)

pf_add_smoke_contract(NAME smoke-persistence-contract COMMAND ${CMAKE_COMMAND}
    "-DPERSISTENCE=$<TARGET_FILE:pf-smoke-persistence>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PersistenceContract.cmake")
set_tests_properties(smoke-persistence-contract PROPERTIES LABELS "harness;core" TIMEOUT 60)

pf_add_smoke_module(pf-smoke-render
    NAME render
    SOURCES
        render/ScaledDoors.cpp render/Main.cpp render/AccessPanels.cpp render/Airlocks.cpp render/Furniture.cpp render/State.cpp render/Walls.cpp render/AgentPaths.cpp render/Transports.cpp render/SecurityScanners.cpp render/BoothWindows.cpp
        render/SerializationRendering.cpp
        render/AgentPoses.cpp
        render/DrawOrder.cpp
        render/DoorOpenApart.cpp
        render/DoorOpenLeft.cpp
        render/DoorOpenRight.cpp
        render/BrokenExtensibles.cpp
        render/BrokenEscalators.cpp
        render/BrokenLifts.cpp
        render/BrokenPlatformLifts.cpp
        render/BrokenShuttles.cpp
        render/Facades.cpp
        render/FacadeDrawOrder.cpp
        render/ViewportCulling.cpp
        render/ViewportDragScroll.cpp
        render/ViewportZoom.cpp
        render/Lifetime.cpp
        render/DoorButtons.cpp
    LIBRARIES pf-headless-render-support
    LABELS render
    TIMEOUT 30
)

add_test(NAME agent-pose-image-tiles COMMAND pf-smoke-render --check agentPoses)
add_test(NAME agent-pose-bed-artwork COMMAND pf-smoke-render --check furniture/bedRenderOffset)
set_tests_properties(agent-pose-image-tiles agent-pose-bed-artwork PROPERTIES
    LABELS "render;functional;smoke;validation-fast;pose-selection" TIMEOUT 30)

pf_add_smoke_contract(NAME smoke-render-contract COMMAND ${CMAKE_COMMAND}
    "-DRENDER=$<TARGET_FILE:pf-smoke-render>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RenderContract.cmake")
set_tests_properties(smoke-render-contract PROPERTIES LABELS "harness;render" TIMEOUT 30)

# Synthetic outcomes exercise harness mechanics, never production smoke coverage.
add_executable(pf-smoke-harness-probe tests/Probe.cpp)
target_link_libraries(pf-smoke-harness-probe PRIVATE pf-smoke-support)
pf_smoke_target_policy(pf-smoke-harness-probe)
set_target_properties(pf-smoke-harness-probe PROPERTIES FOLDER "Tests/Harness")
add_test(NAME smoke-harness-contract COMMAND ${CMAKE_COMMAND}
    "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>"
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Contract.cmake")
set_tests_properties(smoke-harness-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-editor
    NAME editor
    SOURCES
        editor/ScaledDoors.cpp editor/RoomHeightScale.cpp editor/Main.cpp editor/MarkerActions.cpp editor/AccessPanels.cpp editor/Airlocks.cpp editor/Furniture.cpp editor/State.cpp editor/Isolation.cpp editor/SecurityScanners.cpp editor/BoothWindows.cpp
        agent/ActivationEditor.cpp
        agent/GroupEditor.cpp
        agent/GroupAssignmentEditor.cpp
        agent/GroupCountEditor.cpp
        agent/GroupDeleteEditor.cpp
        agent/GroupIdAllocationEditor.cpp
        agent/GroupTopologyEditor.cpp
        agent/GroupClipboardEditor.cpp
        agent/AgentTypeEditor.cpp
        agent/ColourEditor.cpp
        agent/WalkSpeedEditor.cpp
        agent/HeightEditor.cpp
        agent/MobilityProfileEditor.cpp
        agent/PoseEditor.cpp
        tags/RegistryEditor.cpp
        tags/AssignmentEditor.cpp
        tags/RegistryChangeEditor.cpp
        tags/DocumentSaveEditor.cpp
        tags/ReloadEditor.cpp
        tags/DeleteEditor.cpp
        tags/CoordinationEditor.cpp
        tags/ClipboardEditor.cpp
        behaviours/RegistryEditor.cpp
        behaviours/AssignmentEditor.cpp
        behaviours/PortabilityEditor.cpp
        behaviours/DeleteEditor.cpp
        behaviours/SchemaReconciliationEditor.cpp
        behaviours/WorkflowEditor.cpp
        permissions/LocationsEditor.cpp
        permissions/LocationLifecycleEditor.cpp
        permissions/LocationPlacementEditor.cpp
        permissions/DestinationEditor.cpp
        permissions/AccessEditor.cpp
        permissions/AdherenceEditor.cpp
        permissions/LandingAdherenceEditor.cpp
        transports/EscalatorsEditor.cpp
        routing/PlanningEditor.cpp
        routing/PlanningTimeEditor.cpp
        editor/Background.cpp
        editor/Facade.cpp
        editor/DoorPanel.cpp
        editor/BrokenExtensibles.cpp
        editor/BrokenEscalators.cpp
        editor/BrokenLifts.cpp
        editor/BrokenPlatformLifts.cpp
        editor/BrokenShuttles.cpp
        editor/Palette.cpp
        editor/History.cpp
    LIBRARIES pf-agent-editing pf-agent-resources pf-headless-render-support
    LABELS editor
    TIMEOUT 60
)

pf_add_smoke_contract(NAME smoke-editor-contract COMMAND ${CMAKE_COMMAND}
    "-DEDITOR=$<TARGET_FILE:pf-smoke-editor>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorContract.cmake")
set_tests_properties(smoke-editor-contract PROPERTIES LABELS "harness;editor" TIMEOUT 180)

# The historical *-contract entries retain EVERY exhaustive assertion and remain
# in unfiltered CTest. Lightweight lanes reuse their inventories and time budgets.
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(module IN ITEMS simulation permissions routing transports behaviours
            agent-tags agent world persistence render editor)
        set(stress smoke-${module}-contract)
        get_test_property(${stress} LABELS labels)
        get_test_property(${stress} PF_CONTRACT_COMMAND command)
        get_test_property(${stress} TIMEOUT budget)
        # Agent and World only select every check; the others fan out eight children.
        set(processors 8)
        if(module STREQUAL "agent" OR module STREQUAL "world")
            set(processors 1)
        endif()
        set_tests_properties(${stress} PROPERTIES
            LABELS "${labels};validation-stress" PROCESSORS ${processors})
        list(POP_BACK command script)
        list(POP_BACK command flag) # -P
        if(NOT flag STREQUAL "-P")
            message(FATAL_ERROR "${stress}: expected a CMake contract script")
        endif()
        foreach(lane IN ITEMS cli concurrency)
            set(name smoke-${module}-${lane}-contract)
            add_test(NAME ${name} COMMAND ${command}
                "-DLANE=${lane}" "-DPYTHON=${Python3_EXECUTABLE}"
                "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>" -P "${script}")
            set_tests_properties(${name} PROPERTIES
                LABELS "${labels};validation-fast;validation-${lane}" TIMEOUT ${budget})
            if(lane STREQUAL "concurrency")
                set_tests_properties(${name} PROPERTIES PROCESSORS 4)
            endif()
        endforeach()
    endforeach()
    # Single-check Metrics/Startup and synthetic harness contracts stay bounded.
    foreach(name IN ITEMS smoke-metrics-contract smoke-startup-contract
            smoke-startup-failures smoke-harness-contract)
        if(TEST ${name})
            set_property(TEST ${name} APPEND PROPERTY LABELS validation-fast)
        endif()
    endforeach()
    set_tests_properties(smoke-metrics-contract PROPERTIES PROCESSORS 8)
endif()
