# Each smoke check has one module owner; compatibility only launches executables.
# No source globbing: each module explicitly owns its runner and check sources.
function(pf_smoke_target_policy target)
    pf_apply_headless_verification_policy(${target})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PF_OUTPUT_DIR}")
endfunction()

add_library(pf-smoke-support STATIC support/Smoke.cpp)
target_include_directories(pf-smoke-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support"
    PRIVATE "${PF_INCLUDE_DIR}")
target_compile_definitions(pf-smoke-support PRIVATE PF_SMOKE_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_compile_features(pf-smoke-support PUBLIC cxx_std_20)
pf_smoke_target_policy(pf-smoke-support)

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
        if(TARGET prometheum-fermide-headless)
            get_target_property(legacy_sources prometheum-fermide-headless SOURCES)
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
        LABELS "smoke;${MODULE_LABELS}" TIMEOUT ${MODULE_TIMEOUT})
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
        simulation/DoorQueues.cpp
        simulation/CrossingBands.cpp
        simulation/Scale.cpp
    LIBRARIES prometheum-fermide-core pf-pause-position-support
    LABELS core
    TIMEOUT 60
)

add_test(NAME smoke-simulation-contract COMMAND ${CMAKE_COMMAND}
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
        agent/IndividualProperties.cpp
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 30
)

add_test(NAME smoke-agent-contract COMMAND ${CMAKE_COMMAND}
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
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 30
)

add_test(NAME smoke-agent-tags-contract COMMAND ${CMAKE_COMMAND}
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
        behaviours/RuntimeMovement.cpp
        behaviours/RuntimeCallbacks.cpp
        behaviours/RuntimeScheduling.cpp
        behaviours/RuntimeFailures.cpp
        behaviours/RuntimeDeterminism.cpp
        behaviours/RuntimeScale.cpp
        behaviours/RuntimeAuthorization.cpp
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 30
)

add_test(NAME smoke-behaviours-contract COMMAND ${CMAKE_COMMAND}
    "-DBEHAVIOURS=$<TARGET_FILE:pf-smoke-behaviours>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BehavioursContract.cmake")
set_tests_properties(smoke-behaviours-contract PROPERTIES LABELS "harness;core" TIMEOUT 90)

pf_add_smoke_module(pf-smoke-permissions
    NAME permissions
    SOURCES
        permissions/Main.cpp
        permissions/Access.cpp
        permissions/Destinations.cpp
        permissions/Adherence.cpp
        permissions/LandingAdherence.cpp
        permissions/InteractionMobility.cpp
        permissions/InteractionGeometry.cpp
        permissions/Preflight.cpp
        permissions/Refusal.cpp
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 240
)

add_test(NAME smoke-permissions-contract COMMAND ${CMAKE_COMMAND}
    "-DPERMISSIONS=$<TARGET_FILE:pf-smoke-permissions>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PermissionsContract.cmake")
set_tests_properties(smoke-permissions-contract PROPERTIES LABELS "harness;core" TIMEOUT 600)

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
    TIMEOUT 120
)

add_test(NAME smoke-transports-contract COMMAND ${CMAKE_COMMAND}
    "-DTRANSPORTS=$<TARGET_FILE:pf-smoke-transports>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TransportsContract.cmake")
set_tests_properties(smoke-transports-contract PROPERTIES LABELS "harness;core" TIMEOUT 300)

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
        routing/Workspace.cpp
        routing/ThresholdRouteCost.cpp
        routing/StairRouteCost.cpp
        routing/LiftRouteCost.cpp
        routing/ShuttleRouteCost.cpp
        routing/LadderForceBridgeRouteCost.cpp
    LIBRARIES prometheum-fermide-core pf-routing-population-support
    LABELS core
    TIMEOUT 120
)

add_test(NAME smoke-routing-contract COMMAND ${CMAKE_COMMAND}
    "-DROUTING=$<TARGET_FILE:pf-smoke-routing>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RoutingContract.cmake")
set_tests_properties(smoke-routing-contract PROPERTIES LABELS "harness;core" TIMEOUT 300)

pf_add_smoke_module(pf-smoke-world
    NAME world
    SOURCES
        world/Main.cpp
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
        world/LayerDeletion.cpp
    LIBRARIES prometheum-fermide-core
    LABELS core
    TIMEOUT 30
)

add_test(NAME smoke-world-contract COMMAND ${CMAKE_COMMAND}
    "-DWORLD=$<TARGET_FILE:pf-smoke-world>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/WorldContract.cmake")
set_tests_properties(smoke-world-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-persistence
    NAME persistence
    SOURCES
        persistence/Main.cpp
        persistence/Formats.cpp
        persistence/TransactionalWrites.cpp
        persistence/DocumentPaths.cpp
        persistence/DocumentSaves.cpp
        persistence/MalformedInput.cpp
        persistence/RecentDocuments.cpp
        persistence/WorldDocuments.cpp
        persistence/AgentRestoration.cpp
        persistence/Layers.cpp
        persistence/DomainReplay.cpp
        persistence/DoorDocuments.cpp
        persistence/LiftDocuments.cpp
        persistence/ShuttleDocuments.cpp
        persistence/DeepLayerReplay.cpp
        persistence/Restoration.cpp
    LIBRARIES prometheum-fermide-core pf-restoration-support
    LABELS core
    TIMEOUT 30
)

add_test(NAME smoke-persistence-contract COMMAND ${CMAKE_COMMAND}
    "-DPERSISTENCE=$<TARGET_FILE:pf-smoke-persistence>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PersistenceContract.cmake")
set_tests_properties(smoke-persistence-contract PROPERTIES LABELS "harness;core" TIMEOUT 60)

pf_add_smoke_module(pf-smoke-render
    NAME render
    SOURCES
        render/Main.cpp render/State.cpp render/Walls.cpp render/AgentPaths.cpp render/Transports.cpp
        render/SerializationRendering.cpp
        render/DrawOrder.cpp
        render/DoorOpenApart.cpp
        render/DoorOpenLeft.cpp
        render/DoorOpenRight.cpp
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

add_test(NAME smoke-render-contract COMMAND ${CMAKE_COMMAND}
    "-DRENDER=$<TARGET_FILE:pf-smoke-render>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RenderContract.cmake")
set_tests_properties(smoke-render-contract PROPERTIES LABELS "harness;render" TIMEOUT 30)

# Synthetic outcomes exercise harness mechanics, never production smoke coverage.
add_executable(pf-smoke-harness-probe tests/Probe.cpp)
target_link_libraries(pf-smoke-harness-probe PRIVATE pf-smoke-support)
pf_smoke_target_policy(pf-smoke-harness-probe)
add_test(NAME smoke-harness-contract COMMAND ${CMAKE_COMMAND}
    "-DPROBE=$<TARGET_FILE:pf-smoke-harness-probe>"
    "-DSIMULATION=$<TARGET_FILE:pf-smoke-simulation>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Contract.cmake")
set_tests_properties(smoke-harness-contract PROPERTIES LABELS "harness;core" TIMEOUT 30)

pf_add_smoke_module(pf-smoke-editor
    NAME editor
    SOURCES
        editor/Main.cpp editor/State.cpp editor/Isolation.cpp
        agent/ActivationEditor.cpp
        agent/GroupEditor.cpp
        agent/GroupAssignmentEditor.cpp
        agent/GroupCountEditor.cpp
        agent/GroupDeleteEditor.cpp
        agent/GroupIdAllocationEditor.cpp
        agent/GroupTopologyEditor.cpp
        agent/GroupClipboardEditor.cpp
        agent/ColourEditor.cpp
        agent/WalkSpeedEditor.cpp
        agent/HeightEditor.cpp
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
        editor/Palette.cpp
        editor/History.cpp
    LIBRARIES pf-agent-editing pf-headless-render-support
    LABELS editor
    TIMEOUT 60
)

add_test(NAME smoke-editor-contract COMMAND ${CMAKE_COMMAND}
    "-DEDITOR=$<TARGET_FILE:pf-smoke-editor>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/EditorContract.cmake")
set_tests_properties(smoke-editor-contract PROPERTIES LABELS "harness;editor" TIMEOUT 180)
